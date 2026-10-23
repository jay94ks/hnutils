#include "internal.hpp"

#include <optional>
#include <numeric>
#include <string_view>

namespace hncomp {
namespace detail {
namespace {

#include "brotli_dictionary.inc"

constexpr unsigned INSERT_BASE[]{0,1,2,3,4,5,6,8,10,14,18,26,34,50,66,98,130,194,322,578,1090,2114,6210,22594};
constexpr unsigned INSERT_BITS[]{0,0,0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,12,14,24};
constexpr unsigned COPY_BASE[]{2,3,4,5,6,7,8,9,10,12,14,18,22,30,38,54,70,102,134,198,326,582,1094,2118};
constexpr unsigned COPY_BITS[]{0,0,0,0,0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,24};
constexpr unsigned INSERT_GROUP[]{0,0,0,0,8,8,0,16,8,16,16};
constexpr unsigned COPY_GROUP[]{0,8,0,8,0,8,16,0,16,8,16};

unsigned number(BitsIn& bits) {
    if (bits.get(1) == 0) { return 1; }
    const auto width = bits.get(3);
    return width == 0 ? 2 : (1u << width) + bits.get(width) + 1;
}

void zeroAlign(BitsIn& bits) {
    const auto count = static_cast<unsigned>((8 - bits.bit() % 8) % 8);
    if (bits.get(count)) { invalid("nonzero Brotli padding"); }
}

class BroTree {
private:
    std::optional<unsigned> _single;
    std::unique_ptr<Huffman> _tree;

public:
    explicit BroTree(std::vector<unsigned> lengths) {
        unsigned count = 0, only = 0, space = 0;
        for (unsigned i = 0; i < lengths.size(); ++i) { if (lengths[i]) { ++count; only = i; space += 32768u >> lengths[i]; } }
        if (count == 1) { _single = only; }
        else {
            if (count == 0 || space != 32768) { invalid("incomplete Brotli prefix tree"); }
            _tree = std::make_unique<Huffman>(lengths);
        }
    }
    unsigned decode(BitsIn& bits) const { return _single ? *_single : _tree->decode(bits); }
};

BroTree readTree(BitsIn& bits, unsigned alphabet) {
    const auto skip = bits.get(2);
    std::vector<unsigned> lengths(alphabet, 0);
    if (skip == 1) {
        const auto count = bits.get(2) + 1;
        const auto width = static_cast<unsigned>(std::bit_width(alphabet - 1));
        std::vector<unsigned> symbols;
        for (unsigned i = 0; i < count; ++i) {
            const auto symbol = bits.get(width);
            if (symbol >= alphabet || std::find(symbols.begin(), symbols.end(), symbol) != symbols.end()) { invalid("invalid simple Brotli tree"); }
            symbols.push_back(symbol);
        }
        const bool skew = count == 4 && bits.get(1);
        for (unsigned i = 0; i < count; ++i) {
            lengths[symbols[i]] = count <= 2 ? 1 : count == 3 ? (i == 0 ? 1 : 2) : skew ? (i == 0 ? 1 : i == 1 ? 2 : 3) : 2;
        }
        return BroTree(std::move(lengths));
    }
    constexpr unsigned order[]{1,2,3,4,0,5,17,6,16,7,8,9,10,11,12,13,14,15};
    std::vector<unsigned> codeLengths(18, 0);
    unsigned space = 32, nonzero = 0;
    for (unsigned i = skip; i < 18 && space; ++i) {
        const auto prefix = bits.get(2);
        unsigned length;
        if (prefix == 0) { length = 0; }
        else if (prefix == 1) { length = 4; }
        else if (prefix == 2) { length = 3; }
        else if (bits.get(1) == 0) { length = 2; }
        else { length = bits.get(1) ? 5 : 1; }
        codeLengths[order[i]] = length;
        if (length) { const auto weight = 32u >> length; if (weight > space) { invalid("over-subscribed Brotli code length alphabet"); } space -= weight; ++nonzero; }
    }
    if ((space != 0 && nonzero != 1) || nonzero == 0) { invalid("invalid Brotli code length alphabet"); }
    BroTree codes(std::move(codeLengths));
    std::size_t position = 0; unsigned remaining = 32768, previous = 8, lastRepeat = 0, repeat = 0;
    while (position < alphabet && remaining) {
        const auto symbol = codes.decode(bits);
        if (symbol < 16) {
            lengths[position++] = symbol; lastRepeat = 0; repeat = 0;
            if (symbol) { const auto weight = 32768u >> symbol; if (weight > remaining) { invalid("over-subscribed Brotli tree"); } remaining -= weight; previous = symbol; }
        } else {
            const auto width = symbol == 16 ? 2u : 3u;
            const auto base = bits.get(width) + 3;
            unsigned updated = base;
            if (lastRepeat == symbol) {
                if (repeat > alphabet) { invalid("Brotli prefix repeat overflow"); }
                updated += (repeat - 2) << width;
            } else { repeat = 0; }
            const auto count = updated - repeat;
            if (count > alphabet - position) { invalid("Brotli prefix repeat exceeds alphabet"); }
            const auto value = symbol == 16 ? previous : 0;
            for (unsigned i = 0; i < count; ++i) { lengths[position++] = value; }
            if (value) {
                const auto weight = count * (32768u >> value);
                if (weight > remaining) { invalid("Brotli repeat over-subscribes tree"); }
                remaining -= weight;
            }
            repeat = updated; lastRepeat = symbol;
        }
    }
    if (remaining) { invalid("incomplete Brotli tree"); }
    return BroTree(std::move(lengths));
}

struct BlockCategory {
    unsigned types, current = 0, previous = 1;
    std::size_t remaining = SIZE_MAX;
    std::optional<BroTree> typeTree, countTree;

