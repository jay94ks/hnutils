#include "scenario.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sdkv_example {
namespace {

distkv::SBytes keyBytes(std::string_view key) {
    return distkv::SBytes(key.begin(), key.end());
}

std::string keyText(const distkv::SBytes& key) {
    return std::string(key.begin(), key.end());
}

void check(distkv::ERetCode code) {
    if (code != distkv::ERET_OK) { throw std::runtime_error("sdkv application update failed"); }
}

} // namespace

struct CReplica::Impl {
    struct Peer {
        SPeerInfo info;
        std::uint64_t incarnation = 0;
        std::uint64_t sequence = 0;
        std::set<std::string> keys;
        std::set<std::uint64_t> retiredIncarnations;
    };

    std::string id;
    std::uint64_t incarnation = 0;
    std::uint64_t sequence = 0;
    distkv::ITablePtr table = distkv::ITable::create();
    std::map<std::string, Peer> peers;
    std::size_t peakKeys = 0;

    void updatePeak() { peakKeys = std::max(peakKeys, table->snapshot().size()); }
};

/* Keep ownership, discovery, wire ordering, and TTL out of sdkv's generic core. */
CReplica::CReplica(std::string id, std::uint64_t incarnation) : _impl(std::make_unique<Impl>()) {
    if (!ValidId(id) || incarnation == 0) { throw std::invalid_argument("invalid replica identity"); }
    _impl->id = std::move(id);
    _impl->incarnation = incarnation;
}

/* Destroy owned application state. */
CReplica::~CReplica() = default;

/* Share the real sdkv application table, not the host discovery map. */
distkv::ITablePtr CReplica::table() const { return _impl->table; }

/* Return copies of discovery metadata. */
std::vector<SPeerInfo> CReplica::peers() const {
    std::vector<SPeerInfo> result;
    for (const auto& [id, peer] : _impl->peers) { (void)id; result.push_back(peer.info); }
    return result;
}

/* Return the peak application key count. */
std::size_t CReplica::peakKeys() const noexcept { return _impl->peakKeys; }

/* Only the explicit owner may write these demonstration keys. */
asyncpp::task<void> CReplica::setOwned(std::string key, distkv::SBytes value) {
    if (!Owns(_impl->id, key) || value.size() > MAX_DEMO_VALUE) {
        throw std::invalid_argument("key is not owned by this node or demo value is too large");
    }
    auto bytes = keyBytes(key);
    auto code = co_await _impl->table->addAsync(bytes, value);
    if (code == distkv::ERET_ALREADY_EXISTS) {
        code = co_await _impl->table->setAsync(std::move(bytes), std::move(value));
    }
    check(code);
    _impl->updatePeak();
}

/* Advertise only the original writer's values, never cached replicas. */
SBeacon CReplica::beacon() {
    if (_impl->sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("beacon sequence exhausted");
    }
    SBeacon result{_impl->id, _impl->incarnation, ++_impl->sequence, {}};
    for (const auto& entry : _impl->table->snapshot()) {
        const auto key = keyText(entry.key);
        if (Owns(_impl->id, key)) { result.records.push_back({key, entry.value.bytes, entry.value.revision}); }
    }
    return result;
}

/* Validate the whole snapshot before mutating application data or lease metadata. */
asyncpp::task<bool> CReplica::receive(SBeacon beacon, std::string sourceIp, std::uint64_t now) {
    if (beacon.owner == _impl->id || !ValidId(beacon.owner) || beacon.incarnation == 0 ||
        beacon.sequence == 0 || beacon.records.size() > 2 || sourceIp.empty() ||
        now > std::numeric_limits<std::uint64_t>::max() - RECORD_TTL_MS) { co_return false; }
    std::set<std::string> keys;
    for (const auto& record : beacon.records) {
        if (!Owns(beacon.owner, record.key) || record.value.size() > MAX_DEMO_VALUE ||
            record.revision == 0 || !keys.insert(record.key).second) { co_return false; }
    }
    const auto previous = _impl->peers.find(beacon.owner);
    Impl::Peer peer;
    if (previous != _impl->peers.end()) {
        peer = previous->second;
        if (peer.retiredIncarnations.contains(beacon.incarnation)) { co_return false; }
        if (peer.incarnation == beacon.incarnation && beacon.sequence <= peer.sequence) { co_return false; }
        if (peer.incarnation != beacon.incarnation) { peer.retiredIncarnations.insert(peer.incarnation); }
    }
    for (const auto& key : peer.keys) {
        if (!keys.contains(key)) {
            const auto code = co_await _impl->table->removeAsync(keyBytes(key));
            if (code != distkv::ERET_NOT_FOUND) { check(code); }
        }
    }
    for (auto& record : beacon.records) {
        auto key = keyBytes(record.key);
        auto code = co_await _impl->table->addAsync(key, record.value);
        if (code == distkv::ERET_ALREADY_EXISTS) {
            code = co_await _impl->table->setAsync(std::move(key), std::move(record.value));
        }
        check(code);
    }
    peer.info = SPeerInfo{beacon.owner, std::move(sourceIp), now + RECORD_TTL_MS};
    peer.incarnation = beacon.incarnation;
    peer.sequence = beacon.sequence;
    peer.keys = std::move(keys);
    _impl->peers.insert_or_assign(beacon.owner, std::move(peer));
    _impl->updatePeak();
    co_return true;
}

