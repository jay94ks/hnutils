#include <sdkv/listener.hpp>

#include <sdkv/table.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace distkv {

struct ITableListener::Impl {
    struct Link {
        std::weak_ptr<ITable> source;
        std::shared_ptr<std::atomic<bool>> active = std::make_shared<std::atomic<bool>>(true);
        ITableListenerPtr gate;
        CTableSubscription subscription;

        ~Link() { active->store(false); }
    };
    std::mutex mutex;
    std::map<const ITable*, std::shared_ptr<Link>> links;
};

namespace {

/* A gate also stops replay callbacks when removal occurs before subscribe returns. */
class SourceGate final : public ITableListener {
private:
    std::weak_ptr<ITableListener> _target;
    std::shared_ptr<std::atomic<bool>> _active;

public:
    SourceGate(const ITableListenerPtr& target, std::shared_ptr<std::atomic<bool>> active)
        : _target(target), _active(std::move(active)) {}

    void onAdded(const CTableChange& change) override {
        if (auto target = _target.lock(); target && _active->load()) { target->onAdded(change); }
    }

    void onChanged(const CTableChange& change) override {
        if (auto target = _target.lock(); target && _active->load()) { target->onChanged(change); }
    }

    void onRemoved(const CTableChange& change) override {
        if (auto target = _target.lock(); target && _active->load()) { target->onRemoved(change); }
    }

    void onError(const CTableChange& change, std::exception_ptr error) noexcept override {
        if (auto target = _target.lock()) { target->onError(change, std::move(error)); }
    }
};

} // namespace

/* Construct a listener with private registration state. */
ITableListener::ITableListener() : _impl(std::make_unique<Impl>()) {}

/* Cancel all followed sources. */
ITableListener::~ITableListener() = default;

/* Ignore unhandled insertions. */
void ITableListener::onAdded(const CTableChange&) {}

/* Ignore unhandled updates. */
void ITableListener::onChanged(const CTableChange&) {}

/* Ignore unhandled removals. */
void ITableListener::onRemoved(const CTableChange&) {}

/* Applications can override this to log or surface isolated delivery failures. */
void ITableListener::onError(const CTableChange&, std::exception_ptr) noexcept {}

/* Subscribe with an atomic replay/live handoff, permitting reentrant removal. */
ERetCode ITableListener::addSyncFrom(const std::shared_ptr<ITable>& source) {
    auto self = weak_from_this().lock();
    if (!source || !self) { return ERET_INVALID_ARGUMENT; }
    auto link = std::make_shared<Impl::Link>();
    link->source = source;
    link->gate = std::make_shared<SourceGate>(self, link->active);
    {
        std::lock_guard lock(_impl->mutex);
        auto found = _impl->links.find(source.get());
        if (found != _impl->links.end() && !found->second->source.expired()) {
            return ERET_ALREADY_EXISTS;
        }
        _impl->links[source.get()] = link;
    }
    try {
        auto subscription = source->subscribe(link->gate);
        std::lock_guard lock(_impl->mutex);
        const auto found = _impl->links.find(source.get());
        if (found != _impl->links.end() && found->second == link) {
            link->subscription = std::move(subscription);
        }
    } catch (...) {
        std::lock_guard lock(_impl->mutex);
        const auto found = _impl->links.find(source.get());
        if (found != _impl->links.end() && found->second == link) { _impl->links.erase(found); }
        throw;
    }
    return ERET_OK;
}

/* Release a registration without holding the source table's mutex. */
ERetCode ITableListener::removeSyncFrom(const std::shared_ptr<ITable>& source) {
    if (!source) { return ERET_INVALID_ARGUMENT; }
    std::shared_ptr<Impl::Link> removed;
    {
        std::lock_guard lock(_impl->mutex);
        const auto found = _impl->links.find(source.get());
        if (found == _impl->links.end()) { return ERET_NOT_FOUND; }
        removed = std::move(found->second);
        _impl->links.erase(found);
    }
    removed->active->store(false);
    removed->subscription.reset();
    return ERET_OK;
}

} // namespace distkv
