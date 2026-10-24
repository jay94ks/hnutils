#ifndef __INCLUDE_HNHTTP_CONNECTION_HPP__
#define __INCLUDE_HNHTTP_CONNECTION_HPP__

#include <hnhttp/transport.hpp>
#include <array>

namespace hnhttp {

class IHttpServerContext;

/** One accepted request with headers ready, not an eagerly buffered whole body. */
struct SAcceptResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::shared_ptr<IHttpServerContext> context;
};

/**
 * Owns protocol state and the transport I/O lease, not application exchanges. HTTP/1 read-ahead
 * and HTTP/2/3 compression/control state never leave this object. Multiplexed engines require
 * a caller-supervised run task. HTTP/3 requires a confirmed native QUIC stream transport.
 * Bound facades, getters, builders and task initiation/awaits are executor-affine, except
 * executor(), close() and ownership release. Synchronous throwing APIs reject a wrong
 * executor with logic_error; noexcept observers require affinity. Tasks return
 * WRONG_EXECUTOR before protocol effects. Shared ownership does not imply thread safety.
 */
class HNHTTP_API IHttpConnection {
public:
    /** Release without spawning detached protocol work; abort outstanding owned transport on final release. */
    virtual ~IHttpConnection() = default;

    /**
     * Bind a shared TCP/TLS-like stream on its executor for HTTP/1.x or HTTP/2, never HTTP/3.
     * Retain its executor; reject null, invalid options or an already leased adapter without
     * closing it or changing an existing binding. There is no implicit global connection cache.
     */
    static std::shared_ptr<IHttpConnection> createStream(std::shared_ptr<IHttpStream> stream,
        EHttpRole role, SHttpConnectionOptions options = {});

    /** Transfer on its executor an established QUIC connection, requiring options.version == HTTP_3. */
    static std::shared_ptr<IHttpConnection> createQuic(std::unique_ptr<IHttpQuicTransport> transport,
        EHttpRole role, SHttpConnectionOptions options);

    /** Stable owning serial executor inherited from the transport; safe from any thread. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;

    /** Inspect the selected wire protocol, without implicit negotiation or downgrade. */
    virtual EHttpVersion version() const noexcept = 0;

    /** Inspect endpoint role. */
    virtual EHttpRole role() const noexcept = 0;

    /** Inspect connection-level lifecycle; a stream can fail while this remains RUNNING. */
    virtual EConnectionState state() const noexcept = 0;

    /** True if a new exchange may start: idle persistent HTTP/1 or an accepting multiplexed connection. */
    virtual bool canStartExchange() const noexcept = 0;

    /** Copy immutable transport-supplied certificate/ALPN information. */
    virtual STransportSecurity security() const = 0;

    /** Copy configuration; hard bounds cannot be weakened by peer SETTINGS. */
    virtual SHttpConnectionOptions options() const = 0;

    /** Copy the most recent connection error, not an unrelated stream's private error. */
    virtual SHttpError lastError() const = 0;

    /** Thread-safe nonblocking abort request; wake exchanges on executor, never stop a shared reactor. */
    virtual void close() noexcept = 0;

    /**
     * On executor, join connection-owned senders, protocol work and transport waitClosed after
     * close. Also supervise/join the caller-owned run() and handler tasks before stopping the
     * reactor. Cancelling this waiter never cancels cleanup or makes early destruction safe.
     */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};

/** HTTP/1 capability: sequential reuse, not concurrent pipelined execution. */
class IHttp1Connection : public virtual IHttpConnection {
public:
    /** Release the HTTP/1 capability view. */
    virtual ~IHttp1Connection() = default;

    /** True only after both message boundaries are complete and keep-alive rules permit reuse. */
    virtual bool isReusable() const noexcept = 0;
};

/** Boundaries in GOAWAY have different meanings; do not conflate them into a single last-stream field. */
enum EGoawayBoundary { EGOAWAY_H2_LAST_PROCESSED_STREAM,
    EGOAWAY_H3_FIRST_REJECTED_REQUEST_STREAM, EGOAWAY_H3_FIRST_REJECTED_PUSH_ID };

