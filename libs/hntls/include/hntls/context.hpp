#ifndef __INCLUDE_HNTLS_CONTEXT_HPP__
#define __INCLUDE_HNTLS_CONTEXT_HPP__

#include <hntls/options.hpp>
#include <hntls/credentials.hpp>
#include <asyncpp/dispatcher.h>

namespace hntls {
class ITlsSession;
/** Immutable TLS 1.3 policy and credential factory. */
class HNTLS_API ITlsContext {
  public:
    /** Release retained policy and credential ownership. */
    virtual ~ITlsContext() = default;
    /** Validate options and create a context without network I/O or cryptographic work. */
    static std::shared_ptr<ITlsContext> create(STlsOptions options = {}, SIdentity identity = {},
                                               STrustOptions trust = {});
    /** Inspect actual engine availability, not whether a handshake succeeded. */
    virtual bool isImplemented() const noexcept = 0;
    /** Copy immutable configuration. */
    virtual STlsOptions options() const = 0;
    /**
     * Create a session on a caller-owned serial dispatcher; no hidden loop or thread.
     * QUIC transport parameters are owned per connection, not shared context state.
     * Record mode requires empty transport parameters. Invalid inputs throw invalid_argument.
     */
    virtual std::shared_ptr<ITlsSession> createSession(std::shared_ptr<asyncpp::dispatcher> executor,
                                                       EMode mode, SBytes quicTransportParameters = {}) = 0;
};
} // namespace hntls
#endif
