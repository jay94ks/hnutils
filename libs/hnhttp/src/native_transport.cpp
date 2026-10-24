#include "internal.hpp"
#include "transport_lease.hpp"
#include <hntls/stream.hpp>
#include <hnquic/connection.hpp>

namespace hnhttp::detail {
namespace {
ERetCode convert(hntls::ERetCode code) {
    switch (code) {
    case hntls::ERET_OK: return ERET_OK; case hntls::ERET_EOF: return ERET_EOF;
    case hntls::ERET_NOT_IMPLEMENTED: return ERET_UNSUPPORTED;
    case hntls::ERET_INVALID_ARGUMENT: return ERET_INVALID_ARGUMENT; case hntls::ERET_INVALID_STATE: return ERET_INVALID_STATE;
    case hntls::ERET_WRONG_EXECUTOR: return ERET_WRONG_EXECUTOR; case hntls::ERET_CANCELLED: return ERET_CANCELLED;
    case hntls::ERET_TIMEOUT: return ERET_TIMEOUT; case hntls::ERET_LIMIT: return ERET_LIMIT;
    case hntls::ERET_CLOSED: return ERET_CLOSED; case hntls::ERET_BUSY: return ERET_BUSY;
    case hntls::ERET_TRUNCATED: return ERET_TRUNCATED; case hntls::ERET_UNSUPPORTED: return ERET_UNSUPPORTED;
    case hntls::ERET_AUTHENTICATION: return ERET_AUTHENTICATION;
    default: return ERET_IO;
    }
}
ERetCode convert(hnquic::ERetCode code) {
    switch (code) {
    case hnquic::ERET_OK: return ERET_OK; case hnquic::ERET_EOF: return ERET_EOF;
    case hnquic::ERET_NOT_IMPLEMENTED: return ERET_UNSUPPORTED;
    case hnquic::ERET_INVALID_ARGUMENT: return ERET_INVALID_ARGUMENT; case hnquic::ERET_INVALID_STATE: return ERET_INVALID_STATE;
    case hnquic::ERET_WRONG_EXECUTOR: return ERET_WRONG_EXECUTOR; case hnquic::ERET_BUSY: return ERET_BUSY;
    case hnquic::ERET_CANCELLED: return ERET_CANCELLED; case hnquic::ERET_TIMEOUT: return ERET_TIMEOUT;
    case hnquic::ERET_LIMIT: return ERET_LIMIT; case hnquic::ERET_CLOSED: return ERET_CLOSED;
    case hnquic::ERET_STREAM_RESET: return ERET_STREAM_RESET;
    case hnquic::ERET_UNSUPPORTED: return ERET_UNSUPPORTED;
    case hnquic::ERET_IO: return ERET_IO;
    default: return ERET_IO;
    }
}
SHttpError convert(const hntls::STlsError& result) { auto e = error(convert(result.code),result.message); if (result.alert) { e.protocolError = *result.alert; } return e; }
SHttpError convert(const hnquic::SQuicError& result) {
    auto e = error(convert(result.code),result.message); e.streamId = result.streamId;
    e.protocolError = result.applicationError ? result.applicationError : result.transportError; return e;
}
STransportSecurity security(hntls::SPeerInfo peer) {
    STransportSecurity result; result.tls = true; result.alpn = std::move(peer.alpn); result.peerCertificates = std::move(peer.certificateChain);
    result.peerAuthentication = peer.authentication == hntls::EPEER_ACCEPTED ? EPEER_TRANSPORT_ACCEPTED : peer.authentication == hntls::EPEER_REJECTED ? EPEER_TRANSPORT_REJECTED : EPEER_NOT_CHECKED; return result;
}
template<typename T> asyncpp::task<SReadResult> read(std::shared_ptr<T> stream, std::size_t n, SOperationOptions o) {
    if (!current(stream->executor())) { co_return SReadResult{error(ERET_WRONG_EXECUTOR),{}}; }
    auto p = policy(o); if (p != ERET_OK) { co_return SReadResult{error(p),{}}; }
    auto result = co_await stream->readSome(n); co_return SReadResult{convert(result.result),std::move(result.bytes)};
}
template<typename T> asyncpp::task<SWriteResult> write(std::shared_ptr<T> stream, SBytes bytes, SOperationOptions o) {
    if (!current(stream->executor())) { co_return SWriteResult{error(ERET_WRONG_EXECUTOR),0}; }
    auto p = policy(o); if (p != ERET_OK) { co_return SWriteResult{error(p),0}; }
    auto result = co_await stream->writeSome(std::move(bytes)); co_return SWriteResult{convert(result.result),result.acceptedBytes};
}
template<typename T> asyncpp::task<ERetCode> finish(std::shared_ptr<T> stream, SOperationOptions o, bool join) {
    if (!current(stream->executor())) { co_return ERET_WRONG_EXECUTOR; } auto p = policy(o); if (p != ERET_OK) { co_return p; }
    if (join) { co_return convert(co_await stream->waitClosed()); } co_return convert(co_await stream->finishWrite());
}
class CTlsStream final : public IHttpStream {
private:
    std::shared_ptr<hntls::ITlsStream> _stream;
    std::shared_ptr<int> _lease = std::make_shared<int>(0);
public:
    explicit CTlsStream(std::shared_ptr<hntls::ITlsStream> stream) : _stream(std::move(stream)) { claimTransport(_stream,_lease); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _stream->executor(); }
    SAsyncCapabilities capabilities() const noexcept override { return {}; }
    STransportSecurity security() const override { require(executor()); return detail::security(_stream->peerInfo()); }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return read(_stream,n,o); }
    asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions o) override { return write(_stream,std::move(bytes),o); }
    asyncpp::task<ERetCode> finishWrite(SOperationOptions o) override { return finish(_stream,o,false); }
    void close() noexcept override { _stream->close(); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return finish(_stream,o,true); }
};
class CQuicStream final : public IHttpQuicStream {
private:
    std::shared_ptr<hnquic::IQuicStream> _stream;
    static asyncpp::task<ERetCode> fin(std::shared_ptr<hnquic::IQuicStream> stream, SOperationOptions o) {
        if (!current(stream->executor())) { co_return ERET_WRONG_EXECUTOR; } auto p = policy(o); if (p != ERET_OK) { co_return p; } co_return convert(co_await stream->finishWrite());
    }
public:
    explicit CQuicStream(std::shared_ptr<hnquic::IQuicStream> stream) : _stream(std::move(stream)) {}
    std::uint64_t id() const noexcept override { return _stream->id(); }
    EQuicStreamKind kind() const noexcept override { return _stream->kind() == hnquic::ESTREAM_BIDIRECTIONAL ? EQUIC_BIDIRECTIONAL : EQUIC_UNIDIRECTIONAL; }
    EHttpRole initiator() const noexcept override { return _stream->initiator() == hnquic::EROLE_CLIENT ? HTTP_CLIENT : HTTP_SERVER; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _stream->executor(); }
    SAsyncCapabilities capabilities() const noexcept override { return {}; }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return read(_stream,n,o); }
    asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions o) override { return write(_stream,std::move(bytes),o); }
    asyncpp::task<ERetCode> finishWrite(SOperationOptions o) override { return fin(_stream,o); }
    void abortRead(std::uint64_t code) noexcept override { _stream->abortRead(code); }
    void abortWrite(std::uint64_t code) noexcept override { _stream->abortWrite(code); }
};
class CQuicTransport final : public IHttpQuicTransport {
private:
    std::shared_ptr<hnquic::IQuicConnection> _connection;
    std::shared_ptr<int> _lease = std::make_shared<int>(0);
    static asyncpp::task<SQuicStreamResult> stream(std::shared_ptr<hnquic::IQuicConnection> c, bool open, EQuicStreamKind kind, SOperationOptions o) {
        if (!current(c->executor())) { co_return SQuicStreamResult{error(ERET_WRONG_EXECUTOR),{}}; }
        auto p = policy(o); if (p != ERET_OK) { co_return SQuicStreamResult{error(p),{}}; }
        if (open && kind != EQUIC_BIDIRECTIONAL && kind != EQUIC_UNIDIRECTIONAL) { co_return SQuicStreamResult{error(ERET_INVALID_ARGUMENT),{}}; }
        auto result = open ? co_await c->openStream(kind == EQUIC_BIDIRECTIONAL ? hnquic::ESTREAM_BIDIRECTIONAL : hnquic::ESTREAM_UNIDIRECTIONAL) : co_await c->acceptStream();
        if (result.result.code != hnquic::ERET_OK || !result.stream) { co_return SQuicStreamResult{result.result.code == hnquic::ERET_OK ? error(ERET_IO) : convert(result.result),{}}; }
        co_return SQuicStreamResult{error(ERET_OK),std::make_unique<CQuicStream>(std::move(result.stream))};
    }
    static asyncpp::task<ERetCode> joined(std::shared_ptr<hnquic::IQuicConnection> c, SOperationOptions o) {
        if (!current(c->executor())) { co_return ERET_WRONG_EXECUTOR; } auto p = policy(o); if (p != ERET_OK) { co_return p; } co_return convert(co_await c->waitClosed());
    }
public:
    explicit CQuicTransport(std::shared_ptr<hnquic::IQuicConnection> c) : _connection(std::move(c)) { claimTransport(_connection,_lease); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _connection->executor(); }
    SAsyncCapabilities capabilities() const noexcept override { return {}; }
    STransportSecurity security() const override { require(executor()); return detail::security(_connection->peerInfo()); }
    bool handshakeConfirmed() const noexcept override { return _connection->handshakeConfirmed(); }
    asyncpp::task<SQuicStreamResult> acceptStream(SOperationOptions o) override { return stream(_connection,false,EQUIC_BIDIRECTIONAL,o); }
    asyncpp::task<SQuicStreamResult> openStream(EQuicStreamKind kind, SOperationOptions o) override { return stream(_connection,true,kind,o); }
    void close(std::uint64_t code) noexcept override { _connection->close(code); }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override { return joined(_connection,o); }
};
hntls::ERetCode tlsCode(ERetCode code) {
    switch (code) {
    case ERET_OK: return hntls::ERET_OK; case ERET_EOF: return hntls::ERET_EOF;
    case ERET_BUSY: return hntls::ERET_BUSY; case ERET_WRONG_EXECUTOR: return hntls::ERET_WRONG_EXECUTOR;
    case ERET_CANCELLED: return hntls::ERET_CANCELLED; case ERET_TIMEOUT: return hntls::ERET_TIMEOUT;
    case ERET_LIMIT: return hntls::ERET_LIMIT; case ERET_CLOSED: return hntls::ERET_CLOSED;
    case ERET_UNSUPPORTED: return hntls::ERET_NOT_IMPLEMENTED;
    case ERET_IO: return hntls::ERET_IO; case ERET_TRUNCATED: return hntls::ERET_TRUNCATED;
    case ERET_INVALID_ARGUMENT: return hntls::ERET_INVALID_ARGUMENT;
    case ERET_AUTHENTICATION: return hntls::ERET_AUTHENTICATION;
    default: return hntls::ERET_INVALID_STATE;
    }
}
class CRecordTransport final : public hntls::IByteStream {
private:
    std::shared_ptr<IHttpStream> _stream;
    std::shared_ptr<int> _lease = std::make_shared<int>(0);
    static asyncpp::task<hntls::SBytesResult> input(std::shared_ptr<IHttpStream> stream, std::size_t n, hntls::SOperationOptions o) {
        auto result = co_await stream->readSome(n,{o.deadline,o.cancellation}); co_return hntls::SBytesResult{{tlsCode(result.result.code),{},result.result.message},std::move(result.bytes)};
    }
    static asyncpp::task<hntls::SWriteResult> output(std::shared_ptr<IHttpStream> stream, SBytes bytes, hntls::SOperationOptions o) {
        auto result = co_await stream->writeSome(std::move(bytes),{o.deadline,o.cancellation}); co_return hntls::SWriteResult{{tlsCode(result.result.code),{},result.result.message},result.acceptedBytes};
    }
    static asyncpp::task<hntls::ERetCode> finished(std::shared_ptr<IHttpStream> stream, hntls::SOperationOptions o, bool join) {
        if (join) { co_return tlsCode(co_await stream->waitClosed({o.deadline,o.cancellation})); } co_return tlsCode(co_await stream->finishWrite({o.deadline,o.cancellation}));
    }
public:
    explicit CRecordTransport(std::shared_ptr<IHttpStream> stream) : _stream(std::move(stream)) { claimTransport(_stream,_lease); }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _stream->executor(); }
    asyncpp::task<hntls::SBytesResult> readSome(std::size_t n, hntls::SOperationOptions o) override { return input(_stream,n,o); }
    asyncpp::task<hntls::SWriteResult> writeSome(hntls::SBytes bytes, hntls::SOperationOptions o) override { return output(_stream,std::move(bytes),o); }
    asyncpp::task<hntls::ERetCode> finishWrite(hntls::SOperationOptions o) override { return finished(_stream,o,false); }
    void close() noexcept override { _stream->close(); }
    asyncpp::task<hntls::ERetCode> waitClosed(hntls::SOperationOptions o) override { return finished(_stream,o,true); }
};
}
}
namespace hnhttp {
std::shared_ptr<IHttpStream> IHttpStream::fromTls(std::shared_ptr<hntls::ITlsStream> stream) {
    if (!stream || !stream->executor()) { throw std::invalid_argument("null hntls stream/executor"); } detail::require(stream->executor());
    if (!stream->isImplemented()) { detail::unsupported("hntls stream is unavailable"); }
    if (stream->state() != hntls::ESTATE_ESTABLISHED || stream->peerInfo().authentication == hntls::EPEER_REJECTED) { throw std::invalid_argument("TLS stream is not established"); }
    return std::make_shared<detail::CTlsStream>(std::move(stream));
}
std::shared_ptr<hntls::IByteStream> IHttpStream::asTlsTransport(std::shared_ptr<IHttpStream> stream) {
    if (!stream || !stream->executor()) { throw std::invalid_argument("null TLS record transport"); } detail::require(stream->executor());
    if (stream->security().tls) { throw std::invalid_argument("TLS record transport must not already be TLS"); }
    return std::make_shared<detail::CRecordTransport>(std::move(stream));
}
std::unique_ptr<IHttpQuicTransport> IHttpQuicTransport::fromQuic(std::shared_ptr<hnquic::IQuicConnection> connection) {
    if (!connection || !connection->executor()) { throw std::invalid_argument("null hnquic connection"); } detail::require(connection->executor());
    if (!connection->isImplemented()) { detail::unsupported("hnquic packet engine is unavailable"); }
    if (!connection->handshakeConfirmed() || connection->state() != hnquic::ESTATE_ESTABLISHED) { throw std::invalid_argument("QUIC connection is not confirmed"); }
    return std::make_unique<detail::CQuicTransport>(std::move(connection));
}
}
