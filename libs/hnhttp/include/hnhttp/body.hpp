#ifndef __INCLUDE_HNHTTP_BODY_HPP__
#define __INCLUDE_HNHTTP_BODY_HPP__

#include <hnhttp/fields.hpp>
#include <asyncpp/task.h>

namespace hnhttp {

/** Metadata of a prepared outgoing source; unknown length is not an empty body. */
struct SBodyMetadata {
    std::string contentType = "application/octet-stream";
    std::optional<std::uint64_t> contentLength;
    bool replayable = false;
    std::vector<std::string> trailerNames;
};

/**
 * Outgoing entity source, independent from transport and HTTP wire framing. Implementations
 * own their state. Payload Content-Encoding is not automatically added or removed.
 * Once attached, all methods/callbacks run on the connection executor and must not block it.
 * A source may offload copied work but must marshal back before completing; prepare/read
 * must cooperatively yield within the connection budgets, including multipart preflight.
 */
class HNHTTP_API IHttpBodySource {
public:
    /** Release without implicitly rewinding, reading or opening files. */
    virtual ~IHttpBodySource() = default;

    /** Create an owning, replayable binary source. */
    static std::shared_ptr<IHttpBodySource> fromBytes(SBytes bytes,
        std::string contentType = "application/octet-stream");

    /** Freeze/preflight before any HTTP bytes are sent; idempotent without implicit rewind. */
    virtual asyncpp::task<ERetCode> prepare(SOperationOptions options = {}) = 0;

    /** Copy prepared metadata; querying before successful prepare throws a state error. */
    virtual SBodyMetadata metadata() const = 0;

    /** Produce at most maximumBytes; one read at a time, with owned output and explicit EOF. */
    virtual asyncpp::task<SReadResult> readSome(std::size_t maximumBytes, SOperationOptions options = {}) = 0;

    /** Explicitly rewind a replayable source, or return UNSUPPORTED before consuming anything. */
    virtual asyncpp::task<ERetCode> rewind(SOperationOptions options = {}) = 0;

    /** Copy bounded trailers after source EOF; names must match prepared trailerNames. */
    virtual std::vector<SHeaderField> trailers() const = 0;

    /** Copy the latest source diagnostic. */
    virtual SHttpError lastError() const = 0;
};

/**
 * Incoming entity reader: HTTP/1 chunk framing or HTTP/2/3 DATA framing is removed, but
 * content codings and application encodings remain untouched. It retains its exchange.
 * Calls and completion are affine to the connection executor; workers consume copied chunks,
 * not a reader/facade concurrently. Awaiting must suspend without blocking protocol progress.
 */
class IHttpBodyReader {
public:
    /** Release without unbounded draining. An unfinished exchange cannot be reused as-is. */
    virtual ~IHttpBodyReader() = default;

    /** Read an owned bounded chunk, advancing receive credit only after delivery/discard. */
    virtual asyncpp::task<SReadResult> readSome(std::size_t maximumBytes = 65536,
        SOperationOptions options = {}) = 0;

    /** Explicitly consume at most maximumBytes; return LIMIT if more body data remains. */
    virtual asyncpp::task<ERetCode> discard(std::uint64_t maximumBytes,
        SOperationOptions options = {}) = 0;

    /** Collect within an explicit allocation ceiling; OK contains the entire entity, possibly empty. */
    virtual asyncpp::task<SReadResult> collect(std::size_t maximumBytes = 8*1024*1024,
        SOperationOptions options = {}) = 0;

    /** True only when framing, terminal markers and trailers have been validated. */
    virtual bool isComplete() const noexcept = 0;

    /** Copied trailer page source; accessing before completion throws a state error. */
    virtual const IReadOnlyHttpHeaders& trailers() const = 0;

    /** Copy the latest body/protocol/transport diagnostic. */
    virtual SHttpError lastError() const = 0;
};

} // namespace hnhttp
#endif
