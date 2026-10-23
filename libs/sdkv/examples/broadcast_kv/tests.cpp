#include "scenario.hpp"

#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>

#include <array>
#include <stdexcept>

namespace {

using namespace sdkv_example;
using namespace distkv;

SBytes bytes(std::string_view value) { return SBytes(value.begin(), value.end()); }

bool receive(CReplica& replica, SBeacon beacon, std::uint64_t now = 0) {
    return asyncpp::as_promise(replica.receive(std::move(beacon), "192.0.2.42", now)).get();
}

void put(CReplica& replica, std::string key, std::string value) {
    asyncpp::as_promise(replica.setOwned(std::move(key), bytes(value))).get();
}

void expire(CReplica& replica, std::uint64_t now) {
    asyncpp::as_promise(replica.expire(now)).get();
}

SBytes get(CReplica& replica, std::string_view key) {
    SBytes result;
    if (replica.table()->get(bytes(key), result) != ERET_OK) {
        throw std::runtime_error("test key missing");
    }
    return result;
}

class CObserver final : public ITableListener {
private:
    std::vector<CTableChange> _events;

public:
    void onAdded(const CTableChange& change) override { _events.push_back(change); }
    void onChanged(const CTableChange& change) override { _events.push_back(change); }
    void onRemoved(const CTableChange& change) override { _events.push_back(change); }
    const std::vector<CTableChange>& events() const { return _events; }
};

DOCTEST_TEST_SUITE_BEGIN("sdkv_broadcast_kv");

DOCTEST_TEST_CASE("broadcast KV codec rejects malformed datagrams before touching application state") {
    CReplica writer("host1", 1);
    put(writer, "demo/host1", "node application data");
    put(writer, "cluster/message", "shared application data");
    auto encoded = Encode(writer.beacon());
    auto decoded = Decode(std::as_bytes(std::span(encoded)));
    DOCTEST_REQUIRE(decoded.has_value());
    DOCTEST_CHECK(decoded->owner == "host1");
    DOCTEST_CHECK(decoded->records.size() == 2);
    DOCTEST_CHECK(encoded.size() <= MAX_DATAGRAM);
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        DOCTEST_CHECK_FALSE(Decode(std::as_bytes(std::span(encoded.data(), length))));
    }
    auto changed = encoded;
    changed[0] ^= 1;
    DOCTEST_CHECK_FALSE(Decode(std::as_bytes(std::span(changed))));
    changed = encoded;
    changed.push_back(0);
    DOCTEST_CHECK_FALSE(Decode(std::as_bytes(std::span(changed))));
    SBytes oversized(MAX_DATAGRAM + 1);
    DOCTEST_CHECK_FALSE(Decode(std::as_bytes(std::span(oversized))));
    auto invalid = *decoded;
    invalid.records = {decoded->records.front(), decoded->records.front()};
    DOCTEST_CHECK_THROWS_AS(Encode(invalid), std::invalid_argument);
    invalid = *decoded;
    invalid.records.front().value.resize(MAX_DEMO_VALUE + 1);
    DOCTEST_CHECK_THROWS_AS(Encode(invalid), std::invalid_argument);
    DOCTEST_CHECK_FALSE(ValidId("bad/id"));
}

DOCTEST_TEST_CASE("four hosts hold common SDKV application keys and observe real value changes") {
    std::array<std::unique_ptr<CReplica>, 4> replicas;
    for (std::size_t i = 0; i < replicas.size(); ++i) {
        const auto id = "host" + std::to_string(i + 1);
        replicas[i] = std::make_unique<CReplica>(id, i + 1);
        put(*replicas[i], "demo/" + id, "value-" + std::to_string(i + 1));
    }
    put(*replicas[0], "cluster/message", "first");
    auto observer = std::make_shared<CObserver>();
    auto registration = replicas[1]->table()->subscribe(observer, false);
    for (const auto& source : replicas) {
        const auto beacon = source->beacon();
        for (const auto& destination : replicas) { (void)receive(*destination, beacon); }
    }
    for (const auto& replica : replicas) {
        DOCTEST_CHECK(replica->table()->snapshot().size() == 5);
        DOCTEST_CHECK(replica->peers().size() == 3);
        DOCTEST_CHECK(replica->peers().front().ip == "192.0.2.42");
        DOCTEST_CHECK(get(*replica, "cluster/message") == bytes("first"));
        for (int i = 1; i <= 4; ++i) {
            DOCTEST_CHECK(get(*replica, "demo/host" + std::to_string(i)) == bytes("value-" + std::to_string(i)));
        }
    }
    put(*replicas[0], "cluster/message", "second");
    auto update = replicas[0]->beacon();
    for (const auto& replica : replicas) { (void)receive(*replica, update, 1000); }
    for (const auto& replica : replicas) { DOCTEST_CHECK(get(*replica, "cluster/message") == bytes("second")); }
    const auto version = replicas[1]->table()->revision();
    const auto eventCount = observer->events().size();
    auto refresh = replicas[0]->beacon();
    DOCTEST_CHECK(receive(*replicas[1], refresh, 2000));
    DOCTEST_CHECK(replicas[1]->table()->revision() == version);
    DOCTEST_CHECK(observer->events().size() == eventCount);
    const auto& changed = observer->events().back();
    DOCTEST_REQUIRE(changed.prev().has_value());
    DOCTEST_REQUIRE(changed.now().has_value());
    DOCTEST_CHECK(changed.key() == bytes("cluster/message"));
    DOCTEST_CHECK(changed.prev()->bytes == bytes("first"));
    DOCTEST_CHECK(changed.now()->bytes == bytes("second"));
    DOCTEST_CHECK(changed.now()->revision == changed.prev()->revision + 1);
}