    explicit BlockCategory(BitsIn& bits) : types(number(bits)) {
        if (types > 1) {
            typeTree.emplace(readTree(bits, types + 2)); countTree.emplace(readTree(bits, 26));
            remaining = count(bits);
        }
    }
    std::size_t count(BitsIn& bits) {
        constexpr unsigned base[]{1,5,9,13,17,25,33,41,49,65,81,97,113,145,177,209,241,305,369,497,753,1265,2289,4337,8433,16625};
        constexpr unsigned widths[]{2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,6,6,7,8,9,10,11,12,13,24};
        const auto code = countTree->decode(bits); return base[code] + bits.get(widths[code]);
    }
    unsigned next(BitsIn& bits) {
        if (remaining == 0) {
            const auto code = typeTree->decode(bits);
            const auto selected = code == 0 ? previous : code == 1 ? (current + 1) % types : code - 2;
            if (selected >= types) { invalid("invalid Brotli block switch"); }
            previous = current; current = selected; remaining = count(bits);
        }
        if (types > 1) { --remaining; }
        return current;
    }
};

std::vector<unsigned> contextMap(BitsIn& bits, unsigned trees, unsigned size) {
    std::vector<unsigned> map(size, 0);
    if (trees == 1) { return map; }
    const auto rle = bits.get(1) ? bits.get(4) + 1 : 0;
    auto tree = readTree(bits, trees + rle);
    for (std::size_t pos = 0; pos < map.size();) {
        const auto value = tree.decode(bits);
        if (value == 0) { ++pos; }
        else if (value <= rle) {
            const auto length = (1u << value) + bits.get(value);
            if (length > map.size() - pos) { invalid("Brotli context run exceeds map"); }
            pos += length;
        } else { map[pos++] = value - rle; }
    }
    if (bits.get(1)) {
        std::array<unsigned, 256> mtf{}; std::iota(mtf.begin(), mtf.end(), 0);
        for (auto& value : map) { const auto index = value; value = mtf[index]; std::rotate(mtf.begin(), mtf.begin() + index, mtf.begin() + index + 1); }
    }
    std::vector<bool> used(trees, false);
    for (auto value : map) { if (value >= trees) { invalid("Brotli context references missing tree"); } used[value] = true; }
    if (std::find(used.begin(), used.end(), false) != used.end()) { invalid("unused Brotli context tree"); }
    return map;
}

unsigned literalContext(unsigned mode, const SBytes& output) {
    const auto a = output.empty() ? 0 : output.back(), b = output.size() < 2 ? 0 : output[output.size() - 2];
    switch (mode) {
    case 0: return a & 63;
    case 1: return a >> 2;
    case 2: return BROTLI_LUT0[a] | BROTLI_LUT1[b];
    default: return (BROTLI_LUT2[a] << 3) | BROTLI_LUT2[b];
    }
}

SBytes dictionaryWord(unsigned length, std::size_t word) {
    constexpr unsigned depths[]{0,0,0,0,10,10,11,11,10,10,10,10,10,9,9,8,7,7,8,7,7,6,6,5,5};
    if (length < 4 || length > 24 || (word >> depths[length]) >= 121) { invalid("invalid Brotli static dictionary reference"); }
    std::size_t offset = 0;
    for (unsigned i = 4; i < length; ++i) { offset += std::size_t(i) << depths[i]; }
    offset += (word & ((std::size_t{1} << depths[length]) - 1)) * length;
    SBytes value(BROTLI_DICTIONARY + offset, BROTLI_DICTIONARY + offset + length);
    const auto& transform = BROTLI_TRANSFORMS[word >> depths[length]];
    if (transform.type >= 3 && transform.type <= 11) { value.erase(value.begin(), value.begin() + std::min<std::size_t>(value.size(), transform.type - 2)); }
    else if (transform.type >= 12) { value.resize(value.size() - std::min<std::size_t>(value.size(), transform.type - 11)); }
    else if (transform.type) {
        for (std::size_t pos = 0; pos < value.size();) {
            const auto ch = value[pos];
            if (ch < 192) { if (ch >= 'a' && ch <= 'z') { value[pos] ^= 32; } ++pos; }
            else if (ch < 224) { if (pos + 1 < value.size()) { value[pos + 1] ^= 32; } pos += 2; }
            else { if (pos + 2 < value.size()) { value[pos + 2] ^= 5; } pos += 3; }
            if (transform.type == 1) { break; }
        }
    }
    const std::string_view prefix(transform.prefix), suffix(transform.suffix);
    SBytes result(prefix.begin(), prefix.end()); result.insert(result.end(), value.begin(), value.end()); result.insert(result.end(), suffix.begin(), suffix.end());
    return result;
}

void simpleTree(BitsOut& bits, unsigned alphabet, std::vector<unsigned> symbols) {
    bits.put(1, 2); bits.put(symbols.size() - 1, 2);
    for (auto symbol : symbols) { bits.put(symbol, std::bit_width(alphabet - 1)); }
}

void literalTree(BitsOut& bits) {
    // --> A singleton code-length alphabet containing 8 represents 256 equal eight-bit codes.
    bits.put(0, 2);
    constexpr unsigned order[]{1,2,3,4,0,5,17,6,16,7,8,9,10,11,12,13,14,15};
    for (auto symbol : order) { if (symbol == 8) { bits.put(7, 4); } else { bits.put(0, 2); } }
}

unsigned codeFor(unsigned value, const unsigned* bases) {
    unsigned code = 0; while (code < 23 && bases[code + 1] <= value) { ++code; } return code;
}

unsigned command(unsigned insert, unsigned copy) {
    constexpr unsigned group[3][3]{{2,3,6},{4,5,8},{7,9,10}};
    return group[insert / 8][copy / 8] * 64 + (insert & 7) * 8 + (copy & 7);
}

void encodeMeta(BitsOut& bits, SReadOnlyByteSpan input) {
    std::vector<std::size_t> heads(65536, SIZE_MAX);
    std::size_t bestPosition = input.size(), bestLength = 0, bestDistance = 1;
    for (std::size_t pos = 0; pos + 4 <= input.size(); ++pos) {
        const auto hash = ((input[pos] * 251u + input[pos + 1]) * 251u + input[pos + 2]) & 65535u;
        const auto previous = std::exchange(heads[hash], pos);
        if (previous == SIZE_MAX || pos - previous > 65520) { continue; }
        std::size_t count = 0;
        while (pos + count < input.size() && input[pos + count] == input[previous + count]) { ++count; }
        if (count > bestLength) { bestPosition = pos; bestLength = count; bestDistance = pos - previous; }
        if (pos + count == input.size() && count > 8) { break; }
        if (count >= 64) { pos += count - 1; }
    }
    if (bestLength < 4) { bestPosition = input.size(); bestLength = 0; }
    const auto tail = input.size() - bestPosition - bestLength;
    const auto firstInsert = codeFor(static_cast<unsigned>(bestPosition), INSERT_BASE);
    const auto firstCopy = codeFor(static_cast<unsigned>(bestLength ? bestLength : 2), COPY_BASE);
    const auto firstCommand = command(firstInsert, firstCopy);
    const auto tailInsert = codeFor(static_cast<unsigned>(tail), INSERT_BASE);
    const auto tailCommand = command(tailInsert, 0);
    std::vector<unsigned> commands{firstCommand};
    if (tail && tailCommand != firstCommand) { commands.push_back(tailCommand); }
    std::sort(commands.begin(), commands.end());
    unsigned distanceCode = 16, distanceBits = 1, distanceBase = 1;
    for (; distanceCode < 64; ++distanceCode) {
        distanceBits = 1 + ((distanceCode - 16) >> 1);
        distanceBase = ((2 + ((distanceCode - 16) & 1)) << distanceBits) - 3;
        if (bestDistance >= distanceBase && bestDistance - distanceBase < (1u << distanceBits)) { break; }
    }
    bits.put(0, 1); bits.put(0, 2); bits.put(input.size() - 1, 16); bits.put(0, 1);
    bits.put(0, 3); bits.put(0, 6); bits.put(0, 2); bits.put(0, 2);
    literalTree(bits); simpleTree(bits, 704, commands); simpleTree(bits, 64, {distanceCode});
    auto writeCommand = [&](unsigned code, unsigned insertCode, unsigned copyCode, std::size_t insert, std::size_t copy) {
        if (commands.size() == 2) { bits.put(code == commands[1], 1); }
        bits.put(insert - INSERT_BASE[insertCode], INSERT_BITS[insertCode]);
        bits.put(copy - COPY_BASE[copyCode], COPY_BITS[copyCode]);
    };
    writeCommand(firstCommand, firstInsert, firstCopy, bestPosition, bestLength ? bestLength : 2);
    for (std::size_t i = 0; i < bestPosition; ++i) { bits.put(reverse(input[i], 8), 8); }
    if (bestLength) { bits.put(bestDistance - distanceBase, distanceBits); }
    if (tail) {
        writeCommand(tailCommand, tailInsert, 0, tail, 2);
        for (std::size_t i = input.size() - tail; i < input.size(); ++i) { bits.put(reverse(input[i], 8), 8); }
    }
}

} // namespace

/* Standard Brotli stream with real backward copies and canonical literal prefix coding. */
SBytes encodeBrotli(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    BitsOut bits; bits.put(0, 1); // --> WBITS=16, a 65520-byte backward window.
    if (options.maxWindowBytes < 65520) { limit("Brotli encoder requires a 65520-byte window"); }
    for (std::size_t pos = 0; pos < input.size();) {
        const auto chunk = input.subspan(pos, std::min(options.blockBytes, input.size() - pos));
        encodeMeta(bits, chunk); pos += chunk.size();
    }
    bits.put(1, 1); bits.put(1, 1); bits.align(); return bits.drain();
}

/* Standard prefix/context/block-switch decoding, including all 121 static dictionary transforms. */
SBytes decodeBrotli(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    BitsIn bits(input); unsigned windowBits;
    if (bits.get(1) == 0) { windowBits = 16; }
    else {
        const auto first = bits.get(3);
        if (first) { windowBits = first + 17; }
        else { const auto second = bits.get(3); if (second == 1) { invalid("reserved Brotli window code"); } windowBits = second == 0 ? 17 : second + 8; }
    }
    const std::size_t window = (std::size_t{1} << windowBits) - 16;
    if (window > options.maxWindowBytes) { limit("Brotli window exceeds policy"); }
    SBytes output; std::array<std::size_t, 4> distances{4, 11, 15, 16};
    for (;;) {
        const bool last = bits.get(1) != 0;
        if (last && bits.get(1)) { zeroAlign(bits); if (bits.bytes() != input.size()) { invalid("trailing Brotli bytes"); } return output; }
        const auto nibbles = bits.get(2) + 4;
        if (nibbles == 7) {
            if (bits.get(1)) { invalid("reserved Brotli metadata flag"); }
            const auto bytes = bits.get(2); const auto size = bytes ? std::size_t(bits.get(bytes * 8)) + 1 : 0;
            if (bytes > 1 && size <= (std::size_t{1} << ((bytes - 1) * 8))) { invalid("noncanonical Brotli metadata length"); }
            zeroAlign(bits); for (std::size_t i = 0; i < size; ++i) { (void)bits.get(8); }
            continue;
        }
        const auto length = std::size_t(bits.get(nibbles * 4)) + 1;
        if (nibbles > 4 && length <= (std::size_t{1} << ((nibbles - 1) * 4))) { invalid("noncanonical Brotli meta-block length"); }
        room(output, length, options.maxOutputBytes);
        if (!last && bits.get(1)) {
            zeroAlign(bits); for (std::size_t i = 0; i < length; ++i) { output.push_back(static_cast<std::uint8_t>(bits.get(8))); }
            continue;
        }
        BlockCategory literalBlocks(bits), commandBlocks(bits), distanceBlocks(bits);
        const auto postfix = bits.get(2), direct = bits.get(4) << postfix;
        std::vector<unsigned> contextModes;
        for (unsigned i = 0; i < literalBlocks.types; ++i) { contextModes.push_back(bits.get(2)); }
        const auto literalTrees = number(bits);
        const auto literalMap = contextMap(bits, literalTrees, literalBlocks.types * 64);
        const auto distanceTrees = number(bits);
        const auto distanceMap = contextMap(bits, distanceTrees, distanceBlocks.types * 4);
        std::vector<BroTree> literals, commands, distanceCodes;
        for (unsigned i = 0; i < literalTrees; ++i) { literals.emplace_back(readTree(bits, 256)); }
        for (unsigned i = 0; i < commandBlocks.types; ++i) { commands.emplace_back(readTree(bits, 704)); }
        for (unsigned i = 0; i < distanceTrees; ++i) { distanceCodes.emplace_back(readTree(bits, 16 + direct + (48u << postfix))); }
        const auto end = output.size() + length;
        while (output.size() < end) {
            const auto startBit = bits.bit(), startOutput = output.size();
            const auto symbol = commands[commandBlocks.next(bits)].decode(bits), group = symbol / 64;
            const auto insertCode = INSERT_GROUP[group] + ((symbol >> 3) & 7), copyCode = COPY_GROUP[group] + (symbol & 7);
            const auto insert = INSERT_BASE[insertCode] + bits.get(INSERT_BITS[insertCode]);
            const auto count = COPY_BASE[copyCode] + bits.get(COPY_BITS[copyCode]);
            if (insert > end - output.size()) { invalid("Brotli literal insertion exceeds meta-block"); }
            for (unsigned i = 0; i < insert; ++i) {
                const auto block = literalBlocks.next(bits), context = literalContext(contextModes[block], output);
                output.push_back(static_cast<std::uint8_t>(literals[literalMap[block * 64 + context]].decode(bits)));
            }
            if (output.size() == end) { break; }
            const auto code = symbol < 128 ? 0 : distanceCodes[distanceMap[distanceBlocks.next(bits) * 4 + std::min(count - 2, 3u)]].decode(bits);
            std::size_t distance;
            if (code < 4) { distance = distances[code]; }
            else if (code < 16) {
                const auto index = code < 10 ? 0u : 1u;
                const auto value = code < 10 ? code - 4 : code - 10;
                const auto adjustment = static_cast<int>(value / 2 + 1) * (value & 1 ? 1 : -1);
                if (adjustment < 0 && distances[index] <= static_cast<unsigned>(-adjustment)) { invalid("nonpositive Brotli distance"); }
                distance = static_cast<std::size_t>(static_cast<std::int64_t>(distances[index]) + adjustment);
            } else if (code < 16 + direct) { distance = code - 15; }
            else {
                const auto h = (code - direct - 16) >> postfix, low = (code - direct - 16) & ((1u << postfix) - 1);
                const auto width = 1 + (h >> 1);
                distance = ((((2u + (h & 1)) << width) - 4 + bits.get(width)) << postfix) + low + direct + 1;
            }
            if (distance > std::min(window, output.size())) {
                const auto word = dictionaryWord(count, distance - std::min(window, output.size()) - 1);
                if (word.size() > end - output.size()) { invalid("Brotli dictionary word exceeds meta-block"); }
                output.insert(output.end(), word.begin(), word.end());
            } else {
                if (count > end - output.size()) { invalid("Brotli copy exceeds meta-block"); }
                copyMatch(output, distance, count, options.maxOutputBytes);
                if (code != 0) { distances = {distance, distances[0], distances[1], distances[2]}; }
            }
            if (startBit == bits.bit() && startOutput == output.size()) { invalid("nonprogressing Brotli command"); }
        }
        if (last) { zeroAlign(bits); if (bits.bytes() != input.size()) { invalid("trailing Brotli bytes"); } return output; }
    }
}

} // namespace detail
} // namespace hncomp
