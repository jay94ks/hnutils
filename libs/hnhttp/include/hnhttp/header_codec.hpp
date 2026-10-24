#ifndef __INCLUDE_HNHTTP_HEADER_CODEC_HPP__
#define __INCLUDE_HNHTTP_HEADER_CODEC_HPP__
#include <hnhttp/fields.hpp>
#include <span>

namespace hnhttp {
/** Limits for independently tested header-block primitives, not complete HTTP connections. */
struct SHeaderCodecOptions {
    std::size_t maximumEncodedBytes = 131072;
    std::size_t maximumDecodedBytes = 65536;
    std::size_t maximumFields = 128;
    std::size_t maximumDynamicTableBytes = 4096;
};
/** Transactional decoded output; empty on failure, including unsupported encodings. */
struct SHeaderDecodeResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::vector<SHeaderField> fields;
};
/** Header block output; an empty HPACK block is valid and distinct from body EOF. */
struct SHeaderEncodeResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    SBytes bytes;
};
/**
 * Serial-use header codec with private state. HPACK supports raw/Huffman literals and
 * static/dynamic decoding. QPACK dynamic/control capabilities are declared in qpack.hpp.
 * These synchronous bounded primitives do not drive HTTP/2 or QUIC connections.
 */
class HNHTTP_API IHeaderCodec {
public:
    /** Release private directional compression state. */
    virtual ~IHeaderCodec() = default;
    /** Create separate HPACK directional state; sensitive literals are never indexed. */
    static std::shared_ptr<IHeaderCodec> createHpack(SHeaderCodecOptions options = {});
    /** Create QPACK with a zero-capacity encoder until peer settings are explicitly configured. */
    static std::shared_ptr<IHeaderCodec> createQpack(SHeaderCodecOptions options = {});
    /** Encode bounded fields; credentials and EINDEX_NEVER remain never-indexed. */
    virtual SHeaderEncodeResult encode(std::span<const SHeaderField> fields) = 0;
    /** Decode a complete bounded block. A failed HPACK decoder is poisoned, since state may diverge. */
    virtual SHeaderDecodeResult decode(std::span<const std::uint8_t> block) = 0;
};
} // namespace hnhttp
#endif