DOCTEST_TEST_CASE("fresh heartbeats extend five second leases but stale duplicates do not") {
    CReplica owner("host1", 100);
    CReplica replica("host2", 200);
    put(owner, "demo/host1", "value");
    put(owner, "cluster/message", "shared");
    auto first = owner.beacon();
    DOCTEST_CHECK(receive(replica, first, 0));
    DOCTEST_CHECK_FALSE(receive(replica, first, 4000));
    expire(replica, 4999);
    DOCTEST_CHECK(replica.table()->snapshot().size() == 2);
    expire(replica, 5000);
    DOCTEST_CHECK(replica.table()->snapshot().empty());
    DOCTEST_CHECK(replica.peers().empty());
    DOCTEST_CHECK(receive(replica, owner.beacon(), 6000));
    DOCTEST_CHECK(receive(replica, owner.beacon(), 10000));
    expire(replica, 14999);
    DOCTEST_CHECK(replica.table()->snapshot().size() == 2);
    expire(replica, 15000);
    DOCTEST_CHECK(replica.table()->snapshot().empty());
    DOCTEST_CHECK(RECORD_TTL_MS == 5000);
    DOCTEST_CHECK(BROADCAST_INTERVAL_MS == 1000);
}

DOCTEST_TEST_CASE("cached values cannot echo indefinitely or prolong a stopped owner's TTL") {
    CReplica a("host1", 1);
    CReplica b("host2", 2);
    CReplica c("host3", 3);
    put(a, "demo/host1", "one");
    put(a, "cluster/message", "common");
    put(b, "demo/host2", "two");
    put(c, "demo/host3", "three");
    receive(b, a.beacon(), 0);
    receive(c, a.beacon(), 0);
    auto bSnapshot = b.beacon();
    DOCTEST_REQUIRE(bSnapshot.records.size() == 1);
    DOCTEST_CHECK(bSnapshot.records[0].key == "demo/host2");
    receive(c, bSnapshot, 4000);
    auto observer = std::make_shared<CObserver>();
    auto registration = c.table()->subscribe(observer, false);
    expire(c, 5000);
    SBytes value;
    DOCTEST_CHECK(c.table()->get(bytes("cluster/message"), value) == ERET_NOT_FOUND);
    DOCTEST_CHECK(c.table()->get(bytes("demo/host1"), value) == ERET_NOT_FOUND);
    DOCTEST_CHECK(c.table()->get(bytes("demo/host2"), value) == ERET_OK);
    DOCTEST_CHECK(observer->events().size() == 2);
    for (const auto& event : observer->events()) {
        DOCTEST_CHECK(event.prev().has_value());
        DOCTEST_CHECK_FALSE(event.now());
    }
    receive(c, b.beacon(), 6000);
    DOCTEST_CHECK(c.table()->get(bytes("cluster/message"), value) == ERET_NOT_FOUND);
}

DOCTEST_TEST_CASE("owner snapshot removals and process restarts retain local sdkv revision history") {
    CReplica firstOwner("host1", 10);
    CReplica reader("host2", 20);
    put(firstOwner, "cluster/message", "before");
    receive(reader, firstOwner.beacon());
    firstOwner.table()->remove(bytes("cluster/message"));
    receive(reader, firstOwner.beacon(), 100);
    SBytes missing;
    DOCTEST_CHECK(reader.table()->get(bytes("cluster/message"), missing) == ERET_NOT_FOUND);
    CReplica restarted("host1", 30);
    put(restarted, "cluster/message", "after");
    receive(reader, restarted.beacon(), 200);
    STableValue value;
    DOCTEST_CHECK(reader.table()->get(bytes("cluster/message"), value) == ERET_OK);
    DOCTEST_CHECK(value.bytes == bytes("after"));
    DOCTEST_CHECK(value.revision == 3);
    DOCTEST_CHECK_FALSE(receive(reader, firstOwner.beacon(), 300));
    DOCTEST_CHECK(get(reader, "cluster/message") == bytes("after"));
}

DOCTEST_TEST_CASE("ownership violations are rejected atomically instead of becoming a conflict protocol") {
    CReplica replica("host3", 3);
    SBeacon invalid{"host2", 2, 1, {{"demo/host2", bytes("valid"), 1}, {"cluster/message", bytes("bad"), 1}}};
    DOCTEST_CHECK_FALSE(receive(replica, invalid));
    DOCTEST_CHECK(replica.table()->snapshot().empty());
    DOCTEST_CHECK(replica.peers().empty());
    DOCTEST_CHECK_THROWS_AS(
        asyncpp::as_promise(replica.setOwned("cluster/message", bytes("bad"))).get(), std::invalid_argument);
    invalid.records.pop_back();
    DOCTEST_CHECK(receive(replica, invalid));
    DOCTEST_CHECK(get(replica, "demo/host2") == bytes("valid"));
    DOCTEST_CHECK_FALSE(receive(replica, replica.beacon()));
}

DOCTEST_TEST_SUITE_END();

} // namespace
