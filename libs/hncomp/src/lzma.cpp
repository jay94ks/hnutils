#include "internal.hpp"
#include "archive_internal.hpp"

namespace hncomp {
namespace detail {
namespace {

using Prob = std::uint16_t;

struct LengthModel {
    std::vector<Prob> choice = std::vector<Prob>(2, 1024);
    std::vector<Prob> low = std::vector<Prob>(16 * 8, 1024);
    std::vector<Prob> mid = std::vector<Prob>(16 * 8, 1024);
    std::vector<Prob> high = std::vector<Prob>(256, 1024);
};

struct Model {
    std::vector<Prob> match = std::vector<Prob>(12 * 16, 1024);
    std::vector<Prob> rep = std::vector<Prob>(12, 1024);
    std::vector<Prob> rep0 = std::vector<Prob>(12, 1024), rep1 = rep0, rep2 = rep0;
    std::vector<Prob> shortRep = std::vector<Prob>(12 * 16, 1024);
    std::vector<Prob> slots = std::vector<Prob>(4 * 64, 1024);
    std::vector<Prob> distances = std::vector<Prob>(128, 1024);
    std::vector<Prob> align = std::vector<Prob>(16, 1024);
    std::vector<Prob> literal;
    LengthModel length, repeatLength;
    explicit Model(unsigned contexts) : literal(std::size_t{0x300} << contexts, 1024) {}
};

class RangeEncoder {
private:
    std::uint64_t _low = 0;
    std::uint32_t _range = 0xffffffffu;
    std::uint8_t _cache = 0;
    std::size_t _cacheSize = 1;
    SBytes _output;

    void shift() {
        const auto low = static_cast<std::uint32_t>(_low);
        const auto carry = static_cast<std::uint8_t>(_low >> 32);
        if (low < 0xff000000u || carry) {
            auto value = _cache;
            do { _output.push_back(static_cast<std::uint8_t>(value + carry)); value = 255; } while (--_cacheSize);
            _cache = static_cast<std::uint8_t>(low >> 24);
        }
        ++_cacheSize; _low = static_cast<std::uint32_t>(low << 8);
    }

public:
    void bit(Prob& probability, unsigned value) {
        const auto bound = (_range >> 11) * probability;
        if (value == 0) { _range = bound; probability += (2048 - probability) >> 5; }
        else { _low += bound; _range -= bound; probability -= probability >> 5; }
        if (_range < (1u << 24)) { _range <<= 8; shift(); }
    }
    void direct(unsigned value, unsigned count) {
        while (count--) {
            _range >>= 1;
            if ((value >> count) & 1) { _low += _range; }
            if (_range < (1u << 24)) { _range <<= 8; shift(); }
        }
    }
    void tree(std::vector<Prob>& probabilities, int base, unsigned value, unsigned count, bool reversed = false) {
        unsigned node = 1;
        for (unsigned i = 0; i < count; ++i) {
            const auto valueBit = (value >> (reversed ? i : count - i - 1)) & 1;
            bit(probabilities[static_cast<std::size_t>(base + static_cast<int>(node))], valueBit);
            node = node * 2 + valueBit;
        }
    }
    void length(LengthModel& model, unsigned pos, unsigned value) {
        value -= 2;
        if (value < 8) { bit(model.choice[0], 0); tree(model.low, static_cast<int>(pos * 8), value, 3); }
        else {
            bit(model.choice[0], 1); value -= 8;
            if (value < 8) { bit(model.choice[1], 0); tree(model.mid, static_cast<int>(pos * 8), value, 3); }
            else { bit(model.choice[1], 1); tree(model.high, 0, value - 8, 8); }
        }
    }
    SBytes finish() { for (unsigned i = 0; i < 5; ++i) { shift(); } return std::move(_output); }
};

class RangeDecoder {
private:
    Reader& _input;
    std::uint32_t _range = 0xffffffffu, _code = 0;

