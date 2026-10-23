#ifndef __INCLUDE_SDKV_COMMON_HPP__
#define __INCLUDE_SDKV_COMMON_HPP__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#if defined(_WIN32) && defined(SDKV_SHARED)
#if defined(SDKV_BUILDING_LIBRARY)
#define SDKV_API __declspec(dllexport)
#else
#define SDKV_API __declspec(dllimport)
#endif
#else
#define SDKV_API
#endif

namespace distkv {

/** Owned, uninterpreted key or value bytes, including embedded zero bytes. */
using SBytes = std::vector<std::uint8_t>;

/** Borrowed bytes used only for the duration of a synchronous call. */
using SReadOnlyByteSpan = std::span<const std::uint8_t>;

/** Maximum stored value length; split larger data across application-defined keys. */
inline constexpr std::size_t MAX_VALUE_SIZE = 4096;

/** Operation outcomes; allocation failures still throw std::bad_alloc. */
enum ERetCode {
    ERET_OK = 0,                // --> The operation succeeded, including an unchanged value.
    ERET_NOT_FOUND,             // --> The key does not exist.
    ERET_ALREADY_EXISTS,        // --> The key or synchronization source already exists.
    ERET_CONDITION_FAILED,      // --> The expected presence/value did not match.
    ERET_VALUE_TOO_LARGE,       // --> A value exceeds MAX_VALUE_SIZE.
    ERET_INVALID_ARGUMENT,      // --> A null source or direct self-subscription was supplied.
    ERET_REVISION_OVERFLOW,     // --> An increment would wrap a revision/sequence number.
};

/** An owned copy of a value and its local table revision. */
struct STableValue {
    /** Uninterpreted payload, at most MAX_VALUE_SIZE bytes. */
    SBytes bytes;

    /** Key-local counter incremented on real mutations; never reset by removal. */
    std::uint64_t revision = 0;
};

/** One owned key/value entry in a consistent table snapshot. */
struct STableEntry {
    /** Uninterpreted key; empty keys are allowed. */
    SBytes key;

    /** Owned value and its local revision. */
    STableValue value;
};

/** Owned result of an asynchronous lookup. */
struct SGetResult {
    /** ERET_OK or ERET_NOT_FOUND. */
    ERetCode code = ERET_NOT_FOUND;

    /** Present only for a successful lookup, including an empty payload. */
    std::optional<STableValue> value;
};

} // namespace distkv

#endif
