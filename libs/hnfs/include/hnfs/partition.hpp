#ifndef __INCLUDE_HNFS_PARTITION_HPP__
#define __INCLUDE_HNFS_PARTITION_HPP__

#include <hnfs/device.hpp>
#include <array>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

namespace hnfs {

/** Disk layout identity, independent from filesystem identity. */
enum EPartitionScheme { EPARTITION_MBR, EPARTITION_GPT };

/** Explicit geometry and bounded metadata policy; geometry is never guessed. */
struct SPartitionOpenOptions {
    /** Logical, not physical, sector size: 512, 1024, 2048 or 4096 bytes. */
    std::uint32_t logicalSectorSize = 512;
    /** Maximum GPT slots, including unused entries. */
    std::uint32_t maximumEntries = 16384;
    /** Maximum GPT array bytes; streamed CRC validation does not allocate this amount. */
    std::uint64_t maximumEntryArrayBytes = 16*1024*1024;
    /** Maximum EBR records followed, with independent cycle detection. */
    std::uint32_t maximumEbrRecords = 1024;
    /** Explicit recovery: trust only the fully validated backup GPT, without repairing disk. */
    bool useBackupGpt = false;
};

/** Copied partition metadata; GUID arrays retain their exact on-disk byte order. */
struct SPartition {
    /** One-based GPT/primary MBR slot; logical MBR partitions start at five. */
    std::uint32_t index = 0;
    std::uint64_t firstLba = 0, sectorCount = 0;
    std::uint64_t byteOffset = 0, byteSize = 0;
    /** Location of this logical partition's EBR; zero for primary/GPT entries. */
    std::uint64_t ebrLba = 0;
    std::uint8_t mbrType = 0;
    bool bootable = false, logical = false, extendedContainer = false;
    std::array<std::uint8_t,16> typeGuid{}, uniqueGuid{};
    std::uint64_t attributes = 0;
    /** GPT UTF-16 name decoded to UTF-8. */
    std::string name;
};

/** Explicit partition layout for metadata creation/replacement, not data relocation. */
struct SPartitionLayout {
    EPartitionScheme scheme = EPARTITION_GPT;
    std::uint32_t logicalSectorSize = 512;
    /** Nonzero disk GUID in native GPT byte order; supplied by the caller. */
    std::array<std::uint8_t,16> diskGuid{};
    /** GPT slots; at least 128, bounded to 16384. */
    std::uint32_t gptEntryCount = 128;
    /** Exact slot numbers and LBAs; logical MBR entries require explicit ebrLba. */
    std::vector<SPartition> partitions;
};

/** Metadata replacement is never implicit or atomic. */
struct SPartitionWriteOptions {
    /** Permit replacing nonzero bytes at every metadata write location. */
    bool allowOverwrite = false;
    /** Preserve bytes 0..445 (bootstrap and disk signature) when publishing a new MBR. */
    bool preserveBootCode = true;
};

/** Validated read-only MBR/EBR or GPT catalog; never mounts or repairs anything. */
class HNFS_API CPartitionTable {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    explicit CPartitionTable(std::unique_ptr<Impl> impl);
public:
    /** Release catalog/device ownership without publishing anything. */
    ~CPartitionTable();
    /** Validate metadata, CRCs, bounds, overlap and redundant GPT consistency. */
    static std::shared_ptr<CPartitionTable> open(std::shared_ptr<IBlockDevice> device,
        SPartitionOpenOptions options = {});
    /** Perform opening on the borrowed worker; inputs are captured before suspension. */
    static asyncpp::task<std::shared_ptr<CPartitionTable>> openAsync(std::shared_ptr<IBlockDevice> device,
        asyncpp::dispatcher& worker,SPartitionOpenOptions options = {});
    /** Validate the complete proposed layout before writing. Never moves partition contents. */
    static void write(std::shared_ptr<IBlockDevice> device,SPartitionLayout layout,
        SPartitionWriteOptions options = {});
    /** Capture layout and run explicit metadata publication on the worker. */
    static asyncpp::task<void> writeAsync(std::shared_ptr<IBlockDevice> device,SPartitionLayout layout,
        asyncpp::dispatcher& worker,SPartitionWriteOptions options = {});
    /** Inspect disk layout identity. */
    EPartitionScheme scheme() const noexcept;
    /** Report explicit backup-only opening rather than silent recovery. */
    bool usesBackupGpt() const noexcept;
    /** Retrieve validated logical geometry. */
    std::uint32_t logicalSectorSize() const noexcept;
    /** Page in slot order, at most 4096 entries. Offset is a catalog ordinal, not a slot. */
    std::vector<SPartition> list(std::uint64_t offset,std::size_t count = 128) const;
    /** Return a lifetime-owning range, read-only unless explicitly requested otherwise. */
    std::shared_ptr<CDeviceView> openPartition(std::uint32_t index,bool writable = false) const;
};

} // namespace hnfs
#endif
