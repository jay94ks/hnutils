#include <hnquic/client.hpp>
#include <hnquic/server.hpp>
#include "engine.hpp"
#include <socketpp/udp_socket_async.hpp>
#include <asyncpp/launch.h>
#include <asyncpp/scope_guard.h>
#include <stdexcept>

namespace hnquic {
namespace {
using namespace detail;
SQuicError error(ERetCode code, const std::string &message = "") { return {code, {}, {}, {}, message}; }
ERetCode policy(const std::shared_ptr<sockpp::CSocketWatcher> &w, const SOperationOptions &o) {
    if (asyncpp::dispatcher::current() != w.get()) {
        return ERET_WRONG_EXECUTOR;
    }
    if (o.cancellation.stop_requested()) {
        return ERET_CANCELLED;
    }
    if (o.deadline) {
        return *o.deadline <= std::chrono::steady_clock::now() ? ERET_TIMEOUT : ERET_UNSUPPORTED;
    }
    return ERET_OK;
}
void validate(const std::shared_ptr<sockpp::CSocketWatcher> &w,
              const std::shared_ptr<hntls::ITlsContext> &tls, const SQuicOptions &o) {
    constexpr auto maximum = (UINT64_C(1) << 62) - 1;
    if (!w || !tls || !tls->isImplemented() || o.version != 1 || o.maximumBufferedBytes < 1200 ||
        !o.maximumReassemblyRanges || !o.maximumAckRanges || !o.maximumPendingDatagrams ||
        !o.maximumOperationsPerTurn || !o.maximumConnections || o.maximumAckRanges > 4096 ||
        o.maximumConnectionData > maximum || o.maximumStreamData > maximum ||
        o.maximumBidirectionalStreams > (UINT64_C(1) << 60) ||
        o.maximumUnidirectionalStreams > (UINT64_C(1) << 60) || o.idleTimeout.count() < 0 ||
        (o.role != EROLE_CLIENT && o.role != EROLE_SERVER) ||
        static_cast<unsigned>(o.role) != static_cast<unsigned>(tls->options().role) ||
        tls->options().alpn.empty()) {
        throw std::invalid_argument("invalid QUIC reactor/TLS role/ALPN/resource policy");
    }
}
struct ConnectionState {
    std::shared_ptr<sockpp::CSocketWatcher> watcher;
    SQuicOptions options;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<sockpp::CUdpSocketAsync> socket;
    sockpp::CSocketAddress peer;
    std::atomic<bool> closed{false};
    bool started = false, running = false, runDone = false, receiver = false, serverOwned = false,
         accepting = false;
    std::size_t active = 0;
    asyncpp::multi_consumer_event drained;
    void signal() {
        if (closed && !active && (!started || runDone)) {
            drained.set();
        }
    }
    ERetCode entry(const SOperationOptions &o) {
        auto c = policy(watcher, o);
        return c != ERET_OK                                   ? c
               : (closed || engine->state() == ESTATE_CLOSED) ? ERET_CLOSED
               : engine->state() == ESTATE_FAILED             ? engine->error().code
                                                              : ERET_OK;
    }
};
struct Active {
    std::shared_ptr<ConnectionState> state;
    explicit Active(std::shared_ptr<ConnectionState> s) : state(std::move(s)) { ++state->active; }
    ~Active() {
        --state->active;
        state->signal();
    }
};
void closeConnection(const std::shared_ptr<ConnectionState> &s, std::uint64_t code = 0) noexcept {
    if (s->closed.exchange(true)) {
        return;
    }
    auto fn = [s, code] {
        try {
            s->engine->close(code);
        } catch (const SQuicError &e) {
            s->engine->fail(e);
        }
        if (!s->running && s->socket) {
            s->socket->close();
        }
        s->engine->changed.set();
        s->signal();
    };
    if (asyncpp::dispatcher::current() == s->watcher.get()) {
        fn();
    } else {
        s->watcher->push(std::move(fn));
    }
}
class CQuicStream final : public IQuicStream {
  private:
    std::shared_ptr<ConnectionState> _s;
    std::uint64_t _id;
    static asyncpp::task<SReadResult> read(std::shared_ptr<ConnectionState> s, std::uint64_t id,
                                           std::size_t n, SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return SReadResult{error(c), {}};
        }
        if (!n || n > s->options.maximumBufferedBytes) {
            co_return SReadResult{error(ERET_INVALID_ARGUMENT), {}};
        }
        auto &stream = s->engine->at(id);
        if ((id & 2) && (id & 1) == static_cast<unsigned>(s->options.role)) {
            co_return SReadResult{error(ERET_INVALID_STATE), {}};
        }
        if (stream.readBusy) {
            co_return SReadResult{error(ERET_BUSY), {}};
        }
        Active pending(s);
        stream.readBusy = true;
        asyncpp::scope_guard reset([&stream]() noexcept { stream.readBusy = false; });
        std::stop_callback cancel(o.cancellation, [s, id] {
            s->watcher->push([s, id] {
                try {
                    s->engine->abort(id, 0, true);
                } catch (const SQuicError &e) {
                    s->engine->fail(e);
                }
            });
        });
        for (;;) {
            if (o.cancellation.stop_requested()) {
                co_return SReadResult{error(ERET_CANCELLED), {}};
            }
            c = s->entry({});
            if (c != ERET_OK) {
                co_return SReadResult{error(c), {}};
            }
            if (stream.readReset) {
                co_return SReadResult{error(ERET_STREAM_RESET), {}};
            }
            try {
                auto bytes = s->engine->read(id, n);
                if (!bytes.empty()) {
                    co_return SReadResult{error(ERET_OK), std::move(bytes)};
                }
            } catch (const SQuicError &e) {
                s->engine->fail(e);
                co_return SReadResult{e, {}};
            }
            if (stream.finalSize && stream.receive.offset() == *stream.finalSize) {
                co_return SReadResult{error(ERET_EOF), {}};
            }
            s->engine->changed.reset();
            co_await s->engine->changed;
        }
    }
    static asyncpp::task<SWriteResult> write(std::shared_ptr<ConnectionState> s, std::uint64_t id,
                                             SBytes bytes, SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return SWriteResult{error(c), 0};
        }
        if (bytes.empty() || bytes.size() > s->options.maximumBufferedBytes) {
            co_return SWriteResult{error(ERET_INVALID_ARGUMENT), 0};
        }
        auto &stream = s->engine->at(id);
        if ((id & 2) && (id & 1) != static_cast<unsigned>(s->options.role)) {
            co_return SWriteResult{error(ERET_INVALID_STATE), 0};
        }
        if (stream.writeBusy) {
            co_return SWriteResult{error(ERET_BUSY), 0};
        }
        Active pending(s);
        stream.writeBusy = true;
        asyncpp::scope_guard reset([&stream]() noexcept { stream.writeBusy = false; });
        std::stop_callback cancel(o.cancellation,
                                  [s] { s->watcher->push([s] { s->engine->changed.set(); }); });
        for (;;) {
            if (o.cancellation.stop_requested()) {
                co_return SWriteResult{error(ERET_CANCELLED), 0};
            }
            c = s->entry({});
            if (c != ERET_OK) {
                co_return SWriteResult{error(c), 0};
            }
            if (stream.writeReset) {
                co_return SWriteResult{error(ERET_STREAM_RESET), 0};
            }
            if (stream.sendFin) {
                co_return SWriteResult{error(ERET_INVALID_STATE), 0};
            }
            try {
                auto n = s->engine->write(id, bytes);
                if (n) {
                    co_return SWriteResult{error(ERET_OK), n};
                }
            } catch (const SQuicError &e) {
                s->engine->fail(e);
                co_return SWriteResult{e, 0};
            }
            s->engine->changed.reset();
            co_await s->engine->changed;
        }
    }
    static asyncpp::task<ERetCode> finish(std::shared_ptr<ConnectionState> s, std::uint64_t id,
                                          SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return c;
        }
        auto &stream = s->engine->at(id);
        if (stream.writeBusy) {
            co_return ERET_BUSY;
        }
        if (stream.sendFin) {
            co_return ERET_OK;
        }
        try {
            s->engine->write(id, {}, true);
        } catch (const SQuicError &e) {
            co_return e.code;
        }
        co_return ERET_OK;
    }

  public:
    CQuicStream(std::shared_ptr<ConnectionState> s, std::uint64_t id) : _s(std::move(s)), _id(id) {}
    std::uint64_t id() const noexcept override { return _id; }
    EStreamKind kind() const noexcept override {
        return _id & 2 ? ESTREAM_UNIDIRECTIONAL : ESTREAM_BIDIRECTIONAL;
    }
    ERole initiator() const noexcept override { return _id & 1 ? EROLE_SERVER : EROLE_CLIENT; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override {
        return read(_s, _id, n, o);
    }
    asyncpp::task<SWriteResult> writeSome(SBytes b, SOperationOptions o) override {
        return write(_s, _id, std::move(b), o);
    }
    asyncpp::task<ERetCode> finishWrite(SOperationOptions o) override { return finish(_s, _id, o); }
    void abortRead(std::uint64_t code) noexcept override {
        auto s = _s;
        auto id = _id;
        s->watcher->push([s, id, code] {
            try {
                if (!s->closed) {
                    s->engine->abort(id, code, true);
                }
            } catch (const SQuicError &e) {
                s->engine->fail(e);
            }
        });
    }
    void abortWrite(std::uint64_t code) noexcept override {
        auto s = _s;
        auto id = _id;
        s->watcher->push([s, id, code] {
            try {
                if (!s->closed) {
                    s->engine->abort(id, code, false);
                }
            } catch (const SQuicError &e) {
                s->engine->fail(e);
            }
        });
    }
};
asyncpp::task<void> receiveClient(std::shared_ptr<ConnectionState> s) {
    try {
        while (!s->closed) {
            SBytes data(65536);
            auto n = co_await s->socket->receive(std::as_writable_bytes(std::span(data)));
            data.resize(n);
            if (n) {
                co_await s->engine->receive(std::move(data), std::chrono::steady_clock::now());
            }
        }
    } catch (const SQuicError &e) {
        s->engine->fail(e);
        closeConnection(s);
    } catch (const std::system_error &e) {
        if (!s->closed) {
            s->engine->fail(error(ERET_IO, e.what()));
            closeConnection(s);
        }
    } catch (...) {
        s->engine->fail(error(ERET_IO, "QUIC receiver exception"));
        closeConnection(s);
    }
    co_return;
}
class CQuicConnection final : public IQuicConnection {
  private:
    std::shared_ptr<ConnectionState> _s;
    static asyncpp::task<ERetCode> connect(std::shared_ptr<ConnectionState> s, sockpp::CSocketAddress peer,
                                           SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (s->serverOwned || s->engine->state() != ESTATE_IDLE) {
            co_return ERET_INVALID_STATE;
        }
        if (!s->running) {
            co_return ERET_INVALID_STATE;
        }
        Active pending(s);
        std::stop_callback cancel(o.cancellation, [s] { closeConnection(s); });
        try {
            s->peer = peer;
            s->socket = std::make_unique<sockpp::CUdpSocketAsync>(
                sockpp::CUdpSocketAsync::open(*s->watcher, peer.family()));
            co_await s->socket->connect(std::move(peer));
            co_await s->engine->start(std::chrono::steady_clock::now());
            s->engine->changed.set();
        } catch (const SQuicError &e) {
            s->engine->fail(e);
            closeConnection(s);
            co_return e.code;
        } catch (const std::system_error &e) {
            s->engine->fail(error(ERET_IO, e.what()));
            closeConnection(s);
            co_return ERET_IO;
        }
        while (!s->closed && !s->engine->confirmed() && s->engine->state() != ESTATE_FAILED) {
            s->engine->changed.reset();
            co_await s->engine->changed;
        }
        if (o.cancellation.stop_requested()) {
            co_return ERET_CANCELLED;
        }
        co_return s->engine->confirmed() ? ERET_OK : s->engine->error().code;
    }
    static asyncpp::task<ERetCode> run(std::shared_ptr<ConnectionState> s, SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (s->started) {
            co_return ERET_BUSY;
        }
        s->started = true;
        s->running = true;
        std::stop_callback cancel(o.cancellation, [s] { closeConnection(s); });
        asyncpp::async_launch_scope tasks;
        std::uint64_t timer = 0;
        try {
            while (!s->closed && s->engine->state() != ESTATE_FAILED) {
                if (s->socket && !s->receiver) {
                    s->receiver = true;
                    tasks.launch(receiveClient(s));
                }
                if (!s->serverOwned && s->engine->state() != ESTATE_IDLE) {
                    s->engine->tick(std::chrono::steady_clock::now());
                    auto out = s->engine->datagrams(std::chrono::steady_clock::now());
                    for (auto &d : out) {
                        auto n = co_await s->socket->send(std::as_bytes(std::span(d)));
                        need(n == d.size(), "partial UDP datagram", 1);
                    }
                }
                if (s->engine->state() == ESTATE_FAILED || s->engine->state() == ESTATE_CLOSED) {
                    break;
                }
                s->engine->changed.reset();
                std::weak_ptr<ConnectionState> weak = s;
                timer = s->watcher->schedule(std::chrono::steady_clock::now() + std::chrono::milliseconds{10},
                                             [weak] {
                                                 if (auto state = weak.lock()) {
                                                     state->engine->changed.set();
                                                 }
                                             });
                co_await s->engine->changed;
                s->watcher->cancelTimer(timer);
                timer = 0;
            }
        } catch (const SQuicError &e) {
            s->engine->fail(e);
        } catch (const std::system_error &e) {
            s->engine->fail(error(ERET_IO, e.what()));
        } catch (...) {
            s->engine->fail(error(ERET_IO, "QUIC driver exception"));
        }
        if (timer) {
            s->watcher->cancelTimer(timer);
        }
        closeConnection(s);
        if (s->socket) {
            try {
                for (auto &d : s->engine->datagrams(std::chrono::steady_clock::now())) {
                    co_await s->socket->send(std::as_bytes(std::span(d)));
                }
            } catch (...) { /* Abort still joins the receive operation. */
            }
            s->socket->close();
        }
        co_await tasks.join();
        s->running = false;
        s->runDone = true;
        s->signal();
        co_return o.cancellation.stop_requested()
            ? ERET_CANCELLED
            : (s->engine->state() == ESTATE_FAILED ? s->engine->error().code : ERET_OK);
    }
    static asyncpp::task<SStreamResult> streams(std::shared_ptr<ConnectionState> s, bool open,
                                                EStreamKind kind, SOperationOptions o) {
        auto c = s->entry(o);
        if (c != ERET_OK) {
            co_return SStreamResult{error(c), {}};
        }
        if (!s->engine->confirmed()) {
            co_return SStreamResult{error(ERET_INVALID_STATE), {}};
        }
        if (kind != ESTREAM_BIDIRECTIONAL && kind != ESTREAM_UNIDIRECTIONAL) {
            co_return SStreamResult{error(ERET_INVALID_ARGUMENT), {}};
        }
        if (open) {
            auto id = s->engine->open(kind);
            if (!id) {
                co_return SStreamResult{error(ERET_LIMIT), {}};
            }
            co_return SStreamResult{error(ERET_OK), std::make_shared<CQuicStream>(s, *id)};
        }
        if (s->accepting) {
            co_return SStreamResult{error(ERET_BUSY), {}};
        }
        s->accepting = true;
        asyncpp::scope_guard reset([s]() noexcept { s->accepting = false; });
        Active pending(s);
        std::stop_callback cancel(o.cancellation,
                                  [s] { s->watcher->push([s] { s->engine->changed.set(); }); });
        for (;;) {
            if (o.cancellation.stop_requested()) {
                co_return SStreamResult{error(ERET_CANCELLED), {}};
            }
            c = s->entry({});
            if (c != ERET_OK) {
                co_return SStreamResult{error(c), {}};
            }
            auto id = s->engine->accept();
            if (id) {
                co_return SStreamResult{error(ERET_OK), std::make_shared<CQuicStream>(s, *id)};
            }
            s->engine->changed.reset();
            co_await s->engine->changed;
        }
    }
    static asyncpp::task<ERetCode> join(std::shared_ptr<ConnectionState> s, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (!s->closed) {
            co_return ERET_INVALID_STATE;
        }
        co_await s->drained;
        co_return ERET_OK;
    }

