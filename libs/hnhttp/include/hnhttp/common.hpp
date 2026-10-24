#ifndef __INCLUDE_HNHTTP_COMMON_HPP__
#define __INCLUDE_HNHTTP_COMMON_HPP__

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace hnhttp {

#if defined(_WIN32) && defined(HNHTTP_SHARED)
#if defined(HNHTTP_BUILDING_LIBRARY)
#define HNHTTP_API __declspec(dllexport)
#else
#define HNHTTP_API __declspec(dllimport)
#endif
#else
#define HNHTTP_API
#endif

/** Owned protocol/body bytes; asynchronous operations never retain caller spans. */
using SBytes = std::vector<std::uint8_t>;

/** Protocol choice, not an implementation-availability claim or an ALPN negotiator. */
enum EHttpVersion { HTTP_10 = 10, HTTP_11 = 11, HTTP_2 = 20, HTTP_3 = 30 };

/** Known methods; extension methods retain their original token separately. */
enum EHttpMethod { HTTP_INVALID, HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_CONNECT,
    HTTP_OPTIONS, HTTP_TRACE, HTTP_PATCH, HTTP_EXTENSION };

/** Connection endpoint role, independent from an individual stream's initiator. */
enum EHttpRole { HTTP_CLIENT, HTTP_SERVER };

/** Operation outcomes; HTTP status codes such as 404 are not transport/protocol failures. */
enum ERetCode { ERET_OK, ERET_EOF, ERET_INVALID_ARGUMENT, ERET_INVALID_STATE,
    ERET_BUSY, ERET_NOT_RUNNING, ERET_MALFORMED, ERET_TRUNCATED, ERET_LIMIT,
    ERET_IO, ERET_CANCELLED, ERET_TIMEOUT, ERET_CLOSED, ERET_UNSUPPORTED,
    ERET_BODY_PENDING, ERET_STREAM_RESET, ERET_GOAWAY, ERET_BOUNDARY_COLLISION,
    ERET_WRONG_EXECUTOR, ERET_BLOCKED, ERET_AUTHENTICATION, ERET_LENGTH_REQUIRED };

/** Error containment: a multiplexed stream failure need not invalidate its connection. */
enum EErrorScope { EERROR_OPERATION, EERROR_STREAM, EERROR_CONNECTION };

/** Retry evidence only; the library never retries, even when a peer reports non-processing. */
enum ERetryEvidence { ERETRY_UNKNOWN, ERETRY_NOT_PROCESSED };

/** Copied diagnostic; protocolError retains the native HTTP/2, HTTP/3 or QPACK error number. */
struct SHttpError {
    ERetCode code = ERET_OK;
    EErrorScope scope = EERROR_OPERATION;
    std::error_code cause{};
    std::string message{};
    std::optional<std::uint64_t> streamId{};
    std::optional<std::uint64_t> protocolError{};
    std::optional<std::uint64_t> wireOffset{};
    ERetryEvidence retry = ERETRY_UNKNOWN;
};

/**
 * Absolute deadline and cooperative cancellation. Stop callbacks may run on any thread;
 * they only post to the owning executor, never mutate protocol state or resume I/O inline.
 * Unsupported policies fail before I/O; cancellation never means destroying an active task.
 */
struct SOperationOptions {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::stop_token cancellation;
};

/** Owned chunk: OK means nonempty bytes; EOF means empty bytes and a validated boundary. */
struct SReadResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    SBytes bytes;
};

/** Bytes accepted by a write, not a claim that the peer has processed them. */
struct SWriteResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::size_t acceptedBytes = 0;
};

/** Optional external I/O policies; abort-and-join on connection close is always mandatory. */
struct SAsyncCapabilities {
    bool cancellation = false;
    bool deadlines = false;
};

/**
 * Positive per-connection scheduling budgets; exhausted work posts a continuation instead
 * of monopolizing a reactor. Count every frame/chunk/control step, including buffered work.
 * Byte limits bound incremental processing, not the size of an accepted complete message.
 * Share budgets across the connection's work; reset only after a posted continuation,
 * not after an immediately-ready nested await. Zero values are invalid at bind/creation.
 */
struct SHttpSchedulingOptions {
    std::size_t maximumOperationsPerTurn = 64;
    std::size_t maximumBytesPerTurn = 256*1024;
};

