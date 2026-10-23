#include "internal.hpp"

namespace hncomp {
namespace detail {
namespace {

constexpr unsigned LL_BASE[]{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,22,24,28,32,40,48,64,128,256,512,1024,2048,4096,8192,16384,32768,65536};
constexpr unsigned LL_BITS[]{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16};
constexpr unsigned ML_BASE[]{3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,37,39,41,43,47,51,59,67,83,99,131,259,515,1027,2051,4099,8195,16387,32771,65539};
constexpr unsigned ML_BITS[]{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16};

class ReverseBits {
private:
    SReadOnlyByteSpan _input;
    std::size_t _remaining;

public:
    explicit ReverseBits(SReadOnlyByteSpan input) : _input(input), _remaining(0) {
        if (input.empty() || input.back() == 0) { invalid("missing zstd reverse bitstream marker"); }
        _remaining = (input.size() - 1) * 8 + std::bit_width(unsigned(input.back())) - 1;
    }
    unsigned get(unsigned count) {
        if (count > 31 || count > _remaining) { invalid("truncated zstd reverse bitstream"); }
        unsigned result = 0;
        _remaining -= count;
        for (unsigned i = 0; i < count; ++i) { result |= ((_input[(_remaining + i) / 8] >> ((_remaining + i) % 8)) & 1u) << i; }
        return result;
    }
    bool end() const noexcept { return _remaining == 0; }
    std::size_t remaining() const noexcept { return _remaining; }
    unsigned peek(unsigned count) {
        const auto saved = _remaining;
        const auto available = static_cast<unsigned>(std::min<std::size_t>(count, _remaining));
        const auto result = get(available) << (count - available); _remaining = saved; return result;
    }
};

struct FseEntry { unsigned symbol, bits, base; };

struct FseTable {
    unsigned log = 0;
    std::vector<FseEntry> entries;

    FseTable() = default;
    explicit FseTable(unsigned symbol) : entries{{symbol, 0, 0}} {}
    FseTable(const std::vector<int>& frequencies, unsigned accuracy) : log(accuracy) {
        const unsigned size = 1u << log; entries.resize(size);
        unsigned total = 0, present = 0;
        for (auto count : frequencies) { total += static_cast<unsigned>(count < 0 ? -count : count); present += count != 0; }
        if (total != size || present < 2) { invalid("invalid zstd FSE distribution"); }
        int high = static_cast<int>(size) - 1;
        std::vector<unsigned> next(frequencies.size(), 0);
        for (unsigned symbol = 0; symbol < frequencies.size(); ++symbol) {
            if (frequencies[symbol] == -1) { entries[static_cast<unsigned>(high--)].symbol = symbol; next[symbol] = 1; }
            else { next[symbol] = static_cast<unsigned>(frequencies[symbol]); }
        }
        const unsigned step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
        unsigned pos = 0;
        for (unsigned symbol = 0; symbol < frequencies.size(); ++symbol) {
            for (int i = 0; i < frequencies[symbol]; ++i) {
                entries[pos].symbol = symbol;
                do { pos = (pos + step) & mask; } while (static_cast<int>(pos) > high);
            }
        }
        if (pos != 0) { invalid("invalid zstd FSE symbol spread"); }
        for (auto& entry : entries) {
            const auto value = next[entry.symbol]++;
            entry.bits = log - static_cast<unsigned>(std::bit_width(value) - 1);
            entry.base = (value << entry.bits) - size;
        }
    }
    const FseEntry& at(unsigned state) const {
        if (state >= entries.size()) { invalid("invalid zstd FSE state"); }
        return entries[state];
    }
};

FseTable readFse(Reader& reader, SReadOnlyByteSpan input, unsigned maxSymbol, unsigned maxLog) {
    BitsIn bits(input.subspan(reader.pos()));
    const auto log = bits.get(4) + 5;
    if (log > maxLog) { invalid("zstd FSE accuracy exceeds format limit"); }
    unsigned remaining = 1u << log;
    std::vector<int> frequencies;
    while (remaining) {
        if (frequencies.size() > maxSymbol) { invalid("zstd FSE alphabet overflow"); }
        const auto width = static_cast<unsigned>(std::bit_width(remaining + 1));
        const auto cutoff = (1u << width) - 1 - (remaining + 1);
        auto saved = bits;
        auto value = bits.get(width - 1);
        if (value >= cutoff) { bits = saved; value = bits.get(width); if (value >= (1u << (width - 1))) { value -= cutoff; } }
        const int count = static_cast<int>(value) - 1;
        const auto magnitude = static_cast<unsigned>(count < 0 ? -count : count);
        if (magnitude > remaining) { invalid("zstd FSE probability overflow"); }
        frequencies.push_back(count); remaining -= magnitude;
        if (count == 0) {
            unsigned repeat;
            do {
                repeat = bits.get(2);
                if (repeat > maxSymbol + 1 - frequencies.size()) { invalid("zstd FSE zero run exceeds alphabet"); }
                frequencies.insert(frequencies.end(), repeat, 0);
            } while (repeat == 3);
        }
    }
    (void)reader.span(bits.bytes()); return FseTable(frequencies, log);
}

struct ZHuffman {
    unsigned log = 0;
    struct Entry { unsigned symbol = 0, bits = 0; };
    std::vector<Entry> entries;

