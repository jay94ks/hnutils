#ifndef __INCLUDE_HNHTTP_TRANSPORT_HPP__
#define __INCLUDE_HNHTTP_TRANSPORT_HPP__

#include <hnhttp/common.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

namespace certpp {
namespace x509 {
class CCert;
} // namespace x509
} // namespace certpp
namespace hntls { class ITlsStream; class IByteStream; }
namespace hnquic { class IQuicConnection; }

namespace hnhttp {

/** External transport assertions, not certificate validation performed by hnhttp. */
enum EPeerAuthentication { EPEER_NOT_CHECKED, EPEER_TRANSPORT_ACCEPTED, EPEER_TRANSPORT_REJECTED };

/** Immutable-by-contract security snapshot; certificate objects are supplied by the adapter. */
struct STransportSecurity {
    bool tls = false;
    std::string alpn;
    EPeerAuthentication peerAuthentication = EPEER_NOT_CHECKED;
    /** Peer leaf first; empty is valid when no peer certificate is supplied. */
    std::vector<std::shared_ptr<const certpp::x509::CCert>> peerCertificates;
};

/**
 * External ordered byte channel. Accepted I/O starts and completes on executor(), which
 * is serial and sets asyncpp::dispatcher::current() while executing callbacks. Native I/O
 * may run elsewhere, but the adapter marshals completions before resuming the consumer.
 * No blocking I/O, nested run/runOne, future.get, hidden threads or unowned tasks are allowed.
 * Retain operation state/buffers until native completion, including cancellation races.
 * A wrong-executor rejection returns to its caller without migrating or touching I/O state.
 */
class IAsyncByteChannel {
public:
    /** Release after outstanding operations have retained their required state. */
    virtual ~IAsyncByteChannel() = default;

    /**
     * Return a stable non-null owning executor reference; safe from any thread. Sharing
     * extends lifetime, not loop activity. The caller drives it until all work is joined.
     * A socket watcher qualifies only with one runner; a raw parallel worker pool does not.
     */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;

    /** Inspect optional deadline/stop-token support on the owning executor. */
    virtual SAsyncCapabilities capabilities() const noexcept = 0;

    /**
     * Read an owned chunk no larger than maximumBytes. OK is nonempty; EOF has no bytes.
     * Adapters must honor cancellation/deadlines or reject them before I/O, never report a
     * native failure as EOF. One pending read and one pending write may coexist; a second
     * same-direction operation returns BUSY. Foreign-executor awaits return WRONG_EXECUTOR
     * before touching channel state. Unsupported options return UNSUPPORTED before I/O.
     */
    virtual asyncpp::task<SReadResult> readSome(std::size_t maximumBytes, SOperationOptions options = {}) = 0;

    /** Accept owned bytes; partial writes are allowed, zero-progress OK is not. */
    virtual asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions options = {}) = 0;

    /** End only the send direction (transport half-close or QUIC FIN), after preceding writes. */
    virtual asyncpp::task<ERetCode> finishWrite(SOperationOptions options = {}) = 0;
};

/**
 * Shared TCP/TLS-like stream lifetime for HTTP/1 and HTTP/2 only. Exactly one connection
 * holds the logical I/O lease; shared_ptr aliases must not read/write or bind a second
 * connection. Hand-off explicitly transfers that lease, not exclusive memory ownership.
 */
class IHttpStream : public virtual IAsyncByteChannel {
public:
    /** Release the external adapter. */
    virtual ~IHttpStream() = default;
    /** Adapt an established hntls stream; unavailable dummies throw not_supported before I/O. */
    static std::shared_ptr<IHttpStream> fromTls(std::shared_ptr<hntls::ITlsStream> stream);
    /** Adapt an unleased ordered stream for hntls ownership; this does not perform a handshake. */
    static std::shared_ptr<hntls::IByteStream> asTlsTransport(std::shared_ptr<IHttpStream> stream);

    /** Copy transport-supplied TLS/ALPN/certificate metadata. */
    virtual STransportSecurity security() const = 0;

