#ifndef __INCLUDE_HNQUIC_STREAM_HPP__
#define __INCLUDE_HNQUIC_STREAM_HPP__

#include <hnquic/common.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

namespace hnquic {
/** Owned chunk; successful EOF must eventually mean a validated FIN, not a native failure. */
struct SReadResult {
    SQuicError result;
    SBytes bytes;
};
/** Accepted byte count, not peer acknowledgment. */
struct SWriteResult {
    SQuicError result;
    std::size_t acceptedBytes = 0;
};
/**
 * Native QUIC stream contract with owned buffers and directional stream state.
 * Operations are executor-affine except executor(), abortRead(), abortWrite() and release.
 * One read and one write may be pending concurrently; same-direction overlap returns BUSY.
 * Successful reads are nonempty, EOF is empty and requires FIN validation. Direction-invalid
 * operations fail explicitly. The connection run task drives flow control and recovery.
 */
class IQuicStream {
  public:
    /** Release only after pending native work has retained its buffers and been joined. */
    virtual ~IQuicStream() = default;
    /** Native 62-bit stream ID; zero is valid. */
    virtual std::uint64_t id() const noexcept = 0;
    /** Inspect uni/bidirectional semantics. */
    virtual EStreamKind kind() const noexcept = 0;
    /** Inspect the initiating endpoint. */
    virtual ERole initiator() const noexcept = 0;
    /** Stable caller-owned serial executor. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Suspend for owned bytes without blocking other streams. */
    virtual asyncpp::task<SReadResult> readSome(std::size_t maximumBytes, SOperationOptions options = {}) = 0;
    /** Suspend for send credit, allowing bounded partial progress. */
    virtual asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions options = {}) = 0;
    /** Send FIN after preceding writes; this is not CONNECTION_CLOSE. */
    virtual asyncpp::task<ERetCode> finishWrite(SOperationOptions options = {}) = 0;
    /** Request STOP_SENDING without cancelling unrelated streams. */
    virtual void abortRead(std::uint64_t applicationError) noexcept = 0;
    /** Request RESET_STREAM, preserving final-size accounting. */
    virtual void abortWrite(std::uint64_t applicationError) noexcept = 0;
};
/** Failed stream results always have a null stream. */
struct SStreamResult {
    SQuicError result;
    std::shared_ptr<IQuicStream> stream;
};
} // namespace hnquic
#endif
