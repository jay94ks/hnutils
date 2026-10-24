#ifndef __INCLUDE_HNQUIC_CLIENT_HPP__
#define __INCLUDE_HNQUIC_CLIENT_HPP__
#include <hnquic/connection.hpp>
namespace hnquic {
/** Failed connection establishment never returns an application-ready connection. */
struct SConnectResult {
    SQuicError result;
    std::shared_ptr<IQuicConnection> connection;
};
/** QUIC v1 client facade; the caller supervises run() on its existing socketpp reactor. */
class HNQUIC_API IQuicClient {
  public:
    /** Release only after run and outstanding stream operations are joined. */
    virtual ~IQuicClient() = default;
    /** Create a client with client-role TLS/QUIC options. */
    static std::shared_ptr<IQuicClient> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
                                               std::shared_ptr<hntls::ITlsContext> context,
                                               SQuicOptions options = {});
    /** Stable owning reactor. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Start before connect(); drive datagrams, recovery and stream wakeups. */
    virtual asyncpp::task<ERetCode> run(SOperationOptions options = {}) = 0;
    /** Return only after verified TLS Finished and QUIC handshake confirmation. */
    virtual asyncpp::task<SConnectResult> connect(sockpp::CSocketAddress peer,
                                                  SOperationOptions options = {}) = 0;
    /** Abort the client and wake pending operations without stopping the reactor. */
    virtual void close(std::uint64_t applicationError = 0) noexcept = 0;
    /** Join the native connection's run task and datagram accesses. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};
} // namespace hnquic
#endif
