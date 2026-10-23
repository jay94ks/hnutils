#include <sdkv.hpp>

#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace distkv;
using namespace std::chrono_literals;

SReadOnlyByteSpan b(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

SBytes owned(std::string_view value) {
    const auto span = b(value);
    return SBytes(span.begin(), span.end());
}

class Recorder : public ITableListener {
private:
    std::mutex _mutex;
    std::vector<CTableChange> _events;

    void record(const CTableChange& change) {
        {
            std::lock_guard lock(_mutex);
            _events.push_back(change);
        }
        if (hook) { hook(change); }
    }

public:
    std::function<void(const CTableChange&)> hook;

    void onAdded(const CTableChange& change) override { record(change); }

    void onChanged(const CTableChange& change) override { record(change); }

    void onRemoved(const CTableChange& change) override { record(change); }

    std::vector<CTableChange> events() {
        std::lock_guard lock(_mutex);
        return _events;
    }
};

class QueueDispatcher final : public asyncpp::dispatcher {
private:
    std::mutex _mutex;
    std::deque<std::function<void()>> _queue;

public:
    void push(std::function<void()> callback) override {
        std::lock_guard lock(_mutex);
        _queue.push_back(std::move(callback));
    }

    bool runOne() {
        std::function<void()> callback;
        {
            std::lock_guard lock(_mutex);
            if (_queue.empty()) { return false; }
            callback = std::move(_queue.front());
            _queue.pop_front();
        }
        auto previous = current(this);
        try {
            callback();
        } catch (...) {
            current(previous);
            throw;
        }
        current(previous);
        return true;
    }
};

/* A semantic remote cache adapter; deliberately no network implementation. */
class RemoteCache final : public IRemoteTable {
private:
    ITablePtr _cache = ITable::create();
    bool _online = false;
    bool _synced = false;

public:
    void status(bool online, bool synced) { _online = online; _synced = synced; }

    bool isOnline() const noexcept override { return _online; }

    bool isSynced() const noexcept override { return _synced; }

    ERetCode get(const SReadOnlyByteSpan& key, STableValue& value) const override { return _cache->get(key, value); }

    ERetCode add(const SReadOnlyByteSpan& key, const SReadOnlyByteSpan& value) override { return _cache->add(key, value); }

    ERetCode remove(const SReadOnlyByteSpan& key) override { return _cache->remove(key); }

    ERetCode set(const SReadOnlyByteSpan& key, const SReadOnlyByteSpan& value) override { return _cache->set(key, value); }

    ERetCode setIf(const SReadOnlyByteSpan& key, std::optional<SReadOnlyByteSpan> expected,
        const SReadOnlyByteSpan& value) override { return _cache->setIf(key, expected, value); }

    std::vector<STableEntry> snapshot() const override { return _cache->snapshot(); }

    std::uint64_t revision() const override { return _cache->revision(); }

    CTableSubscription subscribe(const ITableListenerPtr& listener, bool replay) override {
        return _cache->subscribe(listener, replay);
    }

    ERetCode addSyncFrom(const ITablePtr& source) override { return _cache->addSyncFrom(source); }

    ERetCode removeSyncFrom(const ITablePtr& source) override { return _cache->removeSyncFrom(source); }
};

DOCTEST_TEST_SUITE_BEGIN("sdkv");

DOCTEST_TEST_CASE("binary keys and values are owned and CRUD uses current local state") {
    auto table = ITable::create();
    SBytes key{0, 1, 0, 255};
    SBytes value{0, 2, 0, 254};
    const auto originalKey = key;
    const auto originalValue = value;
    DOCTEST_CHECK(table->revision() == 0);
    DOCTEST_CHECK(table->set(key, value) == ERET_NOT_FOUND);
    DOCTEST_CHECK(table->remove(key) == ERET_NOT_FOUND);
    DOCTEST_CHECK(table->add(key, value) == ERET_OK);
    DOCTEST_CHECK(table->add(key, value) == ERET_ALREADY_EXISTS);
    key[0] = 9;
    value[0] = 9;
    SBytes output{42};
    DOCTEST_CHECK(table->get(originalKey, output) == ERET_OK);
    DOCTEST_CHECK(output == originalValue);
    output[0] = 99;
    DOCTEST_CHECK(table->get(originalKey, output) == ERET_OK);
    DOCTEST_CHECK(output == originalValue);
    DOCTEST_CHECK(table->get(b("missing"), output) == ERET_NOT_FOUND);
    DOCTEST_CHECK(output == originalValue);
    DOCTEST_CHECK(table->add({}, {}) == ERET_OK);
    DOCTEST_CHECK(table->get({}, output) == ERET_OK);
    DOCTEST_CHECK(output.empty());
    DOCTEST_CHECK(table->remove(originalKey) == ERET_OK);
    DOCTEST_CHECK(table->get(originalKey, output) == ERET_NOT_FOUND);
}

DOCTEST_TEST_CASE("per-key revisions and immutable prev now images describe only effective changes") {
    auto table = ITable::create();
    auto listener = std::make_shared<Recorder>();
    auto subscription = table->subscribe(listener);
    DOCTEST_CHECK(table->add(b("a"), b("one")) == ERET_OK);
    DOCTEST_CHECK(table->add(b("b"), b("unrelated")) == ERET_OK);
    DOCTEST_CHECK(table->set(b("a"), b("one")) == ERET_OK);
    DOCTEST_CHECK(table->setIf(b("a"), b("one"), b("one")) == ERET_OK);
    DOCTEST_CHECK(table->setIf(b("a"), b("wrong"), b("two")) == ERET_CONDITION_FAILED);
    DOCTEST_CHECK(table->set(b("a"), b("two")) == ERET_OK);
    DOCTEST_CHECK(table->remove(b("a")) == ERET_OK);
    DOCTEST_CHECK(table->add(b("a"), b("again")) == ERET_OK);
    const auto events = listener->events();
    DOCTEST_REQUIRE(events.size() == 5);
    DOCTEST_CHECK_FALSE(events[0].prev());
    DOCTEST_REQUIRE(events[0].now().has_value());
    DOCTEST_CHECK(events[0].now()->bytes == owned("one"));
    DOCTEST_CHECK(events[0].now()->revision == 1);
    DOCTEST_CHECK(events[1].now()->revision == 1);
    DOCTEST_CHECK(events[2].prev()->bytes == owned("one"));
    DOCTEST_CHECK(events[2].prev()->revision == 1);
    DOCTEST_CHECK(events[2].now()->bytes == owned("two"));
    DOCTEST_CHECK(events[2].now()->revision == 2);
    DOCTEST_CHECK(events[3].prev()->bytes == owned("two"));
    DOCTEST_CHECK_FALSE(events[3].now());
    DOCTEST_CHECK(events[3].revision() == 3);
    DOCTEST_CHECK_FALSE(events[4].prev());
    DOCTEST_CHECK(events[4].now()->revision == 4);
    DOCTEST_CHECK(table->revision() == 5);
    table.reset();
    DOCTEST_CHECK(events[0].now()->bytes == owned("one"));
}

DOCTEST_TEST_CASE("value size is bounded to 4096 without changing state or emitting failures") {
    auto table = ITable::create();
    auto listener = std::make_shared<Recorder>();
    auto subscription = table->subscribe(listener);
    SBytes limit(MAX_VALUE_SIZE, 0xab);
    SBytes tooLarge(MAX_VALUE_SIZE + 1, 0xcd);
    DOCTEST_CHECK(table->add(b("max"), limit) == ERET_OK);
    const auto revision = table->revision();
    DOCTEST_CHECK(table->add(b("extra"), tooLarge) == ERET_VALUE_TOO_LARGE);
    DOCTEST_CHECK(table->set(b("max"), tooLarge) == ERET_VALUE_TOO_LARGE);
    DOCTEST_CHECK(table->setIf(b("max"), limit, tooLarge) == ERET_VALUE_TOO_LARGE);
    DOCTEST_CHECK(table->setIf(b("other"), std::nullopt, tooLarge) == ERET_VALUE_TOO_LARGE);
    DOCTEST_CHECK(table->revision() == revision);
    DOCTEST_CHECK(listener->events().size() == 1);
    SBytes actual;
    DOCTEST_CHECK(table->get(b("max"), actual) == ERET_OK);
    DOCTEST_CHECK(actual == limit);
}

DOCTEST_TEST_CASE("compare and set distinguishes missing from present empty values") {
    auto table = ITable::create();
    DOCTEST_CHECK(table->setIf(b("k"), SReadOnlyByteSpan{}, b("x")) == ERET_CONDITION_FAILED);
    DOCTEST_CHECK(table->setIf(b("k"), std::nullopt, {}) == ERET_OK);
    DOCTEST_CHECK(table->setIf(b("k"), std::nullopt, b("x")) == ERET_CONDITION_FAILED);
    DOCTEST_CHECK(table->setIf(b("k"), SReadOnlyByteSpan{}, b("x")) == ERET_OK);
    DOCTEST_CHECK(table->setIf(b("k"), b("bad"), b("y")) == ERET_CONDITION_FAILED);
    DOCTEST_CHECK(table->setIf(b("k"), b("x"), b("y")) == ERET_OK);
    STableValue actual;
    DOCTEST_CHECK(table->get(b("k"), actual) == ERET_OK);
    DOCTEST_CHECK(actual.bytes == owned("y"));
    DOCTEST_CHECK(actual.revision == 3);
}

DOCTEST_TEST_CASE("snapshot replay is atomic with live subscription and allows reentrant mutations") {
    auto table = ITable::create();
    table->add(b("a"), b("one"));
    table->add(b("b"), b("two"));
    auto listener = std::make_shared<Recorder>();
    bool updated = false;
    listener->hook = [&](const CTableChange& change) {
        if (change.isReplay() && !updated) {
            updated = true;
            DOCTEST_CHECK(table->set(b("a"), b("new")) == ERET_OK);
        }
    };
    auto subscription = table->subscribe(listener);
    const auto events = listener->events();
    DOCTEST_REQUIRE(events.size() == 3);
    DOCTEST_CHECK(events[0].isReplay());
    DOCTEST_CHECK(events[1].isReplay());
    DOCTEST_CHECK_FALSE(events[2].isReplay());
    DOCTEST_CHECK(events[2].prev()->bytes == owned("one"));
    DOCTEST_CHECK(events[2].now()->bytes == owned("new"));
    const auto snapshot = table->snapshot();
    DOCTEST_REQUIRE(snapshot.size() == 2);
    DOCTEST_CHECK(snapshot[0].key == owned("a"));
    DOCTEST_CHECK(snapshot[0].value.bytes == owned("new"));
}

DOCTEST_TEST_CASE("sync overlays current state follows removals and can detach without rollback") {
    auto source = ITable::create();
    auto target = ITable::create();
    source->add(b("shared"), b("source"));
    target->add(b("shared"), b("local"));
    target->add(b("local-only"), b("keep"));
    DOCTEST_CHECK(target->addSyncFrom(source) == ERET_OK);
    DOCTEST_CHECK(target->addSyncFrom(source) == ERET_ALREADY_EXISTS);
    DOCTEST_CHECK(target->addSyncFrom(target) == ERET_INVALID_ARGUMENT);
    DOCTEST_CHECK(target->addSyncFrom(nullptr) == ERET_INVALID_ARGUMENT);
    SBytes actual;
    DOCTEST_CHECK(target->get(b("shared"), actual) == ERET_OK);
    DOCTEST_CHECK(actual == owned("source"));
    STableValue localValue;
    STableValue sourceValue;
    DOCTEST_CHECK(target->get(b("shared"), localValue) == ERET_OK);
    DOCTEST_CHECK(source->get(b("shared"), sourceValue) == ERET_OK);
    DOCTEST_CHECK(localValue.revision == 2);
    DOCTEST_CHECK(sourceValue.revision == 1);
    DOCTEST_CHECK(target->get(b("local-only"), actual) == ERET_OK);
    source->set(b("shared"), b("new"));
    DOCTEST_CHECK(target->get(b("shared"), actual) == ERET_OK);
    DOCTEST_CHECK(actual == owned("new"));
    source->remove(b("shared"));
    DOCTEST_CHECK(target->get(b("shared"), actual) == ERET_NOT_FOUND);
    DOCTEST_CHECK(target->removeSyncFrom(source) == ERET_OK);
    DOCTEST_CHECK(target->removeSyncFrom(source) == ERET_NOT_FOUND);
    source->add(b("shared"), b("detached"));
    DOCTEST_CHECK(target->get(b("shared"), actual) == ERET_NOT_FOUND);
    DOCTEST_CHECK(target->get(b("local-only"), actual) == ERET_OK);
}

DOCTEST_TEST_CASE("cycles and diamond paths deliver each effective value change once") {
    auto a = ITable::create();
    auto bTable = ITable::create();
    auto c = ITable::create();
    auto d = ITable::create();
    bTable->addSyncFrom(a);
    c->addSyncFrom(a);
    d->addSyncFrom(bTable);
    d->addSyncFrom(c);
    a->addSyncFrom(d);
    auto listener = std::make_shared<Recorder>();
    auto subscription = d->subscribe(listener);
    a->add(b("k"), b("first"));
    a->set(b("k"), b("second"));
    a->set(b("k"), b("second"));
    a->remove(b("k"));
    a->add(b("k"), b("again"));
    DOCTEST_CHECK(listener->events().size() == 4);
    for (const auto& table : {a, bTable, c, d}) {
        STableValue value;
        DOCTEST_CHECK(table->get(b("k"), value) == ERET_OK);
        DOCTEST_CHECK(value.bytes == owned("again"));
        DOCTEST_CHECK(value.revision == 4);
        DOCTEST_CHECK(table->revision() == 4);
    }
}

DOCTEST_TEST_CASE("listeners can follow multiple sources and remove during initial replay") {
    auto a = ITable::create();
    auto bTable = ITable::create();
    a->add(b("a"), b("one"));
    a->add(b("b"), b("two"));
    auto listener = std::make_shared<Recorder>();
    listener->hook = [&](const CTableChange&) { listener->removeSyncFrom(a); };
    DOCTEST_CHECK(listener->addSyncFrom(a) == ERET_OK);
    DOCTEST_CHECK(listener->events().size() == 1);
    a->set(b("a"), b("later"));
    DOCTEST_CHECK(listener->events().size() == 1);
    listener->hook = {};
    DOCTEST_CHECK(listener->addSyncFrom(a) == ERET_OK);
    DOCTEST_CHECK(listener->addSyncFrom(bTable) == ERET_OK);
    DOCTEST_CHECK(listener->addSyncFrom(bTable) == ERET_ALREADY_EXISTS);
    bTable->add(b("c"), b("three"));
    DOCTEST_CHECK(listener->events().size() == 4);
    Recorder stackListener;
    DOCTEST_CHECK(stackListener.addSyncFrom(a) == ERET_INVALID_ARGUMENT);
}

DOCTEST_TEST_CASE("subscription lifetimes are weak and reset is reentrant and idempotent") {
    auto table = ITable::create();
    auto listener = std::make_shared<Recorder>();
    CTableSubscription subscription;
    listener->hook = [&](const CTableChange&) { subscription.reset(); };
    subscription = table->subscribe(listener, false);
    table->add(b("one"), b("1"));
    table->add(b("two"), b("2"));
    DOCTEST_CHECK(listener->events().size() == 1);
    DOCTEST_CHECK_FALSE(subscription.isActive());
    subscription.reset();
    auto live = table->subscribe(listener, false);
    DOCTEST_CHECK(live.isActive());
    std::weak_ptr<ITableListener> weakListener = listener;
    listener.reset();
    DOCTEST_CHECK(weakListener.expired());
    DOCTEST_CHECK_FALSE(live.isActive());
    DOCTEST_CHECK_THROWS_AS(table->subscribe(nullptr), std::invalid_argument);
    auto target = ITable::create();
    target->addSyncFrom(table);
    std::weak_ptr<ITable> weakSource = table;
    table.reset();
    DOCTEST_CHECK(weakSource.expired());
    std::weak_ptr<ITable> weakTarget = target;
    target.reset();
    DOCTEST_CHECK(weakTarget.expired());
}

DOCTEST_TEST_CASE("callback exceptions do not roll back mutations or starve other listeners") {
    class Throwing final : public ITableListener {
    public:
        int errors = 0;
        void onAdded(const CTableChange&) override { throw std::runtime_error("listener error"); }
        void onError(const CTableChange&, std::exception_ptr error) noexcept override {
            if (error) { ++errors; }
        }
    };
    auto table = ITable::create();
    auto bad = std::make_shared<Throwing>();
    auto good = std::make_shared<Recorder>();
    auto badSubscription = table->subscribe(bad);
    auto goodSubscription = table->subscribe(good);
    DOCTEST_CHECK(table->add(b("one"), b("1")) == ERET_OK);
    DOCTEST_CHECK(table->add(b("two"), b("2")) == ERET_OK);
    DOCTEST_CHECK(badSubscription.errorCount() == 2);
    DOCTEST_CHECK(bad->errors == 2);
    DOCTEST_CHECK(good->events().size() == 2);
    DOCTEST_CHECK(table->snapshot().size() == 2);
}

DOCTEST_TEST_CASE("last listener release can unsubscribe from its destructor without a queue lock") {
    class ResetOnDestruction final : public ITableListener {
    private:
        CTableSubscription& _subscription;
        std::shared_ptr<ResetOnDestruction>& _owner;
        bool& _destroyed;
    public:
        ResetOnDestruction(CTableSubscription& subscription,
            std::shared_ptr<ResetOnDestruction>& owner, bool& destroyed)
            : _subscription(subscription), _owner(owner), _destroyed(destroyed) {}
        ~ResetOnDestruction() override { _subscription.reset(); _destroyed = true; }
        void onAdded(const CTableChange&) override { _owner.reset(); }
    };
    auto table = ITable::create();
    CTableSubscription subscription;
    bool destroyed = false;
    std::shared_ptr<ResetOnDestruction> owner;
    owner = std::make_shared<ResetOnDestruction>(subscription, owner, destroyed);
    subscription = table->subscribe(owner, false);
    DOCTEST_CHECK(table->add(b("k"), b("v")) == ERET_OK);
    DOCTEST_CHECK(destroyed);
    DOCTEST_CHECK_FALSE(subscription.isActive());

    auto source = ITable::create();
    std::weak_ptr<ITable> weakSource = source;
    auto listener = std::make_shared<Recorder>();
    listener->hook = [&](const CTableChange&) {
        source.reset();
        DOCTEST_CHECK_FALSE(weakSource.expired());
    };
    auto live = source->subscribe(listener, false);
    DOCTEST_CHECK(source->add(b("last-owner"), b("value")) == ERET_OK);
    DOCTEST_CHECK(weakSource.expired());
    DOCTEST_CHECK_FALSE(live.isActive());
}

DOCTEST_TEST_CASE("concurrent compare and set is atomic with ordered before after notifications") {
    auto table = ITable::create();
    auto listener = std::make_shared<Recorder>();
    auto subscription = table->subscribe(listener);
    SBytes zero(sizeof(std::uint64_t), 0);
    table->add(b("counter"), zero);
    std::atomic<bool> failed = false;
    std::vector<std::jthread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&] {
            for (int count = 0; count < 100; ++count) {
                for (;;) {
                    STableValue previous;
                    if (table->get(b("counter"), previous) != ERET_OK) { failed = true; return; }
                    std::uint64_t number = 0;
                    std::memcpy(&number, previous.bytes.data(), sizeof(number));
                    ++number;
                    SBytes next(sizeof(number));
                    std::memcpy(next.data(), &number, sizeof(number));
                    const auto result = table->setIf(b("counter"), previous.bytes, next);
                    if (result == ERET_OK) { break; }
                    if (result != ERET_CONDITION_FAILED) { failed = true; return; }
                }
            }
        });
    }
    threads.clear();
    DOCTEST_CHECK_FALSE(failed.load());
    STableValue final;
    DOCTEST_CHECK(table->get(b("counter"), final) == ERET_OK);
    std::uint64_t number = 0;
    std::memcpy(&number, final.bytes.data(), sizeof(number));
    DOCTEST_CHECK(number == 400);
    DOCTEST_CHECK(final.revision == 401);
    const auto events = listener->events();
    DOCTEST_REQUIRE(events.size() == 401);
    for (std::size_t i = 1; i < events.size(); ++i) {
        DOCTEST_CHECK(events[i].prev()->bytes == events[i - 1].now()->bytes);
        DOCTEST_CHECK(events[i].prev()->revision + 1 == events[i].now()->revision);
    }
}