    void normalize() { if (_range < (1u << 24)) { _range <<= 8; _code = (_code << 8) | _input.byte(); } }

public:
    explicit RangeDecoder(Reader& input) : _input(input) {
        if (_input.byte() != 0) { invalid("invalid LZMA range prefix"); }
        for (unsigned i = 0; i < 4; ++i) { _code = (_code << 8) | _input.byte(); }
    }
    unsigned bit(Prob& probability) {
        const auto bound = (_range >> 11) * probability;
        unsigned result;
        if (_code < bound) { result = 0; _range = bound; probability += (2048 - probability) >> 5; }
        else { result = 1; _range -= bound; _code -= bound; probability -= probability >> 5; }
        normalize(); return result;
    }
    unsigned direct(unsigned count) {
        unsigned value = 0;
        while (count--) {
            _range >>= 1; const unsigned next = _code >= _range;
            if (next) { _code -= _range; }
            value = (value << 1) | next; normalize();
        }
        return value;
    }
    unsigned tree(std::vector<Prob>& probabilities, int base, unsigned count, bool reversed = false) {
        unsigned node = 1, value = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto next = bit(probabilities[static_cast<std::size_t>(base + static_cast<int>(node))]);
            node = node * 2 + next;
            value |= next << (reversed ? i : count - i - 1);
        }
        return value;
    }
    unsigned length(LengthModel& model, unsigned pos) {
        if (bit(model.choice[0]) == 0) { return 2 + tree(model.low, static_cast<int>(pos * 8), 3); }
        if (bit(model.choice[1]) == 0) { return 10 + tree(model.mid, static_cast<int>(pos * 8), 3); }
        return 18 + tree(model.high, 0, 8);
    }
    bool complete() const noexcept { return _code == 0; }
};

unsigned literalState(unsigned state) { return state < 4 ? 0 : state < 10 ? state - 3 : state - 6; }

void distance(RangeEncoder& coder, Model& model, unsigned length, std::uint32_t distance) {
    unsigned slot;
    if (distance < 4) { slot = distance; }
    else {
        const unsigned high = std::bit_width(distance) - 1;
        slot = high * 2 + ((distance >> (high - 1)) & 1);
    }
    coder.tree(model.slots, static_cast<int>(std::min(length - 2, 3u) * 64), slot, 6);
    if (slot < 4) { return; }
    const unsigned bits = (slot >> 1) - 1, base = (2u | (slot & 1)) << bits;
    const auto footer = distance - base;
    if (slot < 14) { coder.tree(model.distances, static_cast<int>(base) - static_cast<int>(slot) - 1, footer, bits, true); }
    else { coder.direct(footer >> 4, bits - 4); coder.tree(model.align, 0, footer & 15, 4, true); }
}

} // namespace

/* LZMA1 with adaptive literal/match models and an explicit end-of-payload marker. */
static SBytes encodeLzmaPayload(SReadOnlyByteSpan input, const SCompressionOptions& options, bool alone) {
    const auto dictionary = static_cast<unsigned>(std::bit_floor(std::min<std::size_t>(options.maxWindowBytes, 1u << 20)));
    SBytes output;
    if (alone) { output.push_back(93); putLe(output, dictionary, 4); putLe(output, UINT64_MAX, 8); }
    Model model(3); RangeEncoder coder;
    std::vector<std::size_t> heads(65536, SIZE_MAX), previous(65536, SIZE_MAX);
    unsigned state = 0; std::uint32_t lastDistance = 0;
    auto insert = [&](std::size_t pos) {
        if (pos + 3 > input.size()) { return SIZE_MAX; }
        const auto hash = ((input[pos] * 251u + input[pos + 1]) * 251u + input[pos + 2]) & 65535u;
        const auto old = heads[hash]; previous[pos % previous.size()] = old; heads[hash] = pos; return old;
    };
    for (std::size_t pos = 0; pos < input.size();) {
        auto candidate = insert(pos); std::size_t length = 0, dist = 0; auto effort = options.level * 16;
        while (candidate != SIZE_MAX && pos - candidate < std::min<unsigned>(dictionary, 65536) && effort--) {
            std::size_t count = 0;
            while (count < 273 && pos + count < input.size() && input[candidate + count] == input[pos + count]) { ++count; }
            if (count > length) { length = count; dist = pos - candidate; }
            if (count == std::min<std::size_t>(273,input.size()-pos)) { break; }
            const auto next = previous[candidate % previous.size()];
            if (next >= candidate) { break; }
            candidate = next;
        }
        const auto position = static_cast<unsigned>(pos & 3);
        if (length >= 3) {
            coder.bit(model.match[state * 16 + position], 1); coder.bit(model.rep[state], 0);
            coder.length(model.length, position, static_cast<unsigned>(length));
            distance(coder, model, static_cast<unsigned>(length), static_cast<std::uint32_t>(dist - 1));
            state = state < 7 ? 7 : 10; lastDistance = static_cast<std::uint32_t>(dist - 1);
            for (std::size_t i = 1; i < length; ++i) { insert(pos + i); }
            pos += length;
        } else {
            coder.bit(model.match[state * 16 + position], 0);
            const auto context = (pos ? input[pos - 1] >> 5 : 0) * 0x300;
            unsigned node = 1; bool matched = state >= 7;
            const auto matchByte = matched ? input[pos - lastDistance - 1] : 0;
            for (unsigned bit = 8; bit--;) {
                const auto value = (input[pos] >> bit) & 1u, match = (matchByte >> bit) & 1u;
                const auto index = matched ? 0x100 + (match << 8) + node : node;
                coder.bit(model.literal[context + index], value);
                matched = matched && value == match; node = node * 2 + value;
            }
            state = literalState(state); ++pos;
        }
    }
    if (alone) {
        const auto position = static_cast<unsigned>(input.size() & 3);
        coder.bit(model.match[state * 16 + position], 1); coder.bit(model.rep[state], 0);
        coder.length(model.length, position, 2); distance(coder, model, 2, UINT32_MAX);
    }
    auto data = coder.finish(); output.insert(output.end(), data.begin(), data.end()); return output;
}

/* The original LZMA1 wire representation includes its own header and end marker. */
SBytes encodeLzma(SReadOnlyByteSpan input, const SCompressionOptions& options) { return encodeLzmaPayload(input, options, true); }

/* A reset LZMA2 chunk ends at a declared output length, not an LZMA1 end marker. */
SBytes encodeLzma2Chunk(SReadOnlyByteSpan input, unsigned level) {
    SCompressionOptions options; options.level = level; return encodeLzmaPayload(input, options, false);
}

/* Decode LZMA1 literals, normal matches, four repeat distances and short repeats. */
SBytes decodeLzma(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    Reader reader(input);
    unsigned properties = reader.byte();
    if (properties >= 225) { invalid("invalid LZMA properties"); }
    const unsigned lc = properties % 9, lp = (properties / 9) % 5, pb = properties / 45;
    if (lc + lp > 4) { unsupported("LZMA literal context exceeds supported lc+lp <= 4"); }
    const auto dictionary = std::max<std::uint64_t>(4096, reader.le(4));
    if (dictionary > options.maxWindowBytes) { limit("LZMA dictionary exceeds window policy"); }
    const auto expected = reader.le(8);
    if (expected != UINT64_MAX && expected > options.maxOutputBytes) { limit("LZMA declared output exceeds policy"); }
    Model model(lc + lp); RangeDecoder coder(reader);
    unsigned state = 0; std::array<std::uint32_t, 4> reps{};
    SBytes output;
    for (;;) {
        if (expected != UINT64_MAX && output.size() == expected && coder.complete()) { break; }
        const auto pos = static_cast<unsigned>(output.size() & ((1u << pb) - 1));
        if (coder.bit(model.match[state * 16 + pos]) == 0) {
            room(output, 1, options.maxOutputBytes);
            const auto previous = output.empty() ? 0u : unsigned(output.back());
            const auto context = (((output.size() & ((1u << lp) - 1)) << lc) + (previous >> (8 - lc))) * 0x300;
            unsigned symbol = 1;
            if (state >= 7) {
                if (reps[0] >= output.size()) { invalid("LZMA literal match exceeds history"); }
                const auto matchByte = output[output.size() - reps[0] - 1];
                bool matching = true;
                for (unsigned bit = 8; bit--;) {
                    const auto match = (unsigned(matchByte) >> bit) & 1;
                    const auto value = coder.bit(model.literal[context + (matching ? 0x100 + (match << 8) + symbol : symbol)]);
                    symbol = symbol * 2 + value; matching = matching && match == value;
                }
            } else { while (symbol < 256) { symbol = symbol * 2 + coder.bit(model.literal[context + symbol]); } }
            output.push_back(static_cast<std::uint8_t>(symbol)); state = literalState(state);
        } else {
            unsigned length;
            if (coder.bit(model.rep[state])) {
                if (coder.bit(model.rep0[state]) == 0) {
                    if (coder.bit(model.shortRep[state * 16 + pos]) == 0) {
                        state = state < 7 ? 9 : 11; length = 1;
                        copyMatch(output, std::uint64_t(reps[0]) + 1, length, options.maxOutputBytes); continue;
                    }
                } else {
                    std::uint32_t selected;
                    if (coder.bit(model.rep1[state]) == 0) { selected = reps[1]; }
                    else { if (coder.bit(model.rep2[state]) == 0) { selected = reps[2]; } else { selected = reps[3]; reps[3] = reps[2]; } reps[2] = reps[1]; }
                    reps[1] = reps[0]; reps[0] = selected;
                }
                length = coder.length(model.repeatLength, pos); state = state < 7 ? 8 : 11;
            } else {
                length = coder.length(model.length, pos); state = state < 7 ? 7 : 10;
                const auto slot = coder.tree(model.slots, static_cast<int>(std::min(length - 2, 3u) * 64), 6);
                std::uint32_t dist = slot;
                if (slot >= 4) {
                    const auto bits = (slot >> 1) - 1; dist = (2u | (slot & 1)) << bits;
                    if (slot < 14) { dist += coder.tree(model.distances, static_cast<int>(dist) - static_cast<int>(slot) - 1, bits, true); }
                    else {
                        const auto high = coder.direct(bits - 4);
                        const auto low = coder.tree(model.align, 0, 4, true);
                        dist += (high << 4) + low;
                    }
                }
                if (dist == UINT32_MAX) {
                    if (length != 2 || !coder.complete()) { invalid("invalid LZMA end marker"); }
                    break;
                }
                reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0]; reps[0] = dist;
            }
            if (std::uint64_t(reps[0]) + 1 > dictionary) { invalid("LZMA distance exceeds dictionary"); }
            copyMatch(output, std::uint64_t(reps[0]) + 1, length, options.maxOutputBytes);
        }
        if (expected != UINT64_MAX && output.size() > expected) { invalid("LZMA output exceeds declared size"); }
    }
    if (expected != UINT64_MAX && output.size() != expected) { invalid("LZMA content size mismatch"); }
    if (reader.remaining()) { invalid("trailing LZMA data"); }
    return output;
}

