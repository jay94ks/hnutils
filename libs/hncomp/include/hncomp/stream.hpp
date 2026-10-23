#ifndef __INCLUDE_HNCOMP_STREAM_HPP__
#define __INCLUDE_HNCOMP_STREAM_HPP__

#include <hncomp/compressor.hpp>

namespace hncomp {

/**
 * Stateful bounded-output encoder, not an ICompressor subclass.
 * Serialize operations on an instance; repeat pending flush with the unconsumed suffix.
 */
class HNCOMP_API IStreamCompressor : public std::enable_shared_from_this<IStreamCompressor> {
public:
    /** Release the stream. */
    virtual ~IStreamCompressor();

    /** Construct a streaming implementation or reject an unsupported wire mode. */
    static std::shared_ptr<IStreamCompressor> create(SCompressionOptions options = {});

    /** Accept an input prefix and initialize an output prefix; neither span is retained. */
    virtual SStreamResult process(SReadOnlyByteSpan input, SByteSpan output,
        EFlushMode flush = EFLUSH_NONE) = 0;

    /** Query support without silently downgrading flush behavior. */
    virtual bool supportsFlush(EFlushMode mode) const noexcept = 0;

    /** Inspect a copied lifecycle state. */
    virtual EStreamState state() const noexcept = 0;

    /** Discard history, pending output, counters and failure state. */
    virtual void reset() = 0;

    /** Own input/output and retain this stream; await completion before starting another operation. */
    asyncpp::task<SStreamChunk> processAsync(SBytes input, std::size_t outputCapacity,
        EFlushMode flush, asyncpp::dispatcher& worker);
};

/** Stateful decoder with explicit end-of-input, independent from IDecompressor. */
class HNCOMP_API IStreamDecompressor : public std::enable_shared_from_this<IStreamDecompressor> {
public:
    /** Release the stream. */
    virtual ~IStreamDecompressor();

    /** Construct a bounded-history streaming decoder. */
    static std::shared_ptr<IStreamDecompressor> create(SCompressionOptions options = {});

    /** Decode an input prefix; FINAL with an incomplete stream throws EERROR_TRUNCATED. */
    virtual SStreamResult process(SReadOnlyByteSpan input, SByteSpan output,
        EInputMode mode = EINPUT_MORE) = 0;

    /** Inspect a copied lifecycle state. */
    virtual EStreamState state() const noexcept = 0;

    /** Start a new independent stream with the original options. */
    virtual void reset() = 0;

    /** Own input/output and retain the stream; worker must outlive the task. */
    asyncpp::task<SStreamChunk> processAsync(SBytes input, std::size_t outputCapacity,
        EInputMode mode, asyncpp::dispatcher& worker);
};

} // namespace hncomp

#endif
