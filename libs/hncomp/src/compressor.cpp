#include <hncomp/compressor.hpp>

#include "internal.hpp"

#include <coroutine>

namespace hncomp {
namespace {

using namespace detail;

SBytes deflateWhole(SReadOnlyByteSpan input, SCompressionOptions options, bool compressing) {
    SBytes output; std::array<std::uint8_t, 16384> buffer{}; std::size_t pos = 0;
    auto encoder = compressing ? (options.format == EFORMAT_CHUNKED ? chunkedEncoder(options) : deflateEncoder(options)) : nullptr;
    auto decoder = !compressing ? (options.format == EFORMAT_CHUNKED ? chunkedDecoder(options) : deflateDecoder(options)) : nullptr;
    for (;;) {
        const auto progress = compressing ? encoder->process(input.subspan(pos), buffer, EFLUSH_FINISH) :
            decoder->process(input.subspan(pos), buffer, EINPUT_FINAL);
        pos += progress.consumed; output.insert(output.end(), buffer.begin(), buffer.begin() + progress.produced);
        if (progress.status == ESTREAM_FINISHED) {
            if (pos != input.size()) { invalid("trailing bytes after compressed stream"); }
            return output;
        }
        if (!progress.consumed && !progress.produced) { invalid("codec made no forward progress"); }
    }
}

SBytes transform(SReadOnlyByteSpan input, const SCompressionOptions& options, bool compressing) {
    if (input.size() > options.maxInputBytes) { limit("input size exceeds policy"); }
    SBytes output;
    try {
        if (options.format == EFORMAT_CHUNKED) { return deflateWhole(input, options, compressing); }
        switch (options.algorithm) {
        case EALGORITHM_LZ77: output = compressing ? encodeLz77(input, options) : decodeLz77(input, options); break;
        case EALGORITHM_LZ78: output = compressing ? encodeLz78(input, options) : decodeLz78(input, options); break;
        case EALGORITHM_LZMA: output = compressing ? encodeLzma(input, options) : decodeLzma(input, options); break;
        case EALGORITHM_DEFLATE: output = deflateWhole(input, options, compressing); break;
        case EALGORITHM_BZIP2: output = compressing ? encodeBzip2(input, options) : decodeBzip2(input, options); break;
        case EALGORITHM_ZSTD: output = compressing ? encodeZstd(input, options) : decodeZstd(input, options); break;
        case EALGORITHM_BROTLI: output = compressing ? encodeBrotli(input, options) : decodeBrotli(input, options); break;
        case EALGORITHM_SNAPPY: output = compressing ? encodeSnappy(input, options) : decodeSnappy(input, options); break;
        case EALGORITHM_LZ4: output = compressing ? encodeLz4(input, options) : decodeLz4(input, options); break;
        }
    } catch (const NeedInput&) { throw CCompressionError(EERROR_TRUNCATED, "truncated compressed input"); }
    if (output.size() > options.maxOutputBytes) { limit("output size exceeds policy"); }
    return output;
}

class Compressor final : public ICompressor {
private:
    struct Impl { SCompressionOptions options; };
    std::unique_ptr<const Impl> _impl;

public:
    explicit Compressor(SCompressionOptions options) : _impl(std::make_unique<Impl>(Impl{options})) {}
    SBytes compress(SReadOnlyByteSpan input) const override { return transform(input, _impl->options, true); }
};

class Decompressor final : public IDecompressor {
private:
    struct Impl { SCompressionOptions options; };
    std::unique_ptr<const Impl> _impl;

public:
    explicit Decompressor(SCompressionOptions options) : _impl(std::make_unique<Impl>(Impl{options})) {}
    SBytes decompress(SReadOnlyByteSpan input) const override { return transform(input, _impl->options, false); }
};

struct DispatchOn {
    asyncpp::dispatcher* worker;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> continuation) const { worker->push([continuation] { continuation.resume(); }); }
    void await_resume() const noexcept {}
};

asyncpp::task<SBytes> compressOn(std::shared_ptr<ICompressor> compressor, SBytes input, asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; co_return compressor->compress(input);
}

asyncpp::task<SBytes> decompressOn(std::shared_ptr<IDecompressor> decompressor, SBytes input, asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; co_return decompressor->decompress(input);
}

template<typename T, typename TMode>
asyncpp::task<SStreamChunk> processOn(std::shared_ptr<T> stream, SBytes input,
    std::size_t capacity, TMode mode, asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker};
    if (capacity > 1024 * 1024) { limit("async output capacity exceeds 1 MiB per call"); }
    SStreamChunk chunk; chunk.output.resize(capacity);
    chunk.progress = stream->process(input, chunk.output, mode);
    chunk.output.resize(chunk.progress.produced);
    chunk.remainingInput.assign(input.begin() + chunk.progress.consumed, input.end());
    co_return chunk;
}

} // namespace

/* Release polymorphic instances. */
ICompressor::~ICompressor() = default;
IDecompressor::~IDecompressor() = default;
IStreamCompressor::~IStreamCompressor() = default;
IStreamDecompressor::~IStreamDecompressor() = default;

/* Validate before constructing private implementations. */
std::shared_ptr<ICompressor> ICompressor::create(SCompressionOptions options) { return std::make_shared<Compressor>(detail::validate(options)); }

/* Capture independent decoding policy. */
std::shared_ptr<IDecompressor> IDecompressor::create(SCompressionOptions options) { return std::make_shared<Decompressor>(detail::validate(options)); }

/* Own input and lifetime before the lazy task first resumes. */
asyncpp::task<SBytes> ICompressor::compressAsync(SBytes input, asyncpp::dispatcher& worker) { return compressOn(shared_from_this(), std::move(input), &worker); }

/* Decode away from the initiating thread. */
asyncpp::task<SBytes> IDecompressor::decompressAsync(SBytes input, asyncpp::dispatcher& worker) { return decompressOn(shared_from_this(), std::move(input), &worker); }

/* Reject unsupported streaming implementations, never substitute a different algorithm. */
std::shared_ptr<IStreamCompressor> IStreamCompressor::create(SCompressionOptions options) {
    options = detail::validate(options);
    if (options.format == EFORMAT_CHUNKED) { return detail::chunkedEncoder(options); }
    if (options.algorithm == EALGORITHM_DEFLATE) { return detail::deflateEncoder(options); }
    detail::unsupported("native streaming for this algorithm is not implemented yet");
}

/* A decompressor is not implemented by accumulating an unbounded whole stream. */
std::shared_ptr<IStreamDecompressor> IStreamDecompressor::create(SCompressionOptions options) {
    options = detail::validate(options);
    if (options.format == EFORMAT_CHUNKED) { return detail::chunkedDecoder(options); }
    if (options.algorithm == EALGORITHM_DEFLATE) { return detail::deflateDecoder(options); }
    detail::unsupported("native streaming for this algorithm is not implemented yet");
}

/* Own the unconsumed suffix in the result. */
asyncpp::task<SStreamChunk> IStreamCompressor::processAsync(SBytes input, std::size_t capacity,
    EFlushMode mode, asyncpp::dispatcher& worker) { return processOn(shared_from_this(), std::move(input), capacity, mode, &worker); }

/* The caller retains no borrowed buffer across a suspension. */
asyncpp::task<SStreamChunk> IStreamDecompressor::processAsync(SBytes input, std::size_t capacity,
    EInputMode mode, asyncpp::dispatcher& worker) { return processOn(shared_from_this(), std::move(input), capacity, mode, &worker); }

} // namespace hncomp
