#ifndef __SRC_HNFS_INTERNAL_HPP__
#define __SRC_HNFS_INTERNAL_HPP__

#include <hnfs/filesystem.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <utility>

namespace hnfs {
namespace detail {

[[noreturn]] inline void fail(EError code,const char* message) { throw CFileSystemError(code,message); }
inline void range(std::uint64_t capacity,std::uint64_t offset,std::uint64_t count) {
    if (offset>capacity || count>capacity-offset) { fail(EERROR_RANGE,"device range exceeds capacity"); }
}
inline void writable(bool readOnly) { if (readOnly) { fail(EERROR_READ_ONLY,"device is read-only"); } }
inline bool cpio(EFormat format) { return format>=EFORMAT_CPIO_BINARY_LE && format<=EFORMAT_CPIO_CRC; }
inline void validate(SOpenOptions options) {
    if (options.maximumPathBytes<2 || options.maximumPathBytes>1024*1024) { fail(EERROR_LIMIT,"invalid path size policy"); }
}
std::shared_ptr<IArchiveReader> openCpio(std::shared_ptr<IBlockDevice> device,SOpenOptions options);
std::shared_ptr<IArchiveBuilder> createCpio(std::shared_ptr<IBlockDevice> device,SArchiveCreateOptions options);
std::shared_ptr<ITreeReader> indexCpio(std::shared_ptr<IBlockDevice> device,SOpenOptions options);

} // namespace detail
} // namespace hnfs
#endif
