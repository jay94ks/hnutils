#ifndef __INCLUDE_HNFS_FILESYSTEM_HPP__
#define __INCLUDE_HNFS_FILESYSTEM_HPP__

#include <hnfs/device.hpp>
#include <optional>

namespace hnfs {

/** Minimal identity shared by volumes, containers and archive views -- no universal CRUD. */
class HNFS_API IFileSystem {
public:
    /** Release the view without committing pending changes. */
    virtual ~IFileSystem();
    /** Inspect the actual format. */
    virtual EFormat format() const noexcept = 0;
    /** Inspect only format headers; this does not validate or open a volume. */
    static SProbeResult probe(const IBlockDevice& device);
    /** Query the built-in implementation manifest, not hypothetical format features. */
    static SFormatSupport support(EFormat format) noexcept;
    /** Open a read-only format view; unknown/unimplemented drivers fail explicitly. */
    static std::shared_ptr<IFileSystem> open(std::shared_ptr<IBlockDevice> device, SOpenOptions options = {});
};

/** Random-access regular-file reading, independent from pathname lookup. */
class HNFS_API IFileReader {
public:
    /** Release the open file view. */
    virtual ~IFileReader();
    /** Logical length, including holes if the format supports sparse files. */
    virtual std::uint64_t size() const = 0;
    /** Read up to the output capacity; zero denotes EOF, never an I/O error. */
    virtual std::size_t readAt(std::uint64_t offset, SByteSpan output) const = 0;
};

/** File-content editing; does not imply journal transactions or directory mutability. */
class HNFS_API IFileEditor : public virtual IFileReader {
public:
    /** Release without an implicit commit. */
    virtual ~IFileEditor();
    /** Write the whole span or report failure; atomicity is format-specific. */
    virtual void writeAt(std::uint64_t offset, SReadOnlyByteSpan input) = 0;
    /** Set logical length; growth reads as zero unless explicitly documented otherwise. */
    virtual void truncate(std::uint64_t length) = 0;
    /** Flush this file's pending changes, not a filesystem-wide transaction. */
    virtual void flush() = 0;
};

/** Path-oriented read access; sequential archives need not implement this interface. */
class HNFS_API ITreeReader : public virtual IFileSystem {
public:
    /** Release the directory-tree view. */
    virtual ~ITreeReader();
    /** Read copied metadata without following symbolic links. */
    virtual SEntry stat(const std::string& path) const = 0;
    /** Return a bounded directory page; count is at most 4096. */
    virtual std::vector<SEntry> list(const std::string& directory, std::uint64_t offset,
        std::size_t count = 128) const = 0;
    /** Open a regular file without implicitly following links. */
    virtual std::shared_ptr<IFileReader> openFile(const std::string& path) const = 0;
};

/** Mutable namespace; no blanket claim that every format implements it. */
class HNFS_API ITreeEditor : public virtual ITreeReader {
public:
    /** Release without an implicit commit. */
    virtual ~ITreeEditor();
    /** Create one directory; parents must exist. */
    virtual void createDirectory(const std::string& path) = 0;
    /** Exclusively create a regular file. */
    virtual std::shared_ptr<IFileEditor> createFile(const std::string& path) = 0;
    /** Open an existing regular file for content modification. */
    virtual std::shared_ptr<IFileEditor> editFile(const std::string& path) = 0;
    /** Rename within this tree without replacing an existing destination. */
    virtual void rename(const std::string& from, const std::string& to) = 0;
    /** Remove a file or an empty directory; never recurse implicitly. */
    virtual void remove(const std::string& path) = 0;
    /** Persist pending metadata; no cross-file transaction guarantee is implied. */
    virtual void flush() = 0;
};

/** Sequential archive cursor; next() validates/skips any unread preceding payload. */
class HNFS_API IArchiveReader : public virtual IFileSystem {
public:
    /** Release without automatically scanning unread data. */
    virtual ~IArchiveReader();
    /** Return one record; nullopt only after the validated end marker and zero padding. */
    virtual std::optional<SEntry> next() = 0;
    /** Read current data; zero at its end or for an empty span, throw on malformed input. */
    virtual std::size_t read(SByteSpan output) = 0;
    /** Position just after the trailer, excluding trailing device padding; available at EOF. */
    virtual std::uint64_t usedBytes() const = 0;
    /** Construct a read-only archive cursor with owned device lifetime. */
    static std::shared_ptr<IArchiveReader> open(std::shared_ptr<IBlockDevice> device, SOpenOptions options = {});
};

/** Sequential image construction, distinct from editing an existing mounted tree. */
class HNFS_API IArchiveBuilder : public virtual IFileSystem {
public:
    /** Release without implicitly finishing the image. */
    virtual ~IArchiveBuilder();
    /** Begin a regular file/directory; a preceding file must be complete. */
    virtual void beginEntry(SEntry entry) = 0;
    /** Append current file bytes; never exceed its declared size. */
    virtual void write(SReadOnlyByteSpan bytes) = 0;
    /** Complete the image and flush; idempotent only after success. */
    virtual void finish() = 0;
    /** Current image length, including its final trailer after finishing. */
    virtual std::uint64_t usedBytes() const = 0;
    /** Construct an explicit builder; this operation is not a mount or in-place editor. */
    static std::shared_ptr<IArchiveBuilder> create(std::shared_ptr<IBlockDevice> device,
        SArchiveCreateOptions options = {});
};

} // namespace hnfs
#endif
