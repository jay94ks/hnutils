#ifndef __INCLUDE_HNQUIC_CONNECTION_HPP__
#define __INCLUDE_HNQUIC_CONNECTION_HPP__

#include <hnquic/options.hpp>
#include <hnquic/stream.hpp>
#include <hntls/context.hpp>
#include <hntls/session.hpp>
#include <socketpp/address.hpp>
#include <socketpp/watcher.hpp>

namespace hnquic {
/**
 * QUIC v1 protocol boundary, independent from HTTP. All operations except executor(),
 * isImplemented(), close() and release are serial-executor-affine. No internal threads.
 * The client run task owns UDP I/O and recovery. Accepted server connections share
 * their server endpoint's supervised datagram and timer driver.
 */
class HNQUIC_API IQuicConnection {
  public:
    /** Release retained policy/session references after the caller joins tasks. */
    virtual ~IQuicConnection() = default;
    /** Construct an idle client connection; server connections come from IQuicServer::accept(). */
    static std::shared_ptr<IQuicConnection> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
                                                   std::shared_ptr<hntls::ITlsContext> tls,
                                                   SQuicOptions options = {});
    /** Inspect engine availability, not handshake or authentication success. */
    virtual bool isImplemented() const noexcept = 0;
    /** Stable owning serial reactor reference. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Copy local options without exposing packet/stream registries. */
    virtual SQuicOptions options() const = 0;
    /** Inspect the connection lifecycle. */
    virtual EConnectionState state() const noexcept = 0;
    /** Separate TLS handshake completion from QUIC handshake confirmation. */
    virtual bool handshakeConfirmed() const noexcept = 0;
    /** Copy peer metadata from hntls; incomplete handshakes never imply authentication. */
    virtual hntls::SPeerInfo peerInfo() const = 0;
    /** Client connection attempt, driven by a previously started caller-supervised run task. */
    virtual asyncpp::task<ERetCode> connect(sockpp::CSocketAddress peer, SOperationOptions options = {}) = 0;
    /** Run/join recovery, timers and datagram processing on the caller's reactor. */
    virtual asyncpp::task<ERetCode> run(SOperationOptions options = {}) = 0;
    /** Open a native stream when peer MAX_STREAMS credit permits. */
    virtual asyncpp::task<SStreamResult> openStream(EStreamKind kind, SOperationOptions options = {}) = 0;
    /** Accept a peer-created native stream from a bounded admission queue. */
    virtual asyncpp::task<SStreamResult> acceptStream(SOperationOptions options = {}) = 0;
    /** Copy unavailable/protocol/native failure evidence. */
    virtual SQuicError lastError() const = 0;
    /** Thread-safe nonblocking abort request, never a fabricated successful handshake. */
    virtual void close(std::uint64_t applicationError = 0) noexcept = 0;
    /** Join pending datagrams/timers after close without stopping a shared watcher. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};
} // namespace hnquic
#endif
