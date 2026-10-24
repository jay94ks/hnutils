#ifndef __INCLUDE_HNFS_ASYNC_TREE_HPP__
#define __INCLUDE_HNFS_ASYNC_TREE_HPP__

#include <hnfs/indexed_archive.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

namespace hnfs {

/** Async adapter for actual read-only trees, not sequential cursors or mutable namespaces. */
class HNFS_API CTreeReaderAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    /** Retain the tree and borrow a blocking-I/O worker. */
    CTreeReaderAsync(std::shared_ptr<ITreeReader> tree,asyncpp::dispatcher& worker);
    /** Created tasks retain the tree independently of the adapter. */
    ~CTreeReaderAsync();
    /** Open a native read-only tree on the worker; archives still require indexArchive(). */
    static asyncpp::task<std::shared_ptr<ITreeReader>> open(std::shared_ptr<IBlockDevice> device,
        asyncpp::dispatcher& worker,SOpenOptions options = {});
    /** Build an explicitly requested archive index on a worker. */
    static asyncpp::task<std::shared_ptr<CIndexedArchive>> indexArchive(std::shared_ptr<IBlockDevice> device,
        asyncpp::dispatcher& worker,SOpenOptions options = {});
    /** Capture the pathname immediately and query copied metadata on the worker. */
    asyncpp::task<SEntry> stat(std::string path) const;
    /** Read at most count directory entries on the worker. */
    asyncpp::task<std::vector<SEntry>> list(std::string directory,std::uint64_t offset,std::size_t count = 128) const;
    /** Return an owned positional file chunk, capped at 64 MiB. */
    asyncpp::task<SBytes> readFile(std::string path,std::uint64_t offset,std::size_t count) const;
};

} // namespace hnfs
#endif
