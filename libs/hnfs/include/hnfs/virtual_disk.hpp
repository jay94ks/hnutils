#ifndef __INCLUDE_HNFS_VIRTUAL_DISK_HPP__
#define __INCLUDE_HNFS_VIRTUAL_DISK_HPP__
#include <hnfs/device.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>
namespace hnfs {
/** Virtual image container, separate from partition and filesystem formats. */
enum EVirtualDiskFormat { EVIRTUAL_VHD, EVIRTUAL_VHDX };
/** Parsing bounds independent of virtual disk capacity. */
struct SVirtualDiskOpenOptions {
    std::uint32_t maximumBlocks = 4*1024*1024;
    std::uint64_t maximumMetadataBytes = 64*1024*1024;
    SMappedIOOptions mapping;
};
/** Conservative compaction never infers filesystem allocation from deleted contents. */
struct SCompactOptions {
    /** Also reclaim allocated blocks whose virtual contents are entirely zero. */
    bool scanZeroBlocks = true;
    /** Output file sliding-window policy; source policy is changed separately. */
    SMappedIOOptions mapping;
};
/** Result describes host image byte lengths, not physical host filesystem allocation. */
struct SCompactResult {
    std::uint64_t virtualBytes = 0, previousImageBytes = 0, imageBytes = 0;
    std::uint64_t retainedBlocks = 0, removedBlocks = 0;
};
/** Standalone VHD/VHDX block reader and safe copy-out compactor. */
class HNFS_API CVirtualDisk final : public IBlockDevice, public std::enable_shared_from_this<CVirtualDisk> {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    explicit CVirtualDisk(std::unique_ptr<Impl> impl);
public:
    /** Release the image; never implicitly compact or replay a log. */
    ~CVirtualDisk() override;
    /** Auto-detect fixed/dynamic VHD or VHDX; reject parents and pending VHDX logs. */
    static std::shared_ptr<CVirtualDisk> open(std::shared_ptr<IBlockDevice> image,SVirtualDiskOpenOptions options = {});
    /** Open a regular image through the bounded mapped backend. */
    static std::shared_ptr<CVirtualDisk> openFile(const std::filesystem::path& path,SVirtualDiskOpenOptions options = {});
    /** Capture a native pathname and open it on a caller-owned worker. */
    static asyncpp::task<std::shared_ptr<CVirtualDisk>> openFileAsync(std::filesystem::path path,
        asyncpp::dispatcher& worker,SVirtualDiskOpenOptions options = {});
    /** Inspect the image container. */
    EVirtualDiskFormat virtualFormat() const noexcept;
    /** True for standalone dynamically allocated images. */
    bool isDynamic() const noexcept;
    /** Retrieve the logical sector size used by the contained virtual disk. */
    std::uint32_t logicalSectorSize() const noexcept;
    /** Logical capacity, never the host image file length. */
    std::uint64_t size() const noexcept override;
    /** This initial virtual image driver is read-only; raw/partition devices remain writable. */
    bool isReadOnly() const noexcept override;
    /** Read exactly the requested virtual byte range, honoring block holes/sector bitmaps. */
    void readAt(std::uint64_t offset,SByteSpan output) const override;
    /** Always reject mutation of this immutable view. */
    void writeAt(std::uint64_t offset,SReadOnlyByteSpan input) override;
    /** A read-only view has nothing to publish. */
    void flush() override;
    /** Adjust an owned CFileDevice backend without exposing its native/mapped state. */
    SMappedIOOptions mappingOptions() const;
    /** Change a retained mapped-file backend under its I/O lock. */
    void setMappingOptions(SMappedIOOptions options);
    /** Exclusively create a packed dynamic image. Keep the source and virtual capacity intact. */
    SCompactResult compactToFile(const std::filesystem::path& destination,SCompactOptions options = {}) const;
    /** Retain this disk and owned destination before suspension. */
    asyncpp::task<SCompactResult> compactToFileAsync(std::filesystem::path destination,
        asyncpp::dispatcher& worker,SCompactOptions options = {});
};
} // namespace hnfs
#endif
