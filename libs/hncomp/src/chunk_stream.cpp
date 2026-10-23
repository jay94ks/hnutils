#include "internal.hpp"

#include <optional>

namespace hncomp {
namespace detail {
namespace {

SCompressionOptions innerOptions(SCompressionOptions options) {
    options.format = options.algorithm == EALGORITHM_LZMA ? EFORMAT_ALONE : EFORMAT_RAW;
    return validate(options);
}

class ChunkEncoder final : public IStreamCompressor {
private:
    struct Impl {
        SCompressionOptions options, inner;
        SBytes pending;
        std::size_t position = 0, accepted = 0, produced = 0;
        EStreamState state = ESTATE_RUNNING;
        std::optional<EFlushMode> flushing;

        explicit Impl(SCompressionOptions config) : options(config), inner(innerOptions(config)) {
            pending = {'H','N','C','S',1,static_cast<std::uint8_t>(inner.algorithm),static_cast<std::uint8_t>(inner.format),0};
        }
    };
    std::unique_ptr<Impl> _impl;

public:
    explicit ChunkEncoder(SCompressionOptions options) : _impl(std::make_unique<Impl>(options)) {}
    bool supportsFlush(EFlushMode mode) const noexcept override {
        return mode == EFLUSH_NONE || mode == EFLUSH_SYNC || mode == EFLUSH_FULL || mode == EFLUSH_FINISH;
    }
    EStreamState state() const noexcept override { return _impl->state; }
    void reset() override { _impl = std::make_unique<Impl>(_impl->options); }

