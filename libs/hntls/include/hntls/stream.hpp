#ifndef __INCLUDE_HNTLS_STREAM_HPP__
#define __INCLUDE_HNTLS_STREAM_HPP__
#include <hntls/context.hpp>
#include <hntls/session.hpp>

namespace hntls {
/** Partial progress on an owned byte write, not peer acknowledgment. */
struct SWriteResult {
    STlsError result;
    std::size_t acceptedBytes = 0;
};
/**
 * Ordered transport boundary for record-mode TLS. One read and one write may
 * coexist; same-direction overlap returns BUSY. Initiation/completion are affine to
 * the stable serial executor. EOF is empty and distinct from native failure.
 */
class IByteStream {
  public:
    /** Release only after pending operations have been joined. */
    virtual ~IByteStream() = default;
    /** Stable owning serial dispatcher, callable from any thread. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Return nonempty owned data bounded by maximumBytes, or empty EOF. */
    virtual asyncpp::task<SBytesResult> readSome(std::size_t maximumBytes,
                                                 SOperationOptions options = {}) = 0;
    /** Accept an owned buffer, retaining it until native completion. */
    virtual asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions options = {}) = 0;
    /** Finish the send direction after previous bytes. */
    virtual asyncpp::task<ERetCode> finishWrite(SOperationOptions options = {}) = 0;
    /** Thread-safe nonblocking abort; wake pending operations on the executor. */
    virtual void close() noexcept = 0;
    /** Join all callbacks/buffer accesses; never stop the caller's dispatcher. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};
/**
 * Secure application-byte stream owned by hntls, not an HTTP-side record implementation.
 * TLS 1.3 handshake, records, traffic keys and authenticated application I/O live here.
 * Creation performs no I/O; await handshake() before application operations.
 */
class HNTLS_API ITlsStream : public IByteStream {
  public:
    /** Release retained policy and transport ownership. */
    virtual ~ITlsStream() = default;
    /** Create a record-mode TLS stream without performing transport I/O. */
    static std::shared_ptr<ITlsStream> create(std::shared_ptr<IByteStream> transport,
                                              std::shared_ptr<ITlsContext> context);
    /** Availability, not successful construction or negotiation. */
    virtual bool isImplemented() const noexcept = 0;
    /** Run the TLS handshake before application I/O; no hidden background thread. */
    virtual asyncpp::task<ERetCode> handshake(SOperationOptions options = {}) = 0;
    /** Inspect session lifecycle on the executor. */
    virtual ESessionState state() const noexcept = 0;
    /** Copy negotiated metadata; incomplete handshakes never imply authentication. */
    virtual SPeerInfo peerInfo() const = 0;
};
} // namespace hntls
#endif
