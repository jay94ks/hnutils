#ifndef __INCLUDE_HNCOMP_COMPRESSOR_HPP__
#define __INCLUDE_HNCOMP_COMPRESSOR_HPP__

#include <hncomp/common.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>
#include <memory>

namespace hncomp {

/** Stateless whole-message compression; independent from streaming interfaces. */
class HNCOMP_API ICompressor : public std::enable_shared_from_this<ICompressor> {
public:
    /** Release the compressor. */
    virtual ~ICompressor();

    /** Construct a validated self-contained implementation. */
    static std::shared_ptr<ICompressor> create(SCompressionOptions options = {});

    /** Compress a complete message, returning an owned standard/private wire representation. */
    virtual SBytes compress(SReadOnlyByteSpan input) const = 0;

    /** Capture input and shared object lifetime immediately; execute on a caller-owned worker. */
    asyncpp::task<SBytes> compressAsync(SBytes input, asyncpp::dispatcher& worker);
};

/** Stateless whole-message decoding; rejects truncation and trailing garbage. */
class HNCOMP_API IDecompressor : public std::enable_shared_from_this<IDecompressor> {
public:
    /** Release the decompressor. */
    virtual ~IDecompressor();

    /** Construct a validated implementation with bounded expansion. */
    static std::shared_ptr<IDecompressor> create(SCompressionOptions options = {});

    /** Decode exactly one complete stream/message; concatenation policy is format-specific. */
    virtual SBytes decompress(SReadOnlyByteSpan input) const = 0;

    /** Own input and object lifetime; worker must outlive and execute the task. */
    asyncpp::task<SBytes> decompressAsync(SBytes input, asyncpp::dispatcher& worker);
};

} // namespace hncomp

#endif
