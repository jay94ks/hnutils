#include "multiplexed_internal.hpp"
#include <asyncpp/launch.h>

namespace hnhttp::detail::mux {
void validate(const SHttpConnectionOptions& o) {
    if ((o.version != HTTP_2 && o.version != HTTP_3) || !o.limits.ioChunkBytes || !o.limits.maximumHeaderFields
        || !o.limits.maximumHeaderBytes || !o.limits.maximumPendingRequests || !o.limits.maximumMessageFrames
        || !o.limits.maximumPendingControlFrames || !o.limits.maximumPeerSettings
        || !o.scheduling.maximumBytesPerTurn || !o.scheduling.maximumOperationsPerTurn
        || !o.limits.maximumStreamBufferedBytes || !o.limits.maximumConnectionBufferedBytes) { throw std::invalid_argument("invalid multiplexed HTTP bounds"); }
    if (o.version == HTTP_2 && (o.limits.maximumConnectionBufferedBytes < 65535
        || o.http2.initialStreamWindow > 0x7fffffff || o.http2.initialStreamWindow > o.limits.maximumStreamBufferedBytes
        || o.http2.maximumFrameSize < 16384 || o.http2.maximumFrameSize > 16777215
        || o.http2.headerTableSize > o.limits.maximumHeaderCompressionTableBytes || !o.http2.maximumConcurrentStreams
        || !o.http2.maximumEncodedHeaderBytes || !o.http2.maximumContinuationFrames || !o.http2.maximumOutstandingSettings || !o.http2.maximumOutstandingPings)) { throw std::invalid_argument("invalid HTTP/2 bounds"); }
    if (o.version == HTTP_3 && (o.http3.qpackMaximumTableCapacity > o.limits.maximumHeaderCompressionTableBytes
        || o.http3.qpackBlockedStreams > o.limits.maximumPendingRequests || !o.http3.maximumEncodedFieldSectionBytes
        || !o.http3.maximumControlFrameBytes || !o.http3.maximumBlockedHeaderBytes
        || o.limits.maximumPendingRequests > SIZE_MAX/2)) { throw std::invalid_argument("invalid HTTP/3 bounds"); }
}
ERetCode entry(const std::shared_ptr<SConnection>& c, const SOperationOptions& o) {
    if (!current(c->executor)) { return ERET_WRONG_EXECUTOR; } auto p = policy(o); if (p != ERET_OK) { return p; }
    if (c->closed) { return c->failure.code == ERET_OK ? ERET_CLOSED : c->failure.code; } return ERET_OK;
}
void fail(const std::shared_ptr<SConnection>& c, ERetCode code, std::uint64_t protocol, std::string message) {
    if (c->closed.exchange(true)) { return; }
    c->failure = error(code,std::move(message)); c->failure.scope = EERROR_CONNECTION; c->failure.protocolError = protocol;
    c->wire->stop(protocol);
    for (const auto& [id, weak] : c->streams) { if (auto e = weak.lock()) { (void)id;
        if (code == ERET_CLOSED && e->receivedEnd && (e->sentEnd || e->sendFailure.code != ERET_OK)) { e->changed.set(); e->sent.set(); continue; }
        e->failure = c->failure; e->failure.streamId = e->id; e->state = ECONTEXT_FAILED; e->changed.set(); e->sent.set();
    } }
    c->incoming.clear(); c->changed.set();
}
void streamFail(const std::shared_ptr<SExchange>& e, ERetCode code, std::uint64_t protocol) {
    if (e->failure.code != ERET_OK) { return; }
    e->failure = error(code); e->failure.scope = EERROR_STREAM; e->failure.streamId = e->id; e->failure.protocolError = protocol;
    e->state = ECONTEXT_FAILED;
    if (e->buffered) { e->c->wire->consumed(e,e->buffered); e->c->buffered -= e->buffered; e->buffered = 0; e->body.clear(); }
    e->changed.set(); e->sent.set(); e->c->changed.set();
}
asyncpp::task<void> budget(std::shared_ptr<SConnection> c, std::size_t bytes) {
    c->bytes += bytes; ++c->operations;
    if (c->bytes >= c->options.scheduling.maximumBytesPerTurn || c->operations >= c->options.scheduling.maximumOperationsPerTurn) {
        c->bytes = c->operations = 0; co_await asyncpp::defer(c->executor.get());
    }
}
std::shared_ptr<SExchange> incoming(const std::shared_ptr<SConnection>& c, std::uint64_t id) {
    auto e = std::make_shared<SExchange>(); e->c = c; e->id = id; e->state = ECONTEXT_BOUND;
    e->requestHeaders.executor(c->executor); e->responseHeaders.executor(c->executor); e->queries.executor(c->executor); e->trailers.executor(c->executor);
    c->streams[id] = e; return e;
}
static bool forbidden(const std::string& n) { return n == "connection" || n == "proxy-connection" || n == "keep-alive" || n == "transfer-encoding" || n == "upgrade"; }
static bool endInput(const std::shared_ptr<SExchange>& e) {
    if (e->expected && !e->noBody && e->received != *e->expected) { return false; }
    e->receivedEnd = true; e->changed.set(); return true;
}
ERetCode receiveHeaders(const std::shared_ptr<SExchange>& e, std::vector<SHeaderField> fields, bool end) {
    try {
        const bool request = e->c->role == HTTP_SERVER;
        const bool trailer = e->head;
        if (e->receivedEnd || e->receivedTrailers || (trailer && !end && e->c->options.version == HTTP_2)) { return ERET_MALFORMED; }
        std::map<std::string,std::string> pseudo; Headers ordinary; bool regular = false; std::optional<std::uint64_t> length;
        for (auto& field : fields) {
            if (!fieldValue(field.value) || trim(field.value) != field.value || lower(field.name) != field.name) { return ERET_MALFORMED; }
            if (field.name.starts_with(':')) {
                if (trailer || regular || !pseudo.emplace(field.name,field.value).second) { return ERET_MALFORMED; }
            } else {
                regular = true; if (!token(field.name) || forbidden(field.name) || (field.name == "te" && field.value != "trailers")) { return ERET_MALFORMED; }
                if (field.name == "content-length") {
                    if (trailer) { return ERET_MALFORMED; } const auto parsed = number(field.value);
                    if (length && *length != parsed) { return ERET_MALFORMED; } length = parsed;
                }
                ordinary.appendParsed(std::move(field));
            }
        }
        if (trailer) {
            e->trailers = std::move(ordinary); e->trailers.executor(e->c->executor); e->receivedTrailers = true;
            if (end && !endInput(e)) { return ERET_MALFORMED; } return ERET_OK;
        }
        if (request) {
            if (!pseudo.contains(":method") || !token(pseudo[":method"])) { return ERET_MALFORMED; }
            for (const auto& [name,value] : pseudo) { (void)value; if (name != ":method" && name != ":scheme" && name != ":path" && name != ":authority") { return ERET_MALFORMED; } }
            e->method = pseudo[":method"];
            if (e->method == "CONNECT") {
                if (pseudo.contains(":scheme") || pseudo.contains(":path") || !pseudo.contains(":authority") || pseudo[":authority"].empty()) { return ERET_MALFORMED; }
                e->target.form = ETARGET_AUTHORITY; e->target.authority = pseudo[":authority"]; e->rawPath = e->target.authority;
            } else {
                if (!pseudo.contains(":scheme") || !pseudo.contains(":path") || (pseudo[":scheme"] != "http" && pseudo[":scheme"] != "https")) { return ERET_MALFORMED; }
                e->rawPath = pseudo[":path"];
                if (e->rawPath.empty() || (e->rawPath[0] != '/' && !(e->rawPath == "*" && e->method == "OPTIONS"))
                    || e->rawPath.find_first_of("\r\n #") != std::string::npos) { return ERET_MALFORMED; }
                const auto q = e->rawPath.find('?'); e->target.encodedPath = e->rawPath.substr(0,q); e->target.scheme = pseudo[":scheme"];
                if (q != std::string::npos) { e->queries.parse(std::string_view(e->rawPath).substr(q+1),e->c->options.limits); }
                e->target.authority = pseudo.contains(":authority") ? pseudo[":authority"] : ordinary.get("host").value_or("");
                if (e->target.authority.empty()) { return ERET_MALFORMED; }
                if (auto host = ordinary.get("host"); host && *host != e->target.authority) { return ERET_MALFORMED; }
            }
            e->requestHeaders = std::move(ordinary); e->requestHeaders.executor(e->c->executor); e->requestHeaders.freeze(); e->queries.freeze();
        } else {
            if (pseudo.size() != 1 || !pseudo.contains(":status") || pseudo[":status"].size() != 3) { return ERET_MALFORMED; }
            const auto code = number(pseudo[":status"]); if (code < 100 || code > 599 || code == 101) { return ERET_MALFORMED; }
            if (code < 200) {
                if (end || length || ++e->informational > e->c->options.limits.maximumInformationalResponses) { return ERET_MALFORMED; }
                if (e->observer) { e->observer->onInformational({e->c->options.version,static_cast<std::uint16_t>(code),{},ordinary.entries(0,4096)}); }
                return ERET_OK;
            }
            if (code == 204 && length) { return ERET_MALFORMED; }
            e->status = static_cast<std::uint16_t>(code); e->noBody = e->method == "HEAD" || code == 204 || code == 304;
            e->responseHeaders = std::move(ordinary); e->responseHeaders.executor(e->c->executor); e->responseHeaders.freeze();
        }
        e->expected = length; if (length && !e->noBody && *length > e->c->options.limits.maximumBodyBytes) { return ERET_LIMIT; }
        e->head = true; e->state = ECONTEXT_ACTIVE;
        if (end && !endInput(e)) { return ERET_MALFORMED; } e->changed.set();
        if (request) {
            if (e->c->incoming.size() >= e->c->options.limits.maximumPendingRequests) { return ERET_LIMIT; }
            e->c->incoming.push_back(e); e->c->changed.set();
        }
        return ERET_OK;
    } catch (const std::invalid_argument&) { return ERET_MALFORMED; } catch (const std::length_error&) { return ERET_LIMIT; }
}
ERetCode receiveData(const std::shared_ptr<SExchange>& e, SBytes bytes, bool end) {
    if (e->receivedTrailers && end && bytes.empty() && !e->receivedEnd) { return endInput(e) ? ERET_OK : ERET_MALFORMED; }
    if (!e->head || e->receivedEnd || e->receivedTrailers || (e->noBody && !bytes.empty())) { return ERET_MALFORMED; }
    const auto& limits = e->c->options.limits;
    if (bytes.size() > limits.maximumBodyBytes-e->received || (e->expected && !e->noBody && bytes.size() > *e->expected-e->received)) { return ERET_MALFORMED; }
    if (end && e->expected && !e->noBody && e->received+bytes.size() != *e->expected) { return ERET_MALFORMED; }
    if (bytes.size() > limits.maximumStreamBufferedBytes-e->buffered || bytes.size() > limits.maximumConnectionBufferedBytes-e->c->buffered) { return ERET_LIMIT; }
    e->received += bytes.size(); e->buffered += bytes.size(); e->c->buffered += bytes.size();
    if (!bytes.empty()) { e->body.push_back(std::move(bytes)); }
    if (end && !endInput(e)) { return ERET_MALFORMED; } e->changed.set(); return ERET_OK;
}
SLease::~SLease() {
    auto exchange = e;
    if (e->c->closed.load()) { return; }
    auto release = [exchange] {
        if (exchange->state == ECONTEXT_COMPLETE || (exchange->receivedEnd && exchange->sentEnd && !exchange->buffered) || !exchange->id) { return; }
        if (exchange->state != ECONTEXT_COMPLETE && exchange->failure.code == ERET_OK && !exchange->c->closed) {
            exchange->c->wire->reset(exchange,exchange->c->options.version == HTTP_2 ? 8 : 0x10c);
            streamFail(exchange,ERET_CANCELLED,exchange->c->options.version == HTTP_2 ? 8 : 0x10c);
        }
    };
    if (current(e->c->executor)) { release(); } else { e->c->executor->push(std::move(release)); }
}
static ERetCode check(const std::shared_ptr<SExchange>& e, SOperationOptions o) {
    auto p = entry(e->c,o); if (p == ERET_CLOSED && e->receivedEnd && (e->sentEnd || e->sendFailure.code != ERET_OK)) { p = ERET_OK; }
    if (p != ERET_OK) { return p; } return e->failure.code;
}
static void edit(const std::shared_ptr<SExchange>& e) { require(e->c->executor); if (e->sealed || e->failure.code != ERET_OK) { throw std::logic_error("sealed multiplexed message"); } }
static EHttpMethod methodCode(const std::string& name) {
    static const std::string names[]{"","GET","HEAD","POST","PUT","DELETE","CONNECT","OPTIONS","TRACE","PATCH"};
    for (unsigned i = 1; i < std::size(names); ++i) { if (names[i] == name) { return static_cast<EHttpMethod>(i); } } return HTTP_EXTENSION;
}
static asyncpp::task<SReadResult> read(std::shared_ptr<SLease> l, std::size_t n, SOperationOptions o, bool probe = false) {
    auto e = l->e; auto p = check(e,o); if (p != ERET_OK) { co_return SReadResult{error(p),{}}; }
    if (e->reading) { co_return SReadResult{error(ERET_BUSY),{}}; } if (!n && !probe) { co_return SReadResult{error(ERET_INVALID_ARGUMENT),{}}; }
    e->reading = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{e->reading};
    while (e->body.empty() && !e->receivedEnd && e->failure.code == ERET_OK && !e->c->closed) { e->changed.reset(); co_await e->changed; }
    if (e->failure.code != ERET_OK) { co_return SReadResult{e->failure,{}}; }
    if (e->body.empty()) { co_return SReadResult{error(e->receivedEnd ? ERET_EOF : ERET_CLOSED),{}}; }
    if (!n) { co_return SReadResult{error(ERET_LIMIT),{}}; }
    auto& chunk = e->body.front(); const auto count = (std::min)(n,chunk.size()-e->offset);
    SBytes bytes(chunk.begin()+static_cast<std::ptrdiff_t>(e->offset),chunk.begin()+static_cast<std::ptrdiff_t>(e->offset+count));
    e->offset += count; if (e->offset == chunk.size()) { e->offset = 0; e->body.pop_front(); }
    e->buffered -= count; e->c->buffered -= count; e->c->wire->consumed(e,count); e->changed.set(); e->c->changed.set();
    co_await budget(e->c,count); co_return SReadResult{error(ERET_OK),std::move(bytes)};
}
static asyncpp::task<SReadResult> collect(std::shared_ptr<SLease> l, std::uint64_t maximum, bool discard, SOperationOptions o) {
    SBytes output; std::uint64_t count = 0;
    while (true) { auto chunk = co_await read(l,static_cast<std::size_t>((std::min)(UINT64_C(65536),maximum-count)),o,true);
        if (chunk.result.code == ERET_EOF) { co_return SReadResult{error(ERET_OK),std::move(output)}; }
        if (chunk.result.code != ERET_OK) { co_return SReadResult{chunk.result,{}}; }
        count += chunk.bytes.size(); if (!discard) { output.insert(output.end(),chunk.bytes.begin(),chunk.bytes.end()); }
    }
}
class CReader final : public IHttpBodyReader {
private:
    std::shared_ptr<SLease> _l;
    static asyncpp::task<ERetCode> drain(std::shared_ptr<SLease> l, std::uint64_t n, SOperationOptions o) { co_return (co_await mux::collect(std::move(l),n,true,o)).result.code; }
public:
    explicit CReader(std::shared_ptr<SLease> l) : _l(std::move(l)) {}
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return read(_l,n,o); }
    asyncpp::task<ERetCode> discard(std::uint64_t n, SOperationOptions o) override { return drain(_l,n,o); }
    asyncpp::task<SReadResult> collect(std::size_t n, SOperationOptions o) override { return mux::collect(_l,n,false,o); }
    bool isComplete() const noexcept override { return _l->e->receivedEnd && !_l->e->buffered; }
    const IReadOnlyHttpHeaders& trailers() const override { require(_l->e->c->executor); if (!isComplete()) { throw std::logic_error("body pending"); } return _l->e->trailers; }
    SHttpError lastError() const override { require(_l->e->c->executor); return _l->e->failure; }
};
static asyncpp::task<ERetCode> waitSent(std::shared_ptr<SExchange> e, SOperationOptions o) {
    const auto p = check(e,o); if (p != ERET_OK) { co_return p; }
    if (!e->sealed) { co_return ERET_INVALID_STATE; }
    while (e->uploading) { e->sent.reset(); co_await e->sent; }
    co_return e->sendFailure.code != ERET_OK ? e->sendFailure.code : e->failure.code;
}
static asyncpp::task<void> upload(std::shared_ptr<SExchange> e, SBodyMetadata metadata) {
    ERetCode result = ERET_OK; std::uint64_t sent = 0;
    try {
        while (!e->c->closed && e->failure.code == ERET_OK) {
            auto chunk = co_await e->source->readSome(e->c->options.limits.ioChunkBytes);
            if (chunk.result.code == ERET_EOF) {
                if (!chunk.bytes.empty() || (metadata.contentLength && sent != *metadata.contentLength)) { result = ERET_TRUNCATED; break; }
                auto trailers = e->source->trailers();
                if (!trailers.empty()) {
                    std::size_t bytes = 0;
                    for (const auto& field : trailers) {
                        if (!token(field.name) || lower(field.name) != field.name || !fieldValue(field.value) || managed(field.name) || forbidden(field.name)
                            || std::find(metadata.trailerNames.begin(),metadata.trailerNames.end(),field.name) == metadata.trailerNames.end()) { result = ERET_INVALID_ARGUMENT; break; }
                        bytes += field.name.size()+field.value.size()+32;
                    }
                    if (bytes > e->c->options.limits.maximumTrailerBytes || trailers.size() > e->c->options.limits.maximumHeaderFields) { result = ERET_LIMIT; }
                    if (result == ERET_OK) { result = co_await e->c->wire->headers(e,std::move(trailers),true); }
                } else { result = co_await e->c->wire->data(e,{},true); }
                break;
            }
            if (chunk.result.code != ERET_OK) { result = chunk.result.code; break; }
            if (chunk.bytes.empty() || chunk.bytes.size() > e->c->options.limits.ioChunkBytes
                || chunk.bytes.size() > e->c->options.limits.maximumBodyBytes-sent
                || (metadata.contentLength && chunk.bytes.size() > *metadata.contentLength-sent)) { result = ERET_LIMIT; break; }
            sent += chunk.bytes.size(); result = co_await e->c->wire->data(e,std::move(chunk.bytes),false);
            if (result != ERET_OK) { break; } co_await budget(e->c,static_cast<std::size_t>(sent ? 1 : 0));
        }
    } catch (...) { result = ERET_IO; }
    if (result != ERET_OK && e->failure.code == ERET_OK) {
        if (e->c->options.version == HTTP_3 && e->c->role == HTTP_CLIENT && !e->c->closed) {
            // --> A failed request upload must not erase a complete or still-arriving response.
            e->sendFailure = error(result); e->sendFailure.scope = EERROR_STREAM; e->sendFailure.streamId = e->id;
            e->quic->abortWrite(0x10c);
        } else { e->c->wire->reset(e,e->c->options.version == HTTP_2 ? 2 : 0x102); streamFail(e,result,e->c->options.version == HTTP_2 ? 2 : 0x102); }
    }
    if (result == ERET_OK && e->failure.code == ERET_OK && !e->c->closed) { e->sentEnd = true; }
    e->uploading = false; e->sent.set(); e->changed.set(); if (!--e->c->uploads) { e->c->uploadDone.set(); } e->c->changed.set();
}
static asyncpp::task<ERetCode> execute(std::shared_ptr<SLease> l, SOperationOptions o) {
    auto e = l->e; auto p = check(e,o); if (p != ERET_OK) { co_return p; }
    if (!e->c->running || !e->c->ready) { co_return ERET_NOT_RUNNING; }
    if (e->executing) { co_return ERET_BUSY; } if (e->sealed) { co_return ERET_INVALID_STATE; }
    e->executing = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{e->executing};
    if (e->sendOptions.expectContinue || e->sendOptions.framing == EFRAMING_CHUNKED || e->sendOptions.framing == EFRAMING_CLOSE) { co_return ERET_UNSUPPORTED; }
    const bool client = e->c->role == HTTP_CLIENT;
    if (client && (e->method == "CONNECT" || e->target.authority.empty() || e->target.encodedPath.empty()
        || (e->target.encodedPath[0] != '/' && !(e->target.encodedPath == "*" && e->method == "OPTIONS"))
        || e->target.encodedPath.find_first_of("?# \r\n") != std::string::npos || e->target.form == ETARGET_AUTHORITY || e->target.form == ETARGET_ABSOLUTE)) { co_return ERET_INVALID_ARGUMENT; }
    SBodyMetadata metadata; metadata.contentLength = 0;
    if (e->source) { if ((p = co_await e->source->prepare(o)) != ERET_OK) { co_return p; } metadata = e->source->metadata(); }
    if (metadata.contentLength && *metadata.contentLength > e->c->options.limits.maximumBodyBytes) { co_return ERET_LIMIT; }
    if (!fieldValue(metadata.contentType) || metadata.contentType.empty()) { co_return ERET_INVALID_ARGUMENT; }
    if (e->sendOptions.framing == EFRAMING_CONTENT_LENGTH && !metadata.contentLength) { co_return ERET_INVALID_ARGUMENT; }
    const bool suppress = !client && (e->method == "HEAD" || e->status == 204 || e->status == 304);
    std::vector<SHeaderField> fields;
    if (client) {
        e->rawPath = e->target.encodedPath; const auto query = e->queries.wire(); if (!query.empty()) { e->rawPath += '?'+query; }
        auto scheme = e->target.scheme.empty() ? (e->c->wire->security().tls ? "https" : "http") : e->target.scheme;
        if (scheme != "http" && scheme != "https") { co_return ERET_INVALID_ARGUMENT; }
        fields = {{":method",e->method},{":scheme",scheme},{":authority",e->target.authority},{":path",e->rawPath}};
    } else { if (!e->head) { co_return ERET_INVALID_STATE; } fields = {{":status",std::to_string(e->status)}}; }
    auto regular = (client ? e->requestHeaders : e->responseHeaders).entries(0,4096);
    for (auto& field : regular) { field.name = lower(field.name); fields.push_back(std::move(field)); }
    if (metadata.contentLength && (client || e->status != 204)) { fields.push_back({"content-length",std::to_string(*metadata.contentLength)}); }
    if (e->source) { fields.push_back({"content-type",metadata.contentType}); }
    std::size_t fieldBytes = 0;
    for (const auto& field : fields) { if (!fieldValue(field.value) || trim(field.value) != field.value || forbidden(field.name)) { co_return ERET_INVALID_ARGUMENT; } fieldBytes += field.name.size()+field.value.size()+32; }
    if (fieldBytes > e->c->options.limits.maximumHeaderBytes || fields.size() > e->c->options.limits.maximumHeaderFields) { co_return ERET_LIMIT; }
    for (const auto& name : metadata.trailerNames) { if (!token(name) || lower(name) != name || managed(name) || forbidden(name)) { co_return ERET_INVALID_ARGUMENT; } }
    if (client && (p = co_await e->c->wire->open(e)) != ERET_OK) { co_return p; }
    e->sealed = true; e->requestHeaders.freeze(); e->responseHeaders.freeze(); e->queries.freeze();
    const bool noSource = !e->source || suppress;
    if ((p = co_await e->c->wire->headers(e,std::move(fields),noSource)) != ERET_OK) {
        e->c->wire->reset(e,e->c->options.version == HTTP_2 ? 2 : 0x102); streamFail(e,p,e->c->options.version == HTTP_2 ? 2 : 0x102); co_return p;
    }
    if (noSource) { e->sentEnd = true; e->sent.set(); }
    else {
        e->uploading = true; ++e->c->uploads; e->c->uploadDone.reset();
        // --> State owns and tracks this sender; close wakes its I/O and run/waitClosed join it.
        try { asyncpp::launch(upload(e,metadata)); }
        catch (...) {
            e->uploading = false; e->sent.set(); if (!--e->c->uploads) { e->c->uploadDone.set(); }
            e->c->wire->reset(e,e->c->options.version == HTTP_2 ? 2 : 0x102); streamFail(e,ERET_IO,e->c->options.version == HTTP_2 ? 2 : 0x102); throw;
        }
    }
    if (!client) { co_return co_await waitSent(e,o); }
    while (!e->head && e->failure.code == ERET_OK && e->sendFailure.code == ERET_OK && !e->c->closed) { e->changed.reset(); co_await e->changed; }
    co_return e->failure.code != ERET_OK ? e->failure.code : e->head ? ERET_OK : e->sendFailure.code != ERET_OK ? e->sendFailure.code : ERET_CLOSED;
}
class CRequest : public virtual IHttpRequest {
protected:
    std::shared_ptr<SLease> _l;
    void check() const { require(_l->e->c->executor); }
public:
    explicit CRequest(std::shared_ptr<SLease> l) : _l(std::move(l)) {}
    EHttpVersion version() const noexcept override { return _l->e->c->options.version; }
    EHttpMethod method() const noexcept override { return methodCode(_l->e->method); }
    std::string methodName() const override { check(); return _l->e->method; }
    SRequestTarget target() const override { check(); return _l->e->target; }
    std::string path() const override { check(); return _l->e->target.encodedPath; }
    std::string rawTarget() const override { check(); return _l->e->rawPath; }
    const IReadOnlyHttpHeaders& headers() const override { check(); return _l->e->requestHeaders; }
    const IReadOnlyHttpQueries& queries() const override { check(); return _l->e->queries; }
    SHttpError lastError() const override { check(); return _l->e->failure.code == ERET_OK ? _l->e->sendFailure : _l->e->failure; }
};
class CServerRequest final : public CRequest, public IHttpServerRequest {
private:
    static asyncpp::task<ERetCode> parse(std::shared_ptr<SLease> l, SOperationOptions o) { auto p = mux::check(l->e,o); co_return p != ERET_OK ? p : l->e->head ? ERET_OK : ERET_INVALID_STATE; }
public:
    using CRequest::CRequest;
    asyncpp::task<ERetCode> parseOnce(SOperationOptions o) override { return parse(_l,o); }
    std::shared_ptr<IHttpBodyReader> getBody() const override { check(); return std::make_shared<CReader>(_l); }
};
class CClientRequest final : public CRequest, public IHttpClientRequest {
public:
    using CRequest::CRequest;
    using CRequest::method; using CRequest::path; using CRequest::target; using CRequest::headers; using CRequest::queries;
    IHttpClientRequest& method(EHttpMethod code) override {
        static const char* names[]{"","GET","HEAD","POST","PUT","DELETE","CONNECT","OPTIONS","TRACE","PATCH"};
        if (code < HTTP_GET || code > HTTP_PATCH) { throw std::invalid_argument("invalid method"); } return method(std::string(names[code]));
    }
    IHttpClientRequest& method(std::string value) override { edit(_l->e); if (!token(value)) { throw std::invalid_argument("invalid method"); } _l->e->method = std::move(value); return *this; }
    IHttpClientRequest& path(std::string value) override { edit(_l->e); _l->e->target.encodedPath = std::move(value); return *this; }
    IHttpClientRequest& authority(std::string value) override { edit(_l->e); if (!fieldValue(value) || value.empty() || value.find_first_of(" /?#\t") != value.npos) { throw std::invalid_argument("invalid authority"); } _l->e->target.authority = std::move(value); return *this; }
    IHttpClientRequest& target(SRequestTarget value) override { edit(_l->e); _l->e->target = std::move(value); return *this; }
    IHttpHeaders& headers() override { edit(_l->e); return _l->e->requestHeaders; }
    IHttpQueries& queries() override { edit(_l->e); return _l->e->queries; }
    IHttpClientRequest& body(std::shared_ptr<IHttpBodySource> value) override { edit(_l->e); _l->e->source = std::move(value); return *this; }
    IHttpClientRequest& options(SSendOptions value) override { edit(_l->e); _l->e->sendOptions = value; return *this; }
    IHttpClientRequest& observer(std::shared_ptr<IHttpInformationalObserver> value) override { edit(_l->e); _l->e->observer = std::move(value); return *this; }
    asyncpp::task<ERetCode> exec(SOperationOptions o) override { return execute(_l,o); }
    bool isSendComplete() const noexcept override { return _l->e->sentEnd || _l->e->sendFailure.code != ERET_OK; }
    asyncpp::task<ERetCode> waitSent(SOperationOptions o) override { return mux::waitSent(_l->e,o); }
};
class CResponse : public virtual IHttpResponse {
protected:
    std::shared_ptr<SLease> _l;
public:
    explicit CResponse(std::shared_ptr<SLease> l) : _l(std::move(l)) {}
    EHttpVersion version() const noexcept override { return _l->e->c->options.version; }
    std::uint16_t status() const override { require(_l->e->c->executor); return _l->e->status; }
    std::string reason() const override { require(_l->e->c->executor); return {}; }
    const IReadOnlyHttpHeaders& headers() const override { require(_l->e->c->executor); return _l->e->responseHeaders; }
    SHttpError lastError() const override { require(_l->e->c->executor); return _l->e->failure; }
};
class CClientResponse final : public CResponse, public IHttpClientResponse {
public:
    using CResponse::CResponse;
    std::shared_ptr<IHttpBodyReader> getBody() const override { require(_l->e->c->executor); return std::make_shared<CReader>(_l); }
};
class CServerResponse final : public CResponse, public IHttpServerResponse {
private:
    static asyncpp::task<ERetCode> provisional(std::shared_ptr<SLease> l, std::uint16_t code, std::vector<SHeaderField> fields, SOperationOptions o) {
        auto e = l->e; auto p = mux::check(e,o); if (p != ERET_OK) { co_return p; }
        if (e->sealed || code < 100 || code >= 200 || code == 101 || ++e->informational > e->c->options.limits.maximumInformationalResponses) { co_return ERET_INVALID_ARGUMENT; }
        for (auto& f : fields) { f.name = lower(f.name); if (!token(f.name) || !fieldValue(f.value) || managed(f.name) || forbidden(f.name)) { co_return ERET_INVALID_ARGUMENT; } }
        fields.insert(fields.begin(),{":status",std::to_string(code)}); co_return co_await e->c->wire->headers(e,std::move(fields),false);
    }
public:
    using CResponse::CResponse; using CResponse::status; using CResponse::headers;
    IHttpServerResponse& status(std::uint16_t code) override { return status(code,{}); }
    IHttpServerResponse& status(std::uint16_t code, std::string reason) override { edit(_l->e); if (code < 200 || code > 599 || !fieldValue(reason)) { throw std::invalid_argument("invalid response"); } _l->e->status = code; return *this; }
    IHttpHeaders& headers() override { edit(_l->e); return _l->e->responseHeaders; }
    IHttpServerResponse& body(std::shared_ptr<IHttpBodySource> value) override { edit(_l->e); _l->e->source = std::move(value); return *this; }
    IHttpServerResponse& options(SSendOptions value) override { edit(_l->e); _l->e->sendOptions = value; return *this; }
    asyncpp::task<ERetCode> sendInformational(std::uint16_t code, std::vector<SHeaderField> fields, SOperationOptions o) override { return provisional(_l,code,std::move(fields),o); }
    IHttpServerResponse& acceptUpgrade(std::string) override { unsupported("HTTP/2 and HTTP/3 do not support 101 Upgrade"); return *this; }
    asyncpp::task<ERetCode> exec(SOperationOptions o) override { return execute(_l,o); }
};
class CContext : public virtual IHttpContext {
protected:
    std::shared_ptr<SLease> _l;
    static asyncpp::task<ERetCode> finishImpl(std::shared_ptr<SLease> l, SFinishOptions f, SOperationOptions o) {
        auto e = l->e; auto p = mux::check(e,o); if (p != ERET_OK) { co_return p; }
        if (e->sendFailure.code != ERET_OK) { co_return e->sendFailure.code; }
        if ((!e->receivedEnd || e->buffered) && f.discardUnreadBody) { p = (co_await mux::collect(l,f.maximumDiscardBytes,true,o)).result.code; if (p != ERET_OK) { co_return p; } }
        if (!e->receivedEnd || e->buffered || !e->sentEnd || e->uploading) { co_return ERET_BODY_PENDING; }
        e->state = ECONTEXT_COMPLETE; e->c->streams.erase(*e->id); e->c->changed.set(); co_return ERET_OK;
    }
    static asyncpp::task<ERetCode> abortImpl(std::shared_ptr<SLease> l, SOperationOptions o) {
        auto e = l->e; auto p = entry(e->c,o); if (p != ERET_OK) { co_return p; }
        if (e->state == ECONTEXT_COMPLETE) { co_return ERET_OK; }
        const auto code = e->c->options.version == HTTP_2 ? 8u : 0x10cu;
        if (e->id) { e->c->wire->reset(e,code); } streamFail(e,ERET_CANCELLED,code);
        while (e->uploading) { e->sent.reset(); co_await e->sent; } co_return ERET_OK;
    }
public:
    explicit CContext(std::shared_ptr<SLease> l) : _l(std::move(l)) {}
    ERetCode setStream(std::shared_ptr<IHttpStream>) override { return ERET_UNSUPPORTED; }
    std::shared_ptr<IHttpConnection> getConnection() const override { require(_l->e->c->executor); return _l->e->connection; }
    STransportSecurity transportSecurity() const override { require(_l->e->c->executor); return _l->e->c->wire->security(); }
    std::optional<std::uint64_t> streamId() const noexcept override { return _l->e->id; }
    EContextState state() const noexcept override { return _l->e->state; }
    asyncpp::task<ERetCode> finish(SFinishOptions f, SOperationOptions o) override { return finishImpl(_l,f,o); }
    asyncpp::task<ERetCode> abort(SOperationOptions o) override { return abortImpl(_l,o); }
    STunnelResult takeTunnel() override { require(_l->e->c->executor); return {error(ERET_UNSUPPORTED),{}}; }
    SHttpError lastError() const override { require(_l->e->c->executor); return _l->e->failure.code == ERET_OK ? _l->e->sendFailure : _l->e->failure; }
};
class CServerContext final : public CContext, public IHttpServerContext {
public:
    using CContext::CContext;
    std::shared_ptr<IHttpServerRequest> getRequest() const override { require(_l->e->c->executor); return std::make_shared<CServerRequest>(_l); }
    std::shared_ptr<IHttpServerResponse> getResponse() const override { require(_l->e->c->executor); return std::make_shared<CServerResponse>(_l); }
};
class CClientContext final : public CContext, public IHttpClientContext {
public:
    using CContext::CContext;
    std::shared_ptr<IHttpClientRequest> getRequest() const override { require(_l->e->c->executor); return std::make_shared<CClientRequest>(_l); }
    std::shared_ptr<IHttpClientResponse> getResponse() const override { require(_l->e->c->executor); return std::make_shared<CClientResponse>(_l); }
};
std::shared_ptr<IHttpServerContext> serverContext(const std::shared_ptr<SExchange>& e) {
    e->connection = e->c->facade.lock(); auto l = std::make_shared<SLease>(); l->e = e; return std::make_shared<CServerContext>(std::move(l));
}
class CConnection : public virtual IHttpMultiplexedConnection {
protected:
    std::shared_ptr<SConnection> _s;
    static asyncpp::task<ERetCode> runImpl(std::shared_ptr<SConnection> s, SOperationOptions o) {
        auto p = entry(s,o); if (p != ERET_OK) { co_return p; } if (s->running) { co_return ERET_BUSY; }
        s->running = true; s->stopped.reset();
        try { p = co_await s->wire->run(s); } catch (...) { p = ERET_IO; fail(s,p,s->options.version == HTTP_2 ? 2 : 0x102); }
        if (!s->closed) { fail(s,p == ERET_OK ? ERET_CLOSED : p,0); }
        co_await s->uploadDone; s->running = false; s->stopped.set(); s->changed.set();
        co_return s->failure.code == ERET_CLOSED ? ERET_OK : s->failure.code;
    }
    static asyncpp::task<ERetCode> readyImpl(std::shared_ptr<SConnection> s, SOperationOptions o) {
        auto p = entry(s,o); if (p != ERET_OK) { co_return p; } if (!s->running) { co_return ERET_NOT_RUNNING; }
        while (!s->ready && !s->closed) { s->changed.reset(); co_await s->changed; }
        co_return s->closed ? entry(s,o) : ERET_OK;
    }
    static asyncpp::task<SAcceptResult> acceptImpl(std::shared_ptr<SConnection> s, SOperationOptions o) {
        auto p = entry(s,o); if (p != ERET_OK) { co_return SAcceptResult{error(p),{}}; }
        if (s->role != HTTP_SERVER) { co_return SAcceptResult{error(ERET_INVALID_STATE),{}}; }
        if (!s->running) { co_return SAcceptResult{error(ERET_NOT_RUNNING),{}}; }
        if (s->accepting) { co_return SAcceptResult{error(ERET_BUSY),{}}; }
        s->accepting = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{s->accepting};
        while (true) {
            while (s->ready && !s->incoming.empty()) {
                auto e = s->incoming.front(); s->incoming.pop_front(); if (e->failure.code != ERET_OK) { continue; }
                co_return SAcceptResult{error(ERET_OK),serverContext(e)};
            }
            if (s->closed || s->draining) { co_return SAcceptResult{error(s->closed ? entry(s,o) : ERET_GOAWAY),{}}; }
            s->changed.reset(); co_await s->changed;
        }
    }
    static asyncpp::task<ERetCode> joined(std::shared_ptr<SConnection> s, SOperationOptions o) {
        if (!current(s->executor)) { co_return ERET_WRONG_EXECUTOR; } const auto p = policy(o); if (p != ERET_OK) { co_return p; }
        if (!s->closed) { co_return ERET_INVALID_STATE; } co_await s->stopped; co_await s->uploadDone; co_return co_await s->wire->join(o);
    }
    static asyncpp::task<ERetCode> graceful(std::shared_ptr<SConnection> s, SOperationOptions o) {
        auto p = entry(s,o); if (p != ERET_OK) { co_return p; } if (!s->running || !s->ready) { co_return ERET_NOT_RUNNING; }
        s->draining = true; s->changed.set(); if ((p = co_await s->wire->goaway(s)) != ERET_OK) { co_return p; }
        while (!s->closed) {
            bool active = false;
            for (const auto& [id,weak] : s->streams) { (void)id; if (auto e = weak.lock(); e && e->failure.code == ERET_OK && (!e->receivedEnd || !e->sentEnd || e->buffered)) { active = true; break; } }
            if (!active) { fail(s,ERET_CLOSED,0); co_return ERET_OK; } s->changed.reset(); co_await s->changed;
        }
        co_return s->failure.code;
    }
    std::vector<SProtocolSetting> page(std::uint64_t offset, std::size_t n) const {
        require(_s->executor); if (n > 4096) { throw std::length_error("settings page"); }
        auto settings = _s->wire->settings(); if (offset >= settings.size()) { return {}; }
        auto first = settings.begin()+static_cast<std::ptrdiff_t>(offset); return {first,first+static_cast<std::ptrdiff_t>((std::min)(n,settings.size()-offset))};
    }
public:
    explicit CConnection(std::shared_ptr<SConnection> s) : _s(std::move(s)) {}
    ~CConnection() override { close(); }
    std::shared_ptr<SConnection> stateOwner() const { return _s; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->executor; }
    EHttpVersion version() const noexcept override { return _s->options.version; }
    EHttpRole role() const noexcept override { return _s->role; }
    EConnectionState state() const noexcept override { return _s->closed ? (_s->failure.code == ERET_CLOSED ? ECONNECTION_CLOSED : ECONNECTION_FAILED) : _s->draining ? ECONNECTION_DRAINING : _s->running ? ECONNECTION_RUNNING : ECONNECTION_IDLE; }
    bool canStartExchange() const noexcept override { return _s->running && _s->ready && !_s->closed && !_s->draining && !_s->goaway; }
    STransportSecurity security() const override { require(_s->executor); return _s->wire->security(); }
    SHttpConnectionOptions options() const override { require(_s->executor); return _s->options; }
    SHttpError lastError() const override { require(_s->executor); return _s->failure; }
    void close() noexcept override {
        if (_s->closed.load()) { return; }
        auto s = _s; if (current(s->executor)) { fail(s,ERET_CLOSED,0); } else { s->executor->push([s] { fail(s,ERET_CLOSED,0); }); }
    }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return joined(_s,o); }
    asyncpp::task<ERetCode> run(SOperationOptions o) override { return runImpl(_s,o); }
    asyncpp::task<ERetCode> ready(SOperationOptions o) override { return readyImpl(_s,o); }
    asyncpp::task<SAcceptResult> accept(SOperationOptions o) override { return acceptImpl(_s,o); }
    std::optional<SGoawayInfo> peerGoaway() const override { require(_s->executor); return _s->goaway; }
    asyncpp::task<ERetCode> gracefulShutdown(SOperationOptions o) override { return graceful(_s,o); }
};
class CHttp2Connection final : public CConnection, public IHttp2Connection {
private:
    static asyncpp::task<ERetCode> update(std::shared_ptr<SConnection> state, std::vector<SProtocolSetting> values, SOperationOptions o) {
        co_return co_await state->wire->update(std::move(values),o);
    }
    static asyncpp::task<ERetCode> pong(std::shared_ptr<SConnection> state, std::array<std::uint8_t,8> payload, SOperationOptions o) {
        co_return co_await state->wire->ping(payload,o);
    }
public:
    using CConnection::CConnection;
    std::vector<SProtocolSetting> peerSettings(std::uint64_t offset, std::size_t n) const override { return page(offset,n); }
    asyncpp::task<ERetCode> updateSettings(std::vector<SProtocolSetting> values, SOperationOptions o) override { return update(_s,std::move(values),o); }
    asyncpp::task<ERetCode> ping(std::array<std::uint8_t,8> bytes, SOperationOptions o) override { return pong(_s,bytes,o); }
    SHttp2FlowState flowState(std::optional<std::uint32_t> id) const override { require(_s->executor); return _s->wire->flow(id); }
};
class CHttp3Connection final : public CConnection, public IHttp3Connection {
public:
    using CConnection::CConnection;
    std::vector<SProtocolSetting> peerSettings(std::uint64_t offset, std::size_t n) const override { return page(offset,n); }
};
std::shared_ptr<IHttpConnection> facade(const std::shared_ptr<SConnection>& state) {
    std::shared_ptr<IHttpConnection> result;
    if (state->options.version == HTTP_2) { result = std::make_shared<CHttp2Connection>(state); }
    else { result = std::make_shared<CHttp3Connection>(state); } state->facade = result; return result;
}
std::shared_ptr<IHttpClientContext> clientContext(std::shared_ptr<IHttpConnection> connection) {
    auto concrete = std::dynamic_pointer_cast<CConnection>(connection);
    if (!concrete || concrete->role() != HTTP_CLIENT) { throw std::invalid_argument("foreign multiplexed connection"); }
    auto c = concrete->stateOwner(); require(c->executor); auto e = std::make_shared<SExchange>(); e->c = c; e->connection = std::move(connection);
    e->requestHeaders.executor(c->executor); e->responseHeaders.executor(c->executor); e->queries.executor(c->executor); e->trailers.executor(c->executor);
    auto l = std::make_shared<SLease>(); l->e = std::move(e); return std::make_shared<CClientContext>(std::move(l));
}
}