/* Receiving another replica's advertisements cannot prolong an absent owner's lease. */
asyncpp::task<void> CReplica::expire(std::uint64_t now) {
    for (auto it = _impl->peers.begin(); it != _impl->peers.end();) {
        if (now < it->second.info.expiresAt) { ++it; continue; }
        for (const auto& key : it->second.keys) {
            const auto code = co_await _impl->table->removeAsync(keyBytes(key));
            if (code != distkv::ERET_NOT_FOUND) { check(code); }
        }
        it = _impl->peers.erase(it);
    }
}

struct CBroadcastNode::Impl {
    SNodeOptions options;
    CReplica replica;
    sockpp::CSocket receiver;
    sockpp::CUdpBroadcastAsync sender;
    sockpp::CSocketAddress destination;
    std::array<std::byte, 65536> buffer{};
    std::optional<std::uint64_t> nextBroadcast;
    std::uint64_t sent = 0;

    Impl(sockpp::CSocketWatcher& watcher, SNodeOptions configuration)
        : options(std::move(configuration)), replica(options.id, options.incarnation),
          receiver(sockpp::CSocket::open(sockpp::EAF_INET, sockpp::ESOCK_DATAGRAM, sockpp::EPROTO_UDP)),
          sender(sockpp::CUdpBroadcastAsync::open(watcher)),
          destination(sockpp::CSocketAddress::parse(options.broadcastIp, options.port)) {
        if (options.port == 0 || destination.family() != sockpp::EAF_INET) {
            throw std::invalid_argument("discovery requires a nonzero UDP port and IPv4 broadcast address");
        }
        receiver.setOption(sockpp::opts::SReuseAddress{true});
        receiver.setOption(sockpp::opts::SBroadcast{true});
        receiver.bind(sockpp::CSocketAddress::anyIpv4(options.port));
        receiver.setBlocking(false);
        if (!options.interfaceIp.empty()) {
            sender.bind(sockpp::CSocketAddress::parse(options.interfaceIp, 0));
        }
    }
};

/* Separate the reusable receive bind from sender interface/routing selection. */
CBroadcastNode::CBroadcastNode(sockpp::CSocketWatcher& watcher, SNodeOptions options)
    : _impl(std::make_unique<Impl>(watcher, std::move(options))) {}

/* No receive task is left pending when a node is stopped or destroyed. */
CBroadcastNode::~CBroadcastNode() = default;

/* Expose only the application replica API. */
CReplica& CBroadcastNode::replica() noexcept { return _impl->replica; }

/* Return a read-only send counter. */
std::uint64_t CBroadcastNode::sentBeacons() const noexcept { return _impl->sent; }

/* Do bounded nonblocking receives, then one coroutine broadcast at its deadline. */
asyncpp::task<void> CBroadcastNode::tick(std::uint64_t now) {
    for (unsigned count = 0; count < 64; ++count) {
        sockpp::CSocketAddress source;
        std::size_t size = 0;
        try {
            size = _impl->receiver.receiveFrom(_impl->buffer, source);
        } catch (const std::system_error& error) {
            if (sockpp::IsSocketWouldBlock(error.code())) { break; }
            throw;
        }
        auto beacon = Decode(std::span<const std::byte>(_impl->buffer.data(), size));
        if (beacon) {
            (void)co_await _impl->replica.receive(std::move(*beacon), source.toString(), now);
        }
    }
    co_await _impl->replica.expire(now);
    if (!_impl->nextBroadcast || now >= *_impl->nextBroadcast) {
        auto packet = Encode(_impl->replica.beacon());
        const auto sent = co_await _impl->sender.sendTo(
            std::as_bytes(std::span(packet)), _impl->destination);
        if (sent != packet.size()) { throw std::runtime_error("partial UDP datagram send"); }
        ++_impl->sent;
        // --> Skip missed periods rather than flooding the LAN with catch-up sends.
        _impl->nextBroadcast = now + BROADCAST_INTERVAL_MS;
    }
}

/* All lease decisions use a local monotonic clock. */
std::uint64_t MonotonicMillis() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace sdkv_example
