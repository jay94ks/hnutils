#ifndef __INCLUDE_HNQUIC_COMMON_HPP__
#define __INCLUDE_HNQUIC_COMMON_HPP__

#include <hntls/common.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(HNQUIC_SHARED)
#if defined(HNQUIC_BUILDING_LIBRARY)
#define HNQUIC_API __declspec(dllexport)
#else
#define HNQUIC_API __declspec(dllimport)
#endif
#else
#define HNQUIC_API
#endif

namespace hnquic {
/** Owned stream/application bytes; no native/mapped buffers escape. */
using SBytes = std::vector<std::uint8_t>;
/** QUIC operational outcomes; transport/application error numbers are separate diagnostics. */
enum ERetCode {
    ERET_OK,
    ERET_EOF,
    ERET_NOT_IMPLEMENTED,
    ERET_INVALID_ARGUMENT,
    ERET_INVALID_STATE,
    ERET_WRONG_EXECUTOR,
    ERET_BUSY,
    ERET_CANCELLED,
    ERET_TIMEOUT,
    ERET_LIMIT,
    ERET_CLOSED,
    ERET_STREAM_RESET,
    ERET_IO,
    ERET_UNSUPPORTED
};
/** Stream direction determines permitted read/write operations. */
enum EStreamKind { ESTREAM_BIDIRECTIONAL, ESTREAM_UNIDIRECTIONAL };
/** Endpoint role determines native QUIC stream-ID bits. */
enum ERole { EROLE_CLIENT, EROLE_SERVER };
/** Lifecycle; TLS completion and QUIC confirmation remain distinct. */
enum EConnectionState {
    ESTATE_UNAVAILABLE,
    ESTATE_IDLE,
    ESTATE_HANDSHAKING,
    ESTATE_ESTABLISHED,
    ESTATE_DRAINING,
    ESTATE_CLOSED,
    ESTATE_FAILED
};
/** Owned per-operation policy, without implicit retries or early-data replay. */
using SOperationOptions = hntls::SOperationOptions;
/** Copied failure evidence; factory construction never implies a successful connection. */
struct SQuicError {
    ERetCode code = ERET_NOT_IMPLEMENTED;
    std::optional<std::uint64_t> transportError;
    std::optional<std::uint64_t> applicationError;
    std::optional<std::uint64_t> streamId;
    std::string message;
};
} // namespace hnquic
#endif
