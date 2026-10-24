#ifndef __INCLUDE_HNFS_EXTENSIONS_HPP__
#define __INCLUDE_HNFS_EXTENSIONS_HPP__

#include <hnfs/filesystem.hpp>

namespace hnfs {

/** Copied identity used by snapshot, subvolume and container catalogs. */
struct SObjectInfo {
    /** Opaque driver-local identity; callers must not interpret it as a host path. */
    std::string id;
    /** Display name, not necessarily unique or a pathname. */
    std::string name;
};

/** Snapshot browsing only; snapshot creation policies remain format-specific. */
class HNFS_API ISnapshotCatalog : public virtual IFileSystem {
public:
    /** Release the catalog. */
    virtual ~ISnapshotCatalog();
    /** Return a bounded page, not an allocation proportional to the full catalog. */
    virtual std::vector<SObjectInfo> listSnapshots(std::uint64_t offset, std::size_t count = 128) const = 0;
    /** Open a read-only snapshot tree without changing the live root. */
    virtual std::shared_ptr<ITreeReader> openSnapshot(const std::string& id) const = 0;
};

/** Btrfs-style subvolume roots; intentionally not an alias for APFS volumes. */
class HNFS_API ISubvolumeCatalog : public virtual IFileSystem {
public:
    /** Release the catalog. */
    virtual ~ISubvolumeCatalog();
    /** Return a bounded page of subvolume identities. */
    virtual std::vector<SObjectInfo> listSubvolumes(std::uint64_t offset, std::size_t count = 128) const = 0;
    /** Open a selected root with its own directory namespace. */
    virtual std::shared_ptr<ITreeReader> openSubvolume(const std::string& id) const = 0;
};

/** APFS-style multi-volume container, which is not itself a directory tree. */
class HNFS_API IVolumeContainer : public virtual IFileSystem {
public:
    /** Release the container view. */
    virtual ~IVolumeContainer();
    /** Return copied volume identities in a bounded page. */
    virtual std::vector<SObjectInfo> listVolumes(std::uint64_t offset, std::size_t count = 128) const = 0;
    /** Open a volume; its supported interfaces are discovered independently. */
    virtual std::shared_ptr<IFileSystem> openVolume(const std::string& id) const = 0;
};

/** Named file data streams, e.g. NTFS ADS -- not small extended attributes. */
class HNFS_API INamedStreams : public virtual ITreeReader {
public:
    /** Release the stream catalog. */
    virtual ~INamedStreams();
    /** List stream identities for one file in a bounded page. */
    virtual std::vector<SObjectInfo> listStreams(const std::string& path,
        std::uint64_t offset, std::size_t count = 128) const = 0;
    /** Read a selected stream with positional I/O. */
    virtual std::shared_ptr<IFileReader> openStream(const std::string& path, const std::string& id) const = 0;
};

/** Bounded name/value metadata; separate from named file-content streams. */
class HNFS_API IExtendedAttributes : public virtual ITreeReader {
public:
    /** Release the metadata view. */
    virtual ~IExtendedAttributes();
    /** Return a bounded page of attribute names. */
    virtual std::vector<std::string> listAttributes(const std::string& path,
        std::uint64_t offset, std::size_t count = 128) const = 0;
    /** Copy an attribute under an explicit allocation ceiling. */
    virtual SBytes readAttribute(const std::string& path, const std::string& name, std::size_t maximumSize) const = 0;
};

/** A failed publication may be indeterminate, not safely retryable or reversible. */
enum ETransactionState { ETXN_ACTIVE, ETXN_COMMITTED, ETXN_ABORTED, ETXN_INDETERMINATE };

/** Explicit CoW editing scope; destruction abandons only unpublished changes. */
class HNFS_API ICowTransaction : public virtual ITreeEditor {
public:
    /** Abort uncommitted logical changes; destructors must not throw or commit. */
    virtual ~ICowTransaction();
    /** Inspect whether publication happened, failed before publication or is indeterminate. */
    virtual ETransactionState state() const noexcept = 0;
    /** Flush staging data only; never publish a root before commit(). */
    virtual void flush() override = 0;
    /** Publish a new root/checkpoint or throw; durability must be documented by the driver. */
    virtual void commit() = 0;
    /** Abandon an ACTIVE scope; cannot undo COMMITTED/INDETERMINATE roots or perform repair. */
    virtual void abort() noexcept = 0;
};

/** CoW root publication, distinct from ordinary flush and journal recovery. */
class HNFS_API ICowTree : public virtual ITreeReader {
public:
    /** Release the live-root view. */
    virtual ~ICowTree();
    /** Open an isolated edit scope if this driver can guarantee that contract. */
    virtual std::shared_ptr<ICowTransaction> beginTransaction() = 0;
};

/** Read-only recovery status; no generic application transaction is implied. */
struct SJournalState {
    /** Whether recovery is required before the driver can expose a consistent writable tree. */
    bool recoveryRequired = false;
    /** Driver description, including features it cannot safely replay. */
    std::string detail;
};

/** Journal inspection only; recovery/repair is never triggered by interface discovery. */
class HNFS_API IJournalInspector : public virtual IFileSystem {
public:
    /** Release the inspection view. */
    virtual ~IJournalInspector();
    /** Inspect the current journal without modifying the device. */
    virtual SJournalState journalState() const = 0;
};

} // namespace hnfs
#endif
