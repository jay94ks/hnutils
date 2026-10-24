#ifndef __INCLUDE_HNHTTP_ENDPOINT_HPP__
#define __INCLUDE_HNHTTP_ENDPOINT_HPP__

#include <hnhttp/context.hpp>
#include <socketpp/async_socket.hpp>
#include <hntls/context.hpp>
#include <hnquic/options.hpp>

namespace hnhttp {
/** An accepted protocol connection; no HTTP/2 run task or handler is silently created. */
struct SConnectionResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::shared_ptr<IHttpConnection> connection;
};

/** TCP, hntls HTTPS or hnquic HTTP/3 client on a caller-driven reactor; no implicit fallback. */
class HNHTTP_API IHttpClient {
public:
    /** Release ownership; supervise operations and close/join before stopping the reactor. */
    virtual ~IHttpClient() = default;
    /** Create an unconnected HTTP/1 or prior-knowledge HTTP/2 client; HTTP/3 requires hnquic. */
    static std::shared_ptr<IHttpClient> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        SHttpConnectionOptions options = {});
    /** Create an HTTPS HTTP/1.x or HTTP/2 client; enforce peer authentication and matching ALPN. */
    static std::shared_ptr<IHttpClient> createTls(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        std::shared_ptr<hntls::ITlsContext> tls,SHttpConnectionOptions options = {});
    /** Create an HTTP/3 client. Start native run() before connect(); HTTP run() remains separate. */
    static std::shared_ptr<IHttpClient> createQuic(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        std::shared_ptr<hntls::ITlsContext> tls,SHttpConnectionOptions options,hnquic::SQuicOptions quic = {});
    /** Supervise native QUIC UDP/recovery; plain TCP and TLS clients return UNSUPPORTED. */
    virtual asyncpp::task<ERetCode> run(SOperationOptions options = {});
    /** Connect once to an already resolved address, on executor(). */
    virtual asyncpp::task<ERetCode> connect(sockpp::CSocketAddress address, SOperationOptions options = {}) = 0;
    /** Create a fresh exchange using this connection's existing read-ahead state. */
    virtual std::shared_ptr<IHttpClientContext> createRequest() = 0;
    /** Retain the protocol connection without exposing its socket. */
    virtual std::shared_ptr<IHttpConnection> getConnection() const = 0;
    /** Inspect the serial executor from any thread. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Request nonblocking shutdown from any thread. */
    virtual void close() noexcept = 0;
    /** Join connect/connection I/O before releasing reactor resources. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};

/** HTTP listener. TCP/TLS accepted connections are independent; QUIC shares its UDP endpoint lifetime. */
class HNHTTP_API IHttpServer {
public:
    /** Release the listener after its pending accept has been joined. */
    virtual ~IHttpServer() = default;
    /** Create an HTTP/1 or prior-knowledge HTTP/2 listener on a single-runner watcher. */
    static std::shared_ptr<IHttpServer> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        SHttpConnectionOptions options = {});
    /** Create an HTTPS listener using a server-role identity and matching ALPN. */
    static std::shared_ptr<IHttpServer> createTls(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        std::shared_ptr<hntls::ITlsContext> tls,SHttpConnectionOptions options = {});
    /** Create an HTTP/3 UDP listener; run() drives all native QUIC connections and admission. */
    static std::shared_ptr<IHttpServer> createQuic(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        std::shared_ptr<hntls::ITlsContext> tls,SHttpConnectionOptions options,hnquic::SQuicOptions quic);
    /** Supervise native QUIC I/O; TCP/TLS listeners return UNSUPPORTED. */
    virtual asyncpp::task<ERetCode> run(SOperationOptions options = {});
    /** Bind and listen on executor(); backlog must be positive. */
    virtual ERetCode listen(sockpp::CSocketAddress address, int backlog = 128) = 0;
    /** Inspect the bound endpoint, including the OS-selected port when binding port zero. */
    virtual sockpp::CSocketAddress localAddress() const = 0;
    /** HTTP/1 convenience: accept and parse one request head; HTTP/2/3 use acceptConnection(). */
    virtual asyncpp::task<SAcceptResult> accept(SOperationOptions options = {}) = 0;
    /** Accept and finish transport authentication; caller drives HTTP/2/3 run()/ready()/accept(). */
    virtual asyncpp::task<SConnectionResult> acceptConnection(SOperationOptions options = {}) = 0;
    /** Inspect the serial executor from any thread. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Stop admission and wake accepts; TCP/TLS accepted connections survive, shared-UDP QUIC ones close. */
    virtual void close() noexcept = 0;
    /** Join listener work and native QUIC run; application-owned HTTP pumps must be joined separately. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};

/** Adapt a connected socketpp stream; state and native buffer lifetimes remain private. */
class HNHTTP_API CSocketHttpStream final : public IHttpStream {
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
    /** Adopt private asynchronous state. */
    explicit CSocketHttpStream(std::shared_ptr<Impl> impl);
public:
    /** Share ownership of a connected socket and its driving reactor. */
    static std::shared_ptr<CSocketHttpStream> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
        sockpp::CSocketAsync socket);
    /** Request close; retained operations preserve native buffers until completion. */
    ~CSocketHttpStream() override;
    /** Socket adapters cannot be copied. */
    CSocketHttpStream(const CSocketHttpStream&) = delete;
    /** Socket adapters cannot be copied. */
    CSocketHttpStream& operator=(const CSocketHttpStream&) = delete;
    /** Stable owning serial reactor reference. */
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override;
    /** This initial adapter rejects live stop tokens/deadlines before performing I/O. */
    SAsyncCapabilities capabilities() const noexcept override;
    /** Read an owned bounded chunk; no native error is disguised as EOF. */
    asyncpp::task<SReadResult> readSome(std::size_t maximumBytes, SOperationOptions options = {}) override;
    /** Write owned bytes with explicit partial-write reporting. */
    asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions options = {}) override;
    /** Half-close after the pending write completes. */
    asyncpp::task<ERetCode> finishWrite(SOperationOptions options = {}) override;
    /** Plaintext metadata; never claims TLS or authenticated certificates. */
    STransportSecurity security() const override;
    /** Post a close request without blocking the reactor. */
    void close() noexcept override;
    /** Join pending native operations after close. One close waiter at a time. */
    asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) override;
};

} // namespace hnhttp
#endif