DOCTEST_TEST_CASE("concurrent snapshot follow has no gap between replay and live updates") {
    auto source = ITable::create();
    for (std::uint8_t i = 0; i < 100; ++i) { source->add(SBytes{i}, SBytes{i}); }
    auto target = ITable::create();
    std::jthread writer([&] {
        for (int i = 0; i < 500; ++i) {
            source->set(SBytes{0}, SBytes{static_cast<std::uint8_t>(i)});
        }
    });
    DOCTEST_CHECK(target->addSyncFrom(source) == ERET_OK);
    writer.join();
    const auto snapshot = source->snapshot();
    DOCTEST_CHECK(target->snapshot().size() == snapshot.size());
    for (const auto& entry : snapshot) {
        SBytes actual;
        DOCTEST_CHECK(target->get(entry.key, actual) == ERET_OK);
        DOCTEST_CHECK(actual == entry.value.bytes);
    }
}

DOCTEST_TEST_CASE("async operations own inputs and retain table while dispatch is suspended") {
    QueueDispatcher dispatcher;
    auto table = ITable::create();
    std::weak_ptr<ITable> weak = table;
    auto task = table->addAsync(owned("key"), owned("value"), &dispatcher);
    table.reset();
    DOCTEST_CHECK_FALSE(weak.expired());
    auto future = asyncpp::as_promise(std::move(task));
    DOCTEST_CHECK(future.wait_for(0ms) == std::future_status::timeout);
    auto retained = weak.lock();
    DOCTEST_REQUIRE(retained != nullptr);
    DOCTEST_CHECK(retained->revision() == 0);
    DOCTEST_CHECK(dispatcher.runOne());
    DOCTEST_CHECK(future.get() == ERET_OK);
    auto result = asyncpp::as_promise(retained->getAsync(owned("key"))).get();
    DOCTEST_CHECK(result.code == ERET_OK);
    DOCTEST_REQUIRE(result.value.has_value());
    DOCTEST_CHECK(result.value->bytes == owned("value"));
    DOCTEST_CHECK(asyncpp::as_promise(retained->setAsync(owned("key"), owned("next"))).get() == ERET_OK);
    DOCTEST_CHECK(asyncpp::as_promise(retained->setIfAsync(owned("key"), owned("next"), owned("last"))).get() == ERET_OK);
    DOCTEST_CHECK(asyncpp::as_promise(retained->removeAsync(owned("key"))).get() == ERET_OK);
    DOCTEST_CHECK(asyncpp::as_promise(retained->setIfAsync(owned("empty"), std::nullopt, {})).get() == ERET_OK);
    DOCTEST_CHECK(asyncpp::as_promise(retained->getAsync(owned("missing"))).get().code == ERET_NOT_FOUND);
    retained.reset();
    DOCTEST_CHECK(weak.expired());
}

