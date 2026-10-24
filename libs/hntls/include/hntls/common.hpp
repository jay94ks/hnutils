#ifndef __INCLUDE_HNTLS_COMMON_HPP__
#define __INCLUDE_HNTLS_COMMON_HPP__

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(HNTLS_SHARED)
#if defined(HNTLS_BUILDING_LIBRARY)
#define HNTLS_API __declspec(dllexport)
#else
#define HNTLS_API __declspec(dllimport)
#endif
#else
#define HNTLS_API
#endif

namespace hntls {
/** Owned wire bytes; secret material is carried separately in libcertpp secure storage. */
using SBytes = std::vector<std::uint8_t>;
/** Fail-closed outcomes, independent from TLS alert numbers. */
enum ERetCode {
    ERET_OK,
    ERET_NOT_IMPLEMENTED,
    ERET_INVALID_ARGUMENT,
    ERET_INVALID_STATE,
    ERET_WRONG_EXECUTOR,
    ERET_CANCELLED,
    ERET_TIMEOUT,
    ERET_LIMIT,
    ERET_AUTHENTICATION,
    ERET_CLOSED,
    ERET_EOF,
    ERET_BUSY,
    ERET_IO,
    ERET_TRUNCATED,
    ERET_UNSUPPORTED
};
/** TLS endpoint role. */
enum ERole { EROLE_CLIENT, EROLE_SERVER };
/** Record transport and QUIC handshake integration are different wire protocols. */
enum EMode { EMODE_RECORDS, EMODE_QUIC };
/** QUIC encryption levels; not TLS record content types. */
enum EEncryptionLevel { ELEVEL_INITIAL, ELEVEL_EARLY_DATA, ELEVEL_HANDSHAKE, ELEVEL_APPLICATION };
/** Direction of an installed traffic secret. */
enum EDirection { EDIRECTION_READ, EDIRECTION_WRITE };
/** Lifecycle; only ESTATE_ESTABLISHED denotes a completed handshake. */
enum ESessionState {
    ESTATE_UNAVAILABLE,
    ESTATE_NEW,
    ESTATE_HANDSHAKING,
    ESTATE_ESTABLISHED,
    ESTATE_FAILED,
    ESTATE_CLOSED
};
/** Explicit trust outcome; merely receiving a certificate is not authentication. */
enum EPeerAuthentication { EPEER_NOT_CHECKED, EPEER_ACCEPTED, EPEER_REJECTED };
/** Per-operation cancellation; live tokens/deadlines must never be silently ignored. */
struct SOperationOptions {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::stop_token cancellation;
};
/** Copied failure evidence. Alert is absent when no TLS alert was produced. */
struct STlsError {
    ERetCode code = ERET_NOT_IMPLEMENTED;
    std::optional<std::uint8_t> alert;
    std::string message;
};
/** Owned application/ciphertext output, empty on failures. */
struct SBytesResult {
    STlsError result;
    SBytes bytes;
};
} // namespace hntls
#endif
