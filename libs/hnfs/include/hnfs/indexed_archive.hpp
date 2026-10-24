#ifndef __INCLUDE_HNFS_INDEXED_ARCHIVE_HPP__
#define __INCLUDE_HNFS_INDEXED_ARCHIVE_HPP__

#include <hnfs/filesystem.hpp>

namespace hnfs {

/** Optional read-only pathname index; never makes a sequential archive mutable. */
class HNFS_API CIndexedArchive final : public ITreeReader {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    explicit CIndexedArchive(std::unique_ptr<Impl> impl);
public:
    /** Scan and validate the archive, retaining only bounded metadata, not file contents. */
    static std::shared_ptr<CIndexedArchive> open(std::shared_ptr<IBlockDevice> device,SOpenOptions options = {});
    /** Release the index; open file views retain their device lifetime independently. */
    ~CIndexedArchive() override;
    /** Inspect the archive's format. */
    EFormat format() const noexcept override;
    /** Case-sensitive relative pathname lookup; empty or slash denotes root. */
    SEntry stat(const std::string& path) const override;
    /** Lexical direct-child page, at most 4096 entries; no complete-list allocation. */
    std::vector<SEntry> list(const std::string& directory,std::uint64_t offset,std::size_t count = 128) const override;
    /** Positional read-only file view; no mutable index or device handle is returned. */
    std::shared_ptr<IFileReader> openFile(const std::string& path) const override;
};

} // namespace hnfs
#endif
