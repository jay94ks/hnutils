#ifndef __INCLUDE_HNTLS_CLIENT_HPP__
#define __INCLUDE_HNTLS_CLIENT_HPP__
#include <hntls/stream.hpp>
#include <socketpp/address.hpp>
#include <socketpp/watcher.hpp>
namespace hntls {
/** A failed connection never exposes an unauthenticated application stream. */
struct SConnectResult {
    STlsError result;
    std::shared_ptr<ITlsStream> stream;
};
/**
 * TLS 1.3 client using the caller's socketpp reactor. No resolver, hidden thread,
 * system trust store, version downgrade or implicit plaintext fallback.
 */
class HNTLS_API ITlsClient {
  public:
    /** Release after pending operations have completed. */
    virtual ~ITlsClient() = default;
    /** Create a client with a client-role immutable TLS policy. */
    static std::shared_ptr<ITlsClient> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
                                              std::shared_ptr<ITlsContext> context);
    /** Stable caller-owned executor. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Connect TCP and verify the TLS handshake; one attempt per client instance. */
    virtual asyncpp::task<SConnectResult> connect(sockpp::CSocketAddress peer,
                                                  SOperationOptions options = {}) = 0;
    /** Abort connection establishment and any retained application stream. */
    virtual void close() noexcept = 0;
    /** Join transport operations before stopping the shared reactor. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};
} // namespace hntls
#endif