  public:
    explicit CQuicConnection(std::shared_ptr<ConnectionState> s) : _s(std::move(s)) {}
    bool isImplemented() const noexcept override { return true; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    SQuicOptions options() const override { return _s->options; }
    EConnectionState state() const noexcept override {
        return _s->closed ? ESTATE_CLOSED : _s->engine->state();
    }
    bool handshakeConfirmed() const noexcept override { return _s->engine->confirmed(); }
    hntls::SPeerInfo peerInfo() const override { return _s->engine->peerInfo(); }
    asyncpp::task<ERetCode> connect(sockpp::CSocketAddress p, SOperationOptions o) override {
        return connect(_s, std::move(p), o);
    }
    asyncpp::task<ERetCode> run(SOperationOptions o) override { return run(_s, o); }
    asyncpp::task<SStreamResult> openStream(EStreamKind k, SOperationOptions o) override {
        return streams(_s, true, k, o);
    }
    asyncpp::task<SStreamResult> acceptStream(SOperationOptions o) override {
        return streams(_s, false, ESTREAM_BIDIRECTIONAL, o);
    }
    SQuicError lastError() const override { return _s->engine->error(); }
    void close(std::uint64_t code) noexcept override { closeConnection(_s, code); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return join(_s, o); }
};
std::shared_ptr<ConnectionState> makeState(std::shared_ptr<sockpp::CSocketWatcher> w,
                                           std::shared_ptr<hntls::ITlsContext> tls, SQuicOptions o) {
    validate(w, tls, o);
    auto s = std::make_shared<ConnectionState>();
    s->watcher = std::move(w);
    s->options = o;
    s->engine = std::make_unique<Engine>(s->watcher, std::move(tls), o);
    return s;
}
class CQuicClient final : public IQuicClient {
  private:
    std::shared_ptr<IQuicConnection> _connection;
    static asyncpp::task<SConnectResult> connect(std::shared_ptr<IQuicConnection> c, sockpp::CSocketAddress p,
                                                 SOperationOptions o) {
        auto ret = co_await c->connect(std::move(p), o);
        auto diagnostic = ret == ERET_OK ? error(ERET_OK) : c->lastError();
        diagnostic.code = ret;
        co_return SConnectResult{std::move(diagnostic), ret == ERET_OK ? c : nullptr};
    }