struct Lzma2Decoder::Impl {
    std::size_t dictionary;
    std::unique_ptr<Model> model;
    SBytes history;
    unsigned lc = 0, lp = 0, pb = 0, state = 0;
    std::array<std::uint32_t, 4> reps{};
    std::uint64_t position = 0;
    bool initialized = false, needProperties = true, needState = true;
    explicit Impl(unsigned property) {
        if (property > 40) { invalid("invalid LZMA2 dictionary property"); }
        const auto size = property == 40 ? UINT64_C(0xffffffff) : std::uint64_t(2 | (property & 1)) << (property / 2 + 11);
        if (size > 64 * 1024 * 1024) { limit("LZMA2 dictionary exceeds 64 MiB archive policy"); }
        dictionary = static_cast<std::size_t>(size);
    }
    void resetState() { model = std::make_unique<Model>(lc + lp); state = 0; reps.fill(0); needState = false; }
    void emit(std::uint8_t value, SBytes& output) {
        if (history.size() < dictionary) { history.push_back(value); }
        else { history[static_cast<std::size_t>(position % dictionary)] = value; }
        ++position; output.push_back(value);
    }
};

Lzma2Decoder::Lzma2Decoder(unsigned property) : _impl(std::make_unique<Impl>(property)) {}
Lzma2Decoder::~Lzma2Decoder() = default;

