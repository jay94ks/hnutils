#include <sdkv/subscription.hpp>

#include "state.hpp"

#include <limits>
#include <utility>

namespace distkv {

/* Construct an inactive registration. */
CTableSubscription::CTableSubscription() noexcept = default;

/* Adopt a registered queue. */
CTableSubscription::CTableSubscription(std::shared_ptr<Impl> impl) : _impl(std::move(impl)) {}

/* Transfer registration ownership. */
CTableSubscription::CTableSubscription(CTableSubscription&& other) noexcept = default;

/* Cancel the owned registration. */
CTableSubscription::~CTableSubscription() { reset(); }

/* Cancel the old registration and adopt another. */
CTableSubscription& CTableSubscription::operator=(CTableSubscription&& other) noexcept {
    if (this != &other) {
        reset();
        _impl = std::move(other._impl);
    }
    return *this;
}

/* Disable future callback starts, without joining in-flight user code. */
void CTableSubscription::reset() noexcept {
    if (_impl) {
        std::lock_guard lock(_impl->mutex);
        _impl->active = false;
        _impl->queue.clear();
    }
}

/* Test whether both registration and listener are alive. */
bool CTableSubscription::isActive() const noexcept {
    if (!_impl) { return false; }
    std::lock_guard lock(_impl->mutex);
    return _impl->active && !_impl->listener.expired();
}

/* Count isolated callback failures. */
std::uint64_t CTableSubscription::errorCount() const noexcept {
    if (!_impl) { return 0; }
    std::lock_guard lock(_impl->mutex);
    return _impl->errors;
}

/* Deliver in source order, outside both the table and delivery-queue locks. */
void CTableSubscription::drain(const std::shared_ptr<Impl>& impl) noexcept {
    std::unique_lock lock(impl->mutex);
    if (!impl->active || impl->draining) { return; }
    impl->draining = true;
    while (impl->active && !impl->queue.empty()) {
        auto listener = impl->listener.lock();
        if (!listener) {
            impl->active = false;
            impl->queue.clear();
            break;
        }
        auto change = impl->queue.front();
        impl->queue.pop_front();
        lock.unlock();
        try {
            if (!change.prev()) {
                listener->onAdded(change);
            } else if (!change.now()) {
                listener->onRemoved(change);
            } else {
                listener->onChanged(change);
            }
        } catch (...) {
            const auto error = std::current_exception();
            lock.lock();
            if (impl->errors != std::numeric_limits<std::uint64_t>::max()) { ++impl->errors; }
            lock.unlock();
            listener->onError(change, error);
        }
        // --> Releasing the final listener may unsubscribe or run user destructors.
        listener.reset();
        lock.lock();
    }
    impl->draining = false;
}

} // namespace distkv
