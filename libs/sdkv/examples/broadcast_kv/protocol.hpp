#ifndef __SDKV_EXAMPLES_BROADCAST_KV_PROTOCOL_HPP__
#define __SDKV_EXAMPLES_BROADCAST_KV_PROTOCOL_HPP__

#include <sdkv.hpp>

#include <optional>
#include <span>
#include <string>

namespace sdkv_example {

/** One owner's current application value, not a host-discovery record. */
struct SRecord {
    std::string key;
    distkv::SBytes value;
    std::uint64_t revision = 0;
};

/** A full snapshot of one writer's owned keys, carried in one UDP datagram. */
struct SBeacon {
    std::string owner;
    std::uint64_t incarnation = 0;
    std::uint64_t sequence = 0;
    std::vector<SRecord> records;
};

/** The example deliberately avoids fragmented, large application datagrams. */
inline constexpr std::size_t MAX_DATAGRAM = 1200;

/** This example's payload limit; sdkv itself still supports 4096-byte values. */
inline constexpr std::size_t MAX_DEMO_VALUE = 512;

/** Validate a bounded, printable node identifier. */
bool ValidId(std::string_view id) noexcept;

/** Encode a bounded demo packet; invalid application input throws. */
distkv::SBytes Encode(const SBeacon& beacon);

/** Reject malformed, truncated, duplicate-key, or oversized packets atomically. */
std::optional<SBeacon> Decode(std::span<const std::byte> packet);

/** Test the example's single-writer rule without adding policy to sdkv. */
bool Owns(std::string_view owner, std::string_view key);

} // namespace sdkv_example

#endif