DOCTEST_TEST_CASE("remote interface supplies semantic status without implementing a transport") {
    auto adapter = std::make_shared<RemoteCache>();
    IRemoteTablePtr remote = adapter;
    DOCTEST_CHECK_FALSE(remote->isOnline());
    DOCTEST_CHECK_FALSE(remote->isSynced());
    auto local = ITable::create();
    DOCTEST_CHECK(local->addSyncFrom(remote) == ERET_OK);
    adapter->status(true, true);
    DOCTEST_CHECK(remote->isOnline());
    DOCTEST_CHECK(remote->isSynced());
    remote->add(b("k"), b("v"));
    SBytes result;
    DOCTEST_CHECK(local->get(b("k"), result) == ERET_OK);
    DOCTEST_CHECK(result == owned("v"));
    adapter->status(false, false);
    DOCTEST_CHECK(local->get(b("k"), result) == ERET_OK);
    DOCTEST_CHECK(local->removeSyncFrom(remote) == ERET_OK);
}

DOCTEST_TEST_CASE("async dispatch failures propagate and explicit executors always queue consistently") {
    class ThrowingDispatcher final : public asyncpp::dispatcher {
    public:
        void push(std::function<void()>) override { throw std::runtime_error("executor stopped"); }
    } throwing;
    auto table = ITable::create();
    DOCTEST_CHECK_THROWS_AS(
        asyncpp::as_promise(table->addAsync(owned("k"), owned("v"), &throwing)).get(),
        std::runtime_error);
    DOCTEST_CHECK(table->revision() == 0);
    QueueDispatcher dispatcher;
    bool readyInCallback = false;
    std::future<ERetCode> pending;
    dispatcher.push([&] {
        pending = asyncpp::as_promise(table->addAsync(owned("k"), owned("v"), &dispatcher));
        readyInCallback = pending.wait_for(0ms) == std::future_status::ready;
    });
    DOCTEST_CHECK(dispatcher.runOne());
    DOCTEST_CHECK_FALSE(readyInCallback);
    DOCTEST_CHECK(dispatcher.runOne());
    DOCTEST_CHECK(pending.get() == ERET_OK);
    DOCTEST_CHECK_FALSE(dispatcher.runOne());
}

DOCTEST_TEST_SUITE_END();

} // namespace
