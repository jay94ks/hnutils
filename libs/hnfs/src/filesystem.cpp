#include <hnfs/filesystem.hpp>
#include <hnfs/optical.hpp>
#include "internal.hpp"

namespace hnfs {

/* Minimal common identity does not impose tree or mutation operations. */
IFileSystem::~IFileSystem()=default;
IFileReader::~IFileReader()=default;
IFileEditor::~IFileEditor()=default;
ITreeReader::~ITreeReader()=default;
ITreeEditor::~ITreeEditor()=default;
IArchiveReader::~IArchiveReader()=default;
IArchiveBuilder::~IArchiveBuilder()=default;

/* A signature hint is not a successful filesystem validation. */
SProbeResult IFileSystem::probe(const IBlockDevice& device) {
    // The recognition sequence uses physical 2048-byte sectors, independent of file LBAs.
    bool iso=false;
    for (std::uint64_t sector=16;sector<48 && device.size()>=(sector+1)*2048;++sector) {
        const auto descriptor=device.readBytes(sector*2048,7);
        if (std::equal(descriptor.begin()+1,descriptor.begin()+6,"NSR02") || std::equal(descriptor.begin()+1,descriptor.begin()+6,"NSR03")) {
            return {EFORMAT_UDF,"UDF recognition sequence; physical partition reader validates on open"};
        }
        if (std::equal(descriptor.begin()+1,descriptor.begin()+6,"CD001")) { iso=true; }
    }
    if (iso) { return {EFORMAT_ISO9660,"ISO volume descriptor signature; validation deferred to driver"}; }
    const auto header=device.readBytes(0,static_cast<std::size_t>(std::min<std::uint64_t>(512,device.size())));
    auto match=[&](std::size_t at,const char* text,std::size_t count) {
        return at<=header.size() && count<=header.size()-at && std::equal(text,text+count,header.begin()+at);
    };
    auto result=[](EFormat format) { return SProbeResult{format,"header recognition only; driver validation and availability are separate"}; };
    if (match(0,"070701",6)) { return result(EFORMAT_CPIO_NEWC); }
    if (match(0,"070702",6)) { return result(EFORMAT_CPIO_CRC); }
    if (match(0,"070707",6)) { return result(EFORMAT_CPIO_ODC); }
    if (header.size()>=2 && header[0]==0xc7 && header[1]==0x71) { return result(EFORMAT_CPIO_BINARY_LE); }
    if (header.size()>=2 && header[0]==0x71 && header[1]==0xc7) { return result(EFORMAT_CPIO_BINARY_BE); }
    if (match(0,"XFSB",4)) { return result(EFORMAT_XFS); }
    if (match(0,"hsqs",4) || match(0,"sqsh",4)) { return result(EFORMAT_SQUASHFS); }
    if (match(32,"NXSB",4)) { return result(EFORMAT_APFS); }
    if (match(3,"EXFAT   ",8)) { return result(EFORMAT_EXFAT); }
    if (match(3,"NTFS    ",8)) { return result(EFORMAT_NTFS); }
    if (device.size()>=1024+104) {
        const auto super=device.readBytes(1024,104);
        if (super[56]==0x53 && super[57]==0xef) {
            auto word=[&](std::size_t pos) { std::uint32_t value=0; for (unsigned i=0;i<4;++i) { value|=std::uint32_t(super[pos+i])<<(8*i); } return value; };
            auto value=result((word(96)&~UINT32_C(0x1f)) || (word(100)&~UINT32_C(7))?EFORMAT_EXT4:(word(92)&4)?EFORMAT_EXT3:EFORMAT_EXT2);
            value.detail="ext-family feature heuristic; legacy-feature ext4 may be indistinguishable from ext2/ext3"; return value;
        }
    }
    if (device.size()>=65536+72) {
        const auto magic=device.readBytes(65536+64,8);
        if (std::equal(magic.begin(),magic.end(),"_BHRfS_M")) { return result(EFORMAT_BTRFS); }
    }
    if (header.size()==512 && header[510]==0x55 && header[511]==0xaa) {
        auto le=[&](unsigned pos,unsigned count) { std::uint64_t value=0; for (unsigned i=0;i<count;++i) { value|=std::uint64_t(header[pos+i])<<(8*i); } return value; };
        const auto sector=le(11,2),cluster=le(13,1),reserved=le(14,2),fats=le(16,1);
        const auto total=le(19,2)?le(19,2):le(32,4),fatSize=le(22,2)?le(22,2):le(36,4);
        if (sector>=512 && sector<=4096 && (sector&(sector-1))==0 && cluster && cluster<=128 && (cluster&(cluster-1))==0 && reserved && fats>=1 && fats<=2 && fatSize && total<=device.size()/sector) {
            const auto overhead=reserved+fats*fatSize+(le(17,2)*32+sector-1)/sector;
            if (overhead<total) {
                const auto clusters=(total-overhead)/cluster;
                if (clusters) { return result(clusters<4085?EFORMAT_FAT12:clusters<65525?EFORMAT_FAT16:EFORMAT_FAT32); }
            }
        }
    }
    return {};
}

/* Never confuse a signature recognizer with an implemented driver. */
SFormatSupport IFileSystem::support(EFormat format) noexcept {
    SFormatSupport support; support.format=format;
    support.probe=format>EFORMAT_UNKNOWN && format<=EFORMAT_UDF;
    support.treeRead=format==EFORMAT_ISO9660 || format==EFORMAT_UDF;
    support.archiveRead=support.archiveCreate=support.archiveIndexRead=detail::cpio(format); return support;
}

/* Opening a sequential image does not pretend it is a mutable pathname tree. */
std::shared_ptr<IFileSystem> IFileSystem::open(std::shared_ptr<IBlockDevice> device,SOpenOptions options) {
    if (!device) { throw std::invalid_argument("null image device"); }
    const auto format=probe(*device).format;
    if (format==EFORMAT_ISO9660 || format==EFORMAT_UDF) { return COpticalFileSystem::open(std::move(device),options,format); }
    return IArchiveReader::open(std::move(device),options);
}

/* Validate availability before constructing a driver. */
std::shared_ptr<IArchiveReader> IArchiveReader::open(std::shared_ptr<IBlockDevice> device,SOpenOptions options) {
    if (!device) { throw std::invalid_argument("null image device"); } detail::validate(options);
    const auto format=IFileSystem::probe(*device).format;
    if (!detail::cpio(format)) { detail::fail(EERROR_UNSUPPORTED,"no sequential reader is implemented for this format"); }
    return detail::openCpio(std::move(device),options);
}

/* Creation must select an implemented format and a writable target. */
std::shared_ptr<IArchiveBuilder> IArchiveBuilder::create(std::shared_ptr<IBlockDevice> device,SArchiveCreateOptions options) {
    if (!device) { throw std::invalid_argument("null image device"); } detail::validate(options.limits);
    if (!detail::cpio(options.format)) { detail::fail(EERROR_UNSUPPORTED,"no sequential builder is implemented for this format"); }
    detail::writable(device->isReadOnly()); return detail::createCpio(std::move(device),options);
}

} // namespace hnfs
