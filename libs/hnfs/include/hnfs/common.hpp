#ifndef __INCLUDE_HNFS_COMMON_HPP__
#define __INCLUDE_HNFS_COMMON_HPP__

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(HNFS_SHARED)
#if defined(HNFS_BUILDING_LIBRARY)
#define HNFS_API __declspec(dllexport)
#else
#define HNFS_API __declspec(dllimport)
#endif
#else
#define HNFS_API
#endif

namespace hnfs {

/** Owned bytes used at API ownership boundaries. */
using SBytes = std::vector<std::uint8_t>;
/** Borrowed writable bytes; never retained by synchronous calls. */
using SByteSpan = std::span<std::uint8_t>;
/** Borrowed input bytes; never retained by synchronous calls. */
using SReadOnlyByteSpan = std::span<const std::uint8_t>;

/** On-disk formats, not a claim that a read/write driver is available. */
enum EFormat {
    EFORMAT_UNKNOWN, EFORMAT_FAT12, EFORMAT_FAT16, EFORMAT_FAT32, EFORMAT_EXFAT,
    EFORMAT_NTFS, EFORMAT_EXT2, EFORMAT_EXT3, EFORMAT_EXT4, EFORMAT_XFS,
    EFORMAT_BTRFS, EFORMAT_SQUASHFS, EFORMAT_CPIO_BINARY_LE, EFORMAT_CPIO_BINARY_BE,
    EFORMAT_CPIO_ODC, EFORMAT_CPIO_NEWC, EFORMAT_CPIO_CRC, EFORMAT_APFS,
    EFORMAT_ISO9660, EFORMAT_UDF
};

/** Logical errors; OS I/O failures retain their native std::system_error codes. */
enum EError { EERROR_INVALID, EERROR_RANGE, EERROR_READ_ONLY, EERROR_UNSUPPORTED,
    EERROR_TRUNCATED, EERROR_LIMIT, EERROR_STATE, EERROR_CHECKSUM,
    EERROR_NOT_FOUND, EERROR_NOT_DIRECTORY, EERROR_IS_DIRECTORY };

/** Filesystem-format error carrying a stable category. */
class HNFS_API CFileSystemError : public std::runtime_error {
private:
    EError _code;
public:
    /** Construct an error without exposing mutable state. */
    CFileSystemError(EError code, std::string message);
    /** Retrieve the logical category. */
    EError code() const noexcept;
};

/** Directory-entry kinds; links are never implicitly followed. */
enum ENodeKind { ENODE_FILE, ENODE_DIRECTORY, ENODE_SYMLINK, ENODE_SPECIAL };

/** Copied metadata; path and linkTarget are image paths, never host paths. */
struct SEntry {
    /** Relative path, or an empty string for the root entry. */
    std::string path;
    /** File, directory, link or special node. */
    ENodeKind kind = ENODE_FILE;
    /** Logical data length. */
    std::uint64_t size = 0;
    /** Unix permission bits without the file-type field. */
    std::uint32_t permissions = 0644;
    /** Numeric owner and group, where meaningful. */
    std::uint32_t uid = 0, gid = 0;
    /** Unix seconds; drivers must reject an unrepresentable timestamp. */
    std::uint64_t modifiedTime = 0;
};

/** Parsing limits are independent of the backing device's 64-bit capacity. */
struct SOpenOptions {
    /** Maximum records visited by an archive reader. */
    std::uint64_t maximumEntries = 1000000;
    /** Maximum individual name including its terminator. */
    std::size_t maximumPathBytes = 4096;
    /** Aggregate name bytes visited, even though readers do not retain a full index. */
    std::uint64_t maximumNameBytes = UINT64_C(64)*1024*1024;
    /** Maximum aggregate declared file bytes. */
    std::uint64_t maximumDataBytes = UINT64_C(64)*1024*1024*1024;
    /** Maximum zero-filled device tail scanned after an archive trailer. */
    std::uint64_t maximumTrailingPaddingBytes = UINT64_C(16)*1024*1024;
    /** Maximum single optical directory stream, scanned without a whole-stream allocation. */
    std::uint64_t maximumDirectoryBytes = UINT64_C(64)*1024*1024;
    /** Maximum extents retained for one random-access optical file. */
    std::uint32_t maximumExtents = 65536;
    /** Maximum path components and volume descriptors visited. */
    std::uint32_t maximumDepth = 128, maximumDescriptors = 256;
};

/** Creation options for sequential archive builders, not mutable block volumes. */
struct SArchiveCreateOptions {
    /** Explicit format; no extension-based guessing during creation. */
    EFormat format = EFORMAT_CPIO_NEWC;
    /** Require explicit permission before replacing nonzero device contents. */
    bool allowOverwrite = false;
    /** Same entry/path/data policy as readers. */
    SOpenOptions limits;
};

/** Header classification only; drivers validate records as they are accessed. */
struct SProbeResult {
    /** UNKNOWN if no supported signature is recognizable. */
    EFormat format = EFORMAT_UNKNOWN;
    /** Explanation of limited recognition, not a mountability guarantee. */
    std::string detail;
};

/** Honest built-in implementation availability, separate from signature recognition. */
struct SFormatSupport {
    /** The requested format. */
    EFormat format = EFORMAT_UNKNOWN;
    /** A lightweight probe is available. */
    bool probe = false;
    /** Sequential reader/builder interfaces are implemented. */
    bool archiveRead = false, archiveCreate = false;
    /** Native volume tree interfaces, excluding separately requested archive adapters. */
    bool treeRead = false, treeEdit = false;
    /** A separately requested, bounded read-only archive tree adapter is implemented. */
    bool archiveIndexRead = false;
};

/** Return the stable human-readable format name. */
HNFS_API const char* formatName(EFormat format) noexcept;

} // namespace hnfs
#endif
