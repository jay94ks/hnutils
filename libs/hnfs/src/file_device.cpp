#include <hnfs/device.hpp>
#include "internal.hpp"
#include <system_error>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace hnfs {

struct CFileDevice::Impl {
#if defined(_WIN32)
    HANDLE handle=INVALID_HANDLE_VALUE;
    HANDLE mapping=nullptr;
#else
    int handle=-1;
#endif
    std::uint64_t capacity=0;
    bool readOnly=true;
    mutable std::mutex mutex;
    mutable std::uint8_t* view=nullptr;
    mutable std::uint64_t viewOffset=0;
    mutable std::size_t viewSize=0;
    mutable bool dirty=false;
    std::size_t alignment=65536;
    SMappedIOOptions options;
    static void validateMapping(SMappedIOOptions options) {
        if (options.windowBytes<4096 || options.windowBytes>64*1024*1024) { detail::fail(EERROR_LIMIT,"mapping window must be 4 KiB..64 MiB"); }
    }

    ~Impl() {
#if defined(_WIN32)
        if (view) { UnmapViewOfFile(view); }
        if (mapping) { CloseHandle(mapping); }
        if (handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle); }
#else
        if (view) { ::munmap(view,viewSize); }
        if (handle>=0) { ::close(handle); }
#endif
    }

    [[noreturn]] static void ioError(const char* operation) {
#if defined(_WIN32)
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation);
#else
        throw std::system_error(errno,std::generic_category(),operation);
#endif
    }

    static std::unique_ptr<Impl> open(const std::filesystem::path& path,bool create,std::uint64_t requested,bool writable,SMappedIOOptions options) {
        validateMapping(options);
        if (path.native().find(std::filesystem::path::value_type{})!=std::filesystem::path::string_type::npos) { detail::fail(EERROR_INVALID,"NUL in host image path"); }
        if (requested>INT64_MAX) { detail::fail(EERROR_RANGE,"image exceeds signed 64-bit offsets"); }
        auto state=std::make_unique<Impl>(); state->readOnly=!writable; state->options=options;
#if defined(_WIN32)
        SYSTEM_INFO system{}; GetSystemInfo(&system); state->alignment=system.dwAllocationGranularity;
        const auto native=path.native();
        if (native.starts_with(L"\\\\.\\") || native.starts_with(L"\\\\?\\")) { detail::fail(EERROR_UNSUPPORTED,"Windows device namespaces are not image files"); }
        state->handle=CreateFileW(path.c_str(),GENERIC_READ|(writable?GENERIC_WRITE:0),FILE_SHARE_READ,
            nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (state->handle==INVALID_HANDLE_VALUE) { ioError("open filesystem image"); }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(state->handle,&information)) { ioError("inspect filesystem image"); }
        if (GetFileType(state->handle)!=FILE_TYPE_DISK || (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))) { detail::fail(EERROR_UNSUPPORTED,"only regular image files are supported"); }
        if (create) {
            LARGE_INTEGER length{}; length.QuadPart=static_cast<LONGLONG>(requested);
            if (!SetFilePointerEx(state->handle,length,nullptr,FILE_BEGIN) || !SetEndOfFile(state->handle)) { ioError("size filesystem image"); }
        }
        LARGE_INTEGER length{}; if (!GetFileSizeEx(state->handle,&length)) { ioError("size filesystem image"); }
        if (length.QuadPart<0) { detail::fail(EERROR_RANGE,"negative image size"); }
        state->capacity=static_cast<std::uint64_t>(length.QuadPart);
        if (state->capacity) {
            state->mapping=CreateFileMappingW(state->handle,nullptr,writable?PAGE_READWRITE:PAGE_READONLY,0,0,nullptr);
            if (!state->mapping) { ioError("create filesystem image mapping"); }
        }
