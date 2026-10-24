#include "archive_internal.hpp"

#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hncomp {
namespace detail {
namespace {

[[noreturn]] void ioError(const char* message) {
#if defined(_WIN32)
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
#else
    throw std::system_error(errno, std::generic_category(), message);
#endif
}

class MappedStore final : public ArchiveStore {
private:
#if defined(_WIN32)
    HANDLE _file = INVALID_HANDLE_VALUE;
    mutable HANDLE _mapping = nullptr;
#else
    int _file = -1;
#endif
    bool _writable;
    std::uint64_t _size = 0, _capacity = 0;
    mutable std::uint8_t* _view = nullptr;
    mutable std::uint64_t _start = 0;
    mutable std::size_t _mapped = 0;
    mutable bool _dirty = false;
    std::size_t _granularity = 4096;

    void unmap() const noexcept {
        if (_view) {
#if defined(_WIN32)
            UnmapViewOfFile(_view);
#else
            munmap(_view, _mapped);
#endif
            _view = nullptr; _mapped = 0; _dirty = false;
        }
#if defined(_WIN32)
        if (_mapping) { CloseHandle(_mapping); _mapping = nullptr; }
#endif
    }
    void flushView() const {
        if (!_view || !_dirty) { return; }
#if defined(_WIN32)
        if (!FlushViewOfFile(_view, _mapped)) { ioError("flush mapped archive window"); }
#else
        if (msync(_view, _mapped, MS_SYNC)) { ioError("flush mapped archive window"); }
#endif
        _dirty = false;
    }
    void resize(std::uint64_t size) {
        if (size > INT64_MAX) { limit("archive file exceeds signed 64-bit offsets"); }
        flushView(); unmap();
#if defined(_WIN32)
        LARGE_INTEGER end; end.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(_file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(_file)) { ioError("resize mapped archive"); }
#else
        if (ftruncate(_file, static_cast<off_t>(size))) { ioError("resize mapped archive"); }
#endif
        _capacity = size;
    }
    void map(std::uint64_t offset) const {
        if (_view && offset >= _start && offset - _start < _mapped) { return; }
        flushView(); unmap(); _start = offset - offset % _granularity;
        _mapped = static_cast<std::size_t>(std::min<std::uint64_t>(mappingOptions().windowBytes+offset-_start, _capacity - _start));
#if defined(_WIN32)
        _mapping = CreateFileMappingW(_file, nullptr, _writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        if (!_mapping) { ioError("create archive file mapping"); }
        _view = static_cast<std::uint8_t*>(MapViewOfFile(_mapping, _writable ? FILE_MAP_WRITE : FILE_MAP_READ,
            static_cast<DWORD>(_start >> 32), static_cast<DWORD>(_start), _mapped));
        if (!_view) { ioError("map archive window"); }
#else
        auto view = mmap(nullptr, _mapped, _writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, _file, static_cast<off_t>(_start));
        if (view == MAP_FAILED) { _mapped = 0; ioError("map archive window"); }
        _view = static_cast<std::uint8_t*>(view);
#endif
    }

public:
    void setMappingOptions(SMappedIOOptions options) override {
        if (options.windowBytes<4096 || options.windowBytes>64*1024*1024) { limit("mapping window must be 4 KiB..64 MiB"); }
        flushView(); unmap(); ArchiveStore::setMappingOptions(options);
    }
    MappedStore(const std::filesystem::path& path, bool create, bool temporary = false) : _writable(create || temporary) {
#if defined(_WIN32)
        SYSTEM_INFO info{}; GetSystemInfo(&info); _granularity = info.dwAllocationGranularity;
        _file = CreateFileW(path.c_str(), _writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | (temporary ? FILE_FLAG_DELETE_ON_CLOSE : 0), nullptr);
        if (_file == INVALID_HANDLE_VALUE) { ioError("open mapped archive"); }
        LARGE_INTEGER size;
        if (!GetFileSizeEx(_file, &size) || size.QuadPart < 0 || GetFileType(_file) != FILE_TYPE_DISK) {
            CloseHandle(_file); throw std::runtime_error("archive is not a regular disk file");
        }
        _size = _capacity = static_cast<std::uint64_t>(size.QuadPart);
#else
        static_assert(sizeof(off_t) >= 8, "hncomp archives require 64-bit file offsets");
        const auto page = sysconf(_SC_PAGESIZE); if (page > 0) { _granularity = static_cast<std::size_t>(page); }
        if (temporary) {
            auto pattern = (std::filesystem::temp_directory_path() / "hncomp-XXXXXX").string();
            _file = mkstemp(pattern.data());
            if (_file < 0) { ioError("create archive staging file"); }
            if (unlink(pattern.c_str())) { const auto saved = errno; close(_file); errno = saved; ioError("unlink archive staging file"); }
            if (fcntl(_file, F_SETFD, FD_CLOEXEC) < 0) { const auto saved = errno; close(_file); errno = saved; ioError("protect archive staging descriptor"); }
        } else { _file = ::open(path.c_str(), create ? O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0600); }
        if (_file < 0) { ioError("open mapped archive"); }
        struct stat st{};
        if (fstat(_file, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
            close(_file); throw std::runtime_error("archive is not a regular disk file");
        }
        _size = _capacity = static_cast<std::uint64_t>(st.st_size);
#endif
    }
    ~MappedStore() override {
        unmap();
        if (_writable && _capacity != _size) { try { resize(_size); } catch (...) {} }
#if defined(_WIN32)
        if (_file != INVALID_HANDLE_VALUE) { CloseHandle(_file); }
#else
        if (_file >= 0) { close(_file); }
#endif
    }
    std::uint64_t size() const noexcept override { return _size; }
    void read(std::uint64_t offset, SByteSpan output) const override {
        if (offset > _size || output.size() > _size - offset) { throw CCompressionError(EERROR_TRUNCATED, "archive range outside file"); }
        while (!output.empty()) {
            map(offset); const auto delta = static_cast<std::size_t>(offset - _start);
            const auto count = std::min(output.size(), _mapped - delta);
            std::memcpy(output.data(), _view + delta, count); offset += count; output = output.subspan(count);
        }
    }
    void append(SReadOnlyByteSpan input) override {
        if (!_writable) { throw CCompressionError(EERROR_STATE, "read-only mapped archive"); }
        if (input.size() > INT64_MAX - _size) { limit("archive file size overflow"); }
        const auto end = _size + input.size();
        if (end > _capacity) { resize(end > INT64_MAX - 1048575 ? end : (end + 1048575) & ~std::uint64_t(1048575)); }
        while (!input.empty()) {
            map(_size); const auto delta = static_cast<std::size_t>(_size - _start);
            const auto count = std::min(input.size(), _mapped - delta);
            std::memcpy(_view + delta, input.data(), count); _dirty = true; _size += count; input = input.subspan(count);
        }
    }
    void flush() override {
        if (!_writable) { return; }
        resize(_size);
#if defined(_WIN32)
        if (!FlushFileBuffers(_file)) { ioError("flush archive file"); }
#else
        if (fsync(_file)) { ioError("flush archive file"); }
#endif
    }
};

class MemoryStore final : public ArchiveStore {
private:
    SBytes _data;
    std::size_t _maximum;
public:
    MemoryStore(std::size_t maximum, SReadOnlyByteSpan initial) : _maximum(maximum) {
        if (initial.size() > maximum) { limit("memory archive exceeds maximum size"); }
        _data.assign(initial.begin(), initial.end());
    }
    std::uint64_t size() const noexcept override { return _data.size(); }
    void read(std::uint64_t offset, SByteSpan output) const override {
        if (offset > _data.size() || output.size() > _data.size() - offset) { throw CCompressionError(EERROR_TRUNCATED, "archive range outside memory"); }
        std::copy_n(_data.begin() + static_cast<std::size_t>(offset), output.size(), output.begin());
    }
    void append(SReadOnlyByteSpan input) override { room(_data, input.size(), _maximum); _data.insert(_data.end(), input.begin(), input.end()); }
    void flush() override {}
    SReadOnlyByteSpan memory() const override { return _data; }
};

} // namespace

/* Construct a windowed disk backing store, preserving existing paths on CREATE. */
std::shared_ptr<ArchiveStore> archiveFile(const std::filesystem::path& path, bool create, SMappedIOOptions mapping) {
    if (mapping.windowBytes<4096 || mapping.windowBytes>64*1024*1024) { limit("mapping window must be 4 KiB..64 MiB"); }
    auto store=std::make_shared<MappedStore>(path, create); store->setMappingOptions(mapping); return store;
}

/* Staging is private, automatically removed, and never a whole-file RAM allocation. */
std::shared_ptr<ArchiveStore> archiveTemporary(SMappedIOOptions mapping) {
    if (mapping.windowBytes<4096 || mapping.windowBytes>64*1024*1024) { limit("mapping window must be 4 KiB..64 MiB"); }
#if defined(_WIN32)
    wchar_t folder[MAX_PATH + 1]{}, name[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"hnc", 0, name)) { ioError("create archive staging path"); }
    try { auto store=std::make_shared<MappedStore>(name, false, true); store->setMappingOptions(mapping); return store; }
    catch (...) { DeleteFileW(name); throw; }
#else
    auto store=std::make_shared<MappedStore>(std::filesystem::path{}, false, true); store->setMappingOptions(mapping); return store;
#endif
}

/* Memory paths own their input; no caller span is retained. */
std::shared_ptr<ArchiveStore> archiveMemory(std::size_t maximum, SReadOnlyByteSpan initial) { return std::make_shared<MemoryStore>(maximum, initial); }

/* Stream a validated 64-bit range with bounded scratch memory. */
void archiveCopy(const ArchiveStore& source, std::uint64_t offset, std::uint64_t count, const SArchiveSink& sink) {
    if (offset > source.size() || count > source.size() - offset) { invalid("archive range exceeds backing store"); }
    std::array<std::uint8_t, ARCHIVE_CHUNK> buffer{};
    while (count) {
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(count, buffer.size()));
        auto chunk = SByteSpan(buffer).first(amount); source.read(offset, chunk); sink(chunk); offset += amount; count -= amount;
    }
}

/* Incremental CRC-32 for streams larger than addressable contiguous memory. */
void archiveCrcUpdate(std::uint32_t& crc, SReadOnlyByteSpan input) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (unsigned i = 0; i < 256; ++i) {
            auto value = i; for (unsigned b = 0; b < 8; ++b) { value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1))); } values[i] = value;
        }
        return values;
    }();
    for (auto value : input) { crc = (crc >> 8) ^ table[(crc ^ value) & 255]; }
}

std::uint32_t archiveCrc(const ArchiveStore& source, std::uint64_t offset, std::uint64_t count) {
    std::uint32_t crc = 0xffffffffu; archiveCopy(source, offset, count, [&](auto bytes) { archiveCrcUpdate(crc, bytes); }); return ~crc;
}

} // namespace detail
} // namespace hncomp
