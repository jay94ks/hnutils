#include <hnhttp/endpoint.hpp>
#include "internal.hpp"
#include "multiplexed_internal.hpp"
#include <asyncpp/event.h>
#include <atomic>
#include <span>

namespace hnhttp {

asyncpp::task<ERetCode> IHttpClient::run(SOperationOptions o) {
    if (!detail::current(executor())) { co_return ERET_WRONG_EXECUTOR; }
    auto p = detail::policy(o); co_return p != ERET_OK ? p : ERET_UNSUPPORTED;
}
asyncpp::task<ERetCode> IHttpServer::run(SOperationOptions o) {
    if (!detail::current(executor())) { co_return ERET_WRONG_EXECUTOR; }
    auto p = detail::policy(o); co_return p != ERET_OK ? p : ERET_UNSUPPORTED;
}

struct CSocketHttpStream::Impl {
    std::shared_ptr<sockpp::CSocketWatcher> watcher;
    sockpp::CSocketAsync socket;
    std::atomic<bool> closed{false};
    bool closeProcessed = false, reading = false, writing = false;
    asyncpp::multi_consumer_event drained;
    Impl(std::shared_ptr<sockpp::CSocketWatcher> w, sockpp::CSocketAsync s)
        : watcher(std::move(w)), socket(std::move(s)) {}
    void signal() { if (closeProcessed && !reading && !writing) { drained.set(); } }
    ERetCode entry(SOperationOptions options) const {
        if (!detail::current(watcher)) { return ERET_WRONG_EXECUTOR; }
        const auto p = detail::policy(options); return p != ERET_OK ? p : closed ? ERET_CLOSED : ERET_OK;
    }
    static asyncpp::task<SReadResult> read(std::shared_ptr<Impl> s, std::size_t count, SOperationOptions o) {
        auto p = s->entry(o); if (p != ERET_OK) { co_return SReadResult{detail::error(p), {}}; }
        if (!count || count > 16*1024*1024) { co_return SReadResult{detail::error(ERET_INVALID_ARGUMENT), {}}; }
        if (s->reading) { co_return SReadResult{detail::error(ERET_BUSY), {}}; }
        SBytes output(count); s->reading = true; SHttpError failure;
        try {
            count = co_await s->socket.receive(std::as_writable_bytes(std::span(output)));
            output.resize(count); failure = detail::error(count ? ERET_OK : ERET_EOF);
        } catch (const std::system_error& x) { failure = detail::error(s->closed ? ERET_CLOSED : ERET_IO, x.what(), x.code()); output.clear(); }
        catch (...) { s->reading = false; s->signal(); throw; }
        s->reading = false; s->signal(); co_return SReadResult{std::move(failure), std::move(output)};
    }
    static asyncpp::task<SWriteResult> write(std::shared_ptr<Impl> s, SBytes bytes, SOperationOptions o) {
        auto p = s->entry(o); if (p != ERET_OK) { co_return SWriteResult{detail::error(p), 0}; }
        if (bytes.empty()) { co_return SWriteResult{detail::error(ERET_INVALID_ARGUMENT), 0}; }
        if (s->writing) { co_return SWriteResult{detail::error(ERET_BUSY), 0}; }
        s->writing = true; SWriteResult result;
        try {
            result.acceptedBytes = co_await s->socket.send(std::as_bytes(std::span(bytes)));
            result.result = detail::error(result.acceptedBytes ? ERET_OK : ERET_IO);
        } catch (const std::system_error& x) { result.result = detail::error(s->closed ? ERET_CLOSED : ERET_IO, x.what(), x.code()); }
        catch (...) { s->writing = false; s->signal(); throw; }
        s->writing = false; s->signal(); co_return result;
    }
    static asyncpp::task<ERetCode> halfClose(std::shared_ptr<Impl> s, SOperationOptions o) {
        auto p = s->entry(o); if (p != ERET_OK) { co_return p; } if (s->writing) { co_return ERET_BUSY; }
        try { s->socket.shutdown(sockpp::ESHUT_WRITE); } catch (const std::system_error&) { co_return ERET_IO; }
        co_return ERET_OK;
    }
    static asyncpp::task<ERetCode> join(std::shared_ptr<Impl> s, SOperationOptions o) {
        if (!detail::current(s->watcher)) { co_return ERET_WRONG_EXECUTOR; }
        auto p = detail::policy(o); if (p != ERET_OK) { co_return p; }
        if (!s->closed) { co_return ERET_INVALID_STATE; }
        co_await s->drained; co_return ERET_OK;
    }
};

CSocketHttpStream::CSocketHttpStream(std::shared_ptr<Impl> impl) : _impl(std::move(impl)) {}
std::shared_ptr<CSocketHttpStream> CSocketHttpStream::create(std::shared_ptr<sockpp::CSocketWatcher> watcher, sockpp::CSocketAsync socket) {
    if (!watcher || !socket.isOpen() || !socket.usesWatcher(*watcher) || !socket.isByteStream()) { throw std::invalid_argument("invalid reactor/stream socket"); }
    detail::require(watcher);
    (void)socket.peerAddress();
    return std::shared_ptr<CSocketHttpStream>(new CSocketHttpStream(std::make_shared<Impl>(std::move(watcher), std::move(socket))));
}
CSocketHttpStream::~CSocketHttpStream() { close(); }
std::shared_ptr<asyncpp::dispatcher> CSocketHttpStream::executor() const noexcept { return _impl->watcher; }
SAsyncCapabilities CSocketHttpStream::capabilities() const noexcept { return {}; }
STransportSecurity CSocketHttpStream::security() const { detail::require(executor()); return {}; }
asyncpp::task<SReadResult> CSocketHttpStream::readSome(std::size_t n, SOperationOptions o) { return Impl::read(_impl, n, o); }
asyncpp::task<SWriteResult> CSocketHttpStream::writeSome(SBytes bytes, SOperationOptions o) { return Impl::write(_impl, std::move(bytes), o); }
asyncpp::task<ERetCode> CSocketHttpStream::finishWrite(SOperationOptions o) { return Impl::halfClose(_impl, o); }
asyncpp::task<ERetCode> CSocketHttpStream::waitClosed(SOperationOptions o) { return Impl::join(_impl, o); }
void CSocketHttpStream::close() noexcept {
    if (_impl->closed.exchange(true)) { return; }
    auto s = _impl;
    s->watcher->push([s] { (void)s->socket.close(); s->closeProcessed = true; s->signal(); });
}

namespace detail {
struct EndpointState {
    std::shared_ptr<sockpp::CSocketWatcher> watcher;
    SHttpConnectionOptions options;
    std::unique_ptr<sockpp::CSocketAsync> socket;
    std::shared_ptr<IHttpConnection> connection;
    std::shared_ptr<CSocketHttpStream> stream;
    std::atomic<bool> closed{false};
    bool nativePending = false, closing = false;
    std::size_t activeAccepts = 0;
    std::vector<std::weak_ptr<IHttpConnection>> parsingHeads;
    asyncpp::multi_consumer_event drained;
    void signal() { if (closing && !nativePending && !activeAccepts) { drained.set(); } }
};
static void closeEndpoint(const std::shared_ptr<EndpointState>& s) noexcept {
    if (s->closed.exchange(true)) { return; }
    s->watcher->push([s] {
        s->closing = true; if (s->socket) { (void)s->socket->close(); }
        if (s->connection) { s->connection->close(); } s->signal();
        for (const auto& weak : s->parsingHeads) { if (auto c = weak.lock()) { c->close(); } }
    });
}
static asyncpp::task<ERetCode> joinEndpoint(std::shared_ptr<EndpointState> s, SOperationOptions o) {
    if (!current(s->watcher)) { co_return ERET_WRONG_EXECUTOR; }
    auto p = policy(o); if (p != ERET_OK) { co_return p; } if (!s->closed) { co_return ERET_INVALID_STATE; }
    co_await s->drained;
    if (s->connection) { co_return co_await s->connection->waitClosed(o); }
    if (s->stream) { co_return co_await s->stream->waitClosed(o); }
    co_return ERET_OK;
}
class Client final : public IHttpClient {
private:
    std::shared_ptr<EndpointState> _s;
    static asyncpp::task<ERetCode> connectImpl(std::shared_ptr<EndpointState> s, sockpp::CSocketAddress address, SOperationOptions o) {
        if (!current(s->watcher)) { co_return ERET_WRONG_EXECUTOR; }
        auto p = policy(o); if (p != ERET_OK) { co_return p; }
        if (s->closed) { co_return ERET_CLOSED; }
        if (s->nativePending) { co_return ERET_BUSY; }
        if (s->socket || s->connection) { co_return ERET_INVALID_STATE; }
        s->socket = std::make_unique<sockpp::CSocketAsync>(sockpp::CSocketAsync::open(*s->watcher, address.family(), sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP));
        s->nativePending = true;
        try {
            co_await s->socket->connect(address);
            if (s->closed) { p = ERET_CLOSED; }
            else {
                s->stream = CSocketHttpStream::create(s->watcher, std::move(*s->socket));
                s->socket.reset(); s->connection = IHttpConnection::createStream(s->stream, HTTP_CLIENT, s->options);
            }
        } catch (const std::system_error&) { p = s->closed ? ERET_CLOSED : ERET_IO; }
        catch (...) { s->nativePending = false; closeEndpoint(s); s->signal(); throw; }
        s->nativePending = false; if (p != ERET_OK) { closeEndpoint(s); } s->signal(); co_return p;
    }
public:
    explicit Client(std::shared_ptr<EndpointState> s) : _s(std::move(s)) {}
    ~Client() override { closeEndpoint(_s); }
    asyncpp::task<ERetCode> connect(sockpp::CSocketAddress a, SOperationOptions o) override { return connectImpl(_s, std::move(a), o); }
    std::shared_ptr<IHttpClientContext> createRequest() override {
        require(_s->watcher); if (!_s->connection || _s->closed) { throw std::logic_error("client not connected"); }
        return IHttpClientContext::create(_s->connection);
    }
    std::shared_ptr<IHttpConnection> getConnection() const override { require(_s->watcher); return _s->connection; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    void close() noexcept override { closeEndpoint(_s); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return joinEndpoint(_s, o); }
};
class Server final : public IHttpServer {
private:
    std::shared_ptr<EndpointState> _s;
    struct AcceptGuard {
        std::shared_ptr<EndpointState> state;
        std::shared_ptr<IHttpConnection> connection;
        ~AcceptGuard() {
            std::erase_if(state->parsingHeads, [&](const auto& weak) {
                auto live = weak.lock(); return !live || live == connection;
            });
            --state->activeAccepts; state->signal();
        }
    };
    static asyncpp::task<SAcceptResult> acceptImpl(std::shared_ptr<EndpointState> s, SOperationOptions o) {
        if (!current(s->watcher)) { co_return SAcceptResult{error(ERET_WRONG_EXECUTOR), {}}; }
        auto p = policy(o); if (p != ERET_OK) { co_return SAcceptResult{error(p), {}}; }
        if (s->options.version == HTTP_2) { co_return SAcceptResult{error(ERET_UNSUPPORTED),{}}; }
        if (s->closed) { co_return SAcceptResult{error(ERET_CLOSED), {}}; }
        if (!s->socket) { co_return SAcceptResult{error(ERET_INVALID_STATE), {}}; }
        if (s->nativePending) { co_return SAcceptResult{error(ERET_BUSY), {}}; }
        if (s->activeAccepts >= s->options.limits.maximumPendingRequests) { co_return SAcceptResult{error(ERET_LIMIT), {}}; }
        ++s->activeAccepts; AcceptGuard guard{s, {}};
        s->nativePending = true;
        std::shared_ptr<IHttpServerContext> context;
        try {
            auto socket = co_await s->socket->accept(); s->nativePending = false; s->signal();
            if (s->closed) { co_return SAcceptResult{error(ERET_CLOSED), {}}; }
            auto stream = CSocketHttpStream::create(s->watcher, std::move(socket));
            context = IHttpServerContext::create(s->options);
            p = context->setStream(stream);
            guard.connection = context->getConnection(); s->parsingHeads.push_back(guard.connection);
            if (p == ERET_OK) { p = co_await context->getRequest()->parseOnce(o); }
        } catch (const std::system_error& x) {
            s->nativePending = false; s->signal(); co_return SAcceptResult{error(s->closed ? ERET_CLOSED : ERET_IO, x.what(), x.code()), {}};
        } catch (...) { s->nativePending = false; s->signal(); throw; }
        if (p != ERET_OK) {
            if (context && context->getConnection()) { context->getConnection()->close(); co_await context->getConnection()->waitClosed(); }
            co_return SAcceptResult{error(p), {}};
        }
        co_return SAcceptResult{error(ERET_OK), std::move(context)};
    }
    static asyncpp::task<SConnectionResult> acceptConnectionImpl(std::shared_ptr<EndpointState> s, SOperationOptions o) {
        if (!current(s->watcher)) { co_return SConnectionResult{error(ERET_WRONG_EXECUTOR),{}}; }
        auto p = policy(o); if (p != ERET_OK) { co_return SConnectionResult{error(p),{}}; }
        if (s->closed) { co_return SConnectionResult{error(ERET_CLOSED),{}}; }
        if (!s->socket) { co_return SConnectionResult{error(ERET_INVALID_STATE),{}}; }
        if (s->nativePending) { co_return SConnectionResult{error(ERET_BUSY),{}}; }
        ++s->activeAccepts; AcceptGuard guard{s,{}}; s->nativePending = true;
        try {
            auto socket = co_await s->socket->accept(); s->nativePending = false; s->signal();
            if (s->closed) { co_return SConnectionResult{error(ERET_CLOSED),{}}; }
            auto stream = CSocketHttpStream::create(s->watcher,std::move(socket));
            auto connection = IHttpConnection::createStream(std::move(stream),HTTP_SERVER,s->options);
            co_return SConnectionResult{error(ERET_OK),std::move(connection)};
        } catch (const std::system_error& exception) {
            s->nativePending = false; s->signal(); co_return SConnectionResult{error(s->closed ? ERET_CLOSED : ERET_IO,exception.what(),exception.code()),{}};
        } catch (...) { s->nativePending = false; s->signal(); throw; }
    }
public:
    explicit Server(std::shared_ptr<EndpointState> s) : _s(std::move(s)) {}
    ~Server() override { closeEndpoint(_s); }
    ERetCode listen(sockpp::CSocketAddress address, int backlog) override {
        if (!current(_s->watcher)) { return ERET_WRONG_EXECUTOR; }
        if (backlog <= 0) { return ERET_INVALID_ARGUMENT; }
        if (_s->closed) { return ERET_CLOSED; } if (_s->socket) { return ERET_INVALID_STATE; }
        try {
            auto socket = sockpp::CSocketAsync::open(*_s->watcher, address.family(), sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
            socket.bind(address); socket.listen(backlog); _s->socket = std::make_unique<sockpp::CSocketAsync>(std::move(socket));
        } catch (const std::system_error&) { return ERET_IO; } return ERET_OK;
    }
    sockpp::CSocketAddress localAddress() const override {
        require(_s->watcher); if (!_s->socket) { throw std::logic_error("listener not open"); } return _s->socket->localAddress();
    }
    asyncpp::task<SAcceptResult> accept(SOperationOptions o) override { return acceptImpl(_s, o); }
    asyncpp::task<SConnectionResult> acceptConnection(SOperationOptions o) override { return acceptConnectionImpl(_s,o); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    void close() noexcept override { closeEndpoint(_s); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return joinEndpoint(_s, o); }
};
static std::shared_ptr<EndpointState> makeEndpoint(std::shared_ptr<sockpp::CSocketWatcher> watcher, SHttpConnectionOptions options) {
    if (!watcher) { throw std::invalid_argument("null socket watcher"); }
    if (options.version == HTTP_2) { mux::validate(options); } else { validateOptions(options); }
    auto s = std::make_shared<EndpointState>(); s->watcher = std::move(watcher); s->options = options; return s;
}
} // namespace detail

std::shared_ptr<IHttpClient> IHttpClient::create(std::shared_ptr<sockpp::CSocketWatcher> watcher, SHttpConnectionOptions options) {
    return std::make_shared<detail::Client>(detail::makeEndpoint(std::move(watcher), options));
}
std::shared_ptr<IHttpServer> IHttpServer::create(std::shared_ptr<sockpp::CSocketWatcher> watcher, SHttpConnectionOptions options) {
    return std::make_shared<detail::Server>(detail::makeEndpoint(std::move(watcher), options));
}
} // namespace hnhttp
