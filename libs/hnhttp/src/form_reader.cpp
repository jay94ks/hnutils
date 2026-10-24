#include "internal.hpp"
#include "form_internal.hpp"
#include <map>

namespace hnhttp::detail {
namespace {
struct SFormFailure { ERetCode code; };
[[noreturn]] void formBad(ERetCode code = ERET_MALFORMED) { throw SFormFailure{code}; }
struct SParameters { std::string type; std::map<std::string,std::string> values; };
SParameters parameters(std::string_view text) {
    if (!fieldValue(text)) { formBad(); }
    SParameters result; auto semicolon = text.find(';'); result.type = lower(trim(text.substr(0,semicolon)));
    if (semicolon == text.npos) { return result; } text.remove_prefix(semicolon+1);
    while (!text.empty()) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) { text.remove_prefix(1); }
        const auto equal = text.find('='); if (equal == text.npos) { formBad(); }
        const auto name = lower(trim(text.substr(0,equal))); if (!token(name)) { formBad(); } text.remove_prefix(equal+1);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) { text.remove_prefix(1); }
        std::string value;
        if (!text.empty() && text.front() == '"') {
            text.remove_prefix(1); bool closed = false;
            while (!text.empty()) {
                char c = text.front(); text.remove_prefix(1);
                if (c == '"') { closed = true; break; }
                if (c == '\\') { if (text.empty()) { formBad(); } c = text.front(); text.remove_prefix(1); }
                value += c;
            }
            if (!closed) { formBad(); }
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) { text.remove_prefix(1); }
            if (!text.empty() && text.front() != ';') { formBad(); }
        } else {
            const auto end = text.find(';'); value = trim(text.substr(0,end));
            if (!token(value)) { formBad(); } text.remove_prefix(end == text.npos ? text.size() : end);
        }
        if (!result.values.emplace(name,std::move(value)).second) { formBad(); }
        if (!text.empty()) { text.remove_prefix(1); }
    }
    return result;
}
struct SFormState {
    std::shared_ptr<IHttpBodyReader> body;
    SFormOptions options;
    bool multipart = false, started = false, final = false, eof = false, active = false, busy = false;
    std::size_t parts = 0;
    std::size_t operations = 0;
    std::string boundary, buffer;
    SHttpError failure;
};
struct SPartState {
    std::string name;
    std::optional<std::string> filename;
    Headers headers, trailers;
    bool complete = false, stored = false;
    SBytes value;
    std::uint64_t delivered = 0;
    SHttpError failure;
};
asyncpp::task<void> fill(std::shared_ptr<SFormState> s, SOperationOptions o) {
    if (s->eof) { co_return; }
    auto result = co_await s->body->readSome(65536,o);
    if (result.result.code == ERET_EOF) { if (!result.bytes.empty()) { formBad(ERET_IO); } s->eof = true; co_return; }
    if (result.result.code != ERET_OK) { formBad(result.result.code); }
    if (result.bytes.empty() || result.bytes.size() > 65536) { formBad(ERET_IO); }
    s->buffer.append(reinterpret_cast<const char*>(result.bytes.data()),result.bytes.size());
    if (asyncpp::dispatcher::current()) { co_await asyncpp::defer(asyncpp::dispatcher::current()); }
}
asyncpp::task<std::string> line(std::shared_ptr<SFormState> s, std::size_t maximum, SOperationOptions o, bool allowEofLine = false) {
    while (true) {
        const auto end = s->buffer.find("\r\n");
        if (end != s->buffer.npos) {
            if (end > maximum) { formBad(ERET_LIMIT); } auto result = s->buffer.substr(0,end); s->buffer.erase(0,end+2); co_return result;
        }
        if (s->buffer.size() > maximum && s->buffer.size()-maximum > 1) { formBad(ERET_LIMIT); }
        if (s->eof) {
            if (allowEofLine && !s->buffer.empty() && s->buffer.size() <= maximum) { auto result = std::move(s->buffer); s->buffer.clear(); co_return result; }
            formBad(ERET_TRUNCATED);
        } co_await fill(s,o);
    }
}
/** Find a complete MIME delimiter, tolerating a delimiter-like substring in binary content. */
asyncpp::task<SReadResult> partRead(std::shared_ptr<SFormState> s, std::shared_ptr<SPartState> part,
    std::size_t n, SOperationOptions o, bool probe = false) {
    const auto p = policy(o); if (p != ERET_OK) { co_return SReadResult{error(p),{}}; }
    if (part->complete) { co_return SReadResult{error(ERET_EOF),{}}; }
    if (s->failure.code != ERET_OK) { co_return SReadResult{s->failure,{}}; }
    if (s->busy) { co_return SReadResult{error(ERET_BUSY),{}}; }
    if (!n && !probe) { co_return SReadResult{error(ERET_INVALID_ARGUMENT),{}}; }
    s->busy = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{s->busy};
    if (++s->operations >= 64 && asyncpp::dispatcher::current()) { s->operations = 0; co_await asyncpp::defer(asyncpp::dispatcher::current()); }
    try {
        if (part->stored) {
            if (part->delivered == part->value.size()) { part->complete = true; s->active = false; co_return SReadResult{error(ERET_EOF),{}}; }
            if (!n) { co_return SReadResult{error(ERET_LIMIT),{}}; }
            const auto count = (std::min)(n,part->value.size()-static_cast<std::size_t>(part->delivered));
            auto first = part->value.begin()+static_cast<std::ptrdiff_t>(part->delivered); SBytes bytes(first,first+static_cast<std::ptrdiff_t>(count)); part->delivered += count;
            if (part->delivered == part->value.size()) { part->complete = true; s->active = false; }
            co_return SReadResult{error(ERET_OK),std::move(bytes)};
        }
        const auto marker = "\r\n--"+s->boundary;
        while (true) {
            std::size_t found = s->buffer.find(marker), safe = 0, remove = 0; bool terminal = false, more = false;
            while (found != s->buffer.npos) {
                std::size_t end = found+marker.size();
                if (s->buffer.size() < end+2 && !s->eof) { more = true; break; }
                terminal = s->buffer.compare(end,2,"--") == 0; if (terminal) { end += 2; }
                auto pad = end;
                while (end < s->buffer.size() && (s->buffer[end] == ' ' || s->buffer[end] == '\t')) { ++end; }
                if (end-pad > s->options.maximumPartHeaderBytes) { formBad(ERET_LIMIT); }
                if (end == s->buffer.size() && !s->eof) { more = true; break; }
                if (s->buffer.compare(end,2,"\r\n") == 0) { remove = end+2; break; }
                if (terminal && end == s->buffer.size() && s->eof) { remove = end; break; }
                if (end+1 == s->buffer.size() && s->buffer[end] == '\r' && !s->eof) { more = true; break; }
                found = s->buffer.find(marker,found+1);
            }
            if (remove) {
                if (!found) {
                    s->buffer.erase(0,remove); s->final = terminal; s->active = false; part->complete = true;
                    co_return SReadResult{error(ERET_EOF),{}};
                }
                safe = found;
            } else if (more) {
                if (found) { safe = found; } else { co_await fill(s,o); continue; }
            } else {
                if (s->eof) { formBad(ERET_TRUNCATED); }
                if (s->buffer.size() > marker.size()+2) { safe = s->buffer.size()-marker.size()-2; }
                else { co_await fill(s,o); continue; }
            }
            if (!n) { co_return SReadResult{error(ERET_LIMIT),{}}; }
            const auto count = (std::min)(n,safe);
            const auto maximum = part->filename ? s->options.maximumPartBytes : (std::min)(s->options.maximumPartBytes,static_cast<std::uint64_t>(s->options.maximumFieldBytes));
            if (count > maximum-part->delivered) { formBad(ERET_LIMIT); }
            const auto* first = reinterpret_cast<const std::uint8_t*>(s->buffer.data()); SBytes bytes(first,first+count);
            s->buffer.erase(0,count); part->delivered += count; co_return SReadResult{error(ERET_OK),std::move(bytes)};
        }
    } catch (const SFormFailure& failure) { part->failure = s->failure = error(failure.code); co_return SReadResult{s->failure,{}}; }
}
asyncpp::task<SReadResult> partCollect(std::shared_ptr<SFormState> s, std::shared_ptr<SPartState> part,
    std::uint64_t maximum, bool discard, SOperationOptions o) {
    SBytes bytes; std::uint64_t count = 0;
    while (true) {
        auto next = co_await partRead(s,part,static_cast<std::size_t>((std::min)(UINT64_C(65536),maximum-count)),o,true);
        if (next.result.code == ERET_EOF) { co_return SReadResult{error(ERET_OK),std::move(bytes)}; }
        if (next.result.code != ERET_OK) { co_return SReadResult{next.result,{}}; }
        count += next.bytes.size(); if (!discard) { bytes.insert(bytes.end(),next.bytes.begin(),next.bytes.end()); }
    }
}
class CPartReader final : public IHttpBodyReader {
private:
    std::shared_ptr<SFormState> _s; std::shared_ptr<SPartState> _part;
    static asyncpp::task<ERetCode> discardImpl(std::shared_ptr<SFormState> s, std::shared_ptr<SPartState> part, std::uint64_t n, SOperationOptions o) {
        co_return (co_await partCollect(std::move(s),std::move(part),n,true,o)).result.code;
    }
public:
    CPartReader(std::shared_ptr<SFormState> s, std::shared_ptr<SPartState> p) : _s(std::move(s)), _part(std::move(p)) {}
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return partRead(_s,_part,n,o); }
    asyncpp::task<ERetCode> discard(std::uint64_t n, SOperationOptions o) override { return discardImpl(_s,_part,n,o); }
    asyncpp::task<SReadResult> collect(std::size_t n, SOperationOptions o) override { return partCollect(_s,_part,n,false,o); }
    bool isComplete() const noexcept override { return _part->complete; }
    const IReadOnlyHttpHeaders& trailers() const override { if (!_part->complete) { throw std::logic_error("part pending"); } return _part->trailers; }
    SHttpError lastError() const override { return _part->failure; }
};
class CFormPart final : public IHttpFormPart {
private:
    std::shared_ptr<SPartState> _part; std::shared_ptr<IHttpBodyReader> _body;
public:
    CFormPart(std::shared_ptr<SFormState> s, std::shared_ptr<SPartState> p) : _part(std::move(p)),_body(std::make_shared<CPartReader>(std::move(s),_part)) {}
    std::string name() const override { return _part->name; }
    std::optional<std::string> filename() const override { return _part->filename; }
    const IReadOnlyHttpHeaders& headers() const override { return _part->headers; }
    std::shared_ptr<IHttpBodyReader> body() const override { return _body; }
};
asyncpp::task<SFormPartResult> nextPart(std::shared_ptr<SFormState> s, SOperationOptions o) {
    const auto p = policy(o); if (p != ERET_OK) { co_return SFormPartResult{error(p),{}}; }
    if (s->busy) { co_return SFormPartResult{error(ERET_BUSY),{}}; }
    if (s->failure.code != ERET_OK) { co_return SFormPartResult{s->failure,{}}; }
    if (s->active) { co_return SFormPartResult{error(ERET_BODY_PENDING),{}}; }
    s->busy = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{s->busy};
    try {
        if (s->multipart && !s->started) {
            const auto opening = "--"+s->boundary; std::size_t preamble = 0;
            while (true) {
                auto currentLine = co_await line(s,(std::max)(s->options.maximumPreambleBytes,opening.size()+2),o,true);
                auto stripped = currentLine; while (!stripped.empty() && (stripped.back() == ' ' || stripped.back() == '\t')) { stripped.pop_back(); }
                if (stripped == opening || stripped == opening+"--") { s->started = true; s->final = stripped == opening+"--"; break; }
                if (s->eof) { formBad(ERET_TRUNCATED); }
                if (currentLine.size()+2 > s->options.maximumPreambleBytes-preamble) { formBad(ERET_LIMIT); } preamble += currentLine.size()+2;
            }
        }
        if (s->final) {
            std::size_t epilogue = 0;
            while (true) {
                if (s->buffer.size() > s->options.maximumEpilogueBytes-epilogue) { formBad(ERET_LIMIT); }
                epilogue += s->buffer.size(); s->buffer.clear(); if (s->eof) { break; } co_await fill(s,o);
            }
            co_return SFormPartResult{error(ERET_EOF),{}};
        }
        auto part = std::make_shared<SPartState>();
        if (!s->multipart) {
            while (s->buffer.find('&') == s->buffer.npos && !s->eof) {
                if (s->buffer.size() > 3*(s->options.maximumNameBytes+s->options.maximumFieldBytes)+1) { formBad(ERET_LIMIT); } co_await fill(s,o);
            }
            if (s->buffer.empty() && s->eof) { co_return SFormPartResult{error(ERET_EOF),{}}; }
            auto end = s->buffer.find('&'); auto field = s->buffer.substr(0,end); s->buffer.erase(0,end == s->buffer.npos ? s->buffer.size() : end+1);
            const auto equal = field.find('='); part->name = decodeComponent(std::string_view(field).substr(0,equal),true);
            const auto value = equal == field.npos ? std::string{} : decodeComponent(std::string_view(field).substr(equal+1),true);
            if (value.size() > s->options.maximumFieldBytes || value.size() > s->options.maximumPartBytes) { formBad(ERET_LIMIT); }
            part->stored = true; part->value.assign(value.begin(),value.end());
        } else {
            std::size_t headerBytes = 0, fields = 0;
            while (true) {
                auto currentLine = co_await line(s,s->options.maximumPartHeaderBytes-headerBytes,o);
                if (currentLine.size()+2 > s->options.maximumPartHeaderBytes-headerBytes) { formBad(ERET_LIMIT); } headerBytes += currentLine.size()+2;
                if (currentLine.empty()) { break; } if (++fields > s->options.maximumPartHeaderFields) { formBad(ERET_LIMIT); }
                const auto colon = currentLine.find(':'); if (colon == currentLine.npos || !token(std::string_view(currentLine).substr(0,colon))) { formBad(); }
                auto name = lower(currentLine.substr(0,colon)); auto value = trim(std::string_view(currentLine).substr(colon+1));
                if (!fieldValue(value) || (name == "content-disposition" && part->headers.get(name))) { formBad(); }
                if (name == "content-transfer-encoding") { formBad(ERET_UNSUPPORTED); }
                part->headers.appendParsed({std::move(name),std::move(value)});
            }
            auto disposition = part->headers.get("content-disposition"); if (!disposition) { formBad(); }
            const auto parsed = parameters(*disposition); const auto name = parsed.values.find("name");
            if (parsed.type != "form-data" || name == parsed.values.end()) { formBad(); } part->name = name->second;
            if (auto filename = parsed.values.find("filename"); filename != parsed.values.end()) { part->filename = filename->second; }
        }
        if (part->name.size() > s->options.maximumNameBytes || (part->filename && part->filename->size() > s->options.maximumNameBytes)
            || ++s->parts > s->options.maximumParts) { formBad(ERET_LIMIT); }
        s->active = true; co_return SFormPartResult{error(ERET_OK),std::make_shared<CFormPart>(s,std::move(part))};
    } catch (const SFormFailure& failure) { s->failure = error(failure.code); }
    catch (const std::invalid_argument&) { s->failure = error(ERET_MALFORMED); }
    co_return SFormPartResult{s->failure,{}};
}
class CFormReader final : public IHttpFormReader {
private:
    std::shared_ptr<SFormState> _state;
public:
    explicit CFormReader(std::shared_ptr<SFormState> state) : _state(std::move(state)) {}
    asyncpp::task<SFormPartResult> next(SOperationOptions o) override { return nextPart(_state,o); }
    SHttpError lastError() const override { return _state->failure; }
};
}
}
namespace hnhttp {
std::shared_ptr<IHttpFormReader> IHttpFormReader::create(std::shared_ptr<IHttpBodyReader> body, std::string contentType, SFormOptions options) {
    if (!body) { throw std::invalid_argument("null form body"); } detail::validateFormOptions(options);
    detail::SParameters parsed;
    try { parsed = detail::parameters(contentType); } catch (const detail::SFormFailure&) { throw std::invalid_argument("invalid form content type"); }
    auto state = std::make_shared<detail::SFormState>(); state->body = std::move(body); state->options = std::move(options);
    if (parsed.type == "multipart/form-data") {
        const auto boundary = parsed.values.find("boundary");
        if (boundary == parsed.values.end() || boundary->second.empty()) { throw std::invalid_argument("missing multipart boundary"); }
        state->options.boundary = state->boundary = boundary->second; detail::validateFormOptions(state->options); state->multipart = true;
    } else if (parsed.type != "application/x-www-form-urlencoded") { detail::unsupported("unsupported form content type"); }
    return std::make_shared<detail::CFormReader>(std::move(state));
}
}