#else
        const auto page=::sysconf(_SC_PAGESIZE); if (page<=0) { ioError("inspect mapping alignment"); }
        state->alignment=static_cast<std::size_t>(page);
        state->handle=::open(path.c_str(),(writable?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|(create?(O_CREAT|O_EXCL):0),0600);
        if (state->handle<0) { ioError("open filesystem image"); }
        struct stat information{}; if (::fstat(state->handle,&information)) { ioError("inspect filesystem image"); }
        if (!S_ISREG(information.st_mode)) { detail::fail(EERROR_UNSUPPORTED,"only regular image files are supported"); }
        if (create && ::ftruncate(state->handle,static_cast<off_t>(requested))) { ioError("size filesystem image"); }
        if (create) { state->capacity=requested; }
        else { if (information.st_size<0) { detail::fail(EERROR_RANGE,"negative image size"); } state->capacity=static_cast<std::uint64_t>(information.st_size); }
#endif
        return state;
    }

    void flushView() const {
        if (!view || !dirty) { return; }
#if defined(_WIN32)
        if (!FlushViewOfFile(view,viewSize)) { ioError("flush filesystem image mapping"); }
#else
        if (::msync(view,viewSize,MS_SYNC)) { ioError("flush filesystem image mapping"); }
#endif
        dirty=false;
    }

    void unmap() const {
        flushView();
#if defined(_WIN32)
        if (view && !UnmapViewOfFile(view)) { ioError("unmap filesystem image"); }
#else
        if (view && ::munmap(view,viewSize)) { ioError("unmap filesystem image"); }
#endif
        view=nullptr; viewSize=0;
    }
    void map(std::uint64_t offset) const {
        if (view && offset>=viewOffset && offset-viewOffset<viewSize) { return; }
        unmap(); viewOffset=offset-offset%alignment;
        // Include the alignment prefix even for windows smaller than Windows granularity.
        const auto desired=options.windowBytes+offset-viewOffset;
        const auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(desired,capacity-viewOffset));
#if defined(_WIN32)
        view=static_cast<std::uint8_t*>(MapViewOfFile(mapping,readOnly?FILE_MAP_READ:FILE_MAP_WRITE,
            static_cast<DWORD>(viewOffset>>32),static_cast<DWORD>(viewOffset),amount));
        if (!view) { ioError("map filesystem image window"); }
#else
        auto mapped=::mmap(nullptr,amount,readOnly?PROT_READ:PROT_READ|PROT_WRITE,MAP_SHARED,handle,static_cast<off_t>(viewOffset));
        if (mapped==MAP_FAILED) { ioError("map filesystem image window"); }
        view=static_cast<std::uint8_t*>(mapped);
#endif
        viewSize=amount;
    }

    void transfer(std::uint64_t offset,SByteSpan buffer,bool write) const {
        std::lock_guard lock(mutex); detail::range(capacity,offset,buffer.size());
        while (!buffer.empty()) {
            map(offset); const auto inside=static_cast<std::size_t>(offset-viewOffset);
            const auto count=std::min(buffer.size(),viewSize-inside);
            if (write) { std::memcpy(view+inside,buffer.data(),count); dirty=true; }
            else { std::memcpy(buffer.data(),view+inside,count); }
            offset+=count; buffer=buffer.subspan(count);
        }
    }
};

/* Only factories can construct a valid native-file owner. */
CFileDevice::CFileDevice(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
/* Exclusive creation never truncates an existing file. */
std::shared_ptr<CFileDevice> CFileDevice::create(const std::filesystem::path& path,std::uint64_t capacity,SMappedIOOptions options) {
    return std::shared_ptr<CFileDevice>(new CFileDevice(Impl::open(path,true,capacity,true,options)));
}
/* Existing files are immutable unless the caller explicitly requests writes. */
std::shared_ptr<CFileDevice> CFileDevice::open(const std::filesystem::path& path,bool writable,SMappedIOOptions options) {
    return std::shared_ptr<CFileDevice>(new CFileDevice(Impl::open(path,false,0,writable,options)));
}
SMappedIOOptions CFileDevice::mappingOptions() const { std::lock_guard lock(_impl->mutex); return _impl->options; }
void CFileDevice::setMappingOptions(SMappedIOOptions options) { Impl::validateMapping(options); std::lock_guard lock(_impl->mutex); _impl->unmap(); _impl->options=options; }
CFileDevice::~CFileDevice()=default;
std::uint64_t CFileDevice::size() const noexcept { return _impl->capacity; }
bool CFileDevice::isReadOnly() const noexcept { return _impl->readOnly; }
void CFileDevice::readAt(std::uint64_t offset,SByteSpan output) const { _impl->transfer(offset,output,false); }
void CFileDevice::writeAt(std::uint64_t offset,SReadOnlyByteSpan input) {
    detail::writable(isReadOnly());
    // --> The write branch copies from this borrowed span without modifying it.
    _impl->transfer(offset,{const_cast<std::uint8_t*>(input.data()),input.size()},true);
}
void CFileDevice::flush() {
    if (isReadOnly()) { return; } std::lock_guard lock(_impl->mutex);
    _impl->flushView();
#if defined(_WIN32)
    if (!FlushFileBuffers(_impl->handle)) { Impl::ioError("flush filesystem image"); }
#else
    if (::fsync(_impl->handle)) { Impl::ioError("flush filesystem image"); }
#endif
}

} // namespace hnfs
