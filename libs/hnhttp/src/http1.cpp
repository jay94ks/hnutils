#include <hnhttp/context.hpp>
#include "internal.hpp"
#include "multiplexed_internal.hpp"
#include "transport_lease.hpp"
#include <asyncpp/event.h>
#include <asyncpp/launch.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace hnhttp {
namespace detail {

struct Failure { SHttpError value; };
[[noreturn]] static void fail(ERetCode code, const char* message) { throw Failure{error(code, message)}; }
struct Exchange;
struct ConnectionState {
    std::shared_ptr<IHttpStream> stream;
    SHttpConnectionOptions options;
    EHttpRole role;
    std::string buffered;
    std::atomic<bool> closed{false};
    bool leased = false, persistent = true;
    SHttpError failure;
    std::weak_ptr<Exchange> exchange;
    std::size_t turnBytes = 0, turnOperations = 0;
    void close() noexcept { closed = true; stream->close(); }
    asyncpp::task<void> yield(std::size_t bytes = 0) {
        turnBytes += bytes; ++turnOperations;
        if (turnBytes >= options.scheduling.maximumBytesPerTurn || turnOperations >= options.scheduling.maximumOperationsPerTurn) {
            turnBytes = turnOperations = 0; co_await asyncpp::defer(*stream->executor());
        }
    }
    asyncpp::task<bool> fill() {
        if (closed) { fail(ERET_CLOSED, "connection closed"); }
        auto input = co_await stream->readSome(options.limits.ioChunkBytes);
        if (input.result.code == ERET_EOF) { co_return false; }
        if (input.result.code != ERET_OK) { throw Failure{input.result}; }
        if (input.bytes.empty() || input.bytes.size() > options.limits.ioChunkBytes) { fail(ERET_IO, "invalid transport read result"); }
        if (input.bytes.size() > options.limits.maximumReadAheadBytes-buffered.size()) { fail(ERET_LIMIT, "read ahead limit"); }
        buffered.append(reinterpret_cast<const char*>(input.bytes.data()), input.bytes.size());
        co_await yield(input.bytes.size()); co_return true;
    }
    asyncpp::task<std::string> line(std::size_t maximum, bool cleanEof = false) {
        while (true) {
            auto end = buffered.find("\r\n");
            const auto lf = buffered.find('\n');
            if (lf != buffered.npos && (lf == 0 || buffered[lf-1] != '\r')) { fail(ERET_MALFORMED, "bare LF"); }
            if (end != buffered.npos) {
                if (end > maximum) { fail(ERET_LIMIT, "line limit"); }
                auto result = buffered.substr(0, end); buffered.erase(0, end+2);
                if (result.find('\r') != result.npos) { fail(ERET_MALFORMED, "bare CR"); }
                co_await yield(result.size()+2); co_return result;
            }
            if (buffered.size() > maximum && buffered.size()-maximum > 1) { fail(ERET_LIMIT, "line limit"); }
            if (!co_await fill()) { fail(cleanEof && buffered.empty() ? ERET_EOF : ERET_TRUNCATED, "incomplete HTTP line"); }
        }
    }
    asyncpp::task<SBytes> bytes(std::size_t count, bool eofAllowed = false) {
        if (buffered.empty() && !co_await fill()) {
            if (eofAllowed) { co_return SBytes{}; } fail(ERET_TRUNCATED, "incomplete HTTP entity");
        }
        count = (std::min)(count, buffered.size());
        SBytes output(count); std::memcpy(output.data(), buffered.data(), count); buffered.erase(0, count);
        co_await yield(count); co_return output;
    }
    asyncpp::task<void> send(SBytes bytes) {
        std::size_t position = 0;
        while (position < bytes.size()) {
            if (closed) { fail(ERET_CLOSED, "connection closed"); }
            const auto count = (std::min)({options.limits.ioChunkBytes, bytes.size()-position, options.scheduling.maximumBytesPerTurn});
            SBytes part(bytes.begin()+static_cast<std::ptrdiff_t>(position), bytes.begin()+static_cast<std::ptrdiff_t>(position+count));
            auto result = co_await stream->writeSome(std::move(part));
            if (result.result.code != ERET_OK) { throw Failure{result.result}; }
            if (!result.acceptedBytes || result.acceptedBytes > count) { fail(ERET_IO, "invalid write progress"); }
            position += result.acceptedBytes; co_await yield(result.acceptedBytes);
        }
    }
    asyncpp::task<void> sendText(std::string text) { co_await send(SBytes(text.begin(), text.end())); }
};


struct Exchange {
    EHttpRole role;
    SHttpConnectionOptions options;
    std::shared_ptr<IHttp1Connection> connection;
    std::shared_ptr<ConnectionState> c;
    EContextState state = ECONTEXT_DRAFT;
    Headers requestHeaders, responseHeaders, trailers;
    Queries queries;
    std::string method = "GET", rawTarget;
    SRequestTarget target;
    EHttpVersion wireVersion = HTTP_11;
    std::uint16_t status = 200;
    std::string reason = "OK";
    SSendOptions sendOptions;
    std::shared_ptr<IHttpBodySource> source;
    std::shared_ptr<IHttpInformationalObserver> observer;
    bool headReady = false, parsing = false, executing = false, reading = false;
    bool incomingDone = false, outgoingDone = false, chunked = false, closeDelimited = false;
    bool chunkCrlf = false, sealed = false, sendStarted = false, uploadRunning = false;
    std::uint64_t remaining = 0, consumed = 0, chunks = 0;
    SHttpError failure;
    asyncpp::multi_consumer_event sent{false};
    void check() const { if (c) { require(c->stream->executor()); } }
    void edit() const {
        check();
        if (sealed || executing || state == ECONTEXT_FAILED || state == ECONTEXT_COMPLETE) { throw std::logic_error("sealed HTTP message"); }
    }
    void failed(SHttpError value) {
        failure = std::move(value); failure.scope = EERROR_CONNECTION; state = ECONTEXT_FAILED;
        if (c) { c->failure = failure; c->close(); }
    }
};
struct Lease {
    std::shared_ptr<Exchange> e;
    ~Lease() { if (e->c && e->state != ECONTEXT_COMPLETE) { e->c->close(); } }
};
static ERetCode entry(const std::shared_ptr<Exchange>& e, const SOperationOptions& options) {
    if (!e->c) { return ERET_INVALID_STATE; }
    if (!current(e->c->stream->executor())) { return ERET_WRONG_EXECUTOR; }
    auto p = policy(options); if (p != ERET_OK) { return p; }
    if (e->c->closed) { return ERET_CLOSED; }
    return ERET_OK;
}

class Connection final : public IHttp1Connection {
private:
    std::shared_ptr<ConnectionState> _s;
    static asyncpp::task<ERetCode> join(std::shared_ptr<ConnectionState> s, SOperationOptions o) {
        if (!current(s->stream->executor())) { co_return ERET_WRONG_EXECUTOR; }
        auto p = policy(o); if (p != ERET_OK) { co_return p; }
        if (!s->closed) { co_return ERET_INVALID_STATE; }
        auto result = co_await s->stream->waitClosed(o);
        if (auto e = s->exchange.lock(); e && e->sendStarted && !e->outgoingDone) { co_await e->sent; }
        co_return result;
    }
public:
    explicit Connection(std::shared_ptr<ConnectionState> s) : _s(std::move(s)) {}
    ~Connection() override { _s->close(); }
    std::shared_ptr<ConnectionState> implementation() const { return _s; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->stream->executor(); }
    EHttpVersion version() const noexcept override { return _s->options.version; }
    EHttpRole role() const noexcept override { return _s->role; }
    EConnectionState state() const noexcept override {
        return _s->failure.code != ERET_OK ? ECONNECTION_FAILED : _s->closed ? ECONNECTION_CLOSED
            : _s->leased ? ECONNECTION_RUNNING : ECONNECTION_IDLE;
    }
    bool canStartExchange() const noexcept override { return isReusable(); }
    bool isReusable() const noexcept override { return !_s->closed && !_s->leased && _s->persistent; }
    STransportSecurity security() const override { require(executor()); return _s->stream->security(); }
    SHttpConnectionOptions options() const override { require(executor()); return _s->options; }
    SHttpError lastError() const override { require(executor()); return _s->failure; }
    void close() noexcept override { _s->close(); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return join(_s, o); }
};

static void attach(const std::shared_ptr<Exchange>& e, std::shared_ptr<IHttp1Connection> connection) {
    if (!connection) { throw std::invalid_argument("null HTTP connection"); }
    require(connection->executor());
    auto concrete = std::dynamic_pointer_cast<Connection>(connection);
    if (!concrete || concrete->role() != e->role || !concrete->isReusable()) { throw std::logic_error("connection unavailable"); }
    e->connection = std::move(connection); e->c = concrete->implementation(); e->c->leased = true; e->c->exchange = e;
    e->options = e->c->options; e->wireVersion = e->options.version; e->state = ECONTEXT_BOUND;
    auto executor = e->c->stream->executor();
    e->requestHeaders.executor(executor); e->responseHeaders.executor(executor); e->trailers.executor(executor); e->queries.executor(executor);
}
static EHttpVersion parseVersion(std::string_view version) {
    if (version == "HTTP/1.1") { return HTTP_11; } if (version == "HTTP/1.0") { return HTTP_10; }
    fail(ERET_UNSUPPORTED, "unsupported HTTP version");
}
static std::string versionText(EHttpVersion version) { return version == HTTP_10 ? "HTTP/1.0" : "HTTP/1.1"; }
static bool hasToken(const std::optional<std::string>& value, const char* tokenValue) {
    if (!value) { return false; } std::string_view rest(*value);
    while (!rest.empty()) {
        auto end = rest.find(','); if (lower(trim(rest.substr(0, end))) == tokenValue) { return true; }
        if (end == rest.npos) { break; } rest.remove_prefix(end+1);
    } return false;
}
static asyncpp::task<void> parseFields(const std::shared_ptr<Exchange>& e, Headers& fields, bool trailer = false) {
    std::size_t bytes = 0, count = 0;
    const auto& limits = e->options.limits;
    while (true) {
        auto line = co_await e->c->line(trailer ? limits.maximumTrailerBytes : limits.maximumHeaderBytes);
        bytes += line.size()+2;
        if (bytes > (trailer ? limits.maximumTrailerBytes : limits.maximumHeaderBytes)) { fail(ERET_LIMIT, "header bytes limit"); }
        if (line.empty()) { break; }
        if (++count > limits.maximumHeaderFields) { fail(ERET_LIMIT, "header count limit"); }
        auto colon = line.find(':');
        if (colon == line.npos || !token(std::string_view(line).substr(0, colon))) { fail(ERET_MALFORMED, "invalid header name or obs-fold"); }
        auto name = lower(line.substr(0, colon)), value = trim(std::string_view(line).substr(colon+1));
        if (!fieldValue(value)) { fail(ERET_MALFORMED, "invalid header value"); }
        if (trailer && (managed(name) || name == "authorization" || name == "proxy-authorization")) { fail(ERET_MALFORMED, "forbidden trailer"); }
        fields.appendParsed({std::move(name), std::move(value), EINDEX_DEFAULT});
    }
    fields.freeze();
}
static void framing(const std::shared_ptr<Exchange>& e, const Headers& fields, bool request) {
    auto lengths = fields.values("content-length", 0, 4096);
    auto transfers = fields.values("transfer-encoding", 0, 4096);
    if (lengths.size() > 1 || transfers.size() > 1 || (!lengths.empty() && !transfers.empty())) { fail(ERET_MALFORMED, "ambiguous framing"); }
    std::optional<std::uint64_t> length;
    if (!lengths.empty()) { length = number(lengths.front()); }
    if (!transfers.empty() && (e->wireVersion != HTTP_11 || lower(transfers.front()) != "chunked")) { fail(ERET_UNSUPPORTED, "unsupported transfer coding"); }
    if (length && *length > e->options.limits.maximumBodyBytes) { fail(ERET_LIMIT, "entity length limit"); }
    auto connections = fields.values("connection", 0, 4096);
    bool close = false, keep = false;
    for (const auto& v : connections) { close |= hasToken(v, "close"); keep |= hasToken(v, "keep-alive"); }
    e->c->persistent = e->c->persistent && !close &&
        (e->wireVersion == HTTP_11 || (keep && e->options.http1.allowHttp10KeepAlive));
    if (!request && (e->method == "HEAD" || e->status == 204 || e->status == 304)) {
        if (e->status == 204 && (length || !transfers.empty())) { fail(ERET_MALFORMED, "framing forbidden for 204"); }
        e->incomingDone = true; return;
    }
    e->chunked = !transfers.empty(); e->remaining = length.value_or(0);
    e->closeDelimited = !request && !length && !e->chunked;
    if (e->closeDelimited) { e->c->persistent = false; }
    e->incomingDone = !e->chunked && !e->closeDelimited && e->remaining == 0;
}
static asyncpp::task<void> parseHead(const std::shared_ptr<Exchange>& e) {
    if (e->role == HTTP_SERVER) {
        auto line = co_await e->c->line(e->options.limits.maximumStartLineBytes, true);
        auto a = line.find(' '), b = line.find(' ', a == line.npos ? 0 : a+1);
        if (a == line.npos || b == line.npos || line.find(' ', b+1) != line.npos || !token(line.substr(0, a))) { fail(ERET_MALFORMED, "invalid request line"); }
        e->method = line.substr(0, a); e->rawTarget = line.substr(a+1, b-a-1); e->wireVersion = parseVersion(line.substr(b+1));
        if (e->method == "CONNECT" || (e->rawTarget != "*" && !e->rawTarget.starts_with('/'))) { fail(ERET_UNSUPPORTED, "request target form pending"); }
        if (e->rawTarget.empty() || e->rawTarget.find_first_of("#\t") != e->rawTarget.npos || !fieldValue(e->rawTarget)) { fail(ERET_MALFORMED, "invalid request target"); }
        if (e->rawTarget == "*" && e->method != "OPTIONS") { fail(ERET_MALFORMED, "asterisk requires OPTIONS"); }
        const auto query = e->rawTarget.find('?'); e->target.encodedPath = e->rawTarget.substr(0, query);
        (void)decodeComponent(e->target.encodedPath);
        e->target.form = e->rawTarget == "*" ? ETARGET_ASTERISK : ETARGET_ORIGIN;
        if (query != e->rawTarget.npos) { e->queries.parse(std::string_view(e->rawTarget).substr(query+1), e->options.limits); }
        co_await parseFields(e, e->requestHeaders);
        auto host = e->requestHeaders.values("host", 0, 4096);
        if (host.size() > 1 || (e->wireVersion == HTTP_11 && (host.empty() || host[0].empty()))) { fail(ERET_MALFORMED, "missing or duplicate Host"); }
        if (!host.empty()) {
            if (host[0].find_first_of(" \t/@#?\\") != host[0].npos) { fail(ERET_MALFORMED, "invalid Host"); }
            e->target.authority = host[0];
        }
        if (e->requestHeaders.get("expect") || e->requestHeaders.get("upgrade")) { fail(ERET_UNSUPPORTED, "Expect/Upgrade handling pending"); }
        framing(e, e->requestHeaders, true);
        if (e->options.http1.requireExplicitRequestBodyLength &&
            (e->method == "POST" || e->method == "PUT" || e->method == "PATCH" ||
             e->requestHeaders.get("content-type")) &&
            !e->requestHeaders.get("content-length") && !e->chunked) {
            co_await e->c->sendText(versionText(e->wireVersion) +
                " 411 Length Required\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            fail(ERET_LENGTH_REQUIRED, "explicit request body framing required");
        }
        e->queries.freeze();
    } else {
        for (std::size_t informational = 0; ; ++informational) {
            auto line = co_await e->c->line(e->options.limits.maximumStartLineBytes);
            if (line.size() < 13 || line[8] != ' ' || line[12] != ' ') { fail(ERET_MALFORMED, "invalid status line"); }
            e->wireVersion = parseVersion(line.substr(0, 8));
            e->status = static_cast<std::uint16_t>(number(std::string_view(line).substr(9, 3)));
            if (e->status < 100 || e->status > 599 || !fieldValue(std::string_view(line).substr(13))) { fail(ERET_MALFORMED, "invalid response status"); }
            e->reason = line.substr(13); e->responseHeaders = Headers{}; e->responseHeaders.executor(e->c->stream->executor());
            co_await parseFields(e, e->responseHeaders);
            if (e->status >= 200) { break; }
            if (e->status == 101) { fail(ERET_UNSUPPORTED, "Upgrade pending"); }
            if (e->responseHeaders.get("content-length") || e->responseHeaders.get("transfer-encoding")) { fail(ERET_MALFORMED, "informational body framing"); }
            if (informational >= e->options.limits.maximumInformationalResponses) { fail(ERET_LIMIT, "informational limit"); }
            if (e->observer) { e->observer->onInformational({e->wireVersion, e->status, e->reason, e->responseHeaders.entries(0, 4096)}); }
        }
        framing(e, e->responseHeaders, false);
    }
    e->headReady = true; e->state = ECONTEXT_ACTIVE;
}
static ERetCode caught(const std::shared_ptr<Exchange>& e) {
    try { throw; }
    catch (const Failure& f) { e->failed(f.value); }
    catch (const std::length_error& x) { e->failed(error(ERET_LIMIT, x.what())); }
    catch (const std::invalid_argument& x) { e->failed(error(ERET_MALFORMED, x.what())); }
    catch (const std::system_error& x) { e->failed(error(ERET_IO, x.what(), x.code())); }
    catch (...) { e->failed(error(ERET_IO, "HTTP callback/allocation failure")); throw; }
    return e->failure.code;
}
static asyncpp::task<ERetCode> parse(std::shared_ptr<Lease> lease, SOperationOptions o) {
    auto e = lease->e; auto p = entry(e, o); if (p != ERET_OK) { co_return p; }
    if (e->parsing) { co_return ERET_BUSY; } if (e->headReady) { co_return ERET_OK; }
    e->parsing = true; ERetCode result = ERET_OK;
    try { co_await parseHead(e); } catch (...) { e->parsing = false; result = caught(e); }
    e->parsing = false; co_return result;
}

static asyncpp::task<SReadResult> readEntity(std::shared_ptr<Lease> lease, std::size_t count, SOperationOptions o, bool boundaryOnly = false) {
    auto e = lease->e; auto p = entry(e, o); if (p != ERET_OK) { co_return SReadResult{error(p), {}}; }
    if (!count && !boundaryOnly) { co_return SReadResult{error(ERET_INVALID_ARGUMENT), {}}; }
    if (!e->headReady) { co_return SReadResult{error(ERET_INVALID_STATE), {}}; }
    if (e->reading) { co_return SReadResult{error(ERET_BUSY), {}}; }
    if (e->incomingDone) { co_return SReadResult{error(ERET_EOF), {}}; }
    e->reading = true;
    try {
        count = (std::min)({count, e->options.limits.ioChunkBytes, e->options.scheduling.maximumBytesPerTurn});
        if (e->chunked && !e->remaining) {
            if (e->chunkCrlf) { if (!(co_await e->c->line(0)).empty()) { fail(ERET_MALFORMED, "chunk terminator"); } e->chunkCrlf = false; }
            auto line = co_await e->c->line(e->options.limits.maximumChunkLineBytes);
            if (line.find(';') != line.npos) { fail(ERET_UNSUPPORTED, "chunk extensions pending"); }
            e->remaining = number(line, 16);
            if (++e->chunks > e->options.limits.maximumMessageFrames || e->remaining > e->options.limits.maximumBodyBytes-e->consumed) { fail(ERET_LIMIT, "chunk limit"); }
            if (!e->remaining) {
                co_await parseFields(e, e->trailers, true); e->incomingDone = true; e->reading = false;
                co_return SReadResult{error(ERET_EOF), {}};
            }
            e->chunkCrlf = true;
        }
        if (!count) {
            if (e->closeDelimited && e->c->buffered.empty() && !co_await e->c->fill()) {
                e->incomingDone = true; e->reading = false; co_return SReadResult{error(ERET_EOF), {}};
            }
            fail(ERET_LIMIT, "body collection/discard limit");
        }
        if (!e->closeDelimited) { count = static_cast<std::size_t>((std::min<std::uint64_t>)(count, e->remaining)); }
        auto bytes = co_await e->c->bytes(count, e->closeDelimited);
        if (bytes.empty()) { e->incomingDone = true; }
        if (bytes.size() > e->options.limits.maximumBodyBytes-e->consumed) { fail(ERET_LIMIT, "entity limit"); }
        e->consumed += bytes.size();
        if (!e->closeDelimited) { e->remaining -= bytes.size(); if (!e->remaining && !e->chunked) { e->incomingDone = true; } }
        e->reading = false; const auto code = bytes.empty() ? ERET_EOF : ERET_OK;
        co_return SReadResult{error(code), std::move(bytes)};
    } catch (...) { e->reading = false; p = caught(e); }
    co_return SReadResult{error(p), {}};
}
class Reader final : public IHttpBodyReader {
private:
    std::shared_ptr<Lease> _l;
    static asyncpp::task<SReadResult> collectImpl(std::shared_ptr<Lease> l, std::uint64_t maximum, bool discard, SOperationOptions o) {
        SBytes bytes; std::uint64_t total = 0;
        while (true) {
            auto item = co_await readEntity(l, static_cast<std::size_t>((std::min<std::uint64_t>)(65536, maximum-total)), o, true);
            if (item.result.code == ERET_EOF) { co_return SReadResult{error(ERET_OK), std::move(bytes)}; }
            if (item.result.code != ERET_OK) { co_return item; }
            if (item.bytes.size() > maximum-total) { l->e->failed(error(ERET_LIMIT, "body collection/discard limit")); co_return SReadResult{error(ERET_LIMIT), {}}; }
            total += item.bytes.size(); if (!discard) { bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end()); }
        }
    }
    static asyncpp::task<ERetCode> discardImpl(std::shared_ptr<Lease> l, std::uint64_t maximum, SOperationOptions o) {
        auto value = co_await collectImpl(std::move(l), maximum, true, o); co_return value.result.code;
    }
public:
    explicit Reader(std::shared_ptr<Lease> l) : _l(std::move(l)) {}
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return readEntity(_l, n, o); }
    asyncpp::task<ERetCode> discard(std::uint64_t n, SOperationOptions o) override { return discardImpl(_l, n, o); }
    asyncpp::task<SReadResult> collect(std::size_t n, SOperationOptions o) override { return collectImpl(_l, n, false, o); }
    bool isComplete() const noexcept override { return _l->e->incomingDone; }
    const IReadOnlyHttpHeaders& trailers() const override {
        _l->e->check(); if (!isComplete()) { throw std::logic_error("trailers pending"); } return _l->e->trailers;
    }
    SHttpError lastError() const override { _l->e->check(); return _l->e->failure; }
};

