#ifndef __INCLUDE_HNFS_ASYNC_ARCHIVE_HPP__
#define __INCLUDE_HNFS_ASYNC_ARCHIVE_HPP__

#include <hnfs/filesystem.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

namespace hnfs {

/** Positional device operations scheduled on a caller-owned blocking-I/O worker. */
class HNFS_API CDeviceAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    /** Retain the device, borrow the worker; the worker must outlive accepted tasks. */
    CDeviceAsync(std::shared_ptr<IBlockDevice> device,asyncpp::dispatcher& worker);
    /** Created tasks own their arguments and do not reference this adapter. */
    ~CDeviceAsync();
    /** Own the result, capped at 64 MiB per call. */
    asyncpp::task<SBytes> readAt(std::uint64_t offset,std::size_t count) const;
    /** Capture bytes at call time, not by a borrowed span. */
    asyncpp::task<void> writeAt(std::uint64_t offset,SBytes bytes) const;
    /** Execute and report device flush errors on the worker. */
    asyncpp::task<void> flush() const;
};

/** Async sequential reader; await operations in order on a given cursor. */
class HNFS_API CArchiveReaderAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    /** Retain the cursor and borrow a worker; caller continuations resume there. */
    CArchiveReaderAsync(std::shared_ptr<IArchiveReader> reader,asyncpp::dispatcher& worker);
    /** Tasks remain valid after adapter destruction while the worker remains alive. */
    ~CArchiveReaderAsync();
    /** Capture the device immediately and open on the worker. */
    static asyncpp::task<std::shared_ptr<IArchiveReader>> open(std::shared_ptr<IBlockDevice> device,
        asyncpp::dispatcher& worker,SOpenOptions options = {});
    /** Skip/validate the previous payload and return the next record on the worker. */
    asyncpp::task<std::optional<SEntry>> next() const;
    /** Return an owned chunk (at most 64 MiB), never a borrowed caller buffer. */
    asyncpp::task<SBytes> read(std::size_t maximum = 65536) const;
};

/** Async sequential builder; operation ordering remains explicit through co_await. */
class HNFS_API CArchiveBuilderAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    /** Retain the builder and borrow a worker. */
    CArchiveBuilderAsync(std::shared_ptr<IArchiveBuilder> builder,asyncpp::dispatcher& worker);
    /** Destruction never implicitly finishes an image. */
    ~CArchiveBuilderAsync();
    /** Capture options/device immediately; verify overwrite policy on the worker. */
    static asyncpp::task<std::shared_ptr<IArchiveBuilder>> create(std::shared_ptr<IBlockDevice> device,
        SArchiveCreateOptions options,asyncpp::dispatcher& worker);
    /** Capture copied metadata before a lazy task starts. */
    asyncpp::task<void> beginEntry(SEntry entry) const;
    /** Capture owned file bytes before a lazy task starts. */
    asyncpp::task<void> write(SBytes bytes) const;
    /** Explicitly finalize and flush on the worker. */
    asyncpp::task<void> finish() const;
};

} // namespace hnfs
#endif
