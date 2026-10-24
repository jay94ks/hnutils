#include <hnhttp/endpoint.hpp>
#include "internal.hpp"
#include "multiplexed_internal.hpp"
#include <hntls/client.hpp>
#include <hntls/socket_stream.hpp>
#include <hnquic/client.hpp>
#include <hnquic/server.hpp>
#include <asyncpp/event.h>
#include <asyncpp/scope_guard.h>
#include <atomic>

namespace hnhttp {
namespace detail {
namespace {
ERetCode code(hntls::ERetCode c) {
    switch (c) {
    case hntls::ERET_OK:
        return ERET_OK;
    case hntls::ERET_EOF:
        return ERET_EOF;
    case hntls::ERET_AUTHENTICATION:
        return ERET_AUTHENTICATION;
    case hntls::ERET_CLOSED:
        return ERET_CLOSED;
    case hntls::ERET_CANCELLED:
        return ERET_CANCELLED;
    case hntls::ERET_TIMEOUT:
        return ERET_TIMEOUT;
    case hntls::ERET_LIMIT:
        return ERET_LIMIT;
    case hntls::ERET_BUSY:
        return ERET_BUSY;
    case hntls::ERET_WRONG_EXECUTOR:
        return ERET_WRONG_EXECUTOR;
    case hntls::ERET_INVALID_ARGUMENT:
        return ERET_INVALID_ARGUMENT;
    case hntls::ERET_INVALID_STATE:
        return ERET_INVALID_STATE;
    case hntls::ERET_TRUNCATED:
        return ERET_TRUNCATED;
    case hntls::ERET_NOT_IMPLEMENTED:
    case hntls::ERET_UNSUPPORTED:
        return ERET_UNSUPPORTED;
    default:
        return ERET_IO;
    }
}
ERetCode code(hnquic::ERetCode c) {
    switch (c) {
    case hnquic::ERET_OK:
        return ERET_OK;
    case hnquic::ERET_EOF:
        return ERET_EOF;
    case hnquic::ERET_CLOSED:
        return ERET_CLOSED;
    case hnquic::ERET_CANCELLED:
        return ERET_CANCELLED;
    case hnquic::ERET_TIMEOUT:
        return ERET_TIMEOUT;
    case hnquic::ERET_LIMIT:
        return ERET_LIMIT;
    case hnquic::ERET_BUSY:
        return ERET_BUSY;
    case hnquic::ERET_WRONG_EXECUTOR:
        return ERET_WRONG_EXECUTOR;
    case hnquic::ERET_INVALID_ARGUMENT:
        return ERET_INVALID_ARGUMENT;
    case hnquic::ERET_INVALID_STATE:
        return ERET_INVALID_STATE;
    case hnquic::ERET_STREAM_RESET:
        return ERET_STREAM_RESET;
    case hnquic::ERET_NOT_IMPLEMENTED:
    case hnquic::ERET_UNSUPPORTED:
        return ERET_UNSUPPORTED;
    default:
        return ERET_IO;
    }
}
struct SecureState {
    std::shared_ptr<sockpp::CSocketWatcher> watcher;
    std::shared_ptr<hntls::ITlsContext> tls;
    SHttpConnectionOptions options;
    std::shared_ptr<hntls::ITlsClient> tlsClient;
    std::shared_ptr<hnquic::IQuicClient> quicClient;
    std::shared_ptr<hnquic::IQuicServer> quicServer;
    std::unique_ptr<sockpp::CSocketAsync> listener;
    std::shared_ptr<IHttpConnection> connection;
    std::vector<std::weak_ptr<hntls::ITlsStream>> handshakes;
    std::vector<std::weak_ptr<IHttpConnection>> heads;
    std::atomic<bool> closed{false};
    bool used = false, closeProcessed = false, connecting = false, acceptingSocket = false, running = false;
    std::size_t pending = 0;
    std::string authority;
    asyncpp::multi_consumer_event drained;
    void signal() {
        if (closeProcessed && !connecting && !pending && !running) {
            drained.set();
        }
    }
    ERetCode entry(SOperationOptions o) const {
        if (!current(watcher)) {
            return ERET_WRONG_EXECUTOR;
        }
        auto c = policy(o);
        return c != ERET_OK ? c : closed ? ERET_CLOSED : ERET_OK;
    }
};
void closeSecure(const std::shared_ptr<SecureState>& s) noexcept {
    if (s->closed.exchange(true)) {
        return;
    }
    auto fn = [s] {
        if (s->listener) {
            s->listener->close();
        }
        if (s->tlsClient) {
            s->tlsClient->close();
        }
        if (s->quicClient) {
            s->quicClient->close();
        }
        if (s->quicServer) {
            s->quicServer->close();
        }
        if (s->connection) {
            s->connection->close();
        }
        for (auto& weak : s->handshakes) {
            if (auto stream = weak.lock()) {
                stream->close();
            }
        }
        for (auto& weak : s->heads) {
            if (auto connection = weak.lock()) {
                connection->close();
            }
        }
        s->closeProcessed = true;
        s->signal();
    };
    if (current(s->watcher)) {
        fn();
    } else {
        s->watcher->push(std::move(fn));
    }
}
void releaseSecure(const std::shared_ptr<SecureState>& s) noexcept {
    // --> An unused policy has no pending I/O to post; do not retain an idle reactor in its own queue.
    if (!s->used) {
        s->closed = true;
    } else {
        closeSecure(s);
    }
}
asyncpp::task<ERetCode> joinSecure(std::shared_ptr<SecureState> s, SOperationOptions o) {
    if (!current(s->watcher)) {
        co_return ERET_WRONG_EXECUTOR;
    }
    auto c = policy(o);
    if (c != ERET_OK) {
        co_return c;
    }
    if (!s->closed) {
        co_return ERET_INVALID_STATE;
    }
    co_await s->drained;
    if (s->connection) {
        c = co_await s->connection->waitClosed(o);
        if (c != ERET_OK) {
            co_return c;
        }
    }
    if (s->tlsClient) {
        co_return code(co_await s->tlsClient->waitClosed());
    }
    if (s->quicClient) {
        co_return code(co_await s->quicClient->waitClosed());
    }
    if (s->quicServer) {
        co_return code(co_await s->quicServer->waitClosed());
    }
    co_return ERET_OK;
}
asyncpp::task<ERetCode> runSecure(std::shared_ptr<SecureState> s, SOperationOptions o) {
    auto c = s->entry(o);
    if (c != ERET_OK) {
        co_return c;
    }
    if (!s->quicClient && !s->quicServer) {
        co_return ERET_UNSUPPORTED;
    }
    if (s->running) {
        co_return ERET_BUSY;
    }
    s->used = true;
    s->running = true;
    try {
        c = s->quicClient ? code(co_await s->quicClient->run()) : code(co_await s->quicServer->run());
    } catch (...) {
        s->running = false;
        closeSecure(s);
        s->signal();
        throw;
    }
    s->running = false;
    closeSecure(s);
    s->signal();
    co_return c;
}
void checkAlpn(const std::shared_ptr<SecureState>& s, const hntls::SPeerInfo& peer) {
    const std::string protocol = s->options.version == HTTP_3   ? "h3"
                                 : s->options.version == HTTP_2 ? "h2"
                                                                : "http/1.1";
    if (peer.alpn != protocol) {
        throw std::invalid_argument("negotiated HTTP ALPN mismatch");
    }
}
std::shared_ptr<SecureState> makeSecure(std::shared_ptr<sockpp::CSocketWatcher> w,
                                        std::shared_ptr<hntls::ITlsContext> tls, SHttpConnectionOptions o,
                                        EHttpRole role, bool quic) {
    if (!w || !tls || !tls->isImplemented() ||
        static_cast<unsigned>(tls->options().role) != static_cast<unsigned>(role)) {
        throw std::invalid_argument("invalid HTTP TLS context/role/reactor");
    }
    if (quic) {
        if (o.version != HTTP_3) {
            throw std::invalid_argument("QUIC endpoint requires HTTP/3");
        }
        mux::validate(o);
    } else {
        if (o.version == HTTP_2) {
            mux::validate(o);
        } else {
            validateOptions(o);
        }
    }
    auto policy = tls->options();
    const std::string expected = quic ? "h3" : o.version == HTTP_2 ? "h2" : "http/1.1";
    if (policy.alpn != std::vector<std::string>{expected}) {
        throw std::invalid_argument("explicit HTTP version requires exactly matching ALPN policy");
    }
    if (role == HTTP_CLIENT && (!policy.requirePeerAuthentication || policy.serverName.empty())) {
        throw std::invalid_argument("secure HTTP client requires authenticated server identity");
    }
    auto s = std::make_shared<SecureState>();
    s->watcher = std::move(w);
    s->tls = std::move(tls);
    s->options = o;
    return s;
}
class CSecureClient final : public IHttpClient {
private:
    std::shared_ptr<SecureState> _s;
    static asyncpp::task<ERetCode> connect(std::shared_ptr<SecureState> s, sockpp::CSocketAddress address,
                                           SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (s->connecting) {
            co_return ERET_BUSY;
        }
        if (s->connection || s->tlsClient) {
            co_return ERET_INVALID_STATE;
        }
        if (s->quicClient && !s->running) {
            co_return ERET_NOT_RUNNING;
        }
        s->used = true;
        s->connecting = true;
        try {
            auto name = s->tls->options().serverName;
            s->authority = name.find(':') != std::string::npos ? "[" + name + "]" : name;
            if (address.port() != 443) {
                s->authority += ":" + std::to_string(address.port());
            }
            if (s->quicClient) {
                auto r = co_await s->quicClient->connect(address);
                c = code(r.result.code);
                if (r.result.transportError && *r.result.transportError >= 0x100 &&
                    *r.result.transportError < 0x200) {
                    c = ERET_AUTHENTICATION;
                }
                if (c == ERET_OK && !s->closed) {
                    checkAlpn(s, r.connection->peerInfo());
                    s->connection = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(r.connection),
                                                                HTTP_CLIENT, s->options);
                }
            } else {
                s->tlsClient = hntls::ITlsClient::create(s->watcher, s->tls);
                auto r = co_await s->tlsClient->connect(address);
                c = code(r.result.code);
                if (c == ERET_OK && !s->closed) {
                    checkAlpn(s, r.stream->peerInfo());
                    s->connection = IHttpConnection::createStream(IHttpStream::fromTls(r.stream), HTTP_CLIENT,
                                                                  s->options);
                }
            }
        } catch (const std::system_error&) {
            c = s->closed ? ERET_CLOSED : ERET_IO;
        } catch (const std::invalid_argument&) {
            c = ERET_AUTHENTICATION;
        } catch (...) {
            s->connecting = false;
            closeSecure(s);
            s->signal();
            throw;
        }
        s->connecting = false;
        if (s->closed && (c == ERET_OK || c == ERET_TRUNCATED || c == ERET_IO)) {
            c = ERET_CLOSED;
        }
        if (c != ERET_OK) {
            closeSecure(s);
        }
        s->signal();
        co_return c;
    }

public:
    explicit CSecureClient(std::shared_ptr<SecureState> s) : _s(std::move(s)) {}
    ~CSecureClient() override { releaseSecure(_s); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    asyncpp::task<ERetCode> run(SOperationOptions o) override { return runSecure(_s, o); }
    asyncpp::task<ERetCode> connect(sockpp::CSocketAddress a, SOperationOptions o) override {
        return connect(_s, std::move(a), o);
    }
    std::shared_ptr<IHttpClientContext> createRequest() override {
        require(_s->watcher);
        if (!_s->connection || _s->closed) {
            throw std::logic_error("secure HTTP client is not connected");
        }
        auto context = IHttpClientContext::create(_s->connection);
        context->getRequest()->authority(_s->authority);
        return context;
    }
    std::shared_ptr<IHttpConnection> getConnection() const override {
        require(_s->watcher);
        return _s->connection;
    }
    void close() noexcept override { closeSecure(_s); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return joinSecure(_s, o); }
};
struct Pending {
    std::shared_ptr<SecureState> state;
    std::shared_ptr<hntls::ITlsStream> handshake;
    std::shared_ptr<IHttpConnection> head;
    explicit Pending(std::shared_ptr<SecureState> s) : state(std::move(s)) { ++state->pending; }
    ~Pending() {
        std::erase_if(state->handshakes, [this](auto& w) {
            auto v = w.lock();
            return !v || v == handshake;
        });
        std::erase_if(state->heads, [this](auto& w) {
            auto v = w.lock();
            return !v || v == head;
        });
        --state->pending;
        state->signal();
    }
};
class CSecureServer final : public IHttpServer {
private:
    std::shared_ptr<SecureState> _s;
    static asyncpp::task<SConnectionResult> acceptConnection(std::shared_ptr<SecureState> s,
                                                             SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return SConnectionResult{error(c), {}};
        }
        if (s->pending >= s->options.limits.maximumPendingRequests) {
            co_return SConnectionResult{error(ERET_LIMIT), {}};
        }
        if (s->acceptingSocket) {
            co_return SConnectionResult{error(ERET_BUSY), {}};
        }
        Pending guard(s);
        s->acceptingSocket = true;
        std::shared_ptr<IHttpConnection> connection;
        try {
            if (s->quicServer) {
                auto r = co_await s->quicServer->accept();
                s->acceptingSocket = false;
                c = code(r.result.code);
                if (c == ERET_OK && !s->closed) {
                    checkAlpn(s, r.connection->peerInfo());
                    connection = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(r.connection),
                                                             HTTP_SERVER, s->options);
                }
            } else {
                if (!s->listener) {
                    s->acceptingSocket = false;
                    co_return SConnectionResult{error(ERET_INVALID_STATE), {}};
                }
                auto socket = co_await s->listener->accept();
                s->acceptingSocket = false;
                if (s->closed) {
                    co_return SConnectionResult{error(ERET_CLOSED), {}};
                }
                guard.handshake = hntls::ITlsStream::create(
                    hntls::CSocketByteStream::create(s->watcher, std::move(socket)), s->tls);
                s->handshakes.push_back(guard.handshake);
                c = code(co_await guard.handshake->handshake());
                if (c == ERET_OK && !s->closed) {
                    checkAlpn(s, guard.handshake->peerInfo());
                    connection = IHttpConnection::createStream(IHttpStream::fromTls(guard.handshake),
                                                               HTTP_SERVER, s->options);
                }
            }
        } catch (const std::system_error&) {
            c = s->closed ? ERET_CLOSED : ERET_IO;
        } catch (const std::invalid_argument&) {
            c = ERET_AUTHENTICATION;
        } catch (...) {
            s->acceptingSocket = false;
            if (guard.handshake) {
                guard.handshake->close();
            }
            throw;
        }
        s->acceptingSocket = false;
        if (s->closed) {
            c = ERET_CLOSED;
        }
        if (c != ERET_OK) {
            if (guard.handshake) {
                guard.handshake->close();
                co_await guard.handshake->waitClosed();
            }
            if (connection) {
                connection->close();
                co_await connection->waitClosed();
            }
            co_return SConnectionResult{error(c), {}};
        }
        co_return SConnectionResult{error(ERET_OK), std::move(connection)};
    }
    static asyncpp::task<SAcceptResult> accept(std::shared_ptr<SecureState> s, SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return SAcceptResult{error(c), {}};
        }
        if (s->options.version == HTTP_2 || s->options.version == HTTP_3) {
            co_return SAcceptResult{error(ERET_UNSUPPORTED), {}};
        }
        auto r = co_await acceptConnection(s, o);
        if (r.result.code != ERET_OK) {
            co_return SAcceptResult{r.result, {}};
        }
        Pending guard(s);
        guard.head = r.connection;
        s->heads.push_back(guard.head);
        auto context = IHttpServerContext::create(std::dynamic_pointer_cast<IHttp1Connection>(r.connection));
        c = co_await context->getRequest()->parseOnce(o);
        if (c != ERET_OK) {
            r.connection->close();
            co_await r.connection->waitClosed();
            co_return SAcceptResult{error(c), {}};
        }
        co_return SAcceptResult{error(ERET_OK), std::move(context)};
    }

public:
    explicit CSecureServer(std::shared_ptr<SecureState> s) : _s(std::move(s)) {}
    ~CSecureServer() override { releaseSecure(_s); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    asyncpp::task<ERetCode> run(SOperationOptions o) override { return runSecure(_s, o); }
    ERetCode listen(sockpp::CSocketAddress a, int backlog) override {
        auto c = _s->entry({});
        if (c != ERET_OK) {
            return c;
        }
        if (backlog <= 0) {
            return ERET_INVALID_ARGUMENT;
        }
        if (_s->listener) {
            return ERET_INVALID_STATE;
        }
        _s->used = true;
        if (_s->quicServer) {
            return code(_s->quicServer->listen(std::move(a)));
        }
        try {
            auto socket = sockpp::CSocketAsync::open(*_s->watcher, a.family(), sockpp::ESOCK_STREAM,
                                                     sockpp::EPROTO_TCP);
            socket.bind(a);
            socket.listen(backlog);
            _s->listener = std::make_unique<sockpp::CSocketAsync>(std::move(socket));
        } catch (const std::system_error&) {
            return ERET_IO;
        }
        return ERET_OK;
    }
    sockpp::CSocketAddress localAddress() const override {
        require(_s->watcher);
        if (_s->quicServer) {
            return _s->quicServer->localAddress();
        }
        if (!_s->listener) {
            throw std::logic_error("secure HTTP listener not bound");
        }
        return _s->listener->localAddress();
    }
    asyncpp::task<SAcceptResult> accept(SOperationOptions o) override { return accept(_s, o); }
    asyncpp::task<SConnectionResult> acceptConnection(SOperationOptions o) override {
        return acceptConnection(_s, o);
    }
    void close() noexcept override { closeSecure(_s); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return joinSecure(_s, o); }
};
} // namespace
} // namespace detail
std::shared_ptr<IHttpClient> IHttpClient::createTls(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                    std::shared_ptr<hntls::ITlsContext> tls,
                                                    SHttpConnectionOptions o) {
    return std::make_shared<detail::CSecureClient>(
        detail::makeSecure(std::move(w), std::move(tls), o, HTTP_CLIENT, false));
}
std::shared_ptr<IHttpClient> IHttpClient::createQuic(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                     std::shared_ptr<hntls::ITlsContext> tls,
                                                     SHttpConnectionOptions o, hnquic::SQuicOptions q) {
    auto s = detail::makeSecure(w, tls, o, HTTP_CLIENT, true);
    if (q.maximumUnidirectionalStreams < 3 || !q.maximumBidirectionalStreams || !q.maximumStreamData ||
        !q.maximumConnectionData) {
        throw std::invalid_argument("HTTP/3 requires critical-stream and request credit");
    }
    if (q.role != hnquic::EROLE_CLIENT) {
        throw std::invalid_argument("HTTP/3 client requires QUIC client role");
    }
    s->quicClient = hnquic::IQuicClient::create(std::move(w), std::move(tls), q);
    return std::make_shared<detail::CSecureClient>(std::move(s));
}
std::shared_ptr<IHttpServer> IHttpServer::createTls(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                    std::shared_ptr<hntls::ITlsContext> tls,
                                                    SHttpConnectionOptions o) {
    return std::make_shared<detail::CSecureServer>(
        detail::makeSecure(std::move(w), std::move(tls), o, HTTP_SERVER, false));
}
std::shared_ptr<IHttpServer> IHttpServer::createQuic(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                     std::shared_ptr<hntls::ITlsContext> tls,
                                                     SHttpConnectionOptions o, hnquic::SQuicOptions q) {
    auto s = detail::makeSecure(w, tls, o, HTTP_SERVER, true);
    if (q.maximumUnidirectionalStreams < 3 || !q.maximumBidirectionalStreams || !q.maximumStreamData ||
        !q.maximumConnectionData) {
        throw std::invalid_argument("HTTP/3 requires critical-stream and request credit");
    }
    if (q.role != hnquic::EROLE_SERVER) {
        throw std::invalid_argument("HTTP/3 server requires QUIC server role");
    }
    s->quicServer = hnquic::IQuicServer::create(std::move(w), std::move(tls), q);
    return std::make_shared<detail::CSecureServer>(std::move(s));
}
} // namespace hnhttp
