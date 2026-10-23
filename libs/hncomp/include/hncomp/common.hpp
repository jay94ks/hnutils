#ifndef __INCLUDE_HNCOMP_COMMON_HPP__
#define __INCLUDE_HNCOMP_COMMON_HPP__

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(_WIN32) && defined(HNCOMP_SHARED)
#if defined(HNCOMP_BUILDING_LIBRARY)
#define HNCOMP_API __declspec(dllexport)
#else
#define HNCOMP_API __declspec(dllimport)
#endif
#else
#define HNCOMP_API
#endif

namespace hncomp {

/** Owned binary input or output. */
using SBytes = std::vector<std::uint8_t>;

/** Bytes borrowed only during a synchronous operation. */
using SReadOnlyByteSpan = std::span<const std::uint8_t>;

/** Caller-owned output storage. */
using SByteSpan = std::span<std::uint8_t>;

/** Algorithm identity, independent of framing. */
enum EAlgorithm {
    EALGORITHM_LZ77, EALGORITHM_LZ78, EALGORITHM_LZMA, EALGORITHM_DEFLATE,
    EALGORITHM_BZIP2, EALGORITHM_ZSTD, EALGORITHM_BROTLI, EALGORITHM_SNAPPY, EALGORITHM_LZ4,
};

/** Explicit wire container; DEFAULT selects the documented algorithm default. */
enum EFormat {
    EFORMAT_DEFAULT, EFORMAT_RAW, EFORMAT_ZLIB, EFORMAT_GZIP, EFORMAT_FRAMED, EFORMAT_ALONE,
    EFORMAT_CHUNKED, // --> Explicit hncomp HNCS container, not an HTTP content coding.
};

/** Compression, decompression and stream allocation policy; copied by factories. */
struct SCompressionOptions {
    /** Selected lossless algorithm. */
    EAlgorithm algorithm = EALGORITHM_DEFLATE;

    /** DEFAULT means zlib for DEFLATE, framed for LZ4/Snappy and alone for LZMA. */
    EFormat format = EFORMAT_DEFAULT;

    /** Search effort, from one through nine; not a cross-codec ratio guarantee. */
    unsigned level = 5;

    /** Maximum total accepted input bytes per operation or reset interval. */
    std::size_t maxInputBytes = 64 * 1024 * 1024;

    /** Maximum total produced bytes per operation or reset interval. */
    std::size_t maxOutputBytes = 64 * 1024 * 1024;

    /** Maximum permitted history/dictionary window, not a whole-process memory quota. */
    std::size_t maxWindowBytes = 8 * 1024 * 1024;

    /** LZ78 entry ceiling; a full dictionary is frozen rather than reset. */
    std::size_t maxDictionaryEntries = 65536;

    /** Preferred independent block size, between 1 KiB and 64 KiB. */
    std::size_t blockBytes = 32 * 1024;
};

/** Normal progress outcomes; malformed data and policy violations throw. */
enum EStreamStatus {
    ESTREAM_PROGRESS, ESTREAM_NEED_INPUT, ESTREAM_NEED_OUTPUT, ESTREAM_FLUSHED, ESTREAM_FINISHED,
};

/** Flush requests are not interchangeable; unsupported modes are rejected. */
enum EFlushMode { EFLUSH_NONE, EFLUSH_PARTIAL, EFLUSH_SYNC, EFLUSH_FULL, EFLUSH_FINISH };

/** FINAL promises that no bytes follow the provided input suffix. */
enum EInputMode { EINPUT_MORE, EINPUT_FINAL };

/** State changes only through process/reset, never by modifying returned values. */
enum EStreamState { ESTATE_RUNNING, ESTATE_FINISHING, ESTATE_FINISHED, ESTATE_FAILED };

/** Counts apply only to the input and output spans of the current call. */
struct SStreamResult {
    /** Input prefix accepted by the stream. */
    std::size_t consumed = 0;

    /** Output prefix initialized by the stream. */
    std::size_t produced = 0;

    /** Reason the operation returned. */
    EStreamStatus status = ESTREAM_NEED_INPUT;
};

/** Owned async result; no unconsumed input disappears when an input vector is moved. */
struct SStreamChunk {
    /** Per-call progress, with produced equal to output.size(). */
    SStreamResult progress;

    /** Owned produced bytes. */
    SBytes output;

    /** Owned input suffix to supply again, possibly empty. */
    SBytes remainingInput;
};

/** Stable failure categories, separate from normal backpressure. */
enum EError {
    EERROR_INVALID_DATA, EERROR_TRUNCATED, EERROR_UNSUPPORTED, EERROR_LIMIT, EERROR_STATE,
    EERROR_AUTHENTICATION,
};

/** Compression failure with immutable, encapsulated category. */
class HNCOMP_API CCompressionError : public std::runtime_error {
private:
    EError _code;

public:
    /** Construct a categorized diagnostic. */
    CCompressionError(EError code, const char* message);

    /** Return the immutable error category. */
    EError code() const noexcept;
};

} // namespace hncomp

#endif