/** Copied peer shutdown notice with version-specific boundary semantics. */
struct SGoawayInfo {
    EGoawayBoundary meaning = EGOAWAY_H2_LAST_PROCESSED_STREAM;
    std::uint64_t boundary = 0;
    std::optional<std::uint64_t> protocolError;
};

/** HTTP/2/3 scheduling capability; one supervised run() owns all internal protocol pumps. */
class IHttpMultiplexedConnection : public virtual IHttpConnection {
public:
    /** Release capability ownership; no background worker is silently created. */
    virtual ~IHttpMultiplexedConnection() = default;

    /**
     * Run on a caller-owned serial executor/event loop until shutdown or failure. Exactly
     * one run may be active. This task owns and joins/cancels all internal control/stream work.
     * Its options govern connection lifetime, not individual request timeouts. Consume no
     * more than options().scheduling budgets before yielding to the same executor. An
     * instantly-ready transport/source must not prevent timers, workers or control work.
     */
    virtual asyncpp::task<ERetCode> run(SOperationOptions options = {}) = 0;

    /** Wait for protocol preface/control setup under an already active run(), or return NOT_RUNNING. */
    virtual asyncpp::task<ERetCode> ready(SOperationOptions options = {}) = 0;

    /** Accept a server request from a bounded queue; cancellation affects this waiter only. */
    virtual asyncpp::task<SAcceptResult> accept(SOperationOptions options = {}) = 0;

    /** View peer GOAWAY without inventing automatic retry or cross-origin pooling policy. */
    virtual std::optional<SGoawayInfo> peerGoaway() const = 0;

    /** Stop admitting new exchanges, signal GOAWAY and drain existing ones within the explicit deadline. */
    virtual asyncpp::task<ERetCode> gracefulShutdown(SOperationOptions options = {}) = 0;
};

/** Copied HTTP/2 send-credit observation; windows can become negative after SETTINGS changes. */
struct SHttp2FlowState {
    std::int64_t connectionSendWindow = 0;
    std::optional<std::int64_t> streamSendWindow;
};

/** HTTP/2-specific controls; frame parsing, HPACK and flow-control state remain private. */
class IHttp2Connection : public virtual IHttpMultiplexedConnection {
public:
    /** Release the HTTP/2 view. */
    virtual ~IHttp2Connection() = default;

    /** Copy received settings in bounded pages, count <= 4096; absent settings use RFC defaults. */
    virtual std::vector<SProtocolSetting> peerSettings(std::uint64_t offset, std::size_t count = 128) const = 0;

    /** Send bounded local SETTINGS changes and await ACK; never exceed configured hard limits. */
    virtual asyncpp::task<ERetCode> updateSettings(std::vector<SProtocolSetting> settings,
        SOperationOptions options = {}) = 0;

    /** Send a PING and await its matching ACK; payload is copied before suspension. */
    virtual asyncpp::task<ERetCode> ping(std::array<std::uint8_t,8> payload, SOperationOptions options = {}) = 0;

    /** Inspect credit without directly adjusting windows or issuing arbitrary WINDOW_UPDATE frames. */
    virtual SHttp2FlowState flowState(std::optional<std::uint32_t> streamId = {}) const = 0;
};

/** HTTP/3 controls; QUIC flow/congestion state is intentionally not exposed as HTTP/2 windows. */
class IHttp3Connection : public virtual IHttpMultiplexedConnection {
public:
    /** Release the HTTP/3 view. */
    virtual ~IHttp3Connection() = default;

    /** Copy peer settings in bounded pages; HTTP/3 SETTINGS are sent once, not updated like HTTP/2. */
    virtual std::vector<SProtocolSetting> peerSettings(std::uint64_t offset, std::size_t count = 128) const = 0;
};

} // namespace hnhttp
#endif
