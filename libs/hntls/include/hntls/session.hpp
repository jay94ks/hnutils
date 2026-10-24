#ifndef __INCLUDE_HNTLS_SESSION_HPP__
#define __INCLUDE_HNTLS_SESSION_HPP__

#include <hntls/common.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>
#include <memory>

namespace certpp {
class CSecureOctet;
namespace x509 {
class CCert;
}
} // namespace certpp
namespace hntls {
/** One owned handshake flight. QUIC mode uses CRYPTO bytes without TLS record framing. */
struct SHandshakeFlight {
    EEncryptionLevel level = ELEVEL_INITIAL;
    SBytes bytes;
};
/** Immutable secure traffic-secret snapshot, never a pointer to mutable session key storage. */
struct STrafficSecret {
    EEncryptionLevel level = ELEVEL_INITIAL;
    EDirection direction = EDIRECTION_READ;
    std::uint64_t generation = 0;
    std::shared_ptr<const certpp::CSecureOctet> value;
    /** Negotiated TLS cipher-suite code; zero is not a usable traffic secret. */
    std::uint16_t cipherSuite = 0;
};
/** Transport-neutral owned handshake output; failures never publish keys or plaintext. */
struct SHandshakeResult {
    STlsError result;
    std::vector<SHandshakeFlight> outgoing;
    std::vector<STrafficSecret> secrets;
    bool handshakeComplete = false;
    /** Record-mode application plaintext only; always empty in QUIC mode. */
    std::vector<SBytes> plaintext;
    /** A verified TLS close_notify, not transport EOF or local abort. */
    bool peerClosedWrite = false;
};
/** Copied negotiated metadata. QUIC handshake confirmation belongs to hnquic, not this snapshot. */
struct SPeerInfo {
    std::string alpn;
    EPeerAuthentication authentication = EPEER_NOT_CHECKED;
    std::vector<std::shared_ptr<const certpp::x509::CCert>> certificateChain;
    std::optional<std::uint16_t> cipherSuite;
    /** Authenticated peer extension bytes; hnquic validates transport-parameter semantics. */
    SBytes quicTransportParameters;
};
/**
 * Serial-executor TLS state contract. All operations except executor(), isImplemented(),
 * close() and ownership release are executor-affine. Inputs/results own their storage.
 * QUIC mode never emits TLS records; record mode never handles UDP packet protection.
 */
class HNTLS_API ITlsSession {
  public:
    /** Release after the owner has joined outstanding operations. */
    virtual ~ITlsSession() = default;
    /** Inspect actual implementation availability, not configuration validity. */
    virtual bool isImplemented() const noexcept = 0;
    /** Inspect the selected wire-integration mode. */
    virtual EMode mode() const noexcept = 0;
    /** Inspect lifecycle without inferring trust from object creation. */
    virtual ESessionState state() const noexcept = 0;
    /** Stable owning serial executor reference, callable from any thread. */
    virtual std::shared_ptr<asyncpp::dispatcher> executor() const noexcept = 0;
    /** Begin a TLS 1.3 handshake; server sessions await a ClientHello. */
    virtual asyncpp::task<SHandshakeResult> start(SOperationOptions options = {}) = 0;
    /**
     * Consume incremental records or contiguous QUIC CRYPTO bytes at an explicit level.
     * Record mode derives encryption levels internally and ignores the level argument.
     * Partial messages remain bounded by policy; record plaintext is returned in the result.
     */
    virtual asyncpp::task<SHandshakeResult> receive(EEncryptionLevel level, SBytes bytes,
                                                    SOperationOptions options = {}) = 0;
    /** Produce TLS application records only in record mode after a verified handshake. */
    virtual asyncpp::task<SBytesResult> encrypt(SBytes plaintext, SOperationOptions options = {}) = 0;
    /** Produce record-mode close_notify after preceding writes; QUIC uses CONNECTION_CLOSE instead. */
    virtual asyncpp::task<SBytesResult> finishWrite(SOperationOptions options = {}) = 0;
    /** Inspect copied negotiated metadata; trust requires certificate, signature and Finished checks. */
    virtual SPeerInfo peerInfo() const = 0;
    /** Copy the most recent failure diagnostic. */
    virtual STlsError lastError() const = 0;
    /** Thread-safe nonblocking abort; not an implicit successful close_notify exchange. */
    virtual void close() noexcept = 0;
    /** Join pending work after close without stopping the caller's executor. */
    virtual asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) = 0;
};
} // namespace hntls
#endif
