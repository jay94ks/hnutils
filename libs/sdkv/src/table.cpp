#include <sdkv/table.hpp>

#include "state.hpp"

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace distkv {
namespace {

std::atomic<std::uint64_t> nextOrigin{1};

/* Allocate a process-local identity without ever wrapping or reusing it. */
std::uint64_t allocateOrigin() {
    auto value = nextOrigin.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("sdkv table identities exhausted");
        }
        if (nextOrigin.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
}

SBytes copyBytes(const SReadOnlyByteSpan& bytes) {
    return bytes.empty() ? SBytes{} : SBytes(bytes.begin(), bytes.end());
}

bool equalBytes(const SBytes& left, const SReadOnlyByteSpan& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

enum Operation { Add, Remove, Set, SetIf, Mirror };

/* Scheduling owns no thread; the caller drives and retains the dispatcher. */
struct DispatchOn {
    asyncpp::dispatcher* dispatcher;

    bool await_ready() const noexcept {
        // --> Header-only dispatcher TLS can be distinct across DLL boundaries.
        return dispatcher == nullptr;
    }

    void await_suspend(std::coroutine_handle<> continuation) const {
        dispatcher->push([continuation] { continuation.resume(); });
    }

    void await_resume() const noexcept {}
};

/* Own both arguments and table lifetime before the lazy task can suspend. */
asyncpp::task<SGetResult> getOnDispatcher(
    ITablePtr table, SBytes key, asyncpp::dispatcher* dispatcher) {
    co_await DispatchOn{dispatcher};
    STableValue value;
    const auto code = table->get(key, value);
    co_return SGetResult{code, code == ERET_OK ? std::optional<STableValue>(std::move(value)) : std::nullopt};
}

/* Execute the same atomic primitive used by synchronous callers. */
asyncpp::task<ERetCode> mutateOnDispatcher(
    ITablePtr table, Operation operation, SBytes key,
    std::optional<SBytes> expected, SBytes value, asyncpp::dispatcher* dispatcher) {
    co_await DispatchOn{dispatcher};
    switch (operation) {
    case Add: co_return table->add(key, value);
    case Remove: co_return table->remove(key);
    case Set: co_return table->set(key, value);
    case SetIf:
        co_return table->setIf(
            key, expected ? std::optional<SReadOnlyByteSpan>(*expected) : std::nullopt, value);
    default: throw std::logic_error("invalid sdkv async operation");
    }
}

} // namespace

/* Concrete storage and propagation are intentionally hidden from public headers. */
class MemoryTable final : public ITable {
private:
    class Forwarder final : public ITableListener {
    private:
        std::weak_ptr<MemoryTable> _target;

        void forward(const CTableChange& change) {
            if (auto target = _target.lock()) {
                const auto code = target->import(change);
                if (code != ERET_OK) {
                    throw std::runtime_error("sdkv synchronization could not apply a change");
                }
            }
        }

    public:
        explicit Forwarder(const std::shared_ptr<MemoryTable>& target) : _target(target) {}

        void onAdded(const CTableChange& change) override { forward(change); }

        void onChanged(const CTableChange& change) override { forward(change); }

        void onRemoved(const CTableChange& change) override { forward(change); }
    };

    mutable std::mutex _mutex;
    std::map<SBytes, STableValue> _values;
    std::map<SBytes, std::uint64_t> _keyRevisions;
    std::uint64_t _revision = 0;
    const std::uint64_t _origin = allocateOrigin();
    std::uint64_t _sequence = 0;
    std::vector<std::weak_ptr<CTableSubscription::Impl>> _subscribers;
    std::map<SBytes, std::map<std::uint64_t, std::uint64_t>> _seen;
    std::shared_ptr<Forwarder> _forwarder;

    struct Delivery {
        std::shared_ptr<CTableSubscription::Impl> subscriber;
        std::list<CTableChange> events;
    };

    using Deliveries = std::vector<Delivery>;

    /* Allocate every delivery node before committing any visible state change. */
    void prepare(const CTableChange& change, Deliveries& deliveries) {
        for (auto it = _subscribers.begin(); it != _subscribers.end();) {
            auto subscriber = it->lock();
            if (!subscriber) {
                it = _subscribers.erase(it);
                continue;
            }
            std::lock_guard lock(subscriber->mutex);
            if (!subscriber->active || subscriber->listener.expired()) {
                it = _subscribers.erase(it);
                continue;
            }
            deliveries.push_back(Delivery{subscriber, {change}});
            ++it;
        }
    }

    /* Apply one linearizable local mutation or one idempotent imported change. */
    ERetCode mutate(
        Operation operation, const SReadOnlyByteSpan& key,
        std::optional<SReadOnlyByteSpan> expected, const SReadOnlyByteSpan& value,
        const CTableChange* incoming = nullptr) {
        if (value.size() > MAX_VALUE_SIZE || (expected && expected->size() > MAX_VALUE_SIZE)) {
            return ERET_VALUE_TOO_LARGE;
        }
        // --> A callback may release the caller's last external table owner.
        [[maybe_unused]] auto lifetime = shared_from_this();
        auto ownedKey = copyBytes(key);
        auto ownedValue = copyBytes(value);
        Deliveries deliveries;
        {
            std::lock_guard lock(_mutex);
            std::uint64_t* receivedSequence = nullptr;
            if (incoming) {
                const auto& event = *incoming->_impl;
                if (event.origin == _origin) { return ERET_OK; }
                auto& seen = _seen[ownedKey][event.origin];
                if (seen >= event.sequence) { return ERET_OK; }
                receivedSequence = &seen;
            }
            const auto found = _values.find(ownedKey);
            const bool exists = found != _values.end();
            if (operation == Add && exists) { return ERET_ALREADY_EXISTS; }
            if ((operation == Set || operation == Remove) && !exists) { return ERET_NOT_FOUND; }
            if (operation == SetIf &&
                (exists != expected.has_value() || (exists && !equalBytes(found->second.bytes, *expected)))) {
                return ERET_CONDITION_FAILED;
            }
            const bool removing = operation == Remove || (incoming && !incoming->now());
            if ((removing && !exists) || (!removing && exists && found->second.bytes == ownedValue)) {
                if (receivedSequence) { *receivedSequence = incoming->_impl->sequence; }
                return ERET_OK;
            }
            constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto keyVersion = _keyRevisions.find(ownedKey);
            const auto previousRevision = keyVersion == _keyRevisions.end() ? 0 : keyVersion->second;
            if (_revision == maximum || previousRevision == maximum || (!incoming && _sequence == maximum)) {
                return ERET_REVISION_OVERFLOW;
            }
            auto data = std::make_shared<CTableChange::Impl>();
            data->key = ownedKey;
            if (exists) { data->prev = found->second; }
            data->revision = previousRevision + 1;
            data->origin = incoming ? incoming->_impl->origin : _origin;
            data->sequence = incoming ? incoming->_impl->sequence : _sequence + 1;
            if (!removing) { data->now = STableValue{std::move(ownedValue), data->revision}; }

            // --> Stage map and notification allocations before the commit point.
            std::map<SBytes, STableValue> staged;
            if (!removing) { staged.emplace(data->key, *data->now); }
            auto& retainedRevision = _keyRevisions.try_emplace(data->key, previousRevision).first->second;
            prepare(CTableChange(data), deliveries);

            if (removing) {
                _values.erase(found);
            } else if (exists) {
                found->second = std::move(staged.begin()->second);
            } else {
                _values.insert(staged.extract(staged.begin()));
            }
            retainedRevision = data->revision;
            ++_revision;
            if (!incoming) { _sequence = data->sequence; }
            if (receivedSequence) { *receivedSequence = incoming->_impl->sequence; }
            for (auto& delivery : deliveries) {
                std::lock_guard queueLock(delivery.subscriber->mutex);
                if (delivery.subscriber->active) {
                    delivery.subscriber->queue.splice(delivery.subscriber->queue.end(), delivery.events);
                }
            }
        }
        for (const auto& delivery : deliveries) { CTableSubscription::drain(delivery.subscriber); }
        return ERET_OK;
    }

    /* Keep process-local event identity across a graph without sharing local revisions. */
    ERetCode import(const CTableChange& change) {
        return mutate(
            Mirror, change.key(), std::nullopt,
            change.now() ? SReadOnlyByteSpan(change.now()->bytes) : SReadOnlyByteSpan{}, &change);
    }

public:
    ~MemoryTable() override {
        for (const auto& weak : _subscribers) {
            if (auto subscriber = weak.lock()) {
                std::lock_guard lock(subscriber->mutex);
                subscriber->active = false;
                subscriber->queue.clear();
            }
        }
    }

    static ITablePtr create() {
        auto table = std::make_shared<MemoryTable>();
        table->_forwarder = std::make_shared<Forwarder>(table);
        return table;
    }

    ERetCode get(const SReadOnlyByteSpan& key, STableValue& value) const override {
        const auto ownedKey = copyBytes(key);
        std::lock_guard lock(_mutex);
        const auto found = _values.find(ownedKey);
        if (found == _values.end()) { return ERET_NOT_FOUND; }
        value = found->second;
        return ERET_OK;
    }

    ERetCode add(const SReadOnlyByteSpan& key, const SReadOnlyByteSpan& value) override {
        return mutate(Add, key, std::nullopt, value);
    }

    ERetCode remove(const SReadOnlyByteSpan& key) override {
        return mutate(Remove, key, std::nullopt, {});
    }

    ERetCode set(const SReadOnlyByteSpan& key, const SReadOnlyByteSpan& value) override {
        return mutate(Set, key, std::nullopt, value);
    }

    ERetCode setIf(
        const SReadOnlyByteSpan& key, std::optional<SReadOnlyByteSpan> expected,
        const SReadOnlyByteSpan& value) override {
        return mutate(SetIf, key, expected, value);
    }

    std::vector<STableEntry> snapshot() const override {
        std::lock_guard lock(_mutex);
        std::vector<STableEntry> result;
        result.reserve(_values.size());
        for (const auto& [key, value] : _values) { result.push_back({key, value}); }
        return result;
    }

    std::uint64_t revision() const override {
        std::lock_guard lock(_mutex);
        return _revision;
    }

    CTableSubscription subscribe(const ITableListenerPtr& listener, bool replayCurrent) override {
        if (!listener) { throw std::invalid_argument("sdkv listener cannot be null"); }
        [[maybe_unused]] auto lifetime = shared_from_this();
        auto subscriber = std::make_shared<CTableSubscription::Impl>();
        subscriber->listener = listener;
        {
            std::lock_guard lock(_mutex);
            if (replayCurrent) {
                if (_values.size() > std::numeric_limits<std::uint64_t>::max() - _sequence) {
                    throw std::overflow_error("sdkv event sequence exhausted");
                }
                for (const auto& [key, value] : _values) {
                    auto data = std::make_shared<CTableChange::Impl>();
                    data->key = key;
                    data->now = value;
                    data->revision = value.revision;
                    data->origin = _origin;
                    data->sequence = ++_sequence;
                    data->replay = true;
                    subscriber->queue.push_back(CTableChange(std::move(data)));
                }
            }
            std::erase_if(_subscribers, [](const auto& weak) {
                const auto slot = weak.lock();
                if (!slot) { return true; }
                std::lock_guard lock(slot->mutex);
                return !slot->active || slot->listener.expired();
            });
            _subscribers.push_back(subscriber);
        }
        CTableSubscription registration(subscriber);
        CTableSubscription::drain(subscriber);
        return registration;
    }

    ERetCode addSyncFrom(const ITablePtr& source) override {
        if (!source || source.get() == this) { return ERET_INVALID_ARGUMENT; }
        return _forwarder->addSyncFrom(source);
    }

    ERetCode removeSyncFrom(const ITablePtr& source) override {
        return _forwarder->removeSyncFrom(source);
    }
};

/* Destroy a polymorphic table. */
ITable::~ITable() = default;

/* Create encapsulated thread-safe storage. */
ITablePtr ITable::create() { return MemoryTable::create(); }

/* Copy only the payload, preserving the caller's output on a miss. */
ERetCode ITable::get(const SReadOnlyByteSpan& key, SBytes& value) const {
    STableValue result;
    const auto code = get(key, result);
    if (code == ERET_OK) { value = std::move(result.bytes); }
    return code;
}

/* Own inputs and table lifetime immediately, not when the task first resumes. */
asyncpp::task<SGetResult> ITable::getAsync(SBytes key, asyncpp::dispatcher* dispatcher) {
    return getOnDispatcher(shared_from_this(), std::move(key), dispatcher);
}

/* Insert using the synchronous atomic primitive on the selected executor. */
asyncpp::task<ERetCode> ITable::addAsync(SBytes key, SBytes value, asyncpp::dispatcher* dispatcher) {
    return mutateOnDispatcher(shared_from_this(), Add, std::move(key), std::nullopt, std::move(value), dispatcher);
}

/* Remove using the synchronous atomic primitive on the selected executor. */
asyncpp::task<ERetCode> ITable::removeAsync(SBytes key, asyncpp::dispatcher* dispatcher) {
    return mutateOnDispatcher(shared_from_this(), Remove, std::move(key), std::nullopt, {}, dispatcher);
}

/* Set using the synchronous atomic primitive on the selected executor. */
asyncpp::task<ERetCode> ITable::setAsync(SBytes key, SBytes value, asyncpp::dispatcher* dispatcher) {
    return mutateOnDispatcher(shared_from_this(), Set, std::move(key), std::nullopt, std::move(value), dispatcher);
}

/* Compare-and-set with owned expected bytes, retaining the absent/empty distinction. */
asyncpp::task<ERetCode> ITable::setIfAsync(
    SBytes key, std::optional<SBytes> expected, SBytes value, asyncpp::dispatcher* dispatcher) {
    return mutateOnDispatcher(
        shared_from_this(), SetIf, std::move(key), std::move(expected), std::move(value), dispatcher);
}

} // namespace distkv
