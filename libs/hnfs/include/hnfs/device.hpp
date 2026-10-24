#ifndef __INCLUDE_HNFS_DEVICE_HPP__
#define __INCLUDE_HNFS_DEVICE_HPP__

#include <hnfs/common.hpp>
#include <filesystem>
#include <memory>

namespace hnfs {

/** Runtime policy for bounded sliding-window mapped I/O. */
struct SMappedIOOptions {
    /** Useful bytes, 4 KiB..64 MiB; a mapped view may additionally include an OS alignment prefix. */
    std::size_t windowBytes = 1024*1024;
};

/** Fixed-capacity byte device; exact positional I/O never silently returns short data. */
class HNFS_API IBlockDevice {
public:
    /** Release resources without implicitly flushing user changes. */
    virtual ~IBlockDevice();
    /** Fixed capacity in bytes. */
    virtual std::uint64_t size() const noexcept = 0;
    /** True when writes are forbidden. */
    virtual bool isReadOnly() const noexcept = 0;
    /** Fill the complete output or throw; offset+length is checked without overflow. */
    virtual void readAt(std::uint64_t offset, SByteSpan output) const = 0;
    /** Write the complete input or throw; I/O errors may leave a partial write. */
    virtual void writeAt(std::uint64_t offset, SReadOnlyByteSpan input) = 0;
    /** Flush device changes, without claiming filesystem transaction atomicity. */
    virtual void flush() = 0;
    /** Return an independent range copy, capped at 64 MiB per call. */
    SBytes readBytes(std::uint64_t offset, std::size_t count) const;
};

/** Owning memory device; no mutable buffer reference escapes. */
class HNFS_API CMemoryDevice final : public IBlockDevice {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    /** Allocate a zero-filled device. */
    explicit CMemoryDevice(std::size_t capacity, bool readOnly = false);
    /** Copy initial contents before returning. */
    explicit CMemoryDevice(SReadOnlyByteSpan content, bool readOnly = true);
    /** Release owned bytes. */
    ~CMemoryDevice() override;
    /** Stable identity; use shared ownership instead of copying. */
    CMemoryDevice(const CMemoryDevice&) = delete;
    /** Stable identity; use shared ownership instead of assigning. */
    CMemoryDevice& operator=(const CMemoryDevice&) = delete;
    /** Fixed byte capacity. */
    std::uint64_t size() const noexcept override;
    /** Inspect access policy. */
    bool isReadOnly() const noexcept override;
    /** Copy exactly the requested range. */
    void readAt(std::uint64_t offset, SByteSpan output) const override;
    /** Copy input without retaining the span. */
    void writeAt(std::uint64_t offset, SReadOnlyByteSpan input) override;
    /** Memory writes are already visible. */
    void flush() override;
};

/** Regular image-file device; no raw disks, mount operations or native handles are exposed. */
class HNFS_API CFileDevice final : public IBlockDevice {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    explicit CFileDevice(std::unique_ptr<Impl> impl);
public:
    /** Exclusively create a new zero-filled/sparse image, never overwrite an existing file. */
    static std::shared_ptr<CFileDevice> create(const std::filesystem::path& path, std::uint64_t capacity,
        SMappedIOOptions mapping = {});
    /** Open an existing regular image file; read-only by default. */
    static std::shared_ptr<CFileDevice> open(const std::filesystem::path& path, bool writable = false,
        SMappedIOOptions mapping = {});
    /** Retrieve a copied policy under the device lock. */
    SMappedIOOptions mappingOptions() const;
    /** Flush and evict the existing window before applying a validated policy. */
    void setMappingOptions(SMappedIOOptions options);
    /** Close the handle; callers must flush explicitly. */
    ~CFileDevice() override;
    /** Fixed image capacity. */
    std::uint64_t size() const noexcept override;
    /** Inspect access policy. */
    bool isReadOnly() const noexcept override;
    /** Read exactly the requested bytes through bounded sliding mappings. */
    void readAt(std::uint64_t offset, SByteSpan output) const override;
    /** Write within the original image capacity. */
    void writeAt(std::uint64_t offset, SReadOnlyByteSpan input) override;
    /** Report native flush errors. */
    void flush() override;
};

/** A bounded partition/view; callers cannot reach bytes outside the selected range. */
class HNFS_API CDeviceView final : public IBlockDevice {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    /** Retain the parent; writable access cannot weaken a read-only parent. */
    CDeviceView(std::shared_ptr<IBlockDevice> parent, std::uint64_t offset,
        std::uint64_t length, bool readOnly = true);
    /** Release the parent reference. */
    ~CDeviceView() override;
    /** Selected byte length. */
    std::uint64_t size() const noexcept override;
    /** Effective read-only policy. */
    bool isReadOnly() const noexcept override;
    /** Read only inside this view. */
    void readAt(std::uint64_t offset, SByteSpan output) const override;
    /** Write only inside this view. */
    void writeAt(std::uint64_t offset, SReadOnlyByteSpan input) override;
    /** Flush the parent device. */
    void flush() override;
};

} // namespace hnfs
#endif