    /**
     * Thread-safe nonblocking abort request; marshal to executor and wake pending work.
     * No further I/O is admitted after it is processed. Does not implement TLS shutdown.
     */
    virtual void close() noexcept = 0;

    /**
     * On executor, wait after close for all native callbacks, buffer accesses and adapter
     * work to quiesce. Never stop the shared executor. A timeout cancels only this waiter,
     * not cleanup; a successful join is required before releasing reactor resources.
     */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};

/** QUIC direction; a peer unidirectional stream cannot be written by this endpoint. */
enum EQuicStreamKind { EQUIC_BIDIRECTIONAL, EQUIC_UNIDIRECTIONAL };

/** One external QUIC stream, not an emulated connection-wide byte stream. */
class IHttpQuicStream : public virtual IAsyncByteChannel {
public:
    /** Release this stream, never an unrelated QUIC stream. */
    virtual ~IHttpQuicStream() = default;

    /** Native QUIC stream ID, in [0, 2^62-1]; zero is a valid request stream ID. */
    virtual std::uint64_t id() const noexcept = 0;

    /** Inspect stream direction. */
    virtual EQuicStreamKind kind() const noexcept = 0;

    /** Endpoint role that initiated the stream; adapter metadata must match the ID bits. */
    virtual EHttpRole initiator() const noexcept = 0;

    /** Request STOP_SENDING without closing other streams. */
    virtual void abortRead(std::uint64_t applicationError) noexcept = 0;

    /** Request RESET_STREAM without closing other streams. */
    virtual void abortWrite(std::uint64_t applicationError) noexcept = 0;
};

/** Owned result of accepting/opening an external QUIC stream. */
struct SQuicStreamResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::unique_ptr<IHttpQuicStream> stream;
};

/**
 * External QUIC/TLS connection contract. Congestion control, retransmission, transport flow
 * control and handshake are out of scope. The adapter must independently bound its buffers
 * and reserve progress for HTTP/3 control/QPACK streams. fromQuic() adapts this repository's
 * hnquic interface; it never substitutes an external QUIC engine for the current dummy.
 * Apart from executor(), close() and release, methods are executor-affine as on byte channels;
 * wrong-executor tasks and unsupported options fail before stream creation/acceptance effects.
 */
class IHttpQuicTransport {
public:
    /** Release the adapter after retained asynchronous operations complete. */
    virtual ~IHttpQuicTransport() = default;
    /** Adapt an established hnquic connection; caller supervises its native run() separately. */
    static std::unique_ptr<IHttpQuicTransport> fromQuic(std::shared_ptr<hnquic::IQuicConnection> connection);

    /**
     * Stable serial executor, shared with every returned QUIC stream. Same affinity,
     * ownership and continuation rules as IAsyncByteChannel; callable from any thread.
     */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;

    /** Inspect cancellation/deadline support for acceptStream/openStream. */
    virtual SAsyncCapabilities capabilities() const noexcept = 0;

    /** Return negotiated TLS/ALPN metadata; HTTP/3 requires the selected h3 protocol. */
    virtual STransportSecurity security() const = 0;

    /** Require an established handshake for this initial contract; no implicit 0-RTT request replay. */
    virtual bool handshakeConfirmed() const noexcept = 0;

    /** Accept a peer uni/bidirectional stream without conflating their byte ordering. */
    virtual asyncpp::task<SQuicStreamResult> acceptStream(SOperationOptions options = {}) = 0;

    /** Open a local stream, waiting for external QUIC stream credit as needed. */
    virtual asyncpp::task<SQuicStreamResult> openStream(EQuicStreamKind kind, SOperationOptions options = {}) = 0;

    /** Thread-safe nonblocking abort request; wake all pending stream operations on executor. */
    virtual void close(std::uint64_t applicationError) noexcept = 0;

    /** Join adapter/native completion work after close, with the same cleanup rules as IHttpStream. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};

} // namespace hnhttp
#endif