/* Keep probabilities, repeat distances and dictionary across non-reset LZMA2 chunks. */
SBytes Lzma2Decoder::chunk(unsigned control, SReadOnlyByteSpan payload, std::size_t rawSize, unsigned properties) {
    auto& s = *_impl;
    if (control == 1 || control >= 0xe0) { s.history.clear(); s.position = 0; s.initialized = true; s.needState = true; s.needProperties = true; }
    if (!s.initialized) { invalid("LZMA2 stream is missing dictionary reset"); }
    if (rawSize > 2 * 1024 * 1024 || rawSize == 0) { invalid("invalid LZMA2 output chunk size"); }
    SBytes output; output.reserve(rawSize);
    if (control == 1 || control == 2) {
        if (rawSize != payload.size() || rawSize > 65536) { invalid("invalid uncompressed LZMA2 chunk"); }
        for (auto byte : payload) { s.emit(byte,output); } s.needState = true;
    } else {
        if (control < 0x80) { invalid("reserved LZMA2 control"); }
        if (control >= 0xc0) {
            if (properties >= 225) { invalid("invalid LZMA2 properties"); }
            s.lc = properties % 9; s.lp = (properties / 9) % 5; s.pb = properties / 45;
            if (s.lc + s.lp > 4) { invalid("LZMA2 literal properties exceed lc+lp <= 4"); }
            s.needProperties = false;
        }
        if (s.needProperties) { invalid("LZMA2 stream is missing properties"); }
        if (control >= 0xa0) { s.resetState(); }
        if (s.needState || !s.model) { invalid("LZMA2 stream is missing state reset"); }
        Reader reader(payload); RangeDecoder coder(reader); auto& model = *s.model;
        while (output.size() < rawSize) {
            const auto pos = static_cast<unsigned>(s.position & ((1u << s.pb) - 1));
            if (coder.bit(model.match[s.state * 16 + pos]) == 0) {
                const auto previous = s.history.empty() ? 0u : unsigned(s.history[static_cast<std::size_t>((s.position-1)%s.dictionary)]);
                const auto context = (((s.position & ((1u << s.lp) - 1)) << s.lc) + (previous >> (8 - s.lc))) * 0x300;
                unsigned symbol = 1;
                if (s.state >= 7) {
                    if (s.reps[0] >= s.history.size() || s.reps[0] >= s.dictionary) { invalid("LZMA2 literal exceeds history"); }
                    const auto matchByte = s.history[static_cast<std::size_t>((s.position-s.reps[0]-1)%s.dictionary)]; bool matching = true;
                    for (unsigned bit = 8; bit--;) {
                        const auto match = (unsigned(matchByte) >> bit) & 1;
                        const auto value = coder.bit(model.literal[context + (matching ? 0x100 + (match << 8) + symbol : symbol)]);
                        symbol = symbol * 2 + value; matching = matching && match == value;
                    }
                } else { while (symbol < 256) { symbol = symbol * 2 + coder.bit(model.literal[context + symbol]); } }
                s.emit(static_cast<std::uint8_t>(symbol),output); s.state = literalState(s.state);
            } else {
                unsigned length = 0;
                if (coder.bit(model.rep[s.state])) {
                    if (coder.bit(model.rep0[s.state]) == 0) {
                        if (coder.bit(model.shortRep[s.state * 16 + pos]) == 0) { s.state = s.state < 7 ? 9 : 11; length = 1; }
                    } else {
                        std::uint32_t selected;
                        if (coder.bit(model.rep1[s.state]) == 0) { selected = s.reps[1]; }
                        else { if (coder.bit(model.rep2[s.state]) == 0) { selected = s.reps[2]; } else { selected = s.reps[3]; s.reps[3] = s.reps[2]; } s.reps[2] = s.reps[1]; }
                        s.reps[1] = s.reps[0]; s.reps[0] = selected;
                    }
                    if (!length) { length = coder.length(model.repeatLength, pos); s.state = s.state < 7 ? 8 : 11; }
                } else {
                    length = coder.length(model.length, pos); s.state = s.state < 7 ? 7 : 10;
                    const auto slot = coder.tree(model.slots, static_cast<int>(std::min(length - 2, 3u) * 64), 6);
                    std::uint32_t dist = slot;
                    if (slot >= 4) {
                        const auto bits = (slot >> 1) - 1; dist = (2u | (slot & 1)) << bits;
                        if (slot < 14) { dist += coder.tree(model.distances, static_cast<int>(dist) - static_cast<int>(slot) - 1, bits, true); }
                        else { const auto high = coder.direct(bits - 4); const auto low = coder.tree(model.align, 0, 4, true); dist += (high << 4) + low; }
                    }
                    if (dist == UINT32_MAX) { invalid("LZMA2 cannot contain an LZMA1 end marker"); }
                    s.reps[3] = s.reps[2]; s.reps[2] = s.reps[1]; s.reps[1] = s.reps[0]; s.reps[0] = dist;
                }
                if (length > rawSize-output.size() || std::uint64_t(s.reps[0])+1 > s.dictionary || s.reps[0] >= s.history.size()) { invalid("LZMA2 match exceeds chunk or dictionary"); }
                for (unsigned i = 0; i < length; ++i) { s.emit(s.history[static_cast<std::size_t>((s.position-s.reps[0]-1)%s.dictionary)],output); }
            }
        }
        if (!coder.complete() || reader.remaining()) { invalid("LZMA2 chunk range/size mismatch"); }
    }
    return output;
}

} // namespace detail
} // namespace hncomp