  public:
    explicit CQuicClient(std::shared_ptr<IQuicConnection> c) : _connection(std::move(c)) {}
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override {
        return _connection->executor();
    }
    asyncpp::task<ERetCode> run(SOperationOptions o) override { return _connection->run(o); }
    asyncpp::task<SConnectResult> connect(sockpp::CSocketAddress p, SOperationOptions o) override {
        return connect(_connection, std::move(p), o);
    }
    void close(std::uint64_t c) noexcept override { _connection->close(c); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return _connection->waitClosed(o); }
};
struct ServerState {
    std::shared_ptr<sockpp::CSocketWatcher> watcher;
    std::shared_ptr<hntls::ITlsContext> tls;
    SQuicOptions options;
    std::unique_ptr<sockpp::CUdpSocketAsync> socket;
    std::vector<std::shared_ptr<ConnectionState>> connections;
    std::set<ConnectionState *> admitted;
    std::deque<std::shared_ptr<IQuicConnection>> ready;
    std::atomic<bool> closed{false};
    bool started = false, running = false, runDone = false, receiving = false, accepting = false;
    SQuicError error{ERET_OK, {}, {}, {}, {}};
    asyncpp::multi_consumer_event changed, drained;
};
void closeServer(const std::shared_ptr<ServerState> &s) noexcept {
    if (s->closed.exchange(true)) {
        return;
    }
    auto fn = [s] {
        if (s->socket) {
            s->socket->close();
        }
        for (auto &c : s->connections) {
            closeConnection(c);
        }
        s->changed.set();
        if (!s->started) {
            s->drained.set();
        }
    };
    if (asyncpp::dispatcher::current() == s->watcher.get()) {
        fn();
    } else {
        s->watcher->push(std::move(fn));
    }
}
asyncpp::task<void> receiveServer(std::shared_ptr<ServerState> s) {
    try {
        while (!s->closed) {
            SBytes data(65536);
            sockpp::CSocketAddress peer;
            auto n = co_await s->socket->receiveFrom(std::as_writable_bytes(std::span(data)), peer);
            data.resize(n);
            if (!n) {
                continue;
            }
            PacketHeader h;
            try {
                h = parseHeader(data);
            } catch (const SQuicError &) {
                continue;
            }
            std::shared_ptr<ConnectionState> target;
            for (auto &c : s->connections) {
                if (c->peer.toString() == peer.toString() &&
                    (c->engine->localCid() == h.destination || c->engine->originalCid() == h.destination)) {
                    target = c;
                    break;
                }
            }
            if (!target) {
                if (h.level != 0 || n < 1200 || s->connections.size() >= s->options.maximumConnections) {
                    continue;
                }
                target = makeState(s->watcher, s->tls, s->options);
                target->serverOwned = true;
                target->peer = peer;
                co_await target->engine->start(std::chrono::steady_clock::now());
                s->connections.push_back(target);
            }
            try {
                co_await target->engine->receive(std::move(data), std::chrono::steady_clock::now());
            } catch (const SQuicError &e) {
                target->engine->fail(e);
                closeConnection(target);
            }
            s->changed.set();
        }
    } catch (const std::system_error &e) {
        if (!s->closed) {
            s->error = error(ERET_IO, e.what());
            closeServer(s);
        }
    } catch (...) {
        s->error = error(ERET_IO, "server receive exception");
        closeServer(s);
    }
    co_return;
}
class CQuicServer final : public IQuicServer {
  private:
    std::shared_ptr<ServerState> _s;
    static asyncpp::task<ERetCode> run(std::shared_ptr<ServerState> s, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (s->closed || !s->socket) {
            co_return ERET_INVALID_STATE;
        }
        if (s->started) {
            co_return ERET_BUSY;
        }
        s->started = s->running = true;
        std::stop_callback cancel(o.cancellation, [s] { closeServer(s); });
        asyncpp::async_launch_scope tasks;
        tasks.launch(receiveServer(s));
        std::uint64_t timer = 0;
        try {
            while (!s->closed) {
                auto snapshot = s->connections;
                for (auto &connection : snapshot) {
                    try {
                        connection->engine->tick(std::chrono::steady_clock::now());
                        auto out = connection->engine->datagrams(std::chrono::steady_clock::now());
                        for (auto &d : out) {
                            auto n =
                                co_await s->socket->sendTo(std::as_bytes(std::span(d)), connection->peer);
                            need(n == d.size(), "partial server UDP datagram", 1);
                        }
                        if (connection->engine->confirmed() && !s->admitted.contains(connection.get())) {
                            s->admitted.insert(connection.get());
                            s->ready.push_back(std::make_shared<CQuicConnection>(connection));
                            s->changed.set();
                        }
                    } catch (const SQuicError &e) {
                        connection->engine->fail(e);
                        closeConnection(connection);
                    }
                }
                for (auto i = s->connections.begin(); i != s->connections.end();) {
                    if ((*i)->closed || (*i)->engine->state() == ESTATE_FAILED ||
                        (*i)->engine->state() == ESTATE_CLOSED) {
                        s->admitted.erase(i->get());
                        closeConnection(*i);
                        i = s->connections.erase(i);
                    } else {
                        ++i;
                    }
                }
                std::erase_if(s->ready, [](const auto &connection) {
                    return connection->state() != ESTATE_ESTABLISHED;
                });
                s->changed.reset();
                std::weak_ptr<ServerState> weak = s;
                timer = s->watcher->schedule(std::chrono::steady_clock::now() + std::chrono::milliseconds{10},
                                             [weak] {
                                                 if (auto current = weak.lock()) {
                                                     current->changed.set();
                                                 }
                                             });
                co_await s->changed;
                s->watcher->cancelTimer(timer);
                timer = 0;
            }
        } catch (const std::system_error &e) {
            s->error = error(ERET_IO, e.what());
            closeServer(s);
        } catch (...) {
            s->error = error(ERET_IO, "server driver exception");
            closeServer(s);
        }
        if (timer) {
            s->watcher->cancelTimer(timer);
        }
        closeServer(s);
        co_await tasks.join();
        s->running = false;
        s->runDone = true;
        s->changed.set();
        s->drained.set();
        co_return o.cancellation.stop_requested() ? ERET_CANCELLED : s->error.code;
    }
    static asyncpp::task<SAcceptResult> accept(std::shared_ptr<ServerState> s, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return SAcceptResult{error(c), {}};
        }
        if (!s->running) {
            co_return SAcceptResult{error(ERET_INVALID_STATE), {}};
        }
        if (s->accepting) {
            co_return SAcceptResult{error(ERET_BUSY), {}};
        }
        s->accepting = true;
        asyncpp::scope_guard reset([s]() noexcept { s->accepting = false; });
        std::stop_callback cancel(o.cancellation, [s] { s->watcher->push([s] { s->changed.set(); }); });
        for (;;) {
            if (o.cancellation.stop_requested()) {
                co_return SAcceptResult{error(ERET_CANCELLED), {}};
            }
            if (s->closed) {
                co_return SAcceptResult{error(ERET_CLOSED), {}};
            }
            if (!s->ready.empty()) {
                auto connection = std::move(s->ready.front());
                s->ready.pop_front();
                if (connection->state() != ESTATE_ESTABLISHED) {
                    continue;
                }
                co_return SAcceptResult{error(ERET_OK), std::move(connection)};
            }
            s->changed.reset();
            co_await s->changed;
        }
    }
    static asyncpp::task<ERetCode> join(std::shared_ptr<ServerState> s, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (!s->closed) {
            co_return ERET_INVALID_STATE;
        }
        co_await s->drained;
        co_return ERET_OK;
    }

