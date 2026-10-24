#ifndef __SRC_HNCOMP_ARCHIVE_INTERNAL_HPP__
#define __SRC_HNCOMP_ARCHIVE_INTERNAL_HPP__

#include "internal.hpp"

namespace hncomp {
namespace detail {

constexpr std::size_t ARCHIVE_CHUNK = 65536;
constexpr std::size_t ARCHIVE_ENTRIES = 1000000;

/** Private random-access backing store; read/write copies never expose a mapped pointer. */
class ArchiveStore {
    SMappedIOOptions _mappingOptions;
public:
    virtual ~ArchiveStore() = default;
    virtual std::uint64_t size() const noexcept = 0;
    virtual void read(std::uint64_t offset, SByteSpan output) const = 0;
    virtual void append(SReadOnlyByteSpan input) = 0;
    virtual void flush() = 0;
    SMappedIOOptions mappingOptions() const { return _mappingOptions; }
    virtual void setMappingOptions(SMappedIOOptions options) {
        if (options.windowBytes<4096 || options.windowBytes>64*1024*1024) { limit("mapping window must be 4 KiB..64 MiB"); }
        _mappingOptions=options;
    }
    virtual SReadOnlyByteSpan memory() const { unsupported("not a memory store"); }
    SBytes bytes(std::uint64_t offset, std::size_t count) const {
        SBytes result(count); read(offset, result); return result;
    }
};

std::shared_ptr<ArchiveStore> archiveFile(const std::filesystem::path& path, bool create, SMappedIOOptions mapping = {});
std::shared_ptr<ArchiveStore> archiveTemporary(SMappedIOOptions mapping = {});
std::shared_ptr<ArchiveStore> archiveMemory(std::size_t maximum, SReadOnlyByteSpan initial = {});
void archiveCopy(const ArchiveStore& source, std::uint64_t offset, std::uint64_t count, const SArchiveSink& sink);
std::uint32_t archiveCrc(const ArchiveStore& source, std::uint64_t offset, std::uint64_t count);
void archiveCrcUpdate(std::uint32_t& crc, SReadOnlyByteSpan bytes);

/** Stateful LZMA2 chunk decoder, bounded by the declared dictionary (at most 64 MiB). */
class Lzma2Decoder {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    explicit Lzma2Decoder(unsigned dictionaryProperty);
    ~Lzma2Decoder();
    SBytes chunk(unsigned control, SReadOnlyByteSpan payload, std::size_t rawSize, unsigned properties);
};
SBytes encodeLzma2Chunk(SReadOnlyByteSpan input, unsigned level);
void archiveValidateXz(const SXzOptions& options);
void archiveWriteXz(const ArchiveStore& source, const SArchiveSink& sink, unsigned level, const SXzOptions& options);
std::uint64_t archiveReadXz(const ArchiveStore& source, const SArchiveSink& sink);
void archiveGzip(const ArchiveStore& source, const SArchiveSink& sink, bool encode, unsigned level);

} // namespace detail
} // namespace hncomp

#endif
