#include <hntls/stream.hpp>
#include <asyncpp/mutex.h>
#include <asyncpp/event.h>
#include <atomic>
#include <deque>
#include <stdexcept>

namespace hntls {
namespace {
STlsError error(ERetCode c, const char *text = "") { return {c, {}, text}; }
struct Flag {
    bool &value;
    explicit Flag(bool &v) : value(v) { value = true; }
    ~Flag() { value = false; }
};
class CTlsStream final : public ITlsStream {
  private:
    std::shared_ptr<IByteStream> _transport;
    std::shared_ptr<ITlsSession> _session;
    std::size_t _limit, _queued = 0;
    std::deque<SBytes> _plain;
    bool _reading = false, _writing = false, _handshaking = false, _eof = false;
    std::atomic<bool> _closed{false};
    asyncpp::mutex _wireWrite;
    ERetCode entry(const SOperationOptions &o) {
        if (asyncpp::dispatcher::current() != executor().get()) {
            return ERET_WRONG_EXECUTOR;
        }
        if (o.cancellation.stop_requested()) {
            return ERET_CANCELLED;
        }
        if (o.deadline) {
            return *o.deadline <= std::chrono::steady_clock::now() ? ERET_TIMEOUT : ERET_UNSUPPORTED;
        }
        return _closed ? ERET_CLOSED : ERET_OK;
    }
    asyncpp::task<ERetCode> send(SBytes data, SOperationOptions o) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            SBytes part(data.begin() + offset, data.end());
            auto r = co_await _transport->writeSome(std::move(part), o);
            if (r.result.code != ERET_OK) {
                co_return r.result.code;
            }
            if (!r.acceptedBytes || r.acceptedBytes > data.size() - offset) {
                co_return ERET_IO;
            }
            offset += r.acceptedBytes;
        }
        co_return ERET_OK;
    }
    asyncpp::task<ERetCode> consume(SHandshakeResult r, SOperationOptions o) {
        if (r.result.code != ERET_OK) {
            close();
            co_return r.result.code;
        }
        if (!r.outgoing.empty()) {
            auto lock = co_await _wireWrite.lock_scoped();
            for (auto &flight : r.outgoing) {
                auto code = co_await send(std::move(flight.bytes), o);
                if (code != ERET_OK) {
                    close();
                    co_return code;
                }
            }
        }
        for (auto &plain : r.plaintext) {
            if (plain.size() > _limit - _queued) {
                close();
                co_return ERET_LIMIT;
            }
            _queued += plain.size();
            _plain.push_back(std::move(plain));
        }
        _eof |= r.peerClosedWrite;
        co_return ERET_OK;
    }