  public:
    explicit CQuicServer(std::shared_ptr<ServerState> s) : _s(std::move(s)) {}
    bool isImplemented() const noexcept override { return true; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->watcher; }
    SQuicOptions options() const override { return _s->options; }
    SQuicError lastError() const override { return _s->error; }
    ERetCode listen(sockpp::CSocketAddress a) override {
        auto c = policy(_s->watcher, {});
        if (c != ERET_OK) {
            return c;
        }
        if (_s->socket || _s->closed) {
            return ERET_INVALID_STATE;
        }
        try {
            _s->socket = std::make_unique<sockpp::CUdpSocketAsync>(
                sockpp::CUdpSocketAsync::open(*_s->watcher, a.family()));
            _s->socket->bind(a);
        } catch (const std::system_error &e) {
            _s->error = error(ERET_IO, e.what());
            return ERET_IO;
        }
        return ERET_OK;
    }
    sockpp::CSocketAddress localAddress() const override {
        if (asyncpp::dispatcher::current() != _s->watcher.get() || !_s->socket) {
            throw std::logic_error("localAddress requires bound server on owner executor");
        }
        return _s->socket->localAddress();
    }
    asyncpp::task<ERetCode> run(SOperationOptions o) override { return run(_s, o); }
    asyncpp::task<SAcceptResult> accept(SOperationOptions o) override { return accept(_s, o); }
    void close() noexcept override { closeServer(_s); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return join(_s, o); }
};
} // namespace
std::shared_ptr<IQuicConnection> IQuicConnection::create(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                         std::shared_ptr<hntls::ITlsContext> tls,
                                                         SQuicOptions o) {
    if (o.role != EROLE_CLIENT) {
        throw std::invalid_argument("server connections are accepted by IQuicServer");
    }
    return std::make_shared<CQuicConnection>(makeState(std::move(w), std::move(tls), o));
}
std::shared_ptr<IQuicClient> IQuicClient::create(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                 std::shared_ptr<hntls::ITlsContext> tls, SQuicOptions o) {
    if (o.role != EROLE_CLIENT) {
        throw std::invalid_argument("QUIC client requires client role");
    }
    return std::make_shared<CQuicClient>(IQuicConnection::create(std::move(w), std::move(tls), o));
}
std::shared_ptr<IQuicServer> IQuicServer::create(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                 std::shared_ptr<hntls::ITlsContext> tls, SQuicOptions o) {
    if (o.role != EROLE_SERVER) {
        throw std::invalid_argument("QUIC server requires server role");
    }
    validate(w, tls, o);
    auto s = std::make_shared<ServerState>();
    s->watcher = std::move(w);
    s->tls = std::move(tls);
    s->options = o;
    return std::make_shared<CQuicServer>(std::move(s));
}
} // namespace hnquic
