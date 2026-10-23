#include <hncomp/common.hpp>

#include "internal.hpp"

namespace hncomp {

/* Preserve a categorized immutable diagnostic. */
CCompressionError::CCompressionError(EError code, const char* message) : std::runtime_error(message), _code(code) {}

/* Return the category. */
EError CCompressionError::code() const noexcept { return _code; }

namespace detail {

/* Normalize wire formats once; reject invalid policies instead of guessing. */
SCompressionOptions validate(SCompressionOptions options) {
    if (options.algorithm < EALGORITHM_LZ77 || options.algorithm > EALGORITHM_LZ4 ||
        options.level < 1 || options.level > 9 || options.blockBytes < 1024 || options.blockBytes > 65536 ||
        options.maxWindowBytes < 32768 || options.maxWindowBytes > 256 * 1024 * 1024 ||
        options.maxDictionaryEntries > 1048576) {
        throw std::invalid_argument("invalid hncomp algorithm, level, block size or window limit");
    }
    if (options.format == EFORMAT_DEFAULT) {
        options.format = options.algorithm == EALGORITHM_DEFLATE ? EFORMAT_ZLIB :
            options.algorithm == EALGORITHM_LZMA ? EFORMAT_ALONE :
            (options.algorithm == EALGORITHM_LZ4 || options.algorithm == EALGORITHM_SNAPPY) ? EFORMAT_FRAMED : EFORMAT_RAW;
    }
    const bool accepted = options.format == EFORMAT_CHUNKED || (options.algorithm == EALGORITHM_DEFLATE ?
        (options.format == EFORMAT_RAW || options.format == EFORMAT_ZLIB || options.format == EFORMAT_GZIP) :
        options.algorithm == EALGORITHM_LZMA ? options.format == EFORMAT_ALONE :
        (options.algorithm == EALGORITHM_LZ4 || options.algorithm == EALGORITHM_SNAPPY) ?
        (options.format == EFORMAT_RAW || options.format == EFORMAT_FRAMED) : options.format == EFORMAT_RAW);
    if (!accepted) { unsupported("wire format is not defined for this algorithm"); }
    return options;
}

/* Reflected CRC implementation shared by gzip and Snappy (different polynomials). */
std::uint32_t crc32(SReadOnlyByteSpan input, std::uint32_t polynomial) {
    std::uint32_t result = 0xffffffffu;
    for (auto value : input) {
        result ^= value;
        for (unsigned bit = 0; bit < 8; ++bit) { result = (result >> 1) ^ (polynomial & (0u - (result & 1))); }
    }
    return ~result;
}

/* Incremental Adler-32 with bounded arithmetic. */
std::uint32_t adler32(SReadOnlyByteSpan input, std::uint32_t previous) {
    std::uint32_t a = previous & 65535u, b = previous >> 16;
    for (auto value : input) { a = (a + value) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

/* xxHash32, specified arithmetic with explicit little-endian reads. */
std::uint32_t xxh32(SReadOnlyByteSpan input, std::uint32_t seed) {
    constexpr std::uint32_t p1 = 2654435761u, p2 = 2246822519u, p3 = 3266489917u, p4 = 668265263u, p5 = 374761393u;
    Reader reader(input);
    auto round = [](std::uint32_t a, std::uint32_t b) { return std::rotl(a + b * p2, 13) * p1; };
    std::uint32_t hash;
    if (input.size() >= 16) {
        std::uint32_t a = seed + p1 + p2, b = seed + p2, c = seed, d = seed - p1;
        do {
            a = round(a, static_cast<std::uint32_t>(reader.le(4)));
            b = round(b, static_cast<std::uint32_t>(reader.le(4)));
            c = round(c, static_cast<std::uint32_t>(reader.le(4)));
            d = round(d, static_cast<std::uint32_t>(reader.le(4)));
        } while (reader.remaining() >= 16);
        hash = std::rotl(a, 1) + std::rotl(b, 7) + std::rotl(c, 12) + std::rotl(d, 18);
    } else { hash = seed + p5; }
    hash += static_cast<std::uint32_t>(input.size());
    while (reader.remaining() >= 4) { hash = std::rotl(hash + static_cast<std::uint32_t>(reader.le(4)) * p3, 17) * p4; }
    while (reader.remaining()) { hash = std::rotl(hash + reader.byte() * p5, 11) * p1; }
    hash ^= hash >> 15; hash *= p2; hash ^= hash >> 13; hash *= p3; hash ^= hash >> 16;
    return hash;
}

/* xxHash64 content checksum used by Zstandard. */
std::uint64_t xxh64(SReadOnlyByteSpan input) {
    constexpr std::uint64_t p1 = 11400714785074694791ull, p2 = 14029467366897019727ull,
        p3 = 1609587929392839161ull, p4 = 9650029242287828579ull, p5 = 2870177450012600261ull;
    Reader reader(input);
    auto round = [](std::uint64_t a, std::uint64_t b) { return std::rotl(a + b * p2, 31) * p1; };
    auto merge = [&](std::uint64_t a, std::uint64_t b) { return (a ^ round(0, b)) * p1 + p4; };
    std::uint64_t hash;
    if (input.size() >= 32) {
        std::uint64_t a = p1 + p2, b = p2, c = 0, d = 0 - p1;
        do { a = round(a, reader.le(8)); b = round(b, reader.le(8)); c = round(c, reader.le(8)); d = round(d, reader.le(8)); }
        while (reader.remaining() >= 32);
        hash = std::rotl(a, 1) + std::rotl(b, 7) + std::rotl(c, 12) + std::rotl(d, 18);
        hash = merge(merge(merge(merge(hash, a), b), c), d);
    } else { hash = p5; }
    hash += input.size();
    while (reader.remaining() >= 8) { hash = std::rotl(hash ^ round(0, reader.le(8)), 27) * p1 + p4; }
    if (reader.remaining() >= 4) { hash = std::rotl(hash ^ (reader.le(4) * p1), 23) * p2 + p3; }
    while (reader.remaining()) { hash = std::rotl(hash ^ (std::uint64_t(reader.byte()) * p5), 11) * p1; }
    hash ^= hash >> 33; hash *= p2; hash ^= hash >> 29; hash *= p3; hash ^= hash >> 32;
    return hash;
}

} // namespace detail
} // namespace hncomp
