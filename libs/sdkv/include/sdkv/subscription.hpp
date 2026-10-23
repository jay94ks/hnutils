#ifndef __INCLUDE_SDKV_SUBSCRIPTION_HPP__
#define __INCLUDE_SDKV_SUBSCRIPTION_HPP__

#include <sdkv/common.hpp>

#include <memory>

namespace distkv {

/**
 * Move-only ownership of a listener registration.
 *
 * Destruction/reset() prevents further callbacks from starting, but does not
 * join a callback already selected for delivery. The listener is weakly held.
 */
class SDKV_API CTableSubscription {
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;

    /** Adopt a registered delivery queue. */
    explicit CTableSubscription(std::shared_ptr<Impl> impl);

    /** Drain one registration in source order without holding table locks. */
    static void drain(const std::shared_ptr<Impl>& impl) noexcept;

    friend class MemoryTable;

public:
    /** Construct an inactive subscription. */
    CTableSubscription() noexcept;

    /** Transfer registration ownership. */
    CTableSubscription(CTableSubscription&& other) noexcept;

    /** Unsubscribe without waiting for an already-selected callback. */
    ~CTableSubscription();

    /** Replace this registration, unsubscribing the previous one. */
    CTableSubscription& operator=(CTableSubscription&& other) noexcept;

    /** Registrations cannot be copied. */
    CTableSubscription(const CTableSubscription&) = delete;

    /** Registrations cannot be copied. */
    CTableSubscription& operator=(const CTableSubscription&) = delete;

    /** Unsubscribe; safe to call repeatedly and from inside a callback. */
    void reset() noexcept;

    /** Test whether the registration and its listener are still active. */
    bool isActive() const noexcept;

    /** Return the number of callback exceptions caught for this registration. */
    std::uint64_t errorCount() const noexcept;
};

} // namespace distkv

#endif
