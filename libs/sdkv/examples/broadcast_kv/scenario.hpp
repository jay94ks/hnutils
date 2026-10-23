#ifndef __SDKV_EXAMPLES_BROADCAST_KV_SCENARIO_HPP__
#define __SDKV_EXAMPLES_BROADCAST_KV_SCENARIO_HPP__

#include "protocol.hpp"

#include <socketpp.hpp>

#include <memory>

namespace sdkv_example {

/** Each owner's records expire five seconds after its last fresh beacon. */
inline constexpr std::uint64_t RECORD_TTL_MS = 5000;

/** Beacons advertise only locally owned records once per second. */
inline constexpr std::uint64_t BROADCAST_INTERVAL_MS = 1000;

/** Owned copy of transport-level discovery metadata, kept outside sdkv. */
struct SPeerInfo {
    std::string id;
    std::string ip;
    std::uint64_t expiresAt = 0;
};

/**
 * Application replication/lease policy around an opaque sdkv collection.
 * Timestamps are local monotonic milliseconds and are never sent on the wire.
 * Serialize replica operations on one event-loop thread; complete each task
 * before starting another or destroying this replica.
 */
class CReplica {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Create an empty replica with a stable id and nonzero process incarnation. */
    CReplica(std::string id, std::uint64_t incarnation);

    /** Release the replica and its local metadata. */
    ~CReplica();

    CReplica(const CReplica&) = delete;
    CReplica& operator=(const CReplica&) = delete;

    /** Return the sdkv collection that consumers query or subscribe to. */
    distkv::ITablePtr table() const;

    /** Return an owned discovery snapshot; IP comes from recvfrom, not packet claims. */
    std::vector<SPeerInfo> peers() const;

    /** Return the largest application collection observed so far. */
    std::size_t peakKeys() const noexcept;

    /** Set an owned application key; other writers' key names are rejected. */
    asyncpp::task<void> setOwned(std::string key, distkv::SBytes value);

    /** Build a new authoritative snapshot; cached remote keys are never relayed. */
    SBeacon beacon();

    /**
     * Apply a fully validated owner snapshot and refresh its records' leases.
     * @param beacon The decoded full snapshot, copied into the coroutine frame.
     * @param sourceIp The address observed by the UDP receiver.
     * @param now Local monotonic milliseconds.
     * @return true only for a fresh, accepted beacon from another host.
     */
    asyncpp::task<bool> receive(SBeacon beacon, std::string sourceIp, std::uint64_t now);

    /** Remove expired remote records via sdkv, emitting normal removal callbacks. */
    asyncpp::task<void> expire(std::uint64_t now);
};

/** Network settings for a single host process. */
struct SNodeOptions {
    std::string id;
    std::string broadcastIp = "255.255.255.255";
    std::string interfaceIp;
    std::uint16_t port = 45454;
    std::uint64_t incarnation = 1;
};

/**
 * One UDP receiver, one coroutine sender, and one independent sdkv replica.
 * Keep the node and its watcher alive until tick() completes.
 * Serialize node and replica operations on the watcher's event-loop thread.
 */
class CBroadcastNode {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Bind a reusable wildcard discovery port and a separate broadcast sender. */
    CBroadcastNode(sockpp::CSocketWatcher& watcher, SNodeOptions options);

    /** Close sockets; call only after the last tick completes. */
    ~CBroadcastNode();

    CBroadcastNode(const CBroadcastNode&) = delete;
    CBroadcastNode& operator=(const CBroadcastNode&) = delete;

    /** Return this node's encapsulated application replica. */
    CReplica& replica() noexcept;

    /** Expose an observed send count for the example's bounded validation. */
    std::uint64_t sentBeacons() const noexcept;

    /** Process bounded receive work, expire leases, and send at the one-second deadline. */
    asyncpp::task<void> tick(std::uint64_t now);
};

/** Obtain process-local steady-clock milliseconds, not wall-clock time. */
std::uint64_t MonotonicMillis();

} // namespace sdkv_example

#endif
