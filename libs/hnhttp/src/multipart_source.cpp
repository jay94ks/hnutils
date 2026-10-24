#include "internal.hpp"
#include "form_internal.hpp"
#include <atomic>
#include <set>

namespace hnhttp::detail {
void validateFormOptions(const SFormOptions& o) {
    if (!o.maximumParts || !o.maximumNameBytes || !o.maximumFieldBytes || !o.maximumPartBytes
        || !o.maximumPartHeaderBytes || !o.maximumPartHeaderFields || !o.maximumBoundaryAttempts
        || o.maximumNameBytes > SIZE_MAX/8 || o.maximumFieldBytes > SIZE_MAX/8) {
        throw std::invalid_argument("invalid form bounds");
    }
    if (!o.boundary.empty() && (o.boundary.size() > 70 || o.boundary.back() == ' '
        || o.boundary.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'()+_,-./:=? ") != std::string::npos)) {
        throw std::invalid_argument("invalid multipart boundary");
    }
}
namespace {
std::string quote(const std::string& text) {
    if (!fieldValue(text) || text.find('\t') != text.npos) { throw std::invalid_argument("invalid form metadata"); }
    std::string result = "\"";
    for (const auto c : text) { if (c == '\\' || c == '"') { result += '\\'; } result += c; }
    return result+'"';
}
struct SPart {
    std::string name;
    std::optional<std::string> filename;
    std::shared_ptr<IHttpBodySource> source;
    std::string head;
    std::uint64_t size = 0;
};
class CMultipartSource final : public IFormData {
private:
    SFormOptions _options;
    std::vector<SPart> _parts;
    SBodyMetadata _metadata;
    SHttpError _error;
    bool _prepared = false, _busy = false;
    std::size_t _index = 0, _position = 0;
    unsigned _phase = 0;
    std::uint64_t _read = 0;
    std::string _terminal;
    void editable() const { if (_prepared || _busy) { throw std::logic_error("form frozen"); } }
    ERetCode fail(ERetCode code) { _error = error(code); return code; }
    void append(std::string name, std::optional<std::string> filename, std::shared_ptr<IHttpBodySource> source) {
        editable();
        if (!source || name.size() > _options.maximumNameBytes || (filename && filename->size() > _options.maximumNameBytes)
            || _parts.size() >= _options.maximumParts) { throw std::invalid_argument("invalid multipart part"); }
        (void)quote(name); if (filename) { (void)quote(*filename); }
        for (const auto& part : _parts) { if (part.source == source) { throw std::invalid_argument("multipart sources must have independent cursors"); } }
        _parts.push_back({std::move(name),std::move(filename),std::move(source),{},0});
    }
public:
    explicit CMultipartSource(SFormOptions options) : _options(std::move(options)) {}
    EFormEncoding encoding() const noexcept override { return EFORM_MULTIPART; }
    IFormData& addField(std::string name, std::string value) override {
        if (value.size() > _options.maximumFieldBytes || value.size() > _options.maximumPartBytes) { throw std::length_error("form field limit"); }
        append(std::move(name), {}, IHttpBodySource::fromBytes(SBytes(value.begin(),value.end()), "text/plain")); return *this;
    }
    IFormData& addField(std::string name, std::int64_t value) override { return addField(std::move(name), std::to_string(value)); }
    IFormData& addFile(std::string name, std::string filename, std::shared_ptr<IHttpBodySource> source) override {
        append(std::move(name), std::move(filename), std::move(source)); return *this;
    }
    asyncpp::task<ERetCode> prepare(SOperationOptions o) override {
        auto p = policy(o); if (p != ERET_OK) { co_return p; }
        if (_busy) { co_return ERET_BUSY; } if (_prepared) { co_return ERET_OK; }
        _busy = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{_busy};
        static std::atomic<std::uint64_t> sequence{0};
        for (std::size_t attempt = 0; attempt < _options.maximumBoundaryAttempts; ++attempt) {
            const auto boundary = _options.boundary.empty() ? "hnhttp-boundary-"+std::to_string(++sequence) : _options.boundary;
            const auto marker = "--"+boundary; bool collision = false; std::uint64_t total = 0;
            for (auto& part : _parts) {
                if ((p = co_await part.source->prepare(o)) != ERET_OK) { co_return fail(p); }
                const auto metadata = part.source->metadata();
                if (!metadata.replayable || !metadata.trailerNames.empty()) { co_return fail(ERET_UNSUPPORTED); }
                if (!fieldValue(metadata.contentType) || metadata.contentType.empty()) { co_return fail(ERET_INVALID_ARGUMENT); }
                if ((p = co_await part.source->rewind(o)) != ERET_OK) { co_return fail(p); }
                std::string tail; part.size = 0;
                while (true) {
                    auto chunk = co_await part.source->readSome(65536,o);
                    if (chunk.result.code == ERET_EOF) { if (!chunk.bytes.empty()) { co_return fail(ERET_IO); } break; }
                    if (chunk.result.code != ERET_OK) { co_return fail(chunk.result.code); }
                    if (chunk.bytes.empty() || chunk.bytes.size() > 65536) { co_return fail(ERET_IO); }
                    if (chunk.bytes.size() > _options.maximumPartBytes-part.size) { co_return fail(ERET_LIMIT); }
                    part.size += chunk.bytes.size(); tail.append(reinterpret_cast<const char*>(chunk.bytes.data()),chunk.bytes.size());
                    collision |= tail.find(marker) != tail.npos;
                    if (tail.size() > marker.size()) { tail.erase(0,tail.size()-marker.size()); }
                    if (asyncpp::dispatcher::current()) { co_await asyncpp::defer(asyncpp::dispatcher::current()); }
                }
                if ((metadata.contentLength && *metadata.contentLength != part.size) || !part.source->trailers().empty()) { co_return fail(ERET_INVALID_STATE); }
                if ((p = co_await part.source->rewind(o)) != ERET_OK) { co_return fail(p); }
                part.head = marker+"\r\nContent-Disposition: form-data; name="+quote(part.name);
                if (part.filename) { part.head += "; filename="+quote(*part.filename); }
                part.head += "\r\nContent-Type: "+metadata.contentType+"\r\n\r\n";
                if (part.head.size() > _options.maximumPartHeaderBytes || _options.maximumPartHeaderFields < 2) { co_return fail(ERET_LIMIT); }
                if (part.size > UINT64_MAX-part.head.size()-2 || total > UINT64_MAX-part.size-part.head.size()-2) { co_return fail(ERET_LIMIT); }
                total += part.head.size()+part.size+2;
            }
            if (collision) { if (!_options.boundary.empty()) { co_return fail(ERET_BOUNDARY_COLLISION); } continue; }
            _terminal = marker+"--\r\n";
            if (total > UINT64_MAX-_terminal.size()) { co_return fail(ERET_LIMIT); }
            _metadata = {"multipart/form-data; boundary="+quote(boundary), total+_terminal.size(), true, {}};
            _prepared = true; co_return ERET_OK;
        }
        co_return fail(ERET_BOUNDARY_COLLISION);
    }
    SBodyMetadata metadata() const override { if (!_prepared) { throw std::logic_error("form not prepared"); } return _metadata; }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override {
        const auto p = policy(o); if (p != ERET_OK) { co_return SReadResult{error(p),{}}; }
        if (!_prepared || !n) { co_return SReadResult{error(ERET_INVALID_STATE),{}}; }
        if (_busy) { co_return SReadResult{error(ERET_BUSY),{}}; }
        _busy = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{_busy};
        while (true) {
            if (_index == _parts.size()) { _phase = 3; }
            if (_phase == 1) {
                auto chunk = co_await _parts[_index].source->readSome((std::min)(n,std::size_t{65536}),o);
                if (chunk.result.code == ERET_EOF && chunk.bytes.empty()) {
                    if (_read != _parts[_index].size) { co_return SReadResult{error(fail(ERET_TRUNCATED)),{}}; } _phase = 2; _position = 0; continue;
                }
                if (chunk.result.code != ERET_OK || chunk.bytes.empty() || chunk.bytes.size() > n || chunk.bytes.size() > _parts[_index].size-_read) {
                    co_return SReadResult{error(fail(chunk.result.code == ERET_OK || chunk.result.code == ERET_EOF ? ERET_IO : chunk.result.code)),{}};
                }
                _read += chunk.bytes.size(); co_return chunk;
            }
            const std::string_view text = _phase == 0 ? std::string_view(_parts[_index].head) : _phase == 2 ? "\r\n" : std::string_view(_terminal);
            if (_position == text.size()) {
                if (_phase == 3) { co_return SReadResult{error(ERET_EOF),{}}; }
                if (_phase == 0) { _phase = 1; } else { ++_index; _phase = 0; _read = 0; }
                _position = 0; continue;
            }
            const auto length = (std::min)(n,text.size()-_position);
            const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data()+_position); _position += length;
            co_return SReadResult{error(ERET_OK),SBytes(begin,begin+length)};
        }
    }
    asyncpp::task<ERetCode> rewind(SOperationOptions o) override {
        auto p = policy(o); if (p != ERET_OK) { co_return p; }
        if (!_prepared) { co_return ERET_INVALID_STATE; } if (_busy) { co_return ERET_BUSY; }
        _busy = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{_busy};
        for (const auto& part : _parts) { if ((p = co_await part.source->rewind(o)) != ERET_OK) { co_return fail(p); } }
        _index = _position = 0; _phase = 0; _read = 0; co_return ERET_OK;
    }
    std::vector<SHeaderField> trailers() const override { return {}; }
    SHttpError lastError() const override { return _error; }
};
}
std::shared_ptr<IFormData> createMultipart(SFormOptions options) { return std::make_shared<CMultipartSource>(std::move(options)); }
}
