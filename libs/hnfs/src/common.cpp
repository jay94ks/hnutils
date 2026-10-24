#include <hnfs/common.hpp>
#include <utility>

namespace hnfs {

/* Store a logical error code with an owned message. */
CFileSystemError::CFileSystemError(EError code,std::string message) : std::runtime_error(std::move(message)),_code(code) {}

/* Inspect without exposing mutable error state. */
EError CFileSystemError::code() const noexcept { return _code; }

/* Names are identifiers, not an availability promise. */
const char* formatName(EFormat format) noexcept {
    switch (format) {
        case EFORMAT_FAT12: return "FAT12"; case EFORMAT_FAT16: return "FAT16";
        case EFORMAT_FAT32: return "FAT32"; case EFORMAT_EXFAT: return "exFAT";
        case EFORMAT_NTFS: return "NTFS"; case EFORMAT_EXT2: return "ext2";
        case EFORMAT_EXT3: return "ext3"; case EFORMAT_EXT4: return "ext4";
        case EFORMAT_XFS: return "XFS"; case EFORMAT_BTRFS: return "Btrfs";
        case EFORMAT_SQUASHFS: return "SquashFS"; case EFORMAT_APFS: return "APFS";
        case EFORMAT_ISO9660: return "ISO 9660"; case EFORMAT_UDF: return "UDF";
        case EFORMAT_CPIO_BINARY_LE: return "CPIO binary little-endian";
        case EFORMAT_CPIO_BINARY_BE: return "CPIO binary big-endian";
        case EFORMAT_CPIO_ODC: return "CPIO odc"; case EFORMAT_CPIO_NEWC: return "CPIO newc";
        case EFORMAT_CPIO_CRC: return "CPIO crc"; default: return "unknown";
    }
}

} // namespace hnfs
