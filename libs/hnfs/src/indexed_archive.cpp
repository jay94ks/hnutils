#include <hnfs/indexed_archive.hpp>
#include "internal.hpp"

namespace hnfs {

struct CIndexedArchive::Impl { std::shared_ptr<ITreeReader> tree; };

/* Retain a private validated index implementation, not its data structures. */
CIndexedArchive::CIndexedArchive(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
/* Explicit opt-in: full scan and index memory are not imposed on archive cursors. */
std::shared_ptr<CIndexedArchive> CIndexedArchive::open(std::shared_ptr<IBlockDevice> device,SOpenOptions options) {
    if (!device) { throw std::invalid_argument("null archive device"); } detail::validate(options);
    if (!detail::cpio(IFileSystem::probe(*device).format)) { detail::fail(EERROR_UNSUPPORTED,"no indexed archive adapter for this format"); }
    auto impl=std::make_unique<Impl>(); impl->tree=detail::indexCpio(std::move(device),options);
    return std::shared_ptr<CIndexedArchive>(new CIndexedArchive(std::move(impl)));
}
CIndexedArchive::~CIndexedArchive()=default;
EFormat CIndexedArchive::format() const noexcept { return _impl->tree->format(); }
SEntry CIndexedArchive::stat(const std::string& path) const { return _impl->tree->stat(path); }
std::vector<SEntry> CIndexedArchive::list(const std::string& path,std::uint64_t offset,std::size_t count) const { return _impl->tree->list(path,offset,count); }
std::shared_ptr<IFileReader> CIndexedArchive::openFile(const std::string& path) const { return _impl->tree->openFile(path); }

} // namespace hnfs