    void read(Reader& reader) {
        const auto header = reader.byte(); std::vector<unsigned> weights;
        if (header >= 128) {
            const auto count = static_cast<unsigned>(header) - 127;
            for (unsigned i = 0; i < count;) {
                const auto byte = reader.byte(); weights.push_back(byte >> 4); ++i;
                if (i < count) { weights.push_back(byte & 15); ++i; }
            }
        } else {
            if (header == 0) { invalid("empty zstd Huffman weight stream"); }
            const auto data = reader.span(header); Reader weightReader(data);
            const auto table = readFse(weightReader, data, 12, 6);
            ReverseBits bits(weightReader.span(weightReader.remaining()));
            unsigned a = bits.get(table.log), b = bits.get(table.log);
            for (;;) {
                const auto first = table.at(a); weights.push_back(first.symbol);
                if (first.bits > bits.remaining()) { weights.push_back(table.at(b).symbol); break; }
                a = first.base + bits.get(first.bits);
                const auto second = table.at(b); weights.push_back(second.symbol);
                if (second.bits > bits.remaining()) { weights.push_back(table.at(a).symbol); break; }
                b = second.base + bits.get(second.bits);
                if (weights.size() >= 255) { invalid("too many zstd Huffman weights"); }
            }
            if (!bits.end()) { invalid("unconsumed zstd Huffman weight bits"); }
        }
        if (weights.empty() || weights.size() > 255) { invalid("invalid zstd Huffman alphabet"); }
        unsigned sum = 0;
        for (auto weight : weights) { if (weight > 11) { invalid("invalid zstd Huffman weight"); } if (weight) { sum += 1u << (weight - 1); } }
        log = static_cast<unsigned>(std::bit_width(sum));
        if (log == 0 || log > 11) { invalid("invalid zstd Huffman tree depth"); }
        const auto remainder = (1u << log) - sum;
        if (!std::has_single_bit(remainder)) { invalid("zstd implied Huffman weight is not a power of two"); }
        weights.push_back(static_cast<unsigned>(std::bit_width(remainder)));
        entries.assign(1u << log, {}); unsigned pos = 0;
        for (unsigned weight = 1; weight <= log; ++weight) {
            for (unsigned symbol = 0; symbol < weights.size(); ++symbol) {
                if (weights[symbol] != weight) { continue; }
                const unsigned count = 1u << (weight - 1);
                if (count > entries.size() - pos) { invalid("zstd Huffman table overflow"); }
                for (unsigned i = 0; i < count; ++i) { entries[pos++] = {symbol, log + 1 - weight}; }
            }
        }
        if (pos != entries.size()) { invalid("incomplete zstd Huffman table"); }
    }
    SBytes decode(SReadOnlyByteSpan input, std::size_t count) const {
        if (entries.empty()) { invalid("missing previous zstd Huffman table"); }
        ReverseBits bits(input); SBytes output;
        while (output.size() < count) {
            const auto entry = entries[bits.peek(log)];
            (void)bits.get(entry.bits); output.push_back(static_cast<std::uint8_t>(entry.symbol));
        }
        if (!bits.end()) { invalid("zstd Huffman stream length mismatch"); }
        return output;
    }
};

struct ZContext {
    std::array<FseTable, 3> tables;
    ZHuffman huffman;
};

FseTable sequenceTable(unsigned kind, unsigned mode, Reader& reader, SReadOnlyByteSpan input, const FseTable& previous) {
    constexpr int ll[]{4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1};
    constexpr int ml[]{
        1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,
        -1,-1,-1,-1,-1};
    constexpr int of[]{1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1};
    if (mode == 3) { if (previous.entries.empty()) { invalid("missing previous zstd FSE table"); } return previous; }
    if (mode == 1) {
        const auto symbol = reader.byte();
        if (symbol > (kind == 0 ? 35u : kind == 1 ? 31u : 52u)) { invalid("invalid zstd RLE sequence symbol"); }
        return FseTable(symbol);
    }
    if (mode == 2) { return readFse(reader, input, kind == 0 ? 35 : kind == 1 ? 31 : 52, kind == 1 ? 8 : 9); }
    if (kind == 0) { return FseTable(std::vector<int>(std::begin(ll), std::end(ll)), 6); }
    if (kind == 1) { return FseTable(std::vector<int>(std::begin(of), std::end(of)), 5); }
    return FseTable(std::vector<int>(std::begin(ml), std::end(ml)), 6);
}

SBytes readLiterals(Reader& reader, ZContext& context) {
    const auto first = reader.byte(); const auto type = first & 3, format = (first >> 2) & 3;
    if (type < 2) {
        const auto size = (format == 0 || format == 2) ? std::uint64_t(first >> 3) :
            (first >> 4) + (reader.le(format == 3 ? 2 : 1) << 4);
        if (size > 131072) { invalid("zstd literal section exceeds block limit"); }
        if (type == 1) { return SBytes(size, reader.byte()); }
        const auto raw = reader.span(size); return SBytes(raw.begin(), raw.end());
    }
    const unsigned headerBytes = format < 2 ? 3 : format == 2 ? 4 : 5;
    const auto header = std::uint64_t(first) | (reader.le(headerBytes - 1) << 8);
    const unsigned width = format < 2 ? 10 : format == 2 ? 14 : 18;
    const auto regenerated = static_cast<std::size_t>((header >> 4) & ((1u << width) - 1));
    const auto compressed = static_cast<std::size_t>(header >> (4 + width));
    if (regenerated > 131072 || compressed == 0) { invalid("invalid zstd compressed literal sizes"); }
    const auto payload = reader.span(compressed); Reader literals(payload);
    if (type == 2) { context.huffman.read(literals); }
    if (format == 0) { return context.huffman.decode(literals.span(literals.remaining()), regenerated); }
    if (regenerated < 6) { invalid("zstd four-stream literals are too short"); }
    const auto a = literals.le(2), b = literals.le(2), c = literals.le(2);
    const std::size_t sizes[]{static_cast<std::size_t>(a), static_cast<std::size_t>(b), static_cast<std::size_t>(c)};
    const auto segment = (regenerated + 3) / 4; SBytes output;
    for (unsigned i = 0; i < 4; ++i) {
        const auto length = i < 3 ? sizes[i] : literals.remaining();
        const auto count = i < 3 ? segment : regenerated - 3 * segment;
        auto decoded = context.huffman.decode(literals.span(length), count); output.insert(output.end(), decoded.begin(), decoded.end());
    }
    return output;
}

SBytes compressedBlock(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    std::vector<std::size_t> heads(65536, SIZE_MAX);
    std::size_t bestLength = 0, bestPosition = 0, bestDistance = 0;
    for (std::size_t pos = 0; pos + 4 <= input.size(); ++pos) {
        const auto hash = ((input[pos] * 251u + input[pos + 1]) * 251u + input[pos + 2]) & 65535u;
        const auto previous = std::exchange(heads[hash], pos);
        if (previous == SIZE_MAX || pos - previous > options.maxWindowBytes) { continue; }
        std::size_t count = 0;
        while (pos + count < input.size() && input[pos + count] == input[previous + count]) { ++count; }
        if (count > bestLength) { bestLength = count; bestPosition = pos; bestDistance = pos - previous; }
        if (pos + count == input.size() && count >= 8) { break; }
        if (count >= 64) { pos += count - 1; }
    }
    if (bestLength < 4) { return {}; }
    SBytes literals(input.begin(), input.begin() + bestPosition);
    literals.insert(literals.end(), input.begin() + bestPosition + bestLength, input.end());
    SBytes output;
    if (literals.size() <= 31) { putLe(output, literals.size() << 3, 1); }
    else if (literals.size() <= 4095) { putLe(output, (literals.size() << 4) | 4, 2); }
    else { putLe(output, (literals.size() << 4) | 12, 3); }
    output.insert(output.end(), literals.begin(), literals.end());
    unsigned ll = 0, ml = 0;
    while (ll + 1 < std::size(LL_BASE) && LL_BASE[ll + 1] <= bestPosition) { ++ll; }
    while (ml + 1 < std::size(ML_BASE) && ML_BASE[ml + 1] <= bestLength) { ++ml; }
    const auto offset = static_cast<unsigned>(bestDistance + 3);
    const auto of = static_cast<unsigned>(std::bit_width(offset) - 1);
    output.insert(output.end(), {1, 0x54, static_cast<std::uint8_t>(ll), static_cast<std::uint8_t>(of), static_cast<std::uint8_t>(ml)});
    BitsOut bits;
    bits.put(bestPosition - LL_BASE[ll], LL_BITS[ll]); bits.put(bestLength - ML_BASE[ml], ML_BITS[ml]);
    bits.put(offset - (1u << of), of); bits.put(1, 1); bits.align();
    auto encoded = bits.drain(); output.insert(output.end(), encoded.begin(), encoded.end()); return output;
}

void decodeBlock(SReadOnlyByteSpan input, SBytes& output, const SCompressionOptions& options,
    std::size_t window, std::size_t frameStart, std::array<std::size_t, 3>& repeats, ZContext& context) {
    Reader reader(input);
    auto literals = readLiterals(reader, context);
    auto count = static_cast<unsigned>(reader.byte());
    if (count == 255) { count = static_cast<unsigned>(reader.le(2)) + 0x7f00; }
    else if (count >= 128) { count = ((count - 128) << 8) + reader.byte(); }
    if (count == 0) {
        if (reader.remaining()) { invalid("trailing zero-sequence zstd data"); }
        room(output, literals.size(), options.maxOutputBytes); output.insert(output.end(), literals.begin(), literals.end()); return;
    }
    const auto modes = reader.byte();
    if (modes & 3) { invalid("reserved zstd sequence mode bits"); }
    for (unsigned kind = 0; kind < 3; ++kind) { context.tables[kind] = sequenceTable(kind, (modes >> (6 - kind * 2)) & 3, reader, input, context.tables[kind]); }
    ReverseBits bits(reader.span(reader.remaining())); std::size_t pos = 0, produced = 0;
    unsigned llState = bits.get(context.tables[0].log), ofState = bits.get(context.tables[1].log), mlState = bits.get(context.tables[2].log);
    for (unsigned i = 0; i < count; ++i) {
        const auto llEntry = context.tables[0].at(llState), ofEntry = context.tables[1].at(ofState), mlEntry = context.tables[2].at(mlState);
        const auto ll = llEntry.symbol, of = ofEntry.symbol, ml = mlEntry.symbol;
        if (ll >= std::size(LL_BASE) || ml >= std::size(ML_BASE) || of > 31) { invalid("invalid zstd sequence code"); }
        const auto offset = (std::uint64_t{1} << of) + bits.get(of);
        const auto length = ML_BASE[ml] + bits.get(ML_BITS[ml]);
        const auto insert = LL_BASE[ll] + bits.get(LL_BITS[ll]);
        if (insert > literals.size() - pos || produced + insert + length > 131072) { invalid("zstd sequence exceeds block"); }
        room(output, insert, options.maxOutputBytes);
        output.insert(output.end(), literals.begin() + pos, literals.begin() + pos + insert); pos += insert;
        std::size_t distance;
        if (offset > 3) { distance = static_cast<std::size_t>(offset - 3); repeats = {distance, repeats[0], repeats[1]}; }
        else {
            const auto index = static_cast<unsigned>(offset - 1 + (insert == 0));
            if (index == 3) { if (repeats[0] == 1) { invalid("zero repeat offset"); } distance = repeats[0] - 1; repeats = {distance, repeats[0], repeats[1]}; }
            else { distance = repeats[index]; if (index) { for (unsigned j = index; j > 0; --j) { repeats[j] = repeats[j - 1]; } repeats[0] = distance; } }
        }
        if (distance > window) { invalid("zstd offset exceeds frame window"); }
        if (distance > output.size() - frameStart) { invalid("zstd offset crosses frame boundary"); }
        copyMatch(output, distance, length, options.maxOutputBytes); produced += insert + length;
        if (i + 1 < count) {
            llState = llEntry.base + bits.get(llEntry.bits); mlState = mlEntry.base + bits.get(mlEntry.bits); ofState = ofEntry.base + bits.get(ofEntry.bits);
        }
    }
    if (!bits.end() || produced + literals.size() - pos > 131072) { invalid("zstd sequence bitstream/block length mismatch"); }
    room(output, literals.size() - pos, options.maxOutputBytes); output.insert(output.end(), literals.begin() + pos, literals.end());
}

} // namespace

/* Standard frames, using real LZ77 sequences with RLE-coded sequence alphabets. */
SBytes encodeZstd(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    SBytes output{0x28, 0xb5, 0x2f, 0xfd, 0xc4};
    output.push_back(static_cast<std::uint8_t>((std::bit_width(std::bit_ceil(options.blockBytes)) - 11) << 3));
    putLe(output, input.size(), 8);
    std::size_t pos = 0;
    do {
        const auto chunk = input.subspan(pos, std::min(options.blockBytes, input.size() - pos));
        const bool final = pos + chunk.size() == input.size();
        const bool repeated = !chunk.empty() && std::all_of(chunk.begin(), chunk.end(), [&](auto value) { return value == chunk[0]; });
        auto compressed = repeated ? SBytes{} : compressedBlock(chunk, options);
        if (repeated) { putLe(output, (chunk.size() << 3) | 2 | final, 3); output.push_back(chunk[0]); }
        else if (!compressed.empty() && compressed.size() < chunk.size()) {
            putLe(output, (compressed.size() << 3) | 4 | final, 3); output.insert(output.end(), compressed.begin(), compressed.end());
        } else { putLe(output, (chunk.size() << 3) | final, 3); output.insert(output.end(), chunk.begin(), chunk.end()); }
        pos += chunk.size();
    } while (pos < input.size());
    putLe(output, xxh64(input) & 0xffffffffu, 4); return output;
}

/* Standard raw/RLE/compressed blocks, canonical Huffman and FSE entropy tables. */
SBytes decodeZstd(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    Reader reader(input); SBytes output;
    do {
        const auto magic = reader.le(4);
        if ((magic & 0xfffffff0u) == 0x184d2a50u) { const auto size = reader.le(4); (void)reader.span(size); continue; }
        if (magic != 0xfd2fb528u) { invalid("invalid zstd frame magic"); }
        const auto descriptor = reader.byte();
        if (descriptor & 0x18) { invalid("reserved zstd frame header bit"); }
        const auto sizeFlag = descriptor >> 6; const bool single = (descriptor & 32) != 0;
        std::uint64_t window = 0;
        if (!single) { const auto value = reader.byte(); const auto base = std::uint64_t{1} << (10 + (value >> 3)); window = base + (base >> 3) * (value & 7); }
        const auto dictionaryFlag = descriptor & 3;
        if (dictionaryFlag && reader.le(dictionaryFlag == 3 ? 4 : dictionaryFlag) != 0) { unsupported("external zstd dictionaries are not supplied"); }
        const unsigned sizeBytes = sizeFlag == 0 ? (single ? 1 : 0) : sizeFlag == 1 ? 2 : sizeFlag == 2 ? 4 : 8;
        const auto expected = sizeBytes ? reader.le(sizeBytes) + (sizeBytes == 2 ? 256 : 0) : UINT64_MAX;
        if (single) { window = expected; }
        if (window > options.maxWindowBytes) { limit("zstd frame window exceeds policy"); }
        if (expected != UINT64_MAX && expected > options.maxOutputBytes - output.size()) { limit("zstd declared output exceeds policy"); }
        const auto start = output.size(); std::array<std::size_t, 3> repeats{1, 4, 8}; ZContext context;
        bool last;
        do {
            const auto header = reader.le(3); last = (header & 1) != 0;
            const auto type = (header >> 1) & 3, size = header >> 3;
            if (size > std::min<std::uint64_t>(131072, window) && !(size == 0 && window == 0)) { invalid("zstd block exceeds frame window"); }
            if (type == 0 || type == 1) {
                room(output, size, options.maxOutputBytes);
                if (type == 0) { auto data = reader.span(size); output.insert(output.end(), data.begin(), data.end()); }
                else { output.insert(output.end(), size, reader.byte()); }
            } else if (type == 2) { decodeBlock(reader.span(size), output, options, static_cast<std::size_t>(window), start, repeats, context); }
            else { invalid("reserved zstd block type"); }
        } while (!last);
        if (expected != UINT64_MAX && output.size() - start != expected) { invalid("zstd frame size mismatch"); }
        if ((descriptor & 4) && reader.le(4) != (xxh64(SReadOnlyByteSpan(output).subspan(start)) & 0xffffffffu)) { invalid("zstd content checksum mismatch"); }
    } while (reader.remaining());
    return output;
}

} // namespace detail
} // namespace hncomp
