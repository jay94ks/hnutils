#include <hnfs/async_archive.hpp>
#include "internal.hpp"
#include "async_internal.hpp"

namespace hnfs {
namespace {

using detail::DispatchOn;

asyncpp::task<SBytes> readDevice(std::shared_ptr<IBlockDevice> device,std::uint64_t offset,std::size_t count,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; co_return device->readBytes(offset,count);
}
asyncpp::task<void> writeDevice(std::shared_ptr<IBlockDevice> device,std::uint64_t offset,SBytes bytes,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; device->writeAt(offset,bytes);
}
asyncpp::task<void> flushDevice(std::shared_ptr<IBlockDevice> device,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; device->flush();
}
asyncpp::task<std::shared_ptr<IArchiveReader>> openArchive(std::shared_ptr<IBlockDevice> device,SOpenOptions options,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; co_return IArchiveReader::open(std::move(device),options);
}
asyncpp::task<std::optional<SEntry>> nextArchive(std::shared_ptr<IArchiveReader> reader,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; co_return reader->next();
}
asyncpp::task<SBytes> readArchive(std::shared_ptr<IArchiveReader> reader,std::size_t maximum,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker};
    if (maximum>64*1024*1024) { detail::fail(EERROR_LIMIT,"async archive chunk exceeds 64 MiB"); }
    SBytes bytes(maximum); bytes.resize(reader->read(bytes)); co_return bytes;
}
asyncpp::task<std::shared_ptr<IArchiveBuilder>> createArchive(std::shared_ptr<IBlockDevice> device,SArchiveCreateOptions options,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; co_return IArchiveBuilder::create(std::move(device),options);
}
asyncpp::task<void> beginArchive(std::shared_ptr<IArchiveBuilder> builder,SEntry entry,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; builder->beginEntry(std::move(entry));
}
asyncpp::task<void> writeArchive(std::shared_ptr<IArchiveBuilder> builder,SBytes bytes,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; builder->write(bytes);
}
asyncpp::task<void> finishArchive(std::shared_ptr<IArchiveBuilder> builder,asyncpp::dispatcher* worker) {
    co_await DispatchOn{worker}; builder->finish();
}

} // namespace

struct CDeviceAsync::Impl { std::shared_ptr<IBlockDevice> device; asyncpp::dispatcher* worker; };
CDeviceAsync::CDeviceAsync(std::shared_ptr<IBlockDevice> device,asyncpp::dispatcher& worker) : _impl(std::make_unique<Impl>(Impl{std::move(device),&worker})) {
    if (!_impl->device) { throw std::invalid_argument("null async device"); }
}
CDeviceAsync::~CDeviceAsync()=default;
asyncpp::task<SBytes> CDeviceAsync::readAt(std::uint64_t offset,std::size_t count) const { return readDevice(_impl->device,offset,count,_impl->worker); }
asyncpp::task<void> CDeviceAsync::writeAt(std::uint64_t offset,SBytes bytes) const { return writeDevice(_impl->device,offset,std::move(bytes),_impl->worker); }
asyncpp::task<void> CDeviceAsync::flush() const { return flushDevice(_impl->device,_impl->worker); }

struct CArchiveReaderAsync::Impl { std::shared_ptr<IArchiveReader> reader; asyncpp::dispatcher* worker; };
CArchiveReaderAsync::CArchiveReaderAsync(std::shared_ptr<IArchiveReader> reader,asyncpp::dispatcher& worker) : _impl(std::make_unique<Impl>(Impl{std::move(reader),&worker})) {
    if (!_impl->reader) { throw std::invalid_argument("null async archive reader"); }
}
CArchiveReaderAsync::~CArchiveReaderAsync()=default;
asyncpp::task<std::shared_ptr<IArchiveReader>> CArchiveReaderAsync::open(std::shared_ptr<IBlockDevice> device,asyncpp::dispatcher& worker,SOpenOptions options) { return openArchive(std::move(device),options,&worker); }
asyncpp::task<std::optional<SEntry>> CArchiveReaderAsync::next() const { return nextArchive(_impl->reader,_impl->worker); }
asyncpp::task<SBytes> CArchiveReaderAsync::read(std::size_t maximum) const { return readArchive(_impl->reader,maximum,_impl->worker); }

struct CArchiveBuilderAsync::Impl { std::shared_ptr<IArchiveBuilder> builder; asyncpp::dispatcher* worker; };
CArchiveBuilderAsync::CArchiveBuilderAsync(std::shared_ptr<IArchiveBuilder> builder,asyncpp::dispatcher& worker) : _impl(std::make_unique<Impl>(Impl{std::move(builder),&worker})) {
    if (!_impl->builder) { throw std::invalid_argument("null async archive builder"); }
}
CArchiveBuilderAsync::~CArchiveBuilderAsync()=default;
asyncpp::task<std::shared_ptr<IArchiveBuilder>> CArchiveBuilderAsync::create(std::shared_ptr<IBlockDevice> device,SArchiveCreateOptions options,asyncpp::dispatcher& worker) { return createArchive(std::move(device),options,&worker); }
asyncpp::task<void> CArchiveBuilderAsync::beginEntry(SEntry entry) const { return beginArchive(_impl->builder,std::move(entry),_impl->worker); }
asyncpp::task<void> CArchiveBuilderAsync::write(SBytes bytes) const { return writeArchive(_impl->builder,std::move(bytes),_impl->worker); }
asyncpp::task<void> CArchiveBuilderAsync::finish() const { return finishArchive(_impl->builder,_impl->worker); }

} // namespace hnfs