  public:
    CTlsStream(std::shared_ptr<IByteStream> t, std::shared_ptr<ITlsContext> c)
        : _transport(std::move(t)), _session(c->createSession(_transport->executor(), EMODE_RECORDS)),
          _limit(c->options().maximumBufferedBytes) {}
    bool isImplemented() const noexcept override { return _session->isImplemented(); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _transport->executor(); }
    ESessionState state() const noexcept override { return _session->state(); }
    SPeerInfo peerInfo() const override { return _session->peerInfo(); }
    asyncpp::task<ERetCode> handshake(SOperationOptions o) override {
        auto c = entry(o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (_handshaking) {
            co_return ERET_BUSY;
        }
        if (state() != ESTATE_NEW) {
            co_return ERET_INVALID_STATE;
        }
        Flag active(_handshaking);
        std::stop_callback cancel(o.cancellation, [this] { close(); });
        c = co_await consume(co_await _session->start(o), o);
        if (c != ERET_OK) {
            co_return c;
        }
        while (state() != ESTATE_ESTABLISHED) {
            auto input = co_await _transport->readSome(std::min<std::size_t>(16645, _limit), o);
            if (input.result.code != ERET_OK || input.bytes.empty()) {
                close();
                co_return o.cancellation.stop_requested()
                    ? ERET_CANCELLED
                    : (input.result.code == ERET_EOF || input.bytes.empty() ? ERET_TRUNCATED
                                                                            : input.result.code);
            }
            c = co_await consume(co_await _session->receive(ELEVEL_INITIAL, std::move(input.bytes), o), o);
            if (c != ERET_OK) {
                co_return c;
            }
        }
        co_return ERET_OK;
    }
    asyncpp::task<SBytesResult> readSome(std::size_t n, SOperationOptions o) override {
        auto c = entry(o);
        if (c != ERET_OK) {
            co_return SBytesResult{error(c), {}};
        }
        if (_reading) {
            co_return SBytesResult{error(ERET_BUSY), {}};
        }
        if (!n || n > _limit) {
            co_return SBytesResult{error(ERET_INVALID_ARGUMENT), {}};
        }
        if (state() != ESTATE_ESTABLISHED) {
            co_return SBytesResult{error(ERET_INVALID_STATE), {}};
        }
        Flag active(_reading);
        std::stop_callback cancel(o.cancellation, [this] { close(); });
        while (_plain.empty() && !_eof) {
            auto r = co_await _transport->readSome(std::min<std::size_t>(16645, _limit), o);
            if (r.result.code != ERET_OK || r.bytes.empty()) {
                close();
                co_return SBytesResult{
                    error(o.cancellation.stop_requested()
                              ? ERET_CANCELLED
                              : (r.result.code == ERET_EOF || r.bytes.empty() ? ERET_TRUNCATED
                                                                              : r.result.code)),
                    {}};
            }
            c = co_await consume(co_await _session->receive(ELEVEL_APPLICATION, std::move(r.bytes), o), o);
            if (c != ERET_OK) {
                co_return SBytesResult{error(c), {}};
            }
        }
        if (_plain.empty()) {
            co_return SBytesResult{error(ERET_EOF), {}};
        }
        auto &first = _plain.front();
        auto size = std::min(n, first.size());
        SBytes output(first.begin(), first.begin() + size);
        first.erase(first.begin(), first.begin() + size);
        _queued -= size;
        if (first.empty()) {
            _plain.pop_front();
        }
        co_return SBytesResult{error(ERET_OK), std::move(output)};
    }
    asyncpp::task<SWriteResult> writeSome(SBytes data, SOperationOptions o) override {
        auto c = entry(o);
        if (c != ERET_OK) {
            co_return SWriteResult{error(c), 0};
        }
        if (_writing) {
            co_return SWriteResult{error(ERET_BUSY), 0};
        }
        if (state() != ESTATE_ESTABLISHED) {
            co_return SWriteResult{error(ERET_INVALID_STATE), 0};
        }
        if (data.empty() || data.size() > _limit) {
            co_return SWriteResult{error(ERET_INVALID_ARGUMENT), 0};
        }
        Flag active(_writing);
        std::stop_callback cancel(o.cancellation, [this] { close(); });
        auto lock = co_await _wireWrite.lock_scoped();
        auto size = data.size();
        auto encrypted = co_await _session->encrypt(std::move(data), o);
        if (encrypted.result.code != ERET_OK) {
            co_return SWriteResult{encrypted.result, 0};
        }
        c = co_await send(std::move(encrypted.bytes), o);
        if (c != ERET_OK) {
            close();
        }
        co_return SWriteResult{error(o.cancellation.stop_requested() ? ERET_CANCELLED : c),
                               c == ERET_OK ? size : 0};
    }
    asyncpp::task<ERetCode> finishWrite(SOperationOptions o) override {
        auto c = entry(o);
        if (c != ERET_OK) {
            co_return c;
        }
        if (_writing || _handshaking) {
            co_return ERET_BUSY;
        }
        Flag active(_writing);
        auto lock = co_await _wireWrite.lock_scoped();
        auto r = co_await _session->finishWrite(o);
        if (r.result.code != ERET_OK) {
            co_return r.result.code;
        }
        c = co_await send(std::move(r.bytes), o);
        if (c != ERET_OK) {
            close();
            co_return c;
        }
        co_return co_await _transport->finishWrite(o);
    }
    void close() noexcept override {
        if (!_closed.exchange(true)) {
            _session->close();
            _transport->close();
        }
    }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override {
        if (asyncpp::dispatcher::current() != executor().get()) {
            co_return ERET_WRONG_EXECUTOR;
        }
        auto r = co_await _transport->waitClosed(o);
        if (r != ERET_OK) {
            co_return r;
        }
        if (_reading || _writing || _handshaking) {
            co_return ERET_BUSY;
        }
        co_return co_await _session->waitClosed(o);
    }
};
} // namespace
std::shared_ptr<ITlsStream> ITlsStream::create(std::shared_ptr<IByteStream> t,
                                               std::shared_ptr<ITlsContext> c) {
    if (!t || !t->executor() || !c) {
        throw std::invalid_argument("invalid TLS transport/context");
    }
    return std::make_shared<CTlsStream>(std::move(t), std::move(c));
}
} // namespace hntls