    SStreamResult process(SReadOnlyByteSpan input, SByteSpan output, EFlushMode mode) override {
        auto& s = *_impl; SStreamResult result;
        if (!supportsFlush(mode)) { unsupported("HNCS supports NONE, SYNC, FULL and FINISH, not PARTIAL"); }
        if (s.state == ESTATE_FAILED || (s.state == ESTATE_FINISHED && !input.empty())) { throw CCompressionError(EERROR_STATE, "reset completed/failed HNCS encoder"); }
        if (s.state == ESTATE_FINISHED) { return {0,0,ESTREAM_FINISHED}; }
        if (s.flushing && (*s.flushing != mode || !input.empty())) { throw CCompressionError(EERROR_STATE, "drain HNCS flush before new input"); }
        try {
            for (;;) {
                const auto count = std::min(output.size() - result.produced, s.pending.size() - s.position);
                if (count > s.options.maxOutputBytes - s.produced) { limit("HNCS output limit exceeded"); }
                std::copy_n(s.pending.begin() + s.position, count, output.begin() + result.produced);
                s.position += count; s.produced += count; result.produced += count;
                if (s.position != s.pending.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                s.pending.clear(); s.position = 0;
                if (s.flushing) {
                    result.status = *s.flushing == EFLUSH_FINISH ? ESTREAM_FINISHED : ESTREAM_FLUSHED;
                    if (*s.flushing == EFLUSH_FINISH) { s.state = ESTATE_FINISHED; }
                    s.flushing.reset(); return result;
                }
                if (result.produced == output.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                if (result.consumed < input.size()) {
                    const auto chunk = input.subspan(result.consumed, std::min(s.options.blockBytes, input.size() - result.consumed));
                    if (chunk.size() > s.options.maxInputBytes - s.accepted) { limit("HNCS input limit exceeded"); }
                    auto options = s.inner; options.maxInputBytes = chunk.size(); options.maxOutputBytes = 1024 * 1024;
                    const auto compressed = ICompressor::create(options)->compress(chunk);
                    putLe(s.pending, compressed.size(), 4); putLe(s.pending, chunk.size(), 4); putLe(s.pending, crc32(chunk), 4);
                    s.pending.insert(s.pending.end(), compressed.begin(), compressed.end());
                    result.consumed += chunk.size(); s.accepted += chunk.size();
                    if (result.consumed == input.size() && mode != EFLUSH_NONE) {
                        s.flushing = mode;
                        if (mode == EFLUSH_FINISH) {
                            s.state = ESTATE_FINISHING; s.pending.insert(s.pending.end(), 12, 0); putLe(s.pending, s.accepted, 8);
                        }
                    }
                } else {
                    if (mode == EFLUSH_NONE) { result.status = ESTREAM_NEED_INPUT; return result; }
                    s.flushing = mode;
                    if (mode == EFLUSH_FINISH) {
                        s.state = ESTATE_FINISHING; s.pending.assign(12, 0); putLe(s.pending, s.accepted, 8);
                    }
                }
            }
        } catch (...) { s.state = ESTATE_FAILED; throw; }
    }
};

class ChunkDecoder final : public IStreamDecompressor {
private:
    struct Impl {
        enum Stage { Header, Record, Payload, Footer } stage = Header;
        SCompressionOptions options, inner;
        SBytes input, pending;
        std::size_t wanted = 8, position = 0, accepted = 0, produced = 0;
        unsigned raw = 0, checksum = 0;
        EStreamState state = ESTATE_RUNNING;
        explicit Impl(SCompressionOptions config) : options(config), inner(innerOptions(config)) {}

        void parse() {
            Reader reader(input);
            if (stage == Header) {
                if (reader.le(4) != 0x53434e48u || reader.byte() != 1 || reader.byte() != inner.algorithm ||
                    reader.byte() != inner.format || reader.byte() != 0) { invalid("HNCS header/selected algorithm mismatch"); }
                stage = Record; wanted = 12;
            } else if (stage == Record) {
                const auto encoded = reader.le(4); raw = static_cast<unsigned>(reader.le(4)); checksum = static_cast<unsigned>(reader.le(4));
                if (encoded == 0) {
                    if (raw || checksum) { invalid("invalid HNCS end marker"); }
                    stage = Footer; wanted = 8;
                } else {
                    if (encoded > 1024 * 1024 || raw == 0 || raw > 65536) { invalid("HNCS record exceeds format block bounds"); }
                    if (raw > options.maxOutputBytes - produced) { limit("HNCS declared output exceeds policy"); }
                    stage = Payload; wanted = static_cast<std::size_t>(encoded);
                }
            } else if (stage == Payload) {
                auto policy = inner; policy.maxInputBytes = input.size(); policy.maxOutputBytes = raw;
                auto decoded = IDecompressor::create(policy)->decompress(input);
                if (decoded.size() != raw || crc32(decoded) != checksum) { invalid("HNCS record size/checksum mismatch"); }
                pending = std::move(decoded); position = 0; stage = Record; wanted = 12;
            } else {
                if (reader.le(8) != produced) { invalid("HNCS total content size mismatch"); }
                state = ESTATE_FINISHED;
            }
            input.clear();
        }
    };
    std::unique_ptr<Impl> _impl;

public:
    explicit ChunkDecoder(SCompressionOptions options) : _impl(std::make_unique<Impl>(options)) {}
    EStreamState state() const noexcept override { return _impl->state; }
    void reset() override { _impl = std::make_unique<Impl>(_impl->options); }

    SStreamResult process(SReadOnlyByteSpan input, SByteSpan output, EInputMode mode) override {
        auto& s = *_impl; SStreamResult result;
        if (mode != EINPUT_MORE && mode != EINPUT_FINAL) { throw std::invalid_argument("invalid end-of-input mode"); }
        if (s.state == ESTATE_FAILED) { throw CCompressionError(EERROR_STATE, "reset failed HNCS decoder"); }
        try {
            for (;;) {
                const auto count = std::min(output.size() - result.produced, s.pending.size() - s.position);
                std::copy_n(s.pending.begin() + s.position, count, output.begin() + result.produced);
                s.position += count; s.produced += count; result.produced += count;
                if (s.position != s.pending.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                s.pending.clear(); s.position = 0;
                if (s.state == ESTATE_FINISHED) { result.status = ESTREAM_FINISHED; return result; }
                if (result.produced == output.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                const auto copy = std::min(s.wanted - s.input.size(), input.size() - result.consumed);
                if (copy > s.options.maxInputBytes - s.accepted) { limit("HNCS compressed input exceeds policy"); }
                s.input.insert(s.input.end(), input.begin() + result.consumed, input.begin() + result.consumed + copy);
                result.consumed += copy; s.accepted += copy;
                if (s.input.size() != s.wanted) {
                    if (mode == EINPUT_FINAL) { throw CCompressionError(EERROR_TRUNCATED, "truncated HNCS record/footer"); }
                    result.status = ESTREAM_NEED_INPUT; return result;
                }
                s.parse();
            }
        } catch (...) { s.state = ESTATE_FAILED; throw; }
    }
};

} // namespace

/* Explicit private framing is never substituted for a standard native stream. */
std::shared_ptr<IStreamCompressor> chunkedEncoder(SCompressionOptions options) { return std::make_shared<ChunkEncoder>(options); }

/* Decode at most one bounded record into pending output, not a whole-stream buffer. */
std::shared_ptr<IStreamDecompressor> chunkedDecoder(SCompressionOptions options) { return std::make_shared<ChunkDecoder>(options); }

} // namespace detail
} // namespace hncomp
