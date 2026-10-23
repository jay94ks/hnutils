#include "internal.hpp"

#include <optional>

namespace hncomp {
namespace detail {
namespace {

constexpr unsigned LENGTH_BASE[]{3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr unsigned LENGTH_EXTRA[]{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr unsigned DIST_BASE[]{1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr unsigned DIST_EXTRA[]{0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

void crcByte(std::uint32_t& crc, std::uint8_t value) {
    crc ^= value;
    for (unsigned i = 0; i < 8; ++i) { crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
}

std::vector<unsigned> fixedLengths() {
    std::vector<unsigned> result(288);
    for (unsigned i = 0; i < 288; ++i) { result[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8; }
    return result;
}

void fixedSymbol(BitsOut& bits, unsigned value) {
    if (value < 144) { bits.put(reverse(0x30 + value, 8), 8); }
    else if (value < 256) { bits.put(reverse(0x190 + value - 144, 9), 9); }
    else if (value < 280) { bits.put(reverse(value - 256, 7), 7); }
    else { bits.put(reverse(0xc0 + value - 280, 8), 8); }
}

class DeflateEncoder final : public IStreamCompressor {
private:
    struct Impl {
        SCompressionOptions options;
        BitsOut bits;
        SBytes pending;
        std::size_t pendingPos = 0;
        SBytes history;
        std::uint32_t adler = 1, crc = 0xffffffffu;
        std::size_t inputTotal = 0, outputTotal = 0;
        EStreamState state = ESTATE_RUNNING;
        std::optional<EFlushMode> flushing;

        explicit Impl(SCompressionOptions configuration) : options(configuration) {
            if (options.format == EFORMAT_ZLIB) { pending = {0x78, 0x01}; }
            if (options.format == EFORMAT_GZIP) { pending = {31,139,8,0,0,0,0,0,0,255}; }
        }

        void block(SReadOnlyByteSpan chunk, bool final) {
            bits.put(final ? 1 : 0, 1); bits.put(1, 2);
            SBytes input = history; input.insert(input.end(), chunk.begin(), chunk.end());
            std::vector<std::size_t> heads(65536, SIZE_MAX), previous(input.size(), SIZE_MAX);
            auto insert = [&](std::size_t pos) {
                if (pos + 3 > input.size()) { return SIZE_MAX; }
                const auto hash = ((input[pos] * 251u + input[pos + 1]) * 251u + input[pos + 2]) & 65535u;
                auto old = heads[hash]; previous[pos] = old; heads[hash] = pos; return old;
            };
            for (std::size_t i = 0; i < history.size(); ++i) { insert(i); }
            for (std::size_t pos = history.size(); pos < input.size();) {
                auto candidate = insert(pos);
                std::size_t length = 0, distance = 0;
                auto effort = options.level * 16;
                while (candidate != SIZE_MAX && pos - candidate <= 32768 && effort--) {
                    std::size_t count = 0;
                    while (count < 258 && pos + count < input.size() && input[pos + count] == input[candidate + count]) { ++count; }
                    if (count > length) { length = count; distance = pos - candidate; }
                    if (count == std::min<std::size_t>(258,input.size()-pos)) { break; }
                    candidate = previous[candidate];
                }
                if (length < 3) { fixedSymbol(bits, input[pos++]); continue; }
                unsigned code = 0;
                while (code + 1 < std::size(LENGTH_BASE) && LENGTH_BASE[code + 1] <= length) { ++code; }
                fixedSymbol(bits, 257 + code); bits.put(length - LENGTH_BASE[code], LENGTH_EXTRA[code]);
                code = 0;
                while (code + 1 < std::size(DIST_BASE) && DIST_BASE[code + 1] <= distance) { ++code; }
                bits.put(reverse(code, 5), 5); bits.put(distance - DIST_BASE[code], DIST_EXTRA[code]);
                for (std::size_t i = 1; i < length; ++i) { insert(pos + i); }
                pos += length;
            }
            fixedSymbol(bits, 256);
            history.assign(input.end() - std::min<std::size_t>(32768, input.size()), input.end());
            adler = adler32(chunk, adler);
            for (auto value : chunk) { crcByte(crc, value); }
        }

        void flush(EFlushMode mode) {
            if (mode == EFLUSH_PARTIAL) {
                bits.put(0, 1); bits.put(1, 2); fixedSymbol(bits, 256);
            } else if (mode == EFLUSH_SYNC || mode == EFLUSH_FULL) {
                bits.put(0, 3); bits.align(); bits.put(0, 16); bits.put(65535, 16);
                if (mode == EFLUSH_FULL) { history.clear(); }
            } else if (mode == EFLUSH_FINISH) {
                bits.align();
            }
            pending = bits.drain(); pendingPos = 0;
            if (mode == EFLUSH_FINISH) {
                if (options.format == EFORMAT_ZLIB) { putBe(pending, adler, 4); }
                if (options.format == EFORMAT_GZIP) { putLe(pending, ~crc, 4); putLe(pending, inputTotal & 0xffffffffu, 4); }
                state = ESTATE_FINISHING;
            }
            flushing = mode;
        }
    };
    std::unique_ptr<Impl> _impl;

public:
    explicit DeflateEncoder(SCompressionOptions options) : _impl(std::make_unique<Impl>(options)) {}

    bool supportsFlush(EFlushMode mode) const noexcept override { return mode >= EFLUSH_NONE && mode <= EFLUSH_FINISH; }
    EStreamState state() const noexcept override { return _impl->state; }
    void reset() override { _impl = std::make_unique<Impl>(_impl->options); }

    SStreamResult process(SReadOnlyByteSpan input, SByteSpan output, EFlushMode mode) override {
        auto& s = *_impl;
        if (!supportsFlush(mode)) { unsupported("unsupported DEFLATE flush request"); }
        if (s.state == ESTATE_FAILED || (s.state == ESTATE_FINISHED && !input.empty())) {
            throw CCompressionError(EERROR_STATE, "reset the completed/failed encoder");
        }
        if (s.state == ESTATE_FINISHED) { return {0, 0, ESTREAM_FINISHED}; }
        if (s.flushing && (*s.flushing != mode || !input.empty())) {
            throw CCompressionError(EERROR_STATE, "repeat pending flush with empty input");
        }
        SStreamResult result;
        try {
            for (;;) {
                const auto count = std::min(output.size() - result.produced, s.pending.size() - s.pendingPos);
                if (count > s.options.maxOutputBytes - s.outputTotal) { limit("compressed stream output limit exceeded"); }
                std::copy_n(s.pending.begin() + s.pendingPos, count, output.begin() + result.produced);
                s.pendingPos += count; s.outputTotal += count; result.produced += count;
                if (s.pendingPos != s.pending.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                s.pending.clear(); s.pendingPos = 0;
                if (s.flushing) {
                    if (*s.flushing == EFLUSH_FINISH) { s.state = ESTATE_FINISHED; result.status = ESTREAM_FINISHED; }
                    else { result.status = ESTREAM_FLUSHED; }
                    s.flushing.reset(); return result;
                }
                if (result.produced == output.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                const auto remaining = input.subspan(result.consumed);
                if (remaining.empty()) {
                    if (mode == EFLUSH_NONE) { result.status = ESTREAM_NEED_INPUT; return result; }
                    if (mode == EFLUSH_FINISH) { s.block({}, true); }
                    s.flush(mode); continue;
                }
                const auto chunk = remaining.first(std::min(s.options.blockBytes, remaining.size()));
                if (chunk.size() > s.options.maxInputBytes - s.inputTotal) { limit("compression input limit exceeded"); }
                const bool last = chunk.size() == remaining.size();
                s.block(chunk, last && mode == EFLUSH_FINISH);
                s.inputTotal += chunk.size(); result.consumed += chunk.size();
                if (last && mode != EFLUSH_NONE) { s.flush(mode); }
                else { s.pending = s.bits.drain(); }
            }
        } catch (...) { s.state = ESTATE_FAILED; throw; }
    }
};

class DeflateDecoder final : public IStreamDecompressor {
private:
    struct Impl {
        enum Stage { Header, Block, Stored, Symbols, Trailer, Done } stage = Header;
        SCompressionOptions options;
        EStreamState state = ESTATE_RUNNING;
        SBytes input;
        std::size_t bit = 0;
        bool last = false;
        unsigned stored = 0;
        std::unique_ptr<Huffman> literals, distances;
        std::array<std::uint8_t, 32768> history{};
        std::size_t total = 0, accepted = 0;
        unsigned length = 0, distance = 0, window = 32768;
        std::uint32_t adler = 1, crc = 0xffffffffu;
        unsigned gzipStage = 0, gzipFlags = 0;
        std::size_t gzipCursor = 0, gzipExtra = 0;

        explicit Impl(SCompressionOptions configuration) : options(configuration) {}

        void trim() {
            const auto bytes = bit / 8;
            if (bytes) { input.erase(input.begin(), input.begin() + bytes); bit %= 8; }
        }

        void header() {
            Reader reader(input);
            if (options.format == EFORMAT_ZLIB) {
                const auto cmf = reader.byte(), flg = reader.byte();
                if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf * 256u + flg) % 31)) { invalid("invalid zlib header"); }
                window = 1u << ((cmf >> 4) + 8);
                if (flg & 32) { unsupported("zlib preset dictionary was not supplied"); }
            } else if (options.format == EFORMAT_GZIP) {
                if (gzipStage == 0) {
                    if (input.size() < 10) { throw NeedInput{}; }
                    if (reader.le(2) != 0x8b1f || reader.byte() != 8) { invalid("invalid gzip header"); }
                    gzipFlags = reader.byte();
                    if (gzipFlags & 224) { invalid("reserved gzip flags"); }
                    gzipCursor = 10; gzipStage = 1;
                }
                if (gzipStage == 1) {
                    if (gzipFlags & 4) {
                        Reader extra(SReadOnlyByteSpan(input).subspan(gzipCursor));
                        gzipExtra = static_cast<std::size_t>(extra.le(2)); gzipCursor += 2;
                    }
                    gzipStage = 2;
                }
                if (gzipStage == 2) {
                    if (gzipExtra > input.size() - gzipCursor) { throw NeedInput{}; }
                    gzipCursor += gzipExtra; gzipStage = 3;
                }
                // --> Keep the scan cursor across underflow: long names must not cause quadratic reparsing.
                for (; gzipStage <= 4; ++gzipStage) {
                    if (!(gzipFlags & (gzipStage == 3 ? 8 : 16))) { continue; }
                    for (;;) {
                        if (gzipCursor == input.size()) { throw NeedInput{}; }
                        if (input[gzipCursor++] == 0) { break; }
                    }
                }
                if (gzipFlags & 2) {
                    Reader checksum(SReadOnlyByteSpan(input).subspan(gzipCursor));
                    const auto declared = checksum.le(2);
                    if (declared != (crc32(SReadOnlyByteSpan(input).first(gzipCursor)) & 65535u)) { invalid("gzip header checksum mismatch"); }
                    gzipCursor += 2;
                }
                bit = gzipCursor * 8; stage = Block; return;
            }
            bit = reader.pos() * 8; stage = Block;
        }

        void trees(BitsIn& bits) {
            const auto nl = bits.get(5) + 257, nd = bits.get(5) + 1, nc = bits.get(4) + 4;
            if (nl > 286 || nd > 32) { invalid("invalid DEFLATE code counts"); }
            constexpr unsigned actualOrder[]{16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<unsigned> lengths(19, 0);
            for (unsigned i = 0; i < nc; ++i) { lengths[actualOrder[i]] = bits.get(3); }
            auto validateTree = [](std::span<const unsigned> values, bool complete, bool allowEmpty = false) {
                std::array<unsigned, 16> counts{}; unsigned maximum = 0;
                for (auto value : values) { ++counts[value]; maximum = std::max(maximum, value); }
                if (maximum == 0 && allowEmpty) { return; }
                int left = 1;
                for (unsigned i = 1; i <= 15; ++i) {
                    left = left * 2 - static_cast<int>(counts[i]);
                    if (left < 0) { invalid("over-subscribed DEFLATE tree"); }
                }
                if (left && (complete || maximum != 1)) { invalid("incomplete DEFLATE tree"); }
            };
            validateTree(lengths, true);
            Huffman codes(lengths);
            lengths.clear();
            while (lengths.size() < nl + nd) {
                const auto code = codes.decode(bits);
                if (code < 16) { lengths.push_back(code); continue; }
                if (code == 16 && lengths.empty()) { invalid("Huffman repeat without previous code"); }
                const auto repeat = code == 16 ? bits.get(2) + 3 : code == 17 ? bits.get(3) + 3 : bits.get(7) + 11;
                if (repeat > nl + nd - lengths.size()) { invalid("Huffman repeat exceeds alphabet"); }
                const auto value = code == 16 ? lengths.back() : 0;
                lengths.insert(lengths.end(), repeat, value);
            }
            if (lengths[256] == 0) { invalid("DEFLATE tree is missing end-of-block"); }
            validateTree(std::span<const unsigned>(lengths).first(nl), false);
            validateTree(std::span<const unsigned>(lengths).subspan(nl), false, true);
            auto literalTree = std::make_unique<Huffman>(std::vector<unsigned>(lengths.begin(), lengths.begin() + nl));
            auto distanceTree = std::make_unique<Huffman>(std::vector<unsigned>(lengths.begin() + nl, lengths.end()));
            literals = std::move(literalTree); distances = std::move(distanceTree);
        }

        void emit(std::uint8_t value, SByteSpan output, SStreamResult& result) {
            if (total == options.maxOutputBytes) { limit("decompressed stream size limit exceeded"); }
            history[total++ % history.size()] = value;
            output[result.produced++] = value;
            adler = adler32(SReadOnlyByteSpan(&value, 1), adler); crcByte(crc, value);
        }

        void step(SByteSpan output, SStreamResult& result) {
            if (stage == Header) { header(); return; }
            BitsIn bits(input, bit);
            if (stage == Block) {
                const bool isLast = bits.get(1) != 0; const auto kind = bits.get(2);
                if (kind == 0) {
                    bits.align(); const auto size = bits.get(16), inverse = bits.get(16);
                    if ((size ^ inverse) != 65535) { invalid("DEFLATE stored length mismatch"); }
                    stored = size; stage = Stored;
                } else if (kind == 1 || kind == 2) {
                    if (kind == 1) {
                        literals = std::make_unique<Huffman>(fixedLengths());
                        distances = std::make_unique<Huffman>(std::vector<unsigned>(32, 5));
                    } else { trees(bits); }
                    stage = Symbols;
                } else { invalid("reserved DEFLATE block type"); }
                last = isLast; bit = bits.bit(); return;
            }
            if (stage == Stored) {
                if (stored) { const auto value = bits.get(8); emit(static_cast<std::uint8_t>(value), output, result); --stored; bit = bits.bit(); }
                else { stage = last ? Trailer : Block; }
                return;
            }
            if (stage == Symbols) {
                if (length) { emit(history[(total - distance) % history.size()], output, result); --length; return; }
                const auto symbol = literals->decode(bits);
                if (symbol < 256) { emit(static_cast<std::uint8_t>(symbol), output, result); }
                else if (symbol == 256) { stage = last ? Trailer : Block; }
                else {
                    if (symbol > 285) { invalid("reserved DEFLATE length symbol"); }
                    const auto len = LENGTH_BASE[symbol - 257] + bits.get(LENGTH_EXTRA[symbol - 257]);
                    const auto d = distances->decode(bits);
                    if (d >= 30) { invalid("reserved DEFLATE distance symbol"); }
                    const auto dist = DIST_BASE[d] + bits.get(DIST_EXTRA[d]);
                    if (dist > std::min<std::size_t>(total, window)) { invalid("DEFLATE distance exceeds available history/window"); }
                    if (len > options.maxOutputBytes - total) { limit("DEFLATE match exceeds output policy"); }
                    length = len; distance = dist;
                }
                bit = bits.bit(); return;
            }
            if (stage == Trailer) {
                bits.align();
                Reader reader(SReadOnlyByteSpan(input).subspan(bits.bytes()));
                if (options.format == EFORMAT_ZLIB && reader.be(4) != adler) { invalid("zlib Adler32 mismatch"); }
                if (options.format == EFORMAT_GZIP) {
                    if (reader.le(4) != ~crc || reader.le(4) != (total & 0xffffffffu)) { invalid("gzip checksum/size mismatch"); }
                }
                bit = bits.bit() + reader.pos() * 8; stage = Done; state = ESTATE_FINISHED;
            }
        }
    };
    std::unique_ptr<Impl> _impl;

public:
    explicit DeflateDecoder(SCompressionOptions options) : _impl(std::make_unique<Impl>(options)) {}
    EStreamState state() const noexcept override { return _impl->state; }
    void reset() override { _impl = std::make_unique<Impl>(_impl->options); }

    SStreamResult process(SReadOnlyByteSpan input, SByteSpan output, EInputMode mode) override {
        auto& s = *_impl;
        if (mode != EINPUT_MORE && mode != EINPUT_FINAL) { throw std::invalid_argument("invalid input mode"); }
        if (s.state == ESTATE_FAILED) { throw CCompressionError(EERROR_STATE, "reset failed decoder before reuse"); }
        SStreamResult result;
        try {
            for (;;) {
                if (s.state == ESTATE_FINISHED) { result.status = ESTREAM_FINISHED; return result; }
                if (result.produced == output.size()) { result.status = ESTREAM_NEED_OUTPUT; return result; }
                try { s.step(output, result); s.trim(); }
                catch (const NeedInput&) {
                    if (result.consumed == input.size()) {
                        if (mode == EINPUT_FINAL) { throw CCompressionError(EERROR_TRUNCATED, "truncated DEFLATE stream"); }
                        result.status = ESTREAM_NEED_INPUT; return result;
                    }
                    if (s.accepted == s.options.maxInputBytes) { limit("compressed input limit exceeded"); }
                    if (s.input.size() >= s.options.maxWindowBytes) { limit("DEFLATE header exceeds memory policy"); }
                    s.input.push_back(input[result.consumed++]); ++s.accepted;
                }
            }
        } catch (...) { s.state = ESTATE_FAILED; throw; }
    }
};

} // namespace

/* Create a private stateful encoder with retained history across flush boundaries. */
std::shared_ptr<IStreamCompressor> deflateEncoder(SCompressionOptions options) {
    return std::make_shared<DeflateEncoder>(options);
}

/* Create a private incremental decoder; no whole-stream buffering is required. */
std::shared_ptr<IStreamDecompressor> deflateDecoder(SCompressionOptions options) {
    return std::make_shared<DeflateDecoder>(options);
}

} // namespace detail
} // namespace hncomp
