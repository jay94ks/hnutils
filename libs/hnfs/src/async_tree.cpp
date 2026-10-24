#include <hnfs/async_tree.hpp>
#include "internal.hpp"
#include "async_internal.hpp"

namespace hnfs {
namespace {
asyncpp::task<std::shared_ptr<ITreeReader>> openTreeOn(std::shared_ptr<IBlockDevice> device,SOpenOptions options,asyncpp::dispatcher* worker) {
    co_await detail::DispatchOn{worker};
    auto tree=std::dynamic_pointer_cast<ITreeReader>(IFileSystem::open(std::move(device),options));
    if (!tree) { detail::fail(EERROR_UNSUPPORTED,"format has no native tree; request an archive index explicitly"); } co_return tree;
}

asyncpp::task<std::shared_ptr<CIndexedArchive>> indexOn(std::shared_ptr<IBlockDevice> device,SOpenOptions options,asyncpp::dispatcher* worker) {
    co_await detail::DispatchOn{worker}; co_return CIndexedArchive::open(std::move(device),options);
}
asyncpp::task<SEntry> statOn(std::shared_ptr<ITreeReader> tree,std::string path,asyncpp::dispatcher* worker) {
    co_await detail::DispatchOn{worker}; co_return tree->stat(path);
}
asyncpp::task<std::vector<SEntry>> listOn(std::shared_ptr<ITreeReader> tree,std::string path,std::uint64_t offset,std::size_t count,asyncpp::dispatcher* worker) {
    co_await detail::DispatchOn{worker}; co_return tree->list(path,offset,count);
}
asyncpp::task<SBytes> readOn(std::shared_ptr<ITreeReader> tree,std::string path,std::uint64_t offset,std::size_t count,asyncpp::dispatcher* worker) {
    co_await detail::DispatchOn{worker};
    if (count>64*1024*1024) { detail::fail(EERROR_LIMIT,"async tree read exceeds 64 MiB"); }
    auto file=tree->openFile(path);
    const auto length=file->size();
    const auto amount=static_cast<std::size_t>(offset<length?std::min<std::uint64_t>(count,length-offset):0);
    SBytes bytes(amount); bytes.resize(file->readAt(offset,bytes)); co_return bytes;
}

} // namespace

struct CTreeReaderAsync::Impl { std::shared_ptr<ITreeReader> tree; asyncpp::dispatcher* worker; };
CTreeReaderAsync::CTreeReaderAsync(std::shared_ptr<ITreeReader> tree,asyncpp::dispatcher& worker) : _impl(std::make_unique<Impl>(Impl{std::move(tree),&worker})) {
    if (!_impl->tree) { throw std::invalid_argument("null async tree"); }
}
CTreeReaderAsync::~CTreeReaderAsync()=default;
asyncpp::task<std::shared_ptr<ITreeReader>> CTreeReaderAsync::open(std::shared_ptr<IBlockDevice> d,asyncpp::dispatcher& w,SOpenOptions o) { return openTreeOn(std::move(d),o,&w); }
asyncpp::task<std::shared_ptr<CIndexedArchive>> CTreeReaderAsync::indexArchive(std::shared_ptr<IBlockDevice> device,asyncpp::dispatcher& worker,SOpenOptions options) { return indexOn(std::move(device),options,&worker); }
asyncpp::task<SEntry> CTreeReaderAsync::stat(std::string path) const { return statOn(_impl->tree,std::move(path),_impl->worker); }
asyncpp::task<std::vector<SEntry>> CTreeReaderAsync::list(std::string path,std::uint64_t offset,std::size_t count) const { return listOn(_impl->tree,std::move(path),offset,count,_impl->worker); }
asyncpp::task<SBytes> CTreeReaderAsync::readFile(std::string path,std::uint64_t offset,std::size_t count) const { return readOn(_impl->tree,std::move(path),offset,count,_impl->worker); }

} // namespace hnfs
