#include <hnfs/device.hpp>
#include "internal.hpp"

namespace hnfs {

/* An interface never owns a native handle directly. */
IBlockDevice::~IBlockDevice()=default;

/* Bounded convenience copy, independent from a device's full capacity. */
SBytes IBlockDevice::readBytes(std::uint64_t offset,std::size_t count) const {
    detail::range(size(),offset,count);
    if (count>64*1024*1024) { detail::fail(EERROR_LIMIT,"device copy exceeds 64 MiB"); }
    SBytes result(count); readAt(offset,result); return result;
}

struct CMemoryDevice::Impl {
    SBytes bytes;
    bool readOnly;
    mutable std::mutex mutex;
    Impl(std::size_t size,bool immutable) : bytes(size),readOnly(immutable) {}
    Impl(SReadOnlyByteSpan input,bool immutable) : bytes(input.begin(),input.end()),readOnly(immutable) {}
};

/* Allocate zero-initialized bytes behind private state. */
CMemoryDevice::CMemoryDevice(std::size_t capacity,bool readOnly) : _impl(std::make_unique<Impl>(capacity,readOnly)) {}
/* Copy before returning; a caller's input span may be destroyed immediately. */
CMemoryDevice::CMemoryDevice(SReadOnlyByteSpan content,bool readOnly) : _impl(std::make_unique<Impl>(content,readOnly)) {}
CMemoryDevice::~CMemoryDevice()=default;
std::uint64_t CMemoryDevice::size() const noexcept { return _impl->bytes.size(); }
bool CMemoryDevice::isReadOnly() const noexcept { return _impl->readOnly; }
void CMemoryDevice::readAt(std::uint64_t offset,SByteSpan output) const {
    std::lock_guard lock(_impl->mutex); detail::range(size(),offset,output.size());
    std::copy_n(_impl->bytes.begin()+static_cast<std::size_t>(offset),output.size(),output.begin());
}
void CMemoryDevice::writeAt(std::uint64_t offset,SReadOnlyByteSpan input) {
    detail::writable(isReadOnly()); std::lock_guard lock(_impl->mutex); detail::range(size(),offset,input.size());
    std::copy(input.begin(),input.end(),_impl->bytes.begin()+static_cast<std::size_t>(offset));
}
void CMemoryDevice::flush() {}

struct CDeviceView::Impl {
    std::shared_ptr<IBlockDevice> parent;
    std::uint64_t offset,length;
    bool readOnly;
    Impl(std::shared_ptr<IBlockDevice> owner,std::uint64_t start,std::uint64_t size,bool immutable)
        : parent(std::move(owner)),offset(start),length(size),readOnly(immutable) {
        if (!parent) { throw std::invalid_argument("null parent device"); }
        detail::range(parent->size(),offset,length); readOnly=readOnly || parent->isReadOnly();
    }
};

/* Bound a range without exposing the parent or its native state. */
CDeviceView::CDeviceView(std::shared_ptr<IBlockDevice> parent,std::uint64_t offset,std::uint64_t length,bool readOnly)
    : _impl(std::make_unique<Impl>(std::move(parent),offset,length,readOnly)) {}
CDeviceView::~CDeviceView()=default;
std::uint64_t CDeviceView::size() const noexcept { return _impl->length; }
bool CDeviceView::isReadOnly() const noexcept { return _impl->readOnly; }
void CDeviceView::readAt(std::uint64_t offset,SByteSpan output) const {
    detail::range(size(),offset,output.size()); _impl->parent->readAt(_impl->offset+offset,output);
}
void CDeviceView::writeAt(std::uint64_t offset,SReadOnlyByteSpan input) {
    detail::writable(isReadOnly()); detail::range(size(),offset,input.size()); _impl->parent->writeAt(_impl->offset+offset,input);
}
void CDeviceView::flush() { _impl->parent->flush(); }

} // namespace hnfs
