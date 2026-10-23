#ifndef __INCLUDE_SDKV_LISTENER_HPP__
#define __INCLUDE_SDKV_LISTENER_HPP__

#include <sdkv/change.hpp>

#include <exception>
#include <memory>

namespace distkv {

class ITable;
class ITableListener;

/** Shared ownership of an application listener or change-forwarding adapter. */
using ITableListenerPtr = std::shared_ptr<ITableListener>;

/**
 * Receives owned before/after images without decoding application payloads.
 *
 * Each registration serializes its callbacks in source order. Different
 * registrations may run concurrently. Reentrant table operations are allowed.
 * addSyncFrom() requires shared ownership of the listener instance.
 */
class SDKV_API ITableListener : public std::enable_shared_from_this<ITableListener> {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

protected:
    /** Construct an unattached listener with encapsulated source registrations. */
    ITableListener();

public:
    /** Remove source registrations; an in-flight callback may still finish. */
    virtual ~ITableListener();

    /** Listener registrations cannot be copied. */
    ITableListener(const ITableListener&) = delete;

    /** Listener registrations cannot be copied. */
    ITableListener& operator=(const ITableListener&) = delete;

    /** Receive an insertion or initial replay with absent prev() and present now(). */
    virtual void onAdded(const CTableChange& change);

    /** Receive an actual value change with both prev() and now() present. */
    virtual void onChanged(const CTableChange& change);

    /** Receive a removal with present prev() and absent now(). */
    virtual void onRemoved(const CTableChange& change);

    /**
     * Handle a callback exception after the mutation has committed.
     * @param change The notification that failed.
     * @param error The exception caught by the delivery queue.
     * @note Never throw; other registrations continue receiving changes.
     */
    virtual void onError(const CTableChange& change, std::exception_ptr error) noexcept;

    /**
     * Subscribe to a table, including its current entries before live changes.
     * @param source A non-null source table, weakly retained.
     * @return ERET_OK, ERET_ALREADY_EXISTS, or ERET_INVALID_ARGUMENT.
     */
    ERetCode addSyncFrom(const std::shared_ptr<ITable>& source);

    /**
     * Remove a source registration; an already-running callback may finish.
     * @param source The previously registered table.
     * @return ERET_OK, ERET_NOT_FOUND, or ERET_INVALID_ARGUMENT.
     */
    ERetCode removeSyncFrom(const std::shared_ptr<ITable>& source);
};

} // namespace distkv

#endif