/** Local hard limits, applied in addition to peer-advertised protocol limits. */
struct SHttpLimits {
    std::size_t maximumStartLineBytes = 8192;
    std::size_t maximumHeaderBytes = 65536;
    std::size_t maximumHeaderFields = 128;
    std::size_t maximumTrailerBytes = 16384;
    std::size_t maximumQueryFields = 1024;
    std::size_t maximumDecodedQueryBytes = 65536;
    std::uint64_t maximumBodyBytes = UINT64_C(64)*1024*1024*1024;
    std::size_t ioChunkBytes = 65536;
    std::size_t maximumReadAheadBytes = 131072;
    std::size_t maximumStreamBufferedBytes = 256*1024;
    std::size_t maximumConnectionBufferedBytes = 16*1024*1024;
    std::size_t maximumPendingRequests = 128;
    std::size_t maximumPeerSettings = 128;
    std::size_t maximumHeaderCompressionTableBytes = 65536;
    std::size_t maximumPendingControlFrames = 128;
    std::size_t maximumInformationalResponses = 16;
    std::size_t maximumChunkLineBytes = 4096;
    std::uint64_t maximumMessageFrames = 1024*1024;
};

/** Local HTTP/2 SETTINGS; server push is initially disabled and not exposed as an API. */
struct SHttp2Options {
    std::uint32_t headerTableSize = 4096;
    std::uint32_t maximumConcurrentStreams = 100;
    std::uint32_t initialStreamWindow = 65535;
    std::uint32_t maximumFrameSize = 16384;
    std::uint32_t maximumHeaderListSize = 65536;
    std::size_t maximumEncodedHeaderBytes = 131072;
    std::size_t maximumContinuationFrames = 32;
    std::size_t maximumOutstandingSettings = 4;
    std::size_t maximumOutstandingPings = 16;
};

/** HTTP/3/QPACK limits; QUIC transport flow control is not implemented by hnhttp. */
struct SHttp3Options {
    std::uint64_t qpackMaximumTableCapacity = 4096;
    std::uint64_t qpackBlockedStreams = 16;
    std::uint64_t maximumFieldSectionSize = 65536;
    std::size_t maximumEncodedFieldSectionBytes = 131072;
    std::size_t maximumBlockedHeaderBytes = 1024*1024;
    std::size_t maximumControlFrameBytes = 65536;
    std::size_t maximumUnknownStreamBytes = 1024*1024;
};

/** HTTP/1 origin-server framing policy; HTTP/2/3 use stream termination instead. */
struct SHttp1Options {
    /** Opt in to HTTP/1.0 Connection: keep-alive; default remains close after an exchange. */
    bool allowHttp10KeepAlive = false;
    /**
     * Reject POST/PUT/PATCH or Content-Type-bearing requests without Content-Length or
     * chunked framing, sending 411 and closing before exposing an application request.
     * Disable to apply RFC 9112's zero-length interpretation, including an empty POST.
     */
    bool requireExplicitRequestBodyLength = true;
};

/** Immutable connection configuration; protocol selection is explicit, never silently downgraded. */
struct SHttpConnectionOptions {
    EHttpVersion version = HTTP_11;
    SHttpLimits limits;
    SHttp2Options http2;
    SHttp3Options http3;
    SHttpSchedulingOptions scheduling;
    SHttp1Options http1;
};

/** Connection lifecycle, not a substitute for protocol-specific stream state. */
enum EConnectionState { ECONNECTION_IDLE, ECONNECTION_RUNNING, ECONNECTION_DRAINING,
    ECONNECTION_HANDOFF, ECONNECTION_CLOSED, ECONNECTION_FAILED };

/** Request-target forms; an encoded path is never silently decoded or normalized. */
enum ERequestTargetForm { ETARGET_ORIGIN, ETARGET_ABSOLUTE, ETARGET_AUTHORITY, ETARGET_ASTERISK };

/** Copied semantic target; query fields are managed separately. */
struct SRequestTarget {
    ERequestTargetForm form = ETARGET_ORIGIN;
    /** Empty derives http/https from transport metadata during preparation. */
    std::string scheme;
    std::string authority;
    std::string encodedPath = "/";
};

/** Wire framing override; CHUNKED/CLOSE are HTTP/1-only and rejected for HTTP/2/3. */
enum EBodyFraming { EFRAMING_AUTO, EFRAMING_CONTENT_LENGTH, EFRAMING_CHUNKED, EFRAMING_CLOSE };

/** Outgoing policy; Expect is opt-in and never implies automatic retry after failure. */
struct SSendOptions {
    EBodyFraming framing = EFRAMING_AUTO;
    bool keepAlive = true;
    bool expectContinue = false;
};

/** Explicit bounded draining at exchange completion; disabled by default. */
struct SFinishOptions {
    bool discardUnreadBody = false;
    std::uint64_t maximumDiscardBytes = 8*1024*1024;
};

/** Copied setting; HTTP/2 uses 16/32-bit fields, HTTP/3 uses QUIC variable-length integers. */
struct SProtocolSetting {
    std::uint64_t identifier = 0;
    std::uint64_t value = 0;
};

} // namespace hnhttp
#endif
