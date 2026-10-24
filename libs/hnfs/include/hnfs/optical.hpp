#ifndef __INCLUDE_HNFS_OPTICAL_HPP__
#define __INCLUDE_HNFS_OPTICAL_HPP__
#include <hnfs/filesystem.hpp>
namespace hnfs {
/** Read-only optical tree with lazy directory scanning and 64-bit extent-based files. */
class HNFS_API COpticalFileSystem final : public ITreeReader {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    explicit COpticalFileSystem(std::unique_ptr<Impl> impl);
public:
    /** Release device ownership without modifying the image. */
    ~COpticalFileSystem() override;
    /** Auto-select UDF on bridges; explicitly select ISO9660 to use its alternate tree. */
    static std::shared_ptr<COpticalFileSystem> open(std::shared_ptr<IBlockDevice> device,
        SOpenOptions options = {},EFormat format = EFORMAT_UNKNOWN);
    /** Inspect the actual opened tree format. */
    EFormat format() const noexcept override;
    /** Copy metadata, without following symbolic links. */
    SEntry stat(const std::string& path) const override;
    /** Scan a bounded page in media order; never collect the entire tree. */
    std::vector<SEntry> list(const std::string& directory,std::uint64_t offset,std::size_t count = 128) const override;
    /** Retain file extents and device lifetime, independently of the tree. */
    std::shared_ptr<IFileReader> openFile(const std::string& path) const override;
};
} // namespace hnfs
#endif
