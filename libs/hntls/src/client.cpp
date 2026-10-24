#include <hntls/client.hpp>
#include <hntls/socket_stream.hpp>
#include <asyncpp/event.h>
#include <atomic>
#include <span>
#include <stdexcept>

namespace hntls {
namespace {
STlsError error(ERetCode c, const std::string &text = "") { return {c, {}, text}; }
ERetCode policy(const std::shared_ptr<asyncpp::dispatcher> &e, SOperationOptions o) {
    if (asyncpp::dispatcher::current() != e.get()) {
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
} // namespace
struct CSocketByteStream::Impl {
    std::shared_ptr<sockpp::CSocketWatcher> watcher;
    sockpp::CSocketAsync socket;
    std::atomic<bool> closed{false};
    bool processed = false, reading = false, writing = false;
    asyncpp::multi_consumer_event drained;
    Impl(std::shared_ptr<sockpp::CSocketWatcher> w, sockpp::CSocketAsync s)
        : watcher(std::move(w)), socket(std::move(s)) {}
    void signal() {
        if (processed && !reading && !writing) {
            drained.set();
        }
    }
    void close() {
        closed = true;
        if (!processed) {
            socket.close();
            processed = true;
            signal();
        }
    }
    static asyncpp::task<SBytesResult> read(std::shared_ptr<Impl> s, std::size_t n, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return SBytesResult{error(c), {}};
        }
        if (s->closed) {
            co_return SBytesResult{error(ERET_CLOSED), {}};
        }
        if (!n || n > 16 * 1024 * 1024) {
            co_return SBytesResult{error(ERET_INVALID_ARGUMENT), {}};
        }
        if (s->reading) {
            co_return SBytesResult{error(ERET_BUSY), {}};
        }
        SBytesResult r;
        r.bytes.resize(n);
        s->reading = true;
        std::stop_callback cancel(o.cancellation, [s] { s->watcher->push([s] { s->close(); }); });
        try {
            n = co_await s->socket.receive(std::as_writable_bytes(std::span(r.bytes)));
            r.bytes.resize(n);
            r.result = error(n ? ERET_OK : ERET_EOF);
        } catch (const std::system_error &e) {
            r = {error(o.cancellation.stop_requested() ? ERET_CANCELLED : (s->closed ? ERET_CLOSED : ERET_IO),
                       e.what()),
                 {}};
        }
        s->reading = false;
        s->signal();
        co_return r;
    }
    static asyncpp::task<SWriteResult> write(std::shared_ptr<Impl> s, SBytes data, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return SWriteResult{error(c), 0};
        }
        if (s->closed) {
            co_return SWriteResult{error(ERET_CLOSED), 0};
        }
        if (data.empty()) {
            co_return SWriteResult{error(ERET_INVALID_ARGUMENT), 0};
        }
        if (s->writing) {
            co_return SWriteResult{error(ERET_BUSY), 0};
        }
        SWriteResult r;
        s->writing = true;
        std::stop_callback cancel(o.cancellation, [s] { s->watcher->push([s] { s->close(); }); });
        try {
            r.acceptedBytes = co_await s->socket.send(std::as_bytes(std::span(data)));
            r.result = error(r.acceptedBytes ? ERET_OK : ERET_IO);
        } catch (const std::system_error &e) {
            r.result =
                error(o.cancellation.stop_requested() ? ERET_CANCELLED : (s->closed ? ERET_CLOSED : ERET_IO),
                      e.what());
        }
        s->writing = false;
        s->signal();
        co_return r;
    }
    static asyncpp::task<ERetCode> finish(std::shared_ptr<Impl> s, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (s->closed) {
            co_return ERET_CLOSED;
        }
        if (s->writing) {
            co_return ERET_BUSY;
        }
        try {
            s->socket.shutdown(sockpp::ESHUT_WRITE);
        } catch (const std::system_error &) {
            co_return ERET_IO;
        }
        co_return ERET_OK;
    }
    static asyncpp::task<ERetCode> join(std::shared_ptr<Impl> s, SOperationOptions o) {
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
};
CSocketByteStream::CSocketByteStream(std::shared_ptr<Impl> i) : _impl(std::move(i)) {}
CSocketByteStream::~CSocketByteStream() { close(); }
std::shared_ptr<CSocketByteStream> CSocketByteStream::create(std::shared_ptr<sockpp::CSocketWatcher> w,
                                                             sockpp::CSocketAsync s) {
    if (!w || !s.isOpen() || !s.usesWatcher(*w) || !s.isByteStream()) {
        throw std::invalid_argument("invalid TLS TCP transport");
    }
    (void)s.peerAddress();
    return std::shared_ptr<CSocketByteStream>(
        new CSocketByteStream(std::make_shared<Impl>(std::move(w), std::move(s))));
}
std::shared_ptr<asyncpp::dispatcher> CSocketByteStream::executor() const noexcept { return _impl->watcher; }
asyncpp::task<SBytesResult> CSocketByteStream::readSome(std::size_t n, SOperationOptions o) {
    return Impl::read(_impl, n, o);
}
asyncpp::task<SWriteResult> CSocketByteStream::writeSome(SBytes b, SOperationOptions o) {
    return Impl::write(_impl, std::move(b), o);
}
asyncpp::task<ERetCode> CSocketByteStream::finishWrite(SOperationOptions o) { return Impl::finish(_impl, o); }
asyncpp::task<ERetCode> CSocketByteStream::waitClosed(SOperationOptions o) { return Impl::join(_impl, o); }
void CSocketByteStream::close() noexcept {
    auto s = _impl;
    if (s->closed.exchange(true)) {
        return;
    }
    if (asyncpp::dispatcher::current() == s->watcher.get()) {
        s->close();
    } else {
        s->watcher->push([s] { s->close(); });
    }
}
namespace {
class CTlsClient final : public ITlsClient {
  private:
    struct State {
        std::shared_ptr<sockpp::CSocketWatcher> watcher;
        std::shared_ptr<ITlsContext> context;
        std::shared_ptr<ITlsStream> stream;
        std::unique_ptr<sockpp::CSocketAsync> socket;
        std::atomic<bool> closed{false};
        bool started = false, pending = false, closeProcessed = false;
        asyncpp::multi_consumer_event drained;
    };
    std::shared_ptr<State> _state;
    static void abort(std::shared_ptr<State> s) {
        if (!s->closed.exchange(true)) {
            auto fn = [s] {
                if (s->socket) {
                    s->socket->close();
                }
                if (s->stream) {
                    s->stream->close();
                }
                s->closeProcessed = true;
                if (!s->pending) {
                    s->drained.set();
                }
            };
            if (asyncpp::dispatcher::current() == s->watcher.get()) {
                fn();
            } else {
                s->watcher->push(std::move(fn));
            }
        }
    }
    static asyncpp::task<SConnectResult> connect(std::shared_ptr<State> s, sockpp::CSocketAddress peer,
                                                 SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return SConnectResult{error(c), {}};
        }
        if (s->closed || s->started) {
            co_return SConnectResult{error(s->closed ? ERET_CLOSED : ERET_INVALID_STATE), {}};
        }
        s->started = true;
        s->pending = true;
        SConnectResult result;
        std::stop_callback cancelled(o.cancellation, [s] { abort(s); });
        try {
            s->socket = std::make_unique<sockpp::CSocketAsync>(sockpp::CSocketAsync::open(
                *s->watcher, peer.family(), sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP));
            co_await s->socket->connect(std::move(peer));
            auto raw = CSocketByteStream::create(s->watcher, std::move(*s->socket));
            s->socket.reset();
            s->stream = ITlsStream::create(raw, s->context);
            c = co_await s->stream->handshake(o);
            if (c == ERET_OK) {
                result = {error(ERET_OK), s->stream};
            } else {
                result.result = error(c, "TLS client handshake failed");
                abort(s);
            }
        } catch (const std::system_error &e) {
            result.result =
                error(o.cancellation.stop_requested() ? ERET_CANCELLED : (s->closed ? ERET_CLOSED : ERET_IO),
                      e.what());
            abort(s);
        } catch (...) {
            result.result =
                error(o.cancellation.stop_requested() ? ERET_CANCELLED : ERET_IO, "TLS client setup failed");
            abort(s);
        }
        s->pending = false;
        if (s->closed && s->closeProcessed) {
            s->drained.set();
        }
        co_return result;
    }
    static asyncpp::task<ERetCode> join(std::shared_ptr<State> s, SOperationOptions o) {
        auto c = policy(s->watcher, o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (!s->closed) {
            co_return ERET_INVALID_STATE;
        }
        co_await s->drained;
        if (s->stream) {
            co_return co_await s->stream->waitClosed(o);
        }
        co_return ERET_OK;
    }

  public:
    CTlsClient(std::shared_ptr<sockpp::CSocketWatcher> w, std::shared_ptr<ITlsContext> c)
        : _state(std::make_shared<State>()) {
        _state->watcher = std::move(w);
        _state->context = std::move(c);
    }
    ~CTlsClient() override {
        if (!_state->started && !_state->stream && !_state->socket) {
            _state->closed = true;
        } else {
            close();
        }
    }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _state->watcher; }
    asyncpp::task<SConnectResult> connect(sockpp::CSocketAddress p, SOperationOptions o) override {
        return connect(_state, std::move(p), o);
    }
    void close() noexcept override { abort(_state); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return join(_state, o); }
};
} // namespace
std::shared_ptr<ITlsClient> ITlsClient::create(std::shared_ptr<sockpp::CSocketWatcher> w,
                                               std::shared_ptr<ITlsContext> c) {
    if (!w || !c || c->options().role != EROLE_CLIENT) {
        throw std::invalid_argument("TLS client requires client-role context and reactor");
    }
    return std::make_shared<CTlsClient>(std::move(w), std::move(c));
}
} // namespace hntls
