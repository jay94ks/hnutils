#ifndef __INCLUDE_HNHTTP_CONTEXT_HPP__
#define __INCLUDE_HNHTTP_CONTEXT_HPP__

#include <hnhttp/connection.hpp>
#include <hnhttp/message.hpp>

namespace hnhttp {

/** One successful CONNECT stream; closing an HTTP/2/3 tunnel never closes unrelated streams. */
class IHttpTunnel : public virtual IAsyncByteChannel {
public:
    /** Release tunnel ownership without detached I/O. */
    virtual ~IHttpTunnel() = default;

    /** Abort this tunnel; connection-scoped only for HTTP/1. */
    virtual void close() noexcept = 0;
};

/** Explicit transition from HTTP message framing into CONNECT data. */
struct STunnelResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::shared_ptr<IHttpTunnel> tunnel;
};

/** Exchange lifecycle is independent of the connection's multiplexed lifecycle. */
enum EContextState { ECONTEXT_DRAFT, ECONTEXT_BOUND, ECONTEXT_ACTIVE,
    ECONTEXT_COMPLETE, ECONTEXT_TUNNEL, ECONTEXT_FAILED };

/** Shared exchange contract; every context has fresh request/response facades, never a mutable reset(). */
class IHttpContext {
public:
    /** Last unfinished exchange release closes HTTP/1 or queues a supervised multiplexed stream reset. */
    virtual ~IHttpContext() = default;

    /**
     * HTTP/1 convenience: bind shared lifetime on the stream's serial executor once.
     * Failure leaves this context and stream unchanged and never closes a caller's stream.
     * Null returns INVALID_ARGUMENT; a foreign executor returns WRONG_EXECUTOR before binding.
     * Acquire one connection I/O lease; aliases are not concurrent readers or new connections.
     * Reuse getConnection() for the next context, preserving read-ahead and protocol state.
     * HTTP/2 uses createStream()+run(); HTTP/3 uses createQuic()+run() instead.
     */
    virtual ERetCode setStream(std::shared_ptr<IHttpStream> stream) = 0;

    /** Retain the connection without exposing transport or mutable codec buffers. */
    virtual std::shared_ptr<IHttpConnection> getConnection() const = 0;

    /** Copy the external transport security snapshot; no trust claim is added by hnhttp. */
    virtual STransportSecurity transportSecurity() const = 0;

    /** HTTP/1 has no stream ID; HTTP/3 ID zero is valid and never means unbound. */
    virtual std::optional<std::uint64_t> streamId() const noexcept = 0;

    /** Inspect this exchange's state. */
    virtual EContextState state() const noexcept = 0;

    /**
     * Complete only after outbound finalization and inbound body/trailer completion.
     * Default does not drain: BODY_PENDING leaves the exchange active. Optional discard is bounded.
     */
    virtual asyncpp::task<ERetCode> finish(SFinishOptions finish = {}, SOperationOptions options = {}) = 0;

    /** Cancel this exchange; HTTP/1 closes, HTTP/2/3 reset only this stream unless a connection error exists. */
    virtual asyncpp::task<ERetCode> abort(SOperationOptions options = {}) = 0;

    /** Take a successful CONNECT tunnel, preserving buffered bytes and shared-connection isolation. */
    virtual STunnelResult takeTunnel() = 0;

    /** Copy a diagnostic with stream/connection scope and native protocol error evidence. */
    virtual SHttpError lastError() const = 0;
};

/** Server exchange factory; HTTP/2/3 incoming contexts are instead created by multiplexed accept(). */
class HNHTTP_API IHttpServerContext : public virtual IHttpContext {
public:
    /** Release server facade ownership. */
    virtual ~IHttpServerContext() = default;

    /** Declare an unbound HTTP/1 server context; HTTP_2/HTTP_3 options are rejected here. */
    static std::shared_ptr<IHttpServerContext> create(SHttpConnectionOptions options = {});

    /** Declare the next HTTP/1 context using the same connection/read-ahead owner. */
    static std::shared_ptr<IHttpServerContext> create(std::shared_ptr<IHttp1Connection> connection);

    /** Retain a read-only request facade and its incoming body. */
    virtual std::shared_ptr<IHttpServerRequest> getRequest() const = 0;

    /** Retain a mutable-until-sent server response facade. */
    virtual std::shared_ptr<IHttpServerResponse> getResponse() const = 0;
};

/** Client exchange factory: one HTTP/1 lease or one independently owned HTTP/2/3 request stream. */
class HNHTTP_API IHttpClientContext : public virtual IHttpContext {
public:
    /** Release client facade ownership. */
    virtual ~IHttpClientContext() = default;

    /** Declare an unbound HTTP/1 client context for setStream(). */
    static std::shared_ptr<IHttpClientContext> create(SHttpConnectionOptions options = {});

    /** Declare a fresh exchange on an existing connection; exec requires an active multiplexed run(). */
    static std::shared_ptr<IHttpClientContext> create(std::shared_ptr<IHttpConnection> connection);

    /** Retain the outgoing builder. */
    virtual std::shared_ptr<IHttpClientRequest> getRequest() const = 0;

    /** Retain the final response view; headers become accessible after request exec succeeds. */
    virtual std::shared_ptr<IHttpClientResponse> getResponse() const = 0;
};

/** Raw HTTP/1 protocol takeover must preserve any over-read bytes and transport metadata. */
struct SStreamHandoff {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::shared_ptr<IHttpStream> stream;
    SBytes readAhead;

    /** Construct an empty, unsuccessful handoff. */
    SStreamHandoff() = default;

    /** Transfer the logical I/O lease and buffered bytes once. */
    SStreamHandoff(SStreamHandoff&&) noexcept = default;

    /** Transfer the result without duplicating its read-ahead. */
    SStreamHandoff& operator=(SStreamHandoff&&) noexcept = default;

    /** Do not duplicate a protocol takeover. */
    SStreamHandoff(const SStreamHandoff&) = delete;

    /** Do not duplicate a protocol takeover. */
    SStreamHandoff& operator=(const SStreamHandoff&) = delete;
};

/** Optional HTTP/1-only context capability for a validated 101 Upgrade or successful CONNECT. */
class IHttp1Handoff : public virtual IHttpContext {
public:
    /** Release the capability without detaching anything. */
    virtual ~IHttp1Handoff() = default;

    /** Atomically detach after handshake completion with no outstanding I/O; never offered for HTTP/2/3. */
    virtual SStreamHandoff detachStream() = 0;
};

} // namespace hnhttp
#endif
