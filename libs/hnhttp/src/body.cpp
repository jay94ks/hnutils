#include <hnhttp/body.hpp>
#include "internal.hpp"
#include "form_internal.hpp"
#include <cstring>

namespace hnhttp {
namespace detail {

std::string encodeComponent(std::string_view value, bool form) {
    const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || (!form && c == '~') || (form && c == '*')) { result += static_cast<char>(c); }
        else if (form && c == ' ') { result += '+'; }
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
std::string decodeComponent(std::string_view value, bool form) {
    std::string result;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c-'0'; }
        if (c >= 'a' && c <= 'f') { return c-'a'+10; }
        if (c >= 'A' && c <= 'F') { return c-'A'+10; } return -1;
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i+2 >= value.size() || digit(value[i+1]) < 0 || digit(value[i+2]) < 0) { throw std::invalid_argument("bad percent triplet"); }
            result += static_cast<char>((digit(value[i+1]) << 4) | digit(value[i+2])); i += 2;
        } else { result += form && value[i] == '+' ? ' ' : value[i]; }
    }
    return result;
}
void Queries::parse(std::string_view value, const SHttpLimits& limits) {
    std::size_t bytes = 0;
    while (!value.empty()) {
        auto next = value.find('&'); auto part = value.substr(0, next); auto eq = part.find('=');
        SQueryField f{decodeComponent(part.substr(0, eq)), eq == part.npos ? "" : decodeComponent(part.substr(eq+1)), eq != part.npos};
        bytes += f.name.size()+f.value.size();
        if (_fields.size() >= limits.maximumQueryFields || bytes > limits.maximumDecodedQueryBytes) { throw std::length_error("query limit"); }
        _fields.push_back(std::move(f));
        if (next == value.npos) { break; } value.remove_prefix(next+1);
    }
}
std::string Queries::wire() const {
    check(); std::string result;
    for (const auto& f : _fields) {
        if (!result.empty()) { result += '&'; } result += encodeComponent(f.name);
        if (f.hasEquals) { result += '='; result += encodeComponent(f.value); }
    }
    return result;
}

class BytesSource final : public IHttpBodySource {
private:
    SBytes _bytes;
    SBodyMetadata _metadata;
    std::size_t _position = 0;
    bool _prepared = false;
    SHttpError _error;
public:
    BytesSource(SBytes bytes, std::string type) : _bytes(std::move(bytes)) {
        if (!fieldValue(type) || type.empty()) { throw std::invalid_argument("invalid body content type"); }
        _metadata.contentType = std::move(type); _metadata.contentLength = _bytes.size(); _metadata.replayable = true;
    }
    asyncpp::task<ERetCode> prepare(SOperationOptions o) override {
        auto p = policy(o); if (p == ERET_OK) { _prepared = true; } co_return p;
    }
    SBodyMetadata metadata() const override {
        if (!_prepared) { throw std::logic_error("body not prepared"); } return _metadata;
    }
    asyncpp::task<SReadResult> readSome(std::size_t count, SOperationOptions o) override {
        auto p = policy(o); if (p != ERET_OK) { co_return SReadResult{error(p), {}}; }
        if (!_prepared || !count) { co_return SReadResult{error(ERET_INVALID_STATE), {}}; }
        if (_position == _bytes.size()) { co_return SReadResult{error(ERET_EOF), {}}; }
        const auto size = (std::min)(count, _bytes.size()-_position);
        SBytes output(size); std::memcpy(output.data(), _bytes.data()+_position, size); _position += size;
        co_return SReadResult{error(ERET_OK), std::move(output)};
    }
    asyncpp::task<ERetCode> rewind(SOperationOptions o) override {
        auto p = policy(o); if (p == ERET_OK) { _position = 0; } co_return p;
    }
    std::vector<SHeaderField> trailers() const override { return {}; }
    SHttpError lastError() const override { return _error; }
};

class UrlForm final : public IFormData {
private:
    SFormOptions _options;
    std::vector<std::pair<std::string, std::string>> _fields;
    std::shared_ptr<IHttpBodySource> _source;
public:
    explicit UrlForm(SFormOptions options) : _options(std::move(options)) {}
    EFormEncoding encoding() const noexcept override { return EFORM_URLENCODED; }
    IFormData& addField(std::string name, std::string value) override {
        if (_source) { throw std::logic_error("form already prepared"); }
        if (_fields.size() >= _options.maximumParts || name.size() > _options.maximumNameBytes
            || value.size() > _options.maximumFieldBytes) { throw std::length_error("form limit"); }
        _fields.emplace_back(std::move(name), std::move(value)); return *this;
    }
    IFormData& addField(std::string name, std::int64_t value) override { return addField(std::move(name), std::to_string(value)); }
    IFormData& addFile(std::string, std::string, std::shared_ptr<IHttpBodySource>) override { unsupported("urlencoded form has no files"); throw std::logic_error("unreachable"); }
    asyncpp::task<ERetCode> prepare(SOperationOptions o) override {
        auto p = policy(o); if (p != ERET_OK) { co_return p; }
        if (!_source) {
            std::string wire;
            for (const auto& f : _fields) {
                if (!wire.empty()) { wire += '&'; }
                wire += encodeComponent(f.first, true); wire += '='; wire += encodeComponent(f.second, true);
                if (wire.size() > _options.maximumPartBytes) { co_return ERET_LIMIT; }
                if (asyncpp::dispatcher::current()) { co_await asyncpp::defer(asyncpp::dispatcher::current()); }
            }
            _source = IHttpBodySource::fromBytes(SBytes(wire.begin(), wire.end()), "application/x-www-form-urlencoded");
        }
        co_return co_await _source->prepare(o);
    }
    SBodyMetadata metadata() const override { if (!_source) { throw std::logic_error("form not prepared"); } return _source->metadata(); }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override {
        if (!_source) { co_return SReadResult{error(ERET_INVALID_STATE), {}}; } co_return co_await _source->readSome(n, o);
    }
    asyncpp::task<ERetCode> rewind(SOperationOptions o) override {
        if (!_source) { co_return ERET_INVALID_STATE; } co_return co_await _source->rewind(o);
    }
    std::vector<SHeaderField> trailers() const override { return {}; }
    SHttpError lastError() const override { return {}; }
};
} // namespace detail

std::shared_ptr<IHttpBodySource> IHttpBodySource::fromBytes(SBytes bytes, std::string type) {
    return std::make_shared<detail::BytesSource>(std::move(bytes), std::move(type));
}
std::shared_ptr<IFormData> IFormData::create(EFormEncoding encoding, SFormOptions options) {
    detail::validateFormOptions(options);
    if (encoding == EFORM_MULTIPART) { return detail::createMultipart(std::move(options)); }
    if (encoding != EFORM_URLENCODED) { throw std::invalid_argument("invalid form encoding"); }
    return std::make_shared<detail::UrlForm>(std::move(options));
}
} // namespace hnhttp
