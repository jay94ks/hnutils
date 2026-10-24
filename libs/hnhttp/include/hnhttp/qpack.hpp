#ifndef __INCLUDE_HNHTTP_QPACK_HPP__
#define __INCLUDE_HNHTTP_QPACK_HPP__
#include <hnhttp/header_codec.hpp>

namespace hnhttp {
/** Hard bounds for directional QPACK tables, outstanding references and blocked streams. */
struct SQpackOptions {
    SHeaderCodecOptions codec;
    std::size_t maximumBlockedStreams = 16;
    std::size_t maximumOutstandingSections = 1024;
};
/**
 * QPACK field sections and two independent instruction streams. Methods are serialized
 * by the caller. No transport is retained; all returned instruction bytes are owned.
 * A truncated instruction fragment is retained until more input, not treated as malformed.
 * A blocked field section remains caller-owned and can be retried after encoder progress.
 */
class HNHTTP_API IQpackCodec : public IHeaderCodec {
public:
    /** Release compression state and outstanding reference bookkeeping. */
    virtual ~IQpackCodec() = default;
    /** Construct bounded local decoder state and an initially zero-capacity encoder. */
    static std::shared_ptr<IQpackCodec> create(SQpackOptions options = {});
    /** Apply peer SETTINGS before encoding dynamic references; cannot reconfigure live references. */
    virtual ERetCode configurePeer(std::uint64_t maximumTableCapacity, std::uint64_t blockedStreams) = 0;
    /** Emit Set Dynamic Table Capacity; refuse to evict unacknowledged referenced entries. */
    virtual SHeaderEncodeResult setEncoderCapacity(std::size_t capacity) = 0;
    /** Insert a non-sensitive field; returned bytes belong on the encoder stream before use. */
    virtual SHeaderEncodeResult insert(SHeaderField field) = 0;
    /** Duplicate an encoder entry using its current relative index. */
    virtual SHeaderEncodeResult duplicate(std::uint64_t relativeIndex) = 0;
    /** Encode with a native stream ID and bounded optional blocking; never index secrets. */
    virtual SHeaderEncodeResult encodeSection(std::uint64_t streamId, std::span<const SHeaderField> fields,
        bool allowBlocking = false) = 0;
    /** Decode or return BLOCKED without poisoning state; successful dynamic sections queue an ACK. */
    virtual SHeaderDecodeResult decodeSection(std::uint64_t streamId, std::span<const std::uint8_t> block) = 0;
    /** Incrementally apply peer encoder instructions; malformed state is permanently poisoned. */
    virtual SHttpError receiveEncoderInstructions(std::span<const std::uint8_t> bytes) = 0;
    /** Incrementally apply section ACK, stream cancellation and insert-count increment. */
    virtual SHttpError receiveDecoderInstructions(std::span<const std::uint8_t> bytes) = 0;
    /** Cancel a blocked/in-progress receive section and emit Stream Cancellation. */
    virtual SHeaderEncodeResult cancelStream(std::uint64_t streamId) = 0;
    /** Take bounded queued decoder instructions (ACK/increments), clearing only the output queue. */
    virtual SBytes takeDecoderInstructions() = 0;
    /** Total received insertions, including evicted entries; not current table size. */
    virtual std::uint64_t receivedInsertCount() const noexcept = 0;
};
}
#endif
