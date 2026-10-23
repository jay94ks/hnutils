#ifndef __INCLUDE_SDKV_TABLE_HPP__
#define __INCLUDE_SDKV_TABLE_HPP__

#include <sdkv/common.hpp>
#include <sdkv/listener.hpp>
#include <sdkv/subscription.hpp>

#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

#include <memory>

namespace distkv {

class ITable;

/** Shared ownership of a byte-oriented table. */
using ITablePtr = std::shared_ptr<ITable>;

/**
 * Atomic local-state operations and listener-based table synchronization.
 *
 * Values are limited to 4096 bytes. Keys and values are copied, not decoded.
 * Revision numbers are local to a table, not distributed consensus versions.
 */
class SDKV_API ITable : public std::enable_shared_from_this<ITable> {
protected:
    /** Construct an interface implementation. */
    ITable() = default;

public:
    /** Destroy the table. */
    virtual ~ITable();

    /** Tables have stable identity and cannot be copied. */
    ITable(const ITable&) = delete;

    /** Tables have stable identity and cannot be copied. */
    ITable& operator=(const ITable&) = delete;

    /** Create a thread-safe in-memory table, initially empty at revision zero. */
    static ITablePtr create();

    /**
     * Copy the current payload; leave the output unchanged when absent.
     * @param key The exact binary key.
     * @param value Receives owned value bytes.
     * @return ERET_OK or ERET_NOT_FOUND.
     */
    ERetCode get(const SReadOnlyByteSpan& key, SBytes& value) const;

    /**
     * Copy the current payload and key-local revision.
     * @param key The exact binary key.
     * @param value Receives owned data; unchanged when the key is absent.
     * @return ERET_OK or ERET_NOT_FOUND.
     */
    virtual ERetCode get(const SReadOnlyByteSpan& key, STableValue& value) const = 0;

    /**
     * Insert only if absent.
     * @param key The exact binary key, including an optional empty key.
     * @param value Payload of at most MAX_VALUE_SIZE bytes.
     * @return ERET_OK, ERET_ALREADY_EXISTS, ERET_VALUE_TOO_LARGE, or ERET_REVISION_OVERFLOW.
     */
    virtual ERetCode add(const SReadOnlyByteSpan& key, const SReadOnlyByteSpan& value) = 0;

    /**
     * Remove an existing key, retaining its revision history.
     * @param key The exact binary key.
     * @return ERET_OK, ERET_NOT_FOUND, or ERET_REVISION_OVERFLOW.
     */
    virtual ERetCode remove(const SReadOnlyByteSpan& key) = 0;

    /**
     * Update only if present; identical bytes produce no revision/event.
     * @param key The exact binary key.
     * @param value Payload of at most MAX_VALUE_SIZE bytes.
     * @return ERET_OK, ERET_NOT_FOUND, ERET_VALUE_TOO_LARGE, or ERET_REVISION_OVERFLOW.
     */
    virtual ERetCode set(const SReadOnlyByteSpan& key, const SReadOnlyByteSpan& value) = 0;

    /**
     * Atomically compare presence/value and insert or update on a match.
     * @param key The exact binary key.
     * @param expected nullopt requires absence; an empty span requires a present empty value.
     * @param value The new value, at most MAX_VALUE_SIZE bytes.
     * @return ERET_OK, ERET_CONDITION_FAILED, ERET_VALUE_TOO_LARGE, or ERET_REVISION_OVERFLOW.
     */
    virtual ERetCode setIf(
        const SReadOnlyByteSpan& key,
        std::optional<SReadOnlyByteSpan> expected,
        const SReadOnlyByteSpan& value) = 0;

    /** Return a consistent owned snapshot, ordered lexicographically by binary key. */
    virtual std::vector<STableEntry> snapshot() const = 0;

    /** Return the total table-local mutation counter, initially zero. */
    virtual std::uint64_t revision() const = 0;

    /**
     * Register a weakly held listener with an atomic snapshot/live handoff.
     * @param listener A non-null application listener, retained by the caller.
     * @param replayCurrent Replay current entries before later changes.
     * @return An owning subscription; keep it alive while callbacks are wanted.
     * @note Initial replay can invoke callbacks before this call returns.
     */
    virtual CTableSubscription subscribe(const ITableListenerPtr& listener, bool replayCurrent = true) = 0;

    /**
     * Overlay current source entries and apply later inserts/updates/removals.
     * @param source A distinct source; source-only changes are followed.
     * @return ERET_OK or a registration error.
     * @note Local-only keys survive initial overlay. Cycles and diamonds are suppressed.
     */
    virtual ERetCode addSyncFrom(const ITablePtr& source) = 0;

    /**
     * Stop following a source without rolling back committed values.
     * @param source The previously registered source.
     * @return ERET_OK, ERET_NOT_FOUND, or ERET_INVALID_ARGUMENT.
     */
    virtual ERetCode removeSyncFrom(const ITablePtr& source) = 0;

public:
    /**
     * Read asynchronously, owning the key and retaining this shared table.
     * @param key Copied/moved binary key.
     * @param dispatcher Optional executor to resume on before accessing the table.
     * @return An owned lookup result.
     * @note The executor must outlive the task and be actively driven.
     */
    asyncpp::task<SGetResult> getAsync(SBytes key, asyncpp::dispatcher* dispatcher = nullptr);

    /**
     * Insert asynchronously with owned arguments.
     * @param key Owned key bytes.
     * @param value Owned value bytes.
     * @param dispatcher Optional executor, retained/driven by the caller.
     * @return The same operation outcome as add().
     */
    asyncpp::task<ERetCode> addAsync(SBytes key, SBytes value, asyncpp::dispatcher* dispatcher = nullptr);

    /**
     * Remove asynchronously with an owned key.
     * @param key Owned key bytes.
     * @param dispatcher Optional executor, retained/driven by the caller.
     * @return The same operation outcome as remove().
     */
    asyncpp::task<ERetCode> removeAsync(SBytes key, asyncpp::dispatcher* dispatcher = nullptr);

    /**
     * Update asynchronously with owned arguments.
     * @param key Owned key bytes.
     * @param value Owned value bytes.
     * @param dispatcher Optional executor, retained/driven by the caller.
     * @return The same operation outcome as set().
     */
    asyncpp::task<ERetCode> setAsync(SBytes key, SBytes value, asyncpp::dispatcher* dispatcher = nullptr);

    /**
     * Compare-and-set asynchronously with owned arguments.
     * @param key Owned key bytes.
     * @param expected Expected bytes, or nullopt for expected absence.
     * @param value Owned replacement bytes.
     * @param dispatcher Optional executor, retained/driven by the caller.
     * @return The same operation outcome as setIf().
     */
    asyncpp::task<ERetCode> setIfAsync(
        SBytes key, std::optional<SBytes> expected, SBytes value,
        asyncpp::dispatcher* dispatcher = nullptr);
};

} // namespace distkv

#endif
