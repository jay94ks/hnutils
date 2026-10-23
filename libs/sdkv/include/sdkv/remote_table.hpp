#ifndef __INCLUDE_SDKV_REMOTE_TABLE_HPP__
#define __INCLUDE_SDKV_REMOTE_TABLE_HPP__

#include <sdkv/table.hpp>

namespace distkv {

class IRemoteTable;

/** Shared ownership of an application-supplied remote table adapter. */
using IRemoteTablePtr = std::shared_ptr<IRemoteTable>;

/**
 * Semantic view of a remote table, with transport-independent status queries.
 *
 * An adapter can delegate storage/notifications to ITable::create(). No
 * transport, peer discovery, decoding, or conflict resolution is supplied.
 * CRUD/CAS still describes the adapter's current local view, not remote consensus.
 */
class SDKV_API IRemoteTable : public ITable {
public:
    /** Destroy the application-supplied remote adapter. */
    ~IRemoteTable() override;

    /** Test transport availability according to the adapter's own policy. */
    virtual bool isOnline() const noexcept = 0;

    /** Test whether the adapter has caught up to its known source state. */
    virtual bool isSynced() const noexcept = 0;
};

} // namespace distkv

#endif
