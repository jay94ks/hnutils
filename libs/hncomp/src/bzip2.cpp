#include "internal.hpp"

#include <numeric>

namespace hncomp {
namespace detail {
namespace {

class MsbOut {
private:
    SBytes _bytes;
    unsigned _count = 0;

public:
    void put(std::uint64_t value, unsigned count) {
        while (count--) {
            if (_count == 0) { _bytes.push_back(0); }
            _bytes.back() |= static_cast<std::uint8_t>(((value >> count) & 1) << (7 - _count));
            _count = (_count + 1) % 8;
        }
    }
    SBytes take() { return std::move(_bytes); }
};

class MsbIn {
private:
    SReadOnlyByteSpan _input;
    std::size_t _bit = 0;

public:
    explicit MsbIn(SReadOnlyByteSpan input) : _input(input) {}
    std::uint64_t get(unsigned count) {
        if (count > _input.size() * 8 - _bit) { throw NeedInput{}; }
        std::uint64_t value = 0;
        while (count--) { value = (value << 1) | ((_input[_bit / 8] >> (7 - _bit % 8)) & 1); ++_bit; }
        return value;
    }
    bool end() {
        const auto remainder = static_cast<unsigned>((8 - _bit % 8) % 8);
        if (get(remainder)) { return false; }
        return _bit / 8 == _input.size();
    }
};

std::uint32_t checksum(SReadOnlyByteSpan input) {
    std::uint32_t crc = 0xffffffffu;
    for (auto value : input) {
        crc ^= std::uint32_t(value) << 24;
        for (unsigned i = 0; i < 8; ++i) { crc = (crc << 1) ^ (0x04c11db7u & (0u - (crc >> 31))); }
    }
    return ~crc;
}

SBytes firstRle(SReadOnlyByteSpan input) {
    SBytes output;
    for (std::size_t i = 0; i < input.size();) {
        std::size_t count = 1;
        while (count < 259 && i + count < input.size() && input[i + count] == input[i]) { ++count; }
        output.insert(output.end(), std::min<std::size_t>(4, count), input[i]);
        if (count >= 4) { output.push_back(static_cast<std::uint8_t>(count - 4)); }
        i += count;
    }
    return output;
}

std::pair<SBytes, unsigned> bwt(SReadOnlyByteSpan input) {
    const unsigned n = static_cast<unsigned>(input.size());
    std::vector<unsigned> order(n), ranks(n), next(n), shifted(n), count(std::max<unsigned>(256, n));
    for (auto value : input) { ++count[value]; }
    for (unsigned i = 1; i < 256; ++i) { count[i] += count[i - 1]; }
    for (unsigned i = n; i--;) { order[--count[input[i]]] = i; }
    unsigned classes = 1;
    for (unsigned i = 0; i < n; ++i) {
        if (i && input[order[i]] != input[order[i - 1]]) { ++classes; }
        ranks[order[i]] = classes - 1;
    }
    for (unsigned shift = 1; shift < n && classes < n; shift *= 2) {
        std::fill(count.begin(), count.begin() + classes, 0);
        for (unsigned i = 0; i < n; ++i) { shifted[i] = (order[i] + n - shift) % n; ++count[ranks[shifted[i]]]; }
        for (unsigned i = 1; i < classes; ++i) { count[i] += count[i - 1]; }
        for (unsigned i = n; i--;) { order[--count[ranks[shifted[i]]]] = shifted[i]; }
        unsigned updated = 1; next[order[0]] = 0;
        for (unsigned i = 1; i < n; ++i) {
            const auto a = order[i - 1], b = order[i];
            if (ranks[a] != ranks[b] || ranks[(a + shift) % n] != ranks[(b + shift) % n]) { ++updated; }
            next[b] = updated - 1;
        }
        ranks.swap(next); classes = updated;
    }
    SBytes output(n); unsigned original = 0;
    for (unsigned i = 0; i < n; ++i) { output[i] = input[(order[i] + n - 1) % n]; if (order[i] == 0) { original = i; } }
    return {std::move(output), original};
}

class BzTree {
private:
    std::array<unsigned, 21> _count{};
    std::vector<unsigned> _symbols;

public:
    explicit BzTree(const std::vector<unsigned>& lengths) {
        int left = 1;
        for (auto length : lengths) { if (length == 0 || length > 20) { invalid("invalid bzip2 Huffman length"); } ++_count[length]; }
        for (unsigned i = 1; i <= 20; ++i) { left = left * 2 - static_cast<int>(_count[i]); if (left < 0) { invalid("over-subscribed bzip2 tree"); } }
        for (unsigned length = 1; length <= 20; ++length) {
            for (unsigned i = 0; i < lengths.size(); ++i) { if (lengths[i] == length) { _symbols.push_back(i); } }
        }
    }
    unsigned decode(MsbIn& input) const {
        unsigned code = 0, first = 0, index = 0;
        for (unsigned length = 1; length <= 20; ++length) {
            code = (code << 1) | static_cast<unsigned>(input.get(1));
            if (code >= first && code - first < _count[length]) { return _symbols[index + code - first]; }
            index += _count[length]; first = (first + _count[length]) << 1;
        }
        invalid("invalid bzip2 Huffman symbol");
    }
};

} // namespace

/* RLE1 -> cyclic BWT -> MTF -> RUNA/RUNB -> canonical Huffman, with block CRCs. */
SBytes encodeBzip2(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    MsbOut bits; bits.put(0x425a6831u, 32); std::uint32_t combined = 0;
    for (std::size_t pos = 0; pos < input.size();) {
        auto chunk = input.subspan(pos, std::min(options.blockBytes, input.size() - pos));
        auto transformed = bwt(firstRle(chunk));
        const auto crc = checksum(chunk); combined = std::rotl(combined, 1) ^ crc;
        bits.put(0x314159265359ull, 48); bits.put(crc, 32); bits.put(0, 1); bits.put(transformed.second, 24);
        std::array<bool, 256> used{};
        for (auto value : transformed.first) { used[value] = true; }
        std::array<bool, 16> groups{};
        for (unsigned i = 0; i < 256; ++i) { if (used[i]) { groups[i / 16] = true; } }
        for (bool group : groups) { bits.put(group, 1); }
        for (unsigned group = 0; group < 16; ++group) {
            if (groups[group]) { for (unsigned i = 0; i < 16; ++i) { bits.put(used[group * 16 + i], 1); } }
        }
        std::vector<std::uint8_t> alphabet;
        for (unsigned i = 0; i < 256; ++i) { if (used[i]) { alphabet.push_back(static_cast<std::uint8_t>(i)); } }
        const auto alphabetSize = static_cast<unsigned>(alphabet.size()) + 2;
        std::vector<unsigned> symbols; unsigned zeroes = 0;
        auto flushZeroes = [&] {
            if (!zeroes) { return; }
            auto value = zeroes - 1;
            for (;;) { symbols.push_back(value & 1); if (value < 2) { break; } value = (value - 2) / 2; }
            zeroes = 0;
        };
        for (auto value : transformed.first) {
            auto it = std::find(alphabet.begin(), alphabet.end(), value);
            const auto index = static_cast<unsigned>(it - alphabet.begin());
            if (index == 0) { ++zeroes; }
            else { flushZeroes(); symbols.push_back(index + 1); std::rotate(alphabet.begin(), it, it + 1); }
        }
        flushZeroes(); symbols.push_back(alphabetSize - 1);
        const auto selectors = (symbols.size() + 49) / 50;
        bits.put(2, 3); bits.put(selectors, 15);
        for (std::size_t i = 0; i < selectors; ++i) { bits.put(0, 1); }
        const auto width = std::bit_width(alphabetSize - 1);
        for (unsigned table = 0; table < 2; ++table) { bits.put(width, 5); for (unsigned i = 0; i < alphabetSize; ++i) { bits.put(0, 1); } }
        for (auto symbol : symbols) { bits.put(symbol, width); }
        pos += chunk.size();
    }
    bits.put(0x177245385090ull, 48); bits.put(combined, 32); return bits.take();
}

/* Standard multi-block bzip2, including selector MTF and changing Huffman tables. */
SBytes decodeBzip2(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    MsbIn bits(input);
    if (bits.get(24) != 0x425a68u) { invalid("invalid bzip2 header"); }
    const auto level = bits.get(8);
    if (level < '1' || level > '9') { invalid("invalid bzip2 block size"); }
    const auto maximum = static_cast<std::size_t>(level - '0') * 100000;
    if (maximum > options.maxWindowBytes) { limit("bzip2 block exceeds window policy"); }
    SBytes output; std::uint32_t combined = 0;
    for (;;) {
        const auto magic = bits.get(48);
        if (magic == 0x177245385090ull) {
            if (bits.get(32) != combined || !bits.end()) { invalid("bzip2 combined checksum/trailer mismatch"); }
            return output;
        }
        if (magic != 0x314159265359ull) { invalid("invalid bzip2 block magic"); }
        const auto crc = static_cast<std::uint32_t>(bits.get(32));
        if (bits.get(1)) { unsupported("obsolete randomized bzip2 blocks are not supported"); }
        const auto original = static_cast<std::size_t>(bits.get(24));
        std::array<bool, 16> groups{}; for (auto&& group : groups) { group = bits.get(1) != 0; }
        SBytes alphabet;
        for (unsigned group = 0; group < 16; ++group) {
            if (groups[group]) { for (unsigned i = 0; i < 16; ++i) { if (bits.get(1)) { alphabet.push_back(static_cast<std::uint8_t>(group * 16 + i)); } } }
        }
        if (alphabet.empty()) { invalid("empty bzip2 alphabet"); }
        const auto size = static_cast<unsigned>(alphabet.size()) + 2;
        const auto tableCount = static_cast<unsigned>(bits.get(3)), selectorCount = static_cast<unsigned>(bits.get(15));
        if (tableCount < 2 || tableCount > 6 || selectorCount == 0 || selectorCount > 18002) { invalid("invalid bzip2 table/selector count"); }
        std::vector<unsigned> selectors; std::vector<unsigned> tableOrder(tableCount); std::iota(tableOrder.begin(), tableOrder.end(), 0);
        for (unsigned i = 0; i < selectorCount; ++i) {
            unsigned index = 0;
            while (bits.get(1)) { if (++index >= tableCount) { invalid("invalid bzip2 selector"); } }
            selectors.push_back(tableOrder[index]); std::rotate(tableOrder.begin(), tableOrder.begin() + index, tableOrder.begin() + index + 1);
        }
        std::vector<BzTree> trees;
        for (unsigned table = 0; table < tableCount; ++table) {
            int length = static_cast<int>(bits.get(5)); std::vector<unsigned> lengths;
            for (unsigned i = 0; i < size; ++i) {
                for (;;) {
                    if (length < 1 || length > 20) { invalid("invalid bzip2 code length"); }
                    if (bits.get(1) == 0) { break; }
                    length += bits.get(1) ? -1 : 1;
                }
                lengths.push_back(static_cast<unsigned>(length));
            }
            trees.emplace_back(lengths);
        }
        SBytes last; std::size_t symbolCount = 0, run = 0, power = 1;
        for (;;) {
            const auto group = symbolCount++ / 50;
            if (group >= selectors.size()) { invalid("bzip2 selectors exhausted"); }
            const auto symbol = trees[selectors[group]].decode(bits);
            if (symbol <= 1) {
                if (power > maximum || (symbol + 1) * power > maximum - run) { limit("bzip2 zero run exceeds block limit"); }
                run += (symbol + 1) * power; power *= 2; continue;
            }
            if (run) { room(last, run, maximum); last.insert(last.end(), run, alphabet.front()); run = 0; power = 1; }
            if (symbol == size - 1) { break; }
            const auto index = symbol - 1;
            if (index >= alphabet.size()) { invalid("invalid bzip2 MTF index"); }
            room(last, 1, maximum); last.push_back(alphabet[index]);
            std::rotate(alphabet.begin(), alphabet.begin() + index, alphabet.begin() + index + 1);
        }
        if (last.empty() || original >= last.size()) { invalid("bzip2 BWT origin outside block"); }
        std::array<std::size_t, 256> counts{};
        for (auto ch : last) { ++counts[ch]; }
        std::size_t sum = 0;
        for (auto& count : counts) { const auto previous = count; count = sum; sum += previous; }
        std::vector<std::size_t> next(last.size());
        for (std::size_t i = 0; i < last.size(); ++i) { next[counts[last[i]]++] = i; }
        auto row = next[original]; SBytes rle; rle.reserve(last.size());
        for (std::size_t i = 0; i < last.size(); ++i) { rle.push_back(last[row]); row = next[row]; }
        SBytes block;
        for (std::size_t i = 0; i < rle.size();) {
            const auto value = rle[i++]; std::size_t count = 1;
            while (count < 4 && i < rle.size() && rle[i] == value) { ++count; ++i; }
            if (count == 4) { if (i == rle.size()) { invalid("missing bzip2 RLE count"); } count += rle[i++]; }
            room(output, block.size(), options.maxOutputBytes);
            room(block, count, options.maxOutputBytes - output.size()); block.insert(block.end(), count, value);
        }
        if (checksum(block) != crc) { invalid("bzip2 block checksum mismatch"); }
        output.insert(output.end(), block.begin(), block.end()); combined = std::rotl(combined, 1) ^ crc;
    }
}

} // namespace detail
} // namespace hncomp
