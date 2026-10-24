#ifndef __INCLUDE_HNQUIC_SERVER_HPP__
#define __INCLUDE_HNQUIC_SERVER_HPP__

#include <hnquic/connection.hpp>

namespace hnquic {
/** A failed accept never returns an application connection. */
struct SAcceptResult {
    SQuicError result;
    std::shared_ptr<IQuicConnection> connection;
};
/**
 * Server UDP endpoint with connection-ID demultiplexing and bounded admission.
 * Except executor(), isImplemented(), close() and release, operations are executor-affine.
 * One caller supervises run() and one accept() may be pending at a time.
 */
class HNQUIC_API IQuicServer {
  public:
    /** Release after outstanding accept/run work is joined. */
    virtual ~IQuicServer() = default;
    /** Create a server with server-role TLS and QUIC policies and nonempty ALPN. */
    static std::shared_ptr<IQuicServer> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
                                               std::shared_ptr<hntls::ITlsContext> tls, SQuicOptions options);
    /** Inspect implementation availability. */
    virtual bool isImplemented() const noexcept = 0;
    /** Stable owning reactor; the endpoint never stops a caller's shared watcher. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Copy local immutable server policy. */
    virtual SQuicOptions options() const = 0;
    /** Copy unavailable/protocol/native failure evidence. */
    virtual SQuicError lastError() const = 0;
    /** Bind a socketpp UDP endpoint on the owning reactor. */
    virtual ERetCode listen(sockpp::CSocketAddress address) = 0;
    /** Inspect the bound endpoint, including an automatically selected port. */
    virtual sockpp::CSocketAddress localAddress() const = 0;
    /** Supervise UDP receive, CID lookup and recovery for all accepted connections. */
    virtual asyncpp::task<ERetCode> run(SOperationOptions options = {}) = 0;
    /** Accept a TLS-verified, QUIC-confirmed connection with the configured peer-auth policy. */
    virtual asyncpp::task<SAcceptResult> accept(SOperationOptions options = {}) = 0;
    /** Abort the shared UDP endpoint and its connections without stopping the caller's reactor. */
    virtual void close() noexcept = 0;
    /** Join endpoint work; accepted connections have independent owners. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};
} // namespace hnquic
#endif
