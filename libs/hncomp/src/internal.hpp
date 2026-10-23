#ifndef __SRC_HNCOMP_INTERNAL_HPP__
#define __SRC_HNCOMP_INTERNAL_HPP__

#include <hncomp.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <utility>

namespace hncomp {
namespace detail {

/** Internal transactional read underflow, never part of the public API. */
struct NeedInput {};

[[noreturn]] inline void invalid(const char* message) { throw CCompressionError(EERROR_INVALID_DATA, message); }
[[noreturn]] inline void limit(const char* message) { throw CCompressionError(EERROR_LIMIT, message); }
[[noreturn]] inline void unsupported(const char* message) { throw CCompressionError(EERROR_UNSUPPORTED, message); }

inline void room(const SBytes& output, std::size_t count, std::size_t maximum) {
    if (output.size() > maximum || count > maximum - output.size()) { limit("decompressed/output size limit exceeded"); }
}

inline void copyMatch(SBytes& output, std::size_t distance, std::size_t length, std::size_t maximum) {
    if (distance == 0 || distance > output.size()) { invalid("invalid backward distance"); }
    room(output, length, maximum);
    for (std::size_t i = 0; i < length; ++i) { output.push_back(output[output.size() - distance]); }
}

inline void putLe(SBytes& output, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) { output.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
}

inline void putBe(SBytes& output, std::uint64_t value, unsigned bytes) {
    for (unsigned i = bytes; i != 0; --i) { output.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1)))); }
}

class Reader {
private:
    SReadOnlyByteSpan _input;
    std::size_t _pos = 0;

public:
    explicit Reader(SReadOnlyByteSpan input) : _input(input) {}
    std::size_t pos() const noexcept { return _pos; }
    std::size_t remaining() const noexcept { return _input.size() - _pos; }
    std::uint8_t byte() {
        if (_pos == _input.size()) { throw NeedInput{}; }
        return _input[_pos++];
    }
    std::uint64_t le(unsigned count) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) { value |= std::uint64_t(byte()) << (8 * i); }
        return value;
    }
    std::uint64_t be(unsigned count) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) { value = (value << 8) | byte(); }
        return value;
    }
    SReadOnlyByteSpan span(std::size_t size) {
        if (size > remaining()) { throw NeedInput{}; }
        auto result = _input.subspan(_pos, size);
        _pos += size;
        return result;
    }
};

class BitsOut {
private:
    SBytes _bytes;
    std::uint64_t _bits = 0;
    unsigned _count = 0;

public:
    void put(std::uint64_t value, unsigned count) {
        _bits |= (value & ((std::uint64_t{1} << count) - 1)) << _count;
        _count += count;
        while (_count >= 8) { _bytes.push_back(static_cast<std::uint8_t>(_bits)); _bits >>= 8; _count -= 8; }
    }
    void align() { if (_count) { put(0, 8 - _count); } }
    SBytes drain() { return std::exchange(_bytes, {}); }
    unsigned count() const noexcept { return _count; }
};

class BitsIn {
private:
    SReadOnlyByteSpan _input;
    std::size_t _bit = 0;

public:
    explicit BitsIn(SReadOnlyByteSpan input, std::size_t bit = 0) : _input(input), _bit(bit) {}
    std::size_t bit() const noexcept { return _bit; }
    std::size_t bytes() const noexcept { return (_bit + 7) / 8; }
    std::uint32_t get(unsigned count) {
        if (count > 24 || count > _input.size() * 8 - _bit) { throw NeedInput{}; }
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i, ++_bit) { result |= ((_input[_bit / 8] >> (_bit % 8)) & 1u) << i; }
        return result;
    }
    void align() { (void)get(static_cast<unsigned>((8 - _bit % 8) % 8)); }
};

inline unsigned reverse(unsigned value, unsigned count) {
    unsigned result = 0;
    for (unsigned i = 0; i < count; ++i) { result = (result << 1) | (value & 1); value >>= 1; }
    return result;
}

/** Canonical prefix codes, with explicit over-subscription checks. */
class Huffman {
private:
    std::array<unsigned, 16> _counts{};
    std::vector<unsigned> _symbols;
    unsigned _maximum = 0;

public:
    explicit Huffman(const std::vector<unsigned>& lengths) {
        for (auto length : lengths) {
            if (length > 15) { invalid("invalid Huffman code length"); }
            if (length) { ++_counts[length]; _maximum = std::max(_maximum, length); }
        }
        int left = 1;
        for (unsigned i = 1; i <= 15; ++i) {
            left = left * 2 - static_cast<int>(_counts[i]);
            if (left < 0) { invalid("over-subscribed Huffman tree"); }
        }
        for (unsigned length = 1; length <= 15; ++length) {
            for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
                if (lengths[symbol] == length) { _symbols.push_back(symbol); }
            }
        }
    }
    unsigned decode(BitsIn& bits) const {
        unsigned code = 0, first = 0, index = 0;
        for (unsigned length = 1; length <= _maximum; ++length) {
            code = (code << 1) | bits.get(1);
            if (code >= first && code - first < _counts[length]) { return _symbols[index + code - first]; }
            index += _counts[length]; first = (first + _counts[length]) << 1;
        }
        invalid("invalid Huffman symbol");
    }
};

SCompressionOptions validate(SCompressionOptions options);
std::uint32_t crc32(SReadOnlyByteSpan input, std::uint32_t polynomial = 0xedb88320u);
std::uint32_t adler32(SReadOnlyByteSpan input, std::uint32_t previous = 1);
std::uint32_t xxh32(SReadOnlyByteSpan input, std::uint32_t seed = 0);
std::uint64_t xxh64(SReadOnlyByteSpan input);

SBytes encodeLz77(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeLz77(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeLz78(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeLz78(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeSnappy(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeSnappy(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeLz4(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeLz4(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeLzma(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeLzma(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeBzip2(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeBzip2(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeZstd(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeZstd(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes encodeBrotli(SReadOnlyByteSpan input, const SCompressionOptions& options);
SBytes decodeBrotli(SReadOnlyByteSpan input, const SCompressionOptions& options);
std::shared_ptr<IStreamCompressor> deflateEncoder(SCompressionOptions options);
std::shared_ptr<IStreamDecompressor> deflateDecoder(SCompressionOptions options);
std::shared_ptr<IStreamCompressor> chunkedEncoder(SCompressionOptions options);
std::shared_ptr<IStreamDecompressor> chunkedDecoder(SCompressionOptions options);

} // namespace detail
} // namespace hncomp

#endif
