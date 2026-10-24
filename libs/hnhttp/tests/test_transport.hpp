#ifndef __HNHTTP_TEST_TRANSPORT_HPP__
#define __HNHTTP_TEST_TRANSPORT_HPP__
#include <hnhttp.hpp>
#include <hnquic.hpp>
#include <asyncpp/event.h>
#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>
#include <deque>
#include <future>

/** Test-only owned memory channels. This is not QUIC packet/TLS interoperability. */
namespace testhttp {
using namespace hnhttp;
inline SHttpError error(ERetCode code) { SHttpError e; e.code = code; return e; }
struct SDirection {
    std::deque<SBytes> bytes;
    std::size_t offset = 0, buffered = 0;
    bool fin = false, reset = false, stopped = false, reading = false, writing = false;
    asyncpp::multi_consumer_event changed;
};
struct SLink {
    std::shared_ptr<asyncpp::dispatcher> executor;
    SDirection directions[2];
    bool closed = false;
    std::size_t fragment = 257, capacity = 32768;
    void close() { closed = true; for (auto& d : directions) { d.changed.set(); } }
};
inline asyncpp::task<SReadResult> read(std::shared_ptr<SLink> link, unsigned side, std::size_t n) {
    if (asyncpp::dispatcher::current() != link->executor.get()) { co_return SReadResult{error(ERET_WRONG_EXECUTOR),{}}; }
    auto& d = link->directions[1-side]; if (d.reading) { co_return SReadResult{error(ERET_BUSY),{}}; }
    if (!n) { co_return SReadResult{error(ERET_INVALID_ARGUMENT),{}}; }
    d.reading = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{d.reading};
    while (d.bytes.empty() && !d.fin && !d.reset && !d.stopped && !link->closed) { d.changed.reset(); co_await d.changed; }
    if (link->closed || d.stopped) { co_return SReadResult{error(ERET_CLOSED),{}}; }
    if (d.reset) { co_return SReadResult{error(ERET_STREAM_RESET),{}}; }
    if (d.bytes.empty()) { co_return SReadResult{error(ERET_EOF),{}}; }
    auto& front = d.bytes.front(); const auto count = (std::min)({n,link->fragment,front.size()-d.offset});
    SBytes out(front.begin()+static_cast<std::ptrdiff_t>(d.offset),front.begin()+static_cast<std::ptrdiff_t>(d.offset+count));
    d.offset += count; d.buffered -= count; if (d.offset == front.size()) { d.offset = 0; d.bytes.pop_front(); } d.changed.set();
    co_return SReadResult{error(ERET_OK),std::move(out)};
}
inline asyncpp::task<SWriteResult> write(std::shared_ptr<SLink> link, unsigned side, SBytes bytes) {
    if (asyncpp::dispatcher::current() != link->executor.get()) { co_return SWriteResult{error(ERET_WRONG_EXECUTOR),0}; }
    auto& d = link->directions[side]; if (d.writing) { co_return SWriteResult{error(ERET_BUSY),0}; }
    d.writing = true; struct Guard { bool& b; ~Guard() { b = false; } } guard{d.writing};
    while (d.buffered == link->capacity && !d.fin && !d.reset && !d.stopped && !link->closed) { d.changed.reset(); co_await d.changed; }
    if (link->closed || d.fin || d.reset || d.stopped) { co_return SWriteResult{error(ERET_CLOSED),0}; }
    const auto n = (std::min)({bytes.size(),link->fragment,link->capacity-d.buffered});
    d.bytes.emplace_back(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(n)); d.buffered += n; d.changed.set();
    co_return SWriteResult{error(ERET_OK),n};
}
class CByteStream final : public IHttpStream {
private:
    std::shared_ptr<SLink> _link; unsigned _side;
public:
    CByteStream(std::shared_ptr<SLink> link, unsigned side) : _link(std::move(link)),_side(side) {}
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _link->executor; }
    SAsyncCapabilities capabilities() const noexcept override { return {}; }
    STransportSecurity security() const override { return {}; }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions) override { return read(_link,_side,n); }
    asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions) override { return write(_link,_side,std::move(bytes)); }
    asyncpp::task<ERetCode> finishWrite(SOperationOptions) override { _link->directions[_side].fin = true; _link->directions[_side].changed.set(); co_return ERET_OK; }
    void close() noexcept override { _link->close(); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions) override { co_return _link->closed ? ERET_OK : ERET_INVALID_STATE; }
};
inline std::pair<std::shared_ptr<IHttpStream>,std::shared_ptr<IHttpStream>> bytePair(std::shared_ptr<asyncpp::dispatcher> executor) {
    auto link = std::make_shared<SLink>(); link->executor = std::move(executor);
    return {std::make_shared<CByteStream>(link,0),std::make_shared<CByteStream>(link,1)};
}
inline hnquic::SQuicError nativeError(ERetCode code) {
    hnquic::SQuicError result;
    result.code = code == ERET_OK ? hnquic::ERET_OK : code == ERET_EOF ? hnquic::ERET_EOF : code == ERET_STREAM_RESET ? hnquic::ERET_STREAM_RESET : code == ERET_BUSY ? hnquic::ERET_BUSY : hnquic::ERET_CLOSED;
    return result;
}
class CNativeStream final : public hnquic::IQuicStream {
private:
    std::shared_ptr<SLink> _link; unsigned _side; std::uint64_t _id;
    hnquic::EStreamKind _kind; hnquic::ERole _initiator;
    static asyncpp::task<hnquic::SReadResult> input(std::shared_ptr<SLink> link, unsigned side, std::size_t n) {
        auto result = co_await read(std::move(link),side,n); co_return hnquic::SReadResult{nativeError(result.result.code),std::move(result.bytes)};
    }
    static asyncpp::task<hnquic::SWriteResult> output(std::shared_ptr<SLink> link, unsigned side, SBytes bytes) {
        auto result = co_await write(std::move(link),side,std::move(bytes)); co_return hnquic::SWriteResult{nativeError(result.result.code),result.acceptedBytes};
    }
public:
    CNativeStream(std::shared_ptr<SLink> link, unsigned side, std::uint64_t id, hnquic::EStreamKind kind, hnquic::ERole initiator)
        : _link(std::move(link)),_side(side),_id(id),_kind(kind),_initiator(initiator) {}
    std::uint64_t id() const noexcept override { return _id; }
    hnquic::EStreamKind kind() const noexcept override { return _kind; }
    hnquic::ERole initiator() const noexcept override { return _initiator; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _link->executor; }
    asyncpp::task<hnquic::SReadResult> readSome(std::size_t n, hnquic::SOperationOptions) override { return input(_link,_side,n); }
    asyncpp::task<hnquic::SWriteResult> writeSome(SBytes bytes, hnquic::SOperationOptions) override { return output(_link,_side,std::move(bytes)); }
    asyncpp::task<hnquic::ERetCode> finishWrite(hnquic::SOperationOptions) override { _link->directions[_side].fin = true; _link->directions[_side].changed.set(); co_return hnquic::ERET_OK; }
    void abortRead(std::uint64_t) noexcept override { auto& d = _link->directions[1-_side]; d.stopped = true; d.changed.set(); }
    void abortWrite(std::uint64_t) noexcept override { auto& d = _link->directions[_side]; d.reset = true; d.changed.set(); }
};
struct SNativeState {
    std::shared_ptr<asyncpp::dispatcher> executor;
    hnquic::ERole role;
    std::weak_ptr<SNativeState> peer;
    std::vector<std::weak_ptr<SLink>> links;
    std::deque<std::shared_ptr<hnquic::IQuicStream>> incoming;
    std::uint64_t nextBidi = 0, nextUni = 2;
    bool closed = false;
    asyncpp::multi_consumer_event changed;
};
class CNativeConnection final : public hnquic::IQuicConnection {
private:
    std::shared_ptr<SNativeState> _s;
    static asyncpp::task<hnquic::SStreamResult> open(std::shared_ptr<SNativeState> s, hnquic::EStreamKind kind) {
        if (s->closed) { co_return hnquic::SStreamResult{nativeError(ERET_CLOSED),{}}; }
        auto peer = s->peer.lock(); if (!peer || peer->closed) { co_return hnquic::SStreamResult{nativeError(ERET_CLOSED),{}}; }
        const auto id = kind == hnquic::ESTREAM_BIDIRECTIONAL ? s->nextBidi : s->nextUni;
        (kind == hnquic::ESTREAM_BIDIRECTIONAL ? s->nextBidi : s->nextUni) += 4;
        auto link = std::make_shared<SLink>(); link->executor = s->executor;
        s->links.push_back(link); peer->links.push_back(link);
        auto local = std::make_shared<CNativeStream>(link,static_cast<unsigned>(s->role),id,kind,s->role);
        auto remote = std::make_shared<CNativeStream>(link,static_cast<unsigned>(peer->role),id,kind,s->role);
        peer->incoming.push_back(remote); peer->changed.set(); co_return hnquic::SStreamResult{nativeError(ERET_OK),std::move(local)};
    }
    static asyncpp::task<hnquic::SStreamResult> accept(std::shared_ptr<SNativeState> s) {
        while (s->incoming.empty() && !s->closed) { s->changed.reset(); co_await s->changed; }
        if (s->closed) { co_return hnquic::SStreamResult{nativeError(ERET_CLOSED),{}}; }
        auto result = s->incoming.front(); s->incoming.pop_front(); co_return hnquic::SStreamResult{nativeError(ERET_OK),std::move(result)};
    }
    static void stop(std::shared_ptr<SNativeState> s) {
        s->closed = true; for (const auto& weak : s->links) { if (auto link = weak.lock()) { link->close(); } } s->incoming.clear(); s->changed.set();
    }
public:
    explicit CNativeConnection(std::shared_ptr<SNativeState> state) : _s(std::move(state)) {}
    bool isImplemented() const noexcept override { return true; } // Test channel only, never a packet-engine claim.
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _s->executor; }
    hnquic::SQuicOptions options() const override { hnquic::SQuicOptions o; o.role = _s->role; return o; }
    hnquic::EConnectionState state() const noexcept override { return _s->closed ? hnquic::ESTATE_CLOSED : hnquic::ESTATE_ESTABLISHED; }
    bool handshakeConfirmed() const noexcept override { return true; } // Fixture assertion, not a TLS handshake.
    hntls::SPeerInfo peerInfo() const override { hntls::SPeerInfo info; info.alpn = "h3"; info.authentication = hntls::EPEER_ACCEPTED; return info; }
    asyncpp::task<hnquic::ERetCode> connect(sockpp::CSocketAddress, hnquic::SOperationOptions) override { co_return hnquic::ERET_NOT_IMPLEMENTED; }
    asyncpp::task<hnquic::ERetCode> run(hnquic::SOperationOptions) override { co_return hnquic::ERET_NOT_IMPLEMENTED; }
    asyncpp::task<hnquic::SStreamResult> openStream(hnquic::EStreamKind kind, hnquic::SOperationOptions) override { return open(_s,kind); }
    asyncpp::task<hnquic::SStreamResult> acceptStream(hnquic::SOperationOptions) override { return accept(_s); }
    hnquic::SQuicError lastError() const override { return nativeError(_s->closed ? ERET_CLOSED : ERET_OK); }
    void close(std::uint64_t) noexcept override { stop(_s); if (auto peer = _s->peer.lock()) { stop(peer); } }
    asyncpp::task<hnquic::ERetCode> waitClosed(hnquic::SOperationOptions) override { co_return _s->closed ? hnquic::ERET_OK : hnquic::ERET_INVALID_STATE; }
};
inline auto nativePair(std::shared_ptr<asyncpp::dispatcher> executor) {
    auto client = std::make_shared<SNativeState>(), server = std::make_shared<SNativeState>();
    client->executor = server->executor = std::move(executor); client->role = hnquic::EROLE_CLIENT; server->role = hnquic::EROLE_SERVER;
    server->nextBidi = 1; server->nextUni = 3; client->peer = server; server->peer = client;
    return std::pair<std::shared_ptr<hnquic::IQuicConnection>,std::shared_ptr<hnquic::IQuicConnection>>{
        std::make_shared<CNativeConnection>(client),std::make_shared<CNativeConnection>(server)};
}
template<typename T> T drive(std::shared_ptr<sockpp::CSocketWatcher> watcher, asyncpp::task<T> task) {
    auto future = asyncpp::as_promise(std::move(task));
    while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) { watcher->runOne(std::chrono::milliseconds{10}); }
    return future.get();
}
}
#endif