static std::string phrase(std::uint16_t code) {
    switch (code) { case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 304: return "Not Modified";
    case 206: return "Partial Content"; case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 500: return "Internal Server Error";
    case 411: return "Length Required"; case 412: return "Precondition Failed"; case 416: return "Range Not Satisfiable";
    case 503: return "Service Unavailable"; default: return {}; }
}
struct Output { SBodyMetadata metadata; bool chunked = false, suppress = false; };
static asyncpp::task<Output> sendHead(const std::shared_ptr<Exchange>& e) {
    const bool request = e->role == HTTP_CLIENT;
    if (e->sendOptions.expectContinue) { fail(ERET_UNSUPPORTED, "Expect support pending"); }
    Output out;
    if (e->source) {
        auto result = co_await e->source->prepare(); if (result != ERET_OK) { fail(result, "body prepare failed"); }
        out.metadata = e->source->metadata();
        if (!out.metadata.trailerNames.empty()) { fail(ERET_UNSUPPORTED, "outgoing trailers pending"); }
    } else { out.metadata.contentLength = 0; }
    if (out.metadata.contentLength && *out.metadata.contentLength > e->options.limits.maximumBodyBytes) { fail(ERET_LIMIT, "outbound body limit"); }
    out.suppress = !request && (e->method == "HEAD" || e->status == 204 || e->status == 304);
    if (!request && e->status < 200) { fail(ERET_INVALID_ARGUMENT, "final status required"); }
    if (e->sendOptions.framing == EFRAMING_CONTENT_LENGTH && !out.metadata.contentLength) { fail(ERET_INVALID_ARGUMENT, "unknown content length"); }
    out.chunked = e->sendOptions.framing == EFRAMING_CHUNKED || (!out.metadata.contentLength && e->sendOptions.framing == EFRAMING_AUTO && e->wireVersion == HTTP_11);
    if (out.chunked && e->wireVersion != HTTP_11) { fail(ERET_UNSUPPORTED, "HTTP/1.0 chunked forbidden"); }
    if (request && ((!out.metadata.contentLength && !out.chunked) || e->sendOptions.framing == EFRAMING_CLOSE)) { fail(ERET_UNSUPPORTED, "close-delimited request forbidden"); }
    if (e->sendOptions.framing == EFRAMING_CLOSE || (!out.metadata.contentLength && !out.chunked)) { e->c->persistent = false; }
    if (!e->sendOptions.keepAlive || (e->wireVersion == HTTP_10 && !e->options.http1.allowHttp10KeepAlive)) { e->c->persistent = false; }
    std::string head;
    if (request) {
        if (!token(e->method) || e->method == "CONNECT") { fail(ERET_UNSUPPORTED, "invalid/unsupported method"); }
        if (e->target.form != ETARGET_ORIGIN && e->target.form != ETARGET_ASTERISK) { fail(ERET_UNSUPPORTED, "target form pending"); }
        if ((e->target.encodedPath != "*" && !e->target.encodedPath.starts_with('/')) || e->target.encodedPath.find_first_of(" ?#\r\n\t") != e->target.encodedPath.npos
            || !fieldValue(e->target.encodedPath) || (e->target.encodedPath == "*" && e->method != "OPTIONS")) { fail(ERET_INVALID_ARGUMENT, "invalid path"); }
        (void)decodeComponent(e->target.encodedPath);
        if (e->target.authority.empty() || e->target.authority.find_first_of(" \t\r\n/@#?\\") != e->target.authority.npos || !fieldValue(e->target.authority)) { fail(ERET_INVALID_ARGUMENT, "authority required"); }
        e->rawTarget = e->target.encodedPath; auto query = e->queries.wire(); if (!query.empty()) { e->rawTarget += '?'; e->rawTarget += query; }
        head = e->method + " " + e->rawTarget + " " + versionText(e->wireVersion) + "\r\nHost: " + e->target.authority + "\r\n";
    } else { head = versionText(e->wireVersion) + " " + std::to_string(e->status) + " " + e->reason + "\r\n"; }
    auto& fields = request ? e->requestHeaders : e->responseHeaders;
    const auto entries = fields.entries(0, 4096);
    if (entries.size() > e->options.limits.maximumHeaderFields) { fail(ERET_LIMIT, "outbound header count"); }
    for (const auto& field : entries) { head += field.name + ": " + field.value + "\r\n"; }
    if (e->source && !out.suppress) {
        if (!fieldValue(out.metadata.contentType)) { fail(ERET_INVALID_ARGUMENT, "invalid content type"); }
        head += "Content-Type: " + out.metadata.contentType + "\r\n";
    }
    if (!out.suppress) {
        if (out.chunked) { head += "Transfer-Encoding: chunked\r\n"; }
        else if (e->sendOptions.framing != EFRAMING_CLOSE && out.metadata.contentLength) { head += "Content-Length: " + std::to_string(*out.metadata.contentLength) + "\r\n"; }
    } else if (e->method == "HEAD" && out.metadata.contentLength) { head += "Content-Length: " + std::to_string(*out.metadata.contentLength) + "\r\n"; }
    if (!e->c->persistent) { head += "Connection: close\r\n"; }
    else if (e->wireVersion == HTTP_10) { head += "Connection: keep-alive\r\n"; }
    head += "\r\n";
    if (head.size() > e->options.limits.maximumHeaderBytes) { fail(ERET_LIMIT, "outbound header bytes"); }
    fields.freeze(); e->queries.freeze(); e->sealed = true;
    co_await e->c->sendText(std::move(head)); co_return out;
}
static asyncpp::task<void> sendBody(const std::shared_ptr<Exchange>& e, Output output) {
    std::uint64_t sent = 0;
    if (e->source && !output.suppress) {
        while (true) {
            const auto maximum = (std::min)(e->options.limits.ioChunkBytes, e->options.scheduling.maximumBytesPerTurn);
            auto part = co_await e->source->readSome(maximum);
            if (part.result.code == ERET_EOF) { break; }
            if (part.result.code != ERET_OK) { fail(part.result.code, "source read failed"); }
            if (part.bytes.empty() || part.bytes.size() > maximum) { fail(ERET_IO, "source made invalid progress"); }
            if (part.bytes.size() > e->options.limits.maximumBodyBytes-sent || (output.metadata.contentLength && part.bytes.size() > *output.metadata.contentLength-sent)) { fail(ERET_LIMIT, "source length exceeded"); }
            sent += part.bytes.size();
            if (output.chunked) { char digits[32]; auto result = std::to_chars(digits, digits+32, part.bytes.size(), 16); co_await e->c->sendText(std::string(digits, result.ptr)+"\r\n"); }
            co_await e->c->send(std::move(part.bytes)); if (output.chunked) { co_await e->c->sendText("\r\n"); }
        }
        if (output.metadata.contentLength && sent != *output.metadata.contentLength) { fail(ERET_TRUNCATED, "source length mismatch"); }
        if (!e->source->trailers().empty()) { fail(ERET_UNSUPPORTED, "outbound trailers pending"); }
    }
    if (output.chunked && !output.suppress) { co_await e->c->sendText("0\r\n\r\n"); }
}
static asyncpp::task<void> upload(std::shared_ptr<Exchange> e, Output output) {
    try { co_await sendBody(e, output); }
    catch (...) { try { (void)caught(e); } catch (...) {} }
    e->outgoingDone = true; e->sent.set();
    e->uploadRunning = false;
}
static asyncpp::task<ERetCode> execute(std::shared_ptr<Lease> l, SOperationOptions o) {
    auto e = l->e; auto p = entry(e, o); if (p != ERET_OK) { co_return p; }
    if (e->executing) { co_return ERET_BUSY; }
    if (e->sealed || (e->role == HTTP_SERVER && !e->headReady)) { co_return ERET_INVALID_STATE; }
    e->executing = true;
    e->requestHeaders.freeze(); e->responseHeaders.freeze(); e->queries.freeze();
    try {
        auto output = co_await sendHead(e); e->sendStarted = true;
        if (e->role == HTTP_CLIENT) {
            e->uploadRunning = true;
            asyncpp::launch(upload(e, output)); // Exchange owns the sender state; sent is its explicit join.
            co_await parseHead(e);
        } else {
            co_await sendBody(e, output); e->outgoingDone = true; e->sent.set();
        }
        e->executing = false; co_return ERET_OK;
    } catch (...) {
        e->executing = false;
        if (!e->uploadRunning) { e->outgoingDone = true; e->sent.set(); }
        p = caught(e);
    }
    co_return p;
}
static asyncpp::task<ERetCode> waitSent(std::shared_ptr<Lease> l, SOperationOptions o) {
    auto e = l->e; auto p = entry(e, o); if (p != ERET_OK && p != ERET_CLOSED) { co_return p; }
    if (!e->sendStarted) { co_return ERET_INVALID_STATE; }
    co_await e->sent; co_return e->failure.code;
}
static asyncpp::task<ERetCode> finish(std::shared_ptr<Lease> l, SFinishOptions f, SOperationOptions o) {
    auto e = l->e; auto p = entry(e, o); if (p != ERET_OK) { co_return p; }
    if (e->state == ECONTEXT_COMPLETE) { co_return ERET_OK; }
    if (!e->incomingDone && f.discardUnreadBody) {
        Reader reader(l); p = co_await reader.discard(f.maximumDiscardBytes, o); if (p != ERET_OK) { co_return p; }
    }
    if (!e->incomingDone || !e->outgoingDone) { co_return ERET_BODY_PENDING; }
    if (e->failure.code != ERET_OK) { co_return e->failure.code; }
    e->state = ECONTEXT_COMPLETE; e->c->leased = false;
    if (!e->c->persistent) { e->c->close(); } co_return ERET_OK;
}
static EHttpMethod knownMethod(const std::string& m) {
    static const std::pair<const char*, EHttpMethod> methods[] = {{"GET",HTTP_GET},{"HEAD",HTTP_HEAD},{"POST",HTTP_POST},
        {"PUT",HTTP_PUT},{"DELETE",HTTP_DELETE},{"CONNECT",HTTP_CONNECT},{"OPTIONS",HTTP_OPTIONS},{"TRACE",HTTP_TRACE},{"PATCH",HTTP_PATCH}};
    for (const auto& value : methods) { if (m == value.first) { return value.second; } } return HTTP_EXTENSION;
}
class Request : public virtual IHttpRequest {
protected:
    std::shared_ptr<Lease> _l;
public:
    explicit Request(std::shared_ptr<Lease> l) : _l(std::move(l)) {}
    EHttpVersion version() const noexcept override { return _l->e->wireVersion; }
    EHttpMethod method() const noexcept override { return _l->e->role == HTTP_SERVER && !_l->e->headReady ? HTTP_INVALID : knownMethod(_l->e->method); }
    std::string methodName() const override { _l->e->check(); return _l->e->method; }
    SRequestTarget target() const override { _l->e->check(); return _l->e->target; }
    std::string path() const override { _l->e->check(); return _l->e->target.encodedPath; }
    std::string rawTarget() const override { _l->e->check(); return _l->e->rawTarget; }
    const IReadOnlyHttpHeaders& headers() const override { _l->e->check(); return _l->e->requestHeaders; }
    const IReadOnlyHttpQueries& queries() const override { _l->e->check(); return _l->e->queries; }
    SHttpError lastError() const override { _l->e->check(); return _l->e->failure; }
};
class ServerRequest final : public Request, public IHttpServerRequest {
public:
    using Request::Request;
    asyncpp::task<ERetCode> parseOnce(SOperationOptions o) override { return parse(_l, o); }
    std::shared_ptr<IHttpBodyReader> getBody() const override { _l->e->check(); return std::make_shared<Reader>(_l); }
};
class ClientRequest final : public Request, public IHttpClientRequest {
public:
    using Request::Request;
    using Request::method;
    using Request::path;
    using Request::target;
    using Request::headers;
    using Request::queries;
    IHttpClientRequest& method(EHttpMethod m) override {
        static const char* names[] = {"", "GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH"};
        if (m < HTTP_GET || m > HTTP_PATCH) { throw std::invalid_argument("invalid method enum"); } return method(names[m]);
    }
    IHttpClientRequest& method(std::string m) override { _l->e->edit(); if (!token(m)) { throw std::invalid_argument("invalid method"); } _l->e->method = std::move(m); return *this; }
    IHttpClientRequest& path(std::string p) override { _l->e->edit(); _l->e->target.encodedPath = std::move(p); return *this; }
    IHttpClientRequest& authority(std::string a) override { _l->e->edit(); _l->e->target.authority = std::move(a); return *this; }
    IHttpClientRequest& target(SRequestTarget t) override { _l->e->edit(); _l->e->target = std::move(t); return *this; }
    IHttpHeaders& headers() override { _l->e->edit(); return _l->e->requestHeaders; }
    IHttpQueries& queries() override { _l->e->edit(); return _l->e->queries; }
    IHttpClientRequest& body(std::shared_ptr<IHttpBodySource> b) override { _l->e->edit(); _l->e->source = std::move(b); return *this; }
    IHttpClientRequest& options(SSendOptions o) override { _l->e->edit(); _l->e->sendOptions = o; return *this; }
    IHttpClientRequest& observer(std::shared_ptr<IHttpInformationalObserver> o) override { _l->e->edit(); _l->e->observer = std::move(o); return *this; }
    asyncpp::task<ERetCode> exec(SOperationOptions o) override { return execute(_l, o); }
    bool isSendComplete() const noexcept override { return _l->e->outgoingDone; }
    asyncpp::task<ERetCode> waitSent(SOperationOptions o) override { return detail::waitSent(_l, o); }
};
class Response : public virtual IHttpResponse {
protected:
    std::shared_ptr<Lease> _l;
public:
    explicit Response(std::shared_ptr<Lease> l) : _l(std::move(l)) {}
    EHttpVersion version() const noexcept override { return _l->e->wireVersion; }
    std::uint16_t status() const override { _l->e->check(); return _l->e->status; }
    std::string reason() const override { _l->e->check(); return _l->e->reason; }
    const IReadOnlyHttpHeaders& headers() const override { _l->e->check(); return _l->e->responseHeaders; }
    SHttpError lastError() const override { _l->e->check(); return _l->e->failure; }
};
class ClientResponse final : public Response, public IHttpClientResponse {
public:
    using Response::Response;
    std::shared_ptr<IHttpBodyReader> getBody() const override { _l->e->check(); return std::make_shared<Reader>(_l); }
};
class ServerResponse final : public Response, public IHttpServerResponse {
public:
    using Response::Response;
    using Response::status;
    using Response::headers;
    IHttpServerResponse& status(std::uint16_t code) override { return status(code, phrase(code)); }
    IHttpServerResponse& status(std::uint16_t code, std::string reason) override {
        _l->e->edit(); if (code < 200 || code > 599 || !fieldValue(reason)) { throw std::invalid_argument("invalid final status"); }
        _l->e->status = code; _l->e->reason = std::move(reason); return *this;
    }
    IHttpHeaders& headers() override { _l->e->edit(); return _l->e->responseHeaders; }
    IHttpServerResponse& body(std::shared_ptr<IHttpBodySource> source) override { _l->e->edit(); _l->e->source = std::move(source); return *this; }
    IHttpServerResponse& options(SSendOptions o) override { _l->e->edit(); _l->e->sendOptions = o; return *this; }
    asyncpp::task<ERetCode> sendInformational(std::uint16_t, std::vector<SHeaderField>, SOperationOptions) override {
        co_return ERET_UNSUPPORTED;
    }
    IHttpServerResponse& acceptUpgrade(std::string) override { unsupported("Upgrade implementation pending"); return *this; }
    asyncpp::task<ERetCode> exec(SOperationOptions o) override { return execute(_l, o); }
};
class Context : public virtual IHttpContext {
protected:
    std::shared_ptr<Lease> _l;
    static asyncpp::task<ERetCode> abortImpl(std::shared_ptr<Lease> l, SOperationOptions o) {
        auto p = entry(l->e, o); if (p != ERET_OK && p != ERET_CLOSED) { co_return p; }
        // --> Completed exchanges have relinquished their lease; abort cannot affect a successor.
        if (l->e->state == ECONTEXT_COMPLETE) { co_return ERET_OK; }
        l->e->state = ECONTEXT_FAILED; l->e->c->close(); co_return co_await l->e->connection->waitClosed();
    }
public:
    explicit Context(std::shared_ptr<Lease> l) : _l(std::move(l)) {}
    ERetCode setStream(std::shared_ptr<IHttpStream> stream) override {
        if (!stream) { return ERET_INVALID_ARGUMENT; }
        if (!current(stream->executor())) { return ERET_WRONG_EXECUTOR; }
        if (_l->e->c) { return ERET_INVALID_STATE; }
        try { attach(_l->e, std::dynamic_pointer_cast<IHttp1Connection>(IHttpConnection::createStream(stream, _l->e->role, _l->e->options))); }
        catch (const std::logic_error&) { return ERET_INVALID_STATE; }
        return ERET_OK;
    }
    std::shared_ptr<IHttpConnection> getConnection() const override { _l->e->check(); return _l->e->connection; }
    STransportSecurity transportSecurity() const override { _l->e->check(); return _l->e->c ? _l->e->c->stream->security() : STransportSecurity{}; }
    std::optional<std::uint64_t> streamId() const noexcept override { return {}; }
    EContextState state() const noexcept override { return _l->e->state; }
    asyncpp::task<ERetCode> finish(SFinishOptions f, SOperationOptions o) override { return detail::finish(_l, f, o); }
    asyncpp::task<ERetCode> abort(SOperationOptions o) override { return abortImpl(_l, o); }
    STunnelResult takeTunnel() override { _l->e->check(); return {error(ERET_UNSUPPORTED), {}}; }
    SHttpError lastError() const override { _l->e->check(); return _l->e->failure; }
};
class ServerContext final : public Context, public IHttpServerContext {
public:
    using Context::Context;
    std::shared_ptr<IHttpServerRequest> getRequest() const override { _l->e->check(); return std::make_shared<ServerRequest>(_l); }
    std::shared_ptr<IHttpServerResponse> getResponse() const override { _l->e->check(); return std::make_shared<ServerResponse>(_l); }
};
class ClientContext final : public Context, public IHttpClientContext {
public:
    using Context::Context;
    std::shared_ptr<IHttpClientRequest> getRequest() const override { _l->e->check(); return std::make_shared<ClientRequest>(_l); }
    std::shared_ptr<IHttpClientResponse> getResponse() const override { _l->e->check(); return std::make_shared<ClientResponse>(_l); }
};
static std::shared_ptr<Lease> makeLease(EHttpRole role, SHttpConnectionOptions options) {
    validateOptions(options); auto l = std::make_shared<Lease>(); l->e = std::make_shared<Exchange>();
    l->e->role = role; l->e->options = options; l->e->wireVersion = options.version; return l;
}
} // namespace detail

