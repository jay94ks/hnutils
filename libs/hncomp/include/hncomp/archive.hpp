#ifndef __INCLUDE_HNCOMP_ARCHIVE_HPP__
#define __INCLUDE_HNCOMP_ARCHIVE_HPP__

#include <hncomp/compressor.hpp>
#include <filesystem>
#include <functional>
#include <string>

namespace hncomp {

/** Recognized containers; XZ alone holds one anonymous file named "data". */
enum EArchiveFormat { EARCHIVE_ZIP, EARCHIVE_TAR, EARCHIVE_XZ, EARCHIVE_TAR_XZ, EARCHIVE_TAR_GZIP };

/** CREATE permits reads and additions; OPEN is strictly read-only. */
enum EArchiveOpenMode { EARCHIVE_CREATE, EARCHIVE_OPEN };

/** Bounded sliding-window mapping policy; never maps a complete file by default. */
struct SMappedIOOptions {
    /** Useful bytes per window, 4 KiB..64 MiB, plus an OS alignment prefix. */
    std::size_t windowBytes = 1024*1024;
};

/** ZIP encryption on creation; existing ZIP entries select their own scheme when read. */
enum EZipEncryption { EZIP_NONE, EZIP_AES128, EZIP_AES192, EZIP_AES256 };

/** Password bytes are used as supplied (UTF-8 recommended), without normalization. */
struct SZipOptions {
    /** NONE is the default; AES creation uses the WinZip AE-2 format. */
    EZipEncryption encryption = EZIP_NONE;
    /** Creation requires a nonempty password when encryption is selected. */
    std::string password;
};

/** Standard XZ prefilter identifiers; LZMA2 is appended automatically. */
enum EXzFilter { EXZ_DELTA = 3, EXZ_POWERPC = 5, EXZ_ARM = 7, EXZ_ARMTHUMB = 8, EXZ_SPARC = 9, EXZ_ARM64 = 10 };

/** One size-preserving prefilter in encoding order. */
struct SXzFilterOptions {
    /** Selected architecture or byte delta transform. */
    EXzFilter filter = EXZ_DELTA;
    /** BCJ start address; must satisfy the selected instruction alignment. */
    std::uint32_t startOffset = 0;
    /** Delta history distance, from 1 to 256; unused by BCJ. */
    unsigned deltaDistance = 1;
};

/** Standard XZ integrity checks, not authentication. */
enum EXzCheck { EXZ_CHECK_NONE = 0, EXZ_CHECK_CRC32 = 1, EXZ_CHECK_CRC64 = 4, EXZ_CHECK_SHA256 = 10 };

/** XZ creation settings; readers detect checks and filter chains from each block. */
struct SXzOptions {
    /** Up to three Delta/BCJ filters, applied in this order before LZMA2. */
    std::vector<SXzFilterOptions> filters;
    /** Integrity check for original unfiltered data. */
    EXzCheck check = EXZ_CHECK_CRC32;
};

/** File creation selects a container by extension; opening detects its contents. */
struct SArchiveOpenOptions {
    /** Search effort from one through nine. */
    unsigned compressionLevel = 5;
    /** OPEN never modifies the file; CREATE fails if the destination exists. */
    EArchiveOpenMode mode = EARCHIVE_OPEN;
    /** Aggregate expansion ceiling during opening; TAR headers/padding count too. */
    std::uint64_t maximumExpandedBytes = UINT64_C(64) * 1024 * 1024 * 1024;
    /** Creation scheme/password or a password for opening an encrypted ZIP. */
    SZipOptions zip;
    /** XZ/TAR_XZ creation filters and integrity check. */
    SXzOptions xz;
    /** File, staging and subsequently added source-file mapping policy. */
    SMappedIOOptions mapping;
};

/** Memory creation has no file name from which to infer a container. */
struct SArchiveMemoryOptions {
    /** Explicit output container. */
    EArchiveFormat format = EARCHIVE_ZIP;
    /** Search effort from one through nine. */
    unsigned compressionLevel = 5;
    /** ZIP-only encryption settings. */
    SZipOptions zip;
    /** XZ/TAR_XZ-only creation settings. */
    SXzOptions xz;
};

/** Copied metadata, never a mutable reference to an archive index. */
struct SArchiveEntry {
    /** Portable relative UTF-8 path; directories have a trailing slash. */
    std::string name;
    /** True for a directory, false for a regular file. */
    bool directory = false;
    /** Uncompressed file length. */
    std::uint64_t size = 0;
    /** True when ZIP entry data requires a password. */
    bool encrypted = false;
};

/** Consume a borrowed output chunk before returning; exceptions abort the operation. */
using SArchiveSink = std::function<void(SReadOnlyByteSpan)>;

/** Receives the complete memory container once; the span is valid only during the call. */
using SArchiveCompletion = std::function<void(SReadOnlyByteSpan)>;

/** Archive access with private reader/writer implementations and explicit finalization. */
class HNCOMP_API IArchive : public std::enable_shared_from_this<IArchive> {
public:
    /** Release resources; destruction never invokes user callbacks or implicitly finishes. */
    virtual ~IArchive();
    /** Open by signature or create by extension, using windowed mapped file I/O. */
    static std::shared_ptr<IArchive> createFromFile(const std::filesystem::path& filename,
        SArchiveOpenOptions options = {});
    /** Bound both memory staging and final output; invoke completion only on explicit finish. */
    static std::shared_ptr<IArchive> createInMemory(std::size_t maximumSize,
        SArchiveMemoryOptions options, SArchiveCompletion completion);
    /** Copy the provided container before returning; detect format from contents. */
    static std::shared_ptr<IArchive> openFromMemory(SReadOnlyByteSpan memory,
        std::uint64_t maximumExpandedBytes = UINT64_C(256) * 1024 * 1024, std::string password = {});
    /** Inspect the detected/selected container. */
    virtual EArchiveFormat format() const noexcept = 0;
    /** Query the original opening mode; a sealed creator still reports false. */
    virtual bool isReadOnly() const noexcept = 0;
    /** Copied current mapping policy; memory stores retain policy for added source files. */
    virtual SMappedIOOptions mappingOptions() const = 0;
    /** Serialize with other operations. Apply to output/staging and future source files. */
    virtual void setMappingOptions(SMappedIOOptions options) = 0;
    /** Number of indexed entries, without copying metadata. */
    virtual std::uint64_t entryCount() const noexcept = 0;
    /** Copy at most count entries (0..4096); a range past the end returns an empty page. */
    virtual std::vector<SArchiveEntry> entries(std::uint64_t offset, std::size_t count = 128) const = 0;
    /** Append a directory; reject duplicate/unsafe names and all read-only mutations. */
    virtual void addDirectory(std::string name) = 0;
    /** Append a complete file; the span is not retained. */
    virtual void addFile(std::string name, SReadOnlyByteSpan content) = 0;
    /** Append a regular source file through bounded mappings, without loading it whole. */
    virtual void addFileFromFile(std::string name, const std::filesystem::path& source) = 0;
    /** Stream a complete regular file and verify checksums; chunks are provisional until success. */
    virtual void readFile(const std::string& name, SArchiveSink sink) const = 0;
    /** Collect a file with an explicit allocation/expansion ceiling (default 64 MiB). */
    SBytes readFile(const std::string& name, std::size_t maximumSize = 64 * 1024 * 1024) const;
    /** Seal a creator; idempotent after success, forbidden for read-only instances. */
    virtual void finish() = 0;
    /** Open/create on a caller-owned dispatcher, capturing the owned path immediately. */
    static asyncpp::task<std::shared_ptr<IArchive>> createFromFileAsync(std::filesystem::path filename,
        SArchiveOpenOptions options, asyncpp::dispatcher& worker);
    /** Own the container before suspension; parse and validate it on a worker. */
    static asyncpp::task<std::shared_ptr<IArchive>> openFromMemoryAsync(SBytes memory,
        std::uint64_t maximumExpandedBytes, asyncpp::dispatcher& worker, std::string password = {});
    /** Copy and decode on a worker; retain this object until completion. */
    asyncpp::task<SBytes> readFileAsync(std::string name, std::size_t maximumSize, asyncpp::dispatcher& worker);
    /** Stream on a worker without collecting a whole file; the sink executes on that worker. */
    asyncpp::task<void> readFileAsync(std::string name, SArchiveSink sink, asyncpp::dispatcher& worker);
    /** Own file bytes and name before suspension; serialize operations per instance. */
    asyncpp::task<void> addFileAsync(std::string name, SBytes content, asyncpp::dispatcher& worker);
    /** Map a source file on a worker without retaining caller-owned paths. */
    asyncpp::task<void> addFileFromFileAsync(std::string name, std::filesystem::path source, asyncpp::dispatcher& worker);
    /** Seal on a worker; memory completion runs on that worker and may throw. */
    asyncpp::task<void> finishAsync(asyncpp::dispatcher& worker);
};

} // namespace hncomp

#endif