std::shared_ptr<IHttpConnection> IHttpConnection::createStream(std::shared_ptr<IHttpStream> stream, EHttpRole role, SHttpConnectionOptions options) {
    if (options.version == HTTP_2) { return detail::mux::h2(std::move(stream),role,options); }
    detail::validateOptions(options);
    if (!stream || !stream->executor() || (role != HTTP_CLIENT && role != HTTP_SERVER)) { throw std::invalid_argument("invalid HTTP stream/role"); }
    detail::require(stream->executor());
    auto state = std::make_shared<detail::ConnectionState>(); state->stream = std::move(stream); state->role = role; state->options = options;
    detail::claimTransport(state->stream,state);
    return std::make_shared<detail::Connection>(state);
}
std::shared_ptr<IHttpConnection> IHttpConnection::createQuic(std::unique_ptr<IHttpQuicTransport> transport, EHttpRole role, SHttpConnectionOptions options) {
    if (options.version != HTTP_3) { throw std::invalid_argument("QUIC requires HTTP/3"); }
    return detail::mux::h3(std::move(transport),role,options);
}
std::shared_ptr<IHttpServerContext> IHttpServerContext::create(SHttpConnectionOptions options) {
    return std::make_shared<detail::ServerContext>(detail::makeLease(HTTP_SERVER, options));
}
std::shared_ptr<IHttpServerContext> IHttpServerContext::create(std::shared_ptr<IHttp1Connection> connection) {
    auto l = detail::makeLease(HTTP_SERVER, connection ? connection->options() : SHttpConnectionOptions{}); detail::attach(l->e, std::move(connection));
    return std::make_shared<detail::ServerContext>(std::move(l));
}
std::shared_ptr<IHttpClientContext> IHttpClientContext::create(SHttpConnectionOptions options) {
    return std::make_shared<detail::ClientContext>(detail::makeLease(HTTP_CLIENT, options));
}
std::shared_ptr<IHttpClientContext> IHttpClientContext::create(std::shared_ptr<IHttpConnection> connection) {
    if (connection && (connection->version() == HTTP_2 || connection->version() == HTTP_3)) { return detail::mux::clientContext(std::move(connection)); }
    auto l = detail::makeLease(HTTP_CLIENT, connection ? connection->options() : SHttpConnectionOptions{});
    detail::attach(l->e, std::dynamic_pointer_cast<IHttp1Connection>(connection)); return std::make_shared<detail::ClientContext>(std::move(l));
}
} // namespace hnhttp
