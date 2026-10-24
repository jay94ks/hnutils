#include <hnfs/virtual_disk.hpp>
#include "disk_internal.hpp"
#include "async_internal.hpp"
#include <map>
#include <set>
#include <random>
#include <cstring>

namespace hnfs {
namespace {
using namespace detail;
constexpr std::uint64_t M=1024*1024;
using Guid=std::array<std::uint8_t,16>;
const Guid BAT{0x66,0x77,0xc2,0x2d,0x23,0xf6,0x00,0x42,0x9d,0x64,0x11,0x5e,0x9b,0xfd,0x4a,0x08};
const Guid META{0x06,0xa2,0x7c,0x8b,0x90,0x47,0x9a,0x4b,0xb8,0xfe,0x57,0x5f,0x05,0x0f,0x88,0x6e};
const Guid PARAM{0x37,0x67,0xa1,0xca,0x36,0xfa,0x43,0x4d,0xb3,0xb6,0x33,0xf0,0xaa,0x44,0xe7,0x6b};
const Guid SIZE{0x24,0x42,0xa5,0x2f,0x1b,0xcd,0x76,0x48,0xb2,0x11,0x5d,0xbe,0xd8,0x3b,0xf4,0xb8};
const Guid ID{0xab,0x12,0xca,0xbe,0xe6,0xb2,0x23,0x45,0x93,0xef,0xc3,0x09,0xe0,0x00,0xc7,0x46};
const Guid LOGICAL{0x1d,0xbf,0x41,0x81,0x6f,0xa9,0x09,0x47,0xba,0x47,0xf2,0x33,0xa8,0xfa,0xab,0x5f};
const Guid PHYSICAL{0xc7,0x48,0xa3,0xcd,0x5d,0x44,0x71,0x44,0x9c,0xc9,0xe9,0x88,0x52,0x51,0xc5,0x56};
bool zero(SReadOnlyByteSpan b) { return std::all_of(b.begin(),b.end(),[](auto v) { return v==0; }); }
bool magic(SReadOnlyByteSpan b,const char* s,std::size_t size) { return b.size()>=size && std::equal(b.begin(),b.begin()+size,s); }
Guid guid(SReadOnlyByteSpan b,std::size_t pos) { range(b.size(),pos,16); Guid g{}; std::copy_n(b.begin()+pos,16,g.begin()); return g; }
void put(SBytes& b,std::size_t at,std::uint64_t value,unsigned width,bool be=false) {
    for (unsigned i=0;i<width;++i) { b.at(at+(be?width-i-1:i))=static_cast<std::uint8_t>(value>>(8*i)); }
}
std::uint64_t align(std::uint64_t value,std::uint64_t unit) { return multiply((add(value,unit-1))/unit,unit); }
std::uint32_t crc32c(SReadOnlyByteSpan b) {
    auto crc=UINT32_MAX;
    for (auto c:b) { crc^=c; for (unsigned i=0;i<8;++i) { crc=(crc>>1)^(UINT32_C(0x82f63b78)&(0-(crc&1))); } } return crc^UINT32_MAX;
}
bool validCrc(SBytes b,const char* sig,std::size_t n) {
    if (!magic(b,sig,n)) { return false; } const auto expected=little(b,4,4); put(b,4,0,4); return crc32c(b)==expected;
}
void checksum(SBytes& b,std::size_t at) {
    put(b,at,0,4,true); std::uint32_t sum=0; for (auto v:b) { sum+=v; } put(b,at,~sum,4,true);
}
void verifySum(SBytes b,std::size_t at) {
    const auto expected=big(b,at,4); checksum(b,at); if (big(b,at,4)!=expected) { fail(EERROR_CHECKSUM,"VHD ones-complement checksum mismatch"); }
}
struct Region { std::uint64_t offset,length; };
void nonoverlap(std::vector<Region> regions) {
    std::sort(regions.begin(),regions.end(),[](auto a,auto b) { return a.offset<b.offset; });
    for (std::size_t i=1;i<regions.size();++i) { if (regions[i].offset-regions[i-1].offset<regions[i-1].length) { fail(EERROR_INVALID,"overlapping virtual disk structures"); } }
}
void copyRange(const IBlockDevice& from,IBlockDevice& to,std::uint64_t source,std::uint64_t destination,std::uint64_t length) {
    SBytes bytes(static_cast<std::size_t>(std::min<std::uint64_t>(M,length)));
    while (length) { const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),length)); auto span=SByteSpan(bytes).first(n); from.readAt(source,span); to.writeAt(destination,span); source+=n; destination+=n; length-=n; }
}
} // namespace

struct CVirtualDisk::Impl {
    std::shared_ptr<IBlockDevice> image;
    SVirtualDiskOpenOptions options;
    EVirtualDiskFormat format=EVIRTUAL_VHD;
    bool dynamic=false;
    std::uint64_t capacity=0,blockSize=0,bitmapSize=0,batOffset=0,batLength=0,chunkRatio=0;
    std::uint32_t sector=512;
    std::vector<std::uint64_t> blocks;
    SBytes footer,header,regions;
    Region metadata{},log{};
    std::uint64_t parameterOffset=0;
    bool unknownMetadata=false;

    void budget(std::uint64_t count) const {
        if (!count || count>options.maximumBlocks || multiply(count,32)>options.maximumMetadataBytes) { fail(EERROR_LIMIT,"virtual disk BAT/index exceeds policy"); }
    }
    void vhd() {
        if (image->size()<512) { fail(EERROR_TRUNCATED,"short VHD footer"); }
        footer=image->readBytes(image->size()-512,512);
        if (!magic(footer,"conectix",8)) { fail(EERROR_INVALID,"missing VHD footer"); } verifySum(footer,64);
        if (big(footer,12,4)!=0x10000 || (big(footer,8,4)&~UINT64_C(3)) || footer[84]) { fail(EERROR_UNSUPPORTED,"unsupported VHD version/features or saved state"); }
        capacity=big(footer,48,8); const auto type=big(footer,60,4);
        if (!capacity || capacity%512 || capacity>UINT64_C(2040)*1024*1024*1024) { fail(EERROR_RANGE,"unsupported VHD capacity"); }
        if (type==2) { range(image->size()-512,0,capacity); if (capacity!=image->size()-512 || big(footer,16,8)!=UINT64_MAX) { fail(EERROR_INVALID,"fixed VHD layout mismatch"); } return; }
        if (type!=3) { fail(EERROR_UNSUPPORTED,"differencing VHD is not supported"); } dynamic=true;
        const auto headerAt=big(footer,16,8); if (headerAt%512) { fail(EERROR_INVALID,"unaligned VHD dynamic header"); }
        header=image->readBytes(headerAt,1024); if (!magic(header,"cxsparse",8)) { fail(EERROR_INVALID,"missing VHD dynamic header"); } verifySum(header,36);
        if (big(header,8,8)!=UINT64_MAX || big(header,24,4)!=0x10000 || !zero(SReadOnlyByteSpan(header).subspan(40,16))) { fail(EERROR_UNSUPPORTED,"unsupported VHD dynamic header or parent"); }
        const auto slots=big(header,28,4); blockSize=big(header,32,4);
        if (blockSize<512 || blockSize>256*M || (blockSize&(blockSize-1))) { fail(EERROR_INVALID,"invalid VHD block size"); }
        const auto count=(capacity+blockSize-1)/blockSize; budget(count);
        if (slots<count || slots>options.maximumBlocks || slots*4>options.maximumMetadataBytes) { fail(EERROR_LIMIT,"VHD BAT size exceeds policy"); }
        batOffset=big(header,16,8); batLength=align(slots*4,512); bitmapSize=align((blockSize/512+7)/8,512);
        if (batOffset%512) { fail(EERROR_INVALID,"unaligned VHD BAT"); } range(image->size()-512,batOffset,batLength);
        std::vector<Region> occupied{{0,512},{headerAt,1024},{batOffset,batLength},{image->size()-512,512}};
        if (image->readBytes(0,512)!=footer) { fail(EERROR_INVALID,"VHD redundant footers disagree"); }
        blocks.resize(static_cast<std::size_t>(count));
        for (std::uint64_t i=0;i<slots;++i) {
            const auto entry=big(image->readBytes(batOffset+i*4,4),0,4);
            if (entry==UINT32_MAX) { continue; }
            if (i>=count) { fail(EERROR_INVALID,"VHD allocation beyond virtual capacity"); }
            const auto offset=multiply(entry,512); range(image->size()-512,offset,bitmapSize+blockSize); blocks[static_cast<std::size_t>(i)]=offset;
            occupied.push_back({offset,bitmapSize+blockSize});
        } nonoverlap(std::move(occupied));
    }
    void vhdx() {
        format=EVIRTUAL_VHDX;
        if (image->size()<M || !magic(image->readBytes(0,8),"vhdxfile",8)) { fail(EERROR_INVALID,"missing VHDX identifier"); }
        auto h1=image->readBytes(65536,4096),h2=image->readBytes(131072,4096); const bool v1=validCrc(h1,"head",4),v2=validCrc(h2,"head",4);
        if ((!v1 && !v2) || (v1 && v2 && little(h1,8,8)==little(h2,8,8))) { fail(EERROR_CHECKSUM,"VHDX has no unique current header"); }
        header=v1 && (!v2 || little(h1,8,8)>little(h2,8,8))?std::move(h1):std::move(h2);
        if (little(header,66,2)!=1 || !zero(SReadOnlyByteSpan(header).subspan(48,16))) { fail(EERROR_UNSUPPORTED,"unsupported VHDX version or pending recovery log"); }
        log={little(header,72,8),little(header,68,4)};
        if (log.offset%M || log.length%M || log.offset<M || log.length<M || log.length>options.maximumMetadataBytes) { fail(EERROR_RANGE,"invalid VHDX log geometry"); } range(image->size(),log.offset,log.length);
        auto r1=image->readBytes(192*1024,65536),r2=image->readBytes(256*1024,65536); const bool a=validCrc(r1,"regi",4),b=validCrc(r2,"regi",4);
        if (!a && !b) { fail(EERROR_CHECKSUM,"both VHDX region tables are invalid"); }
        if (a && b && r1!=r2) { fail(EERROR_INVALID,"VHDX redundant region tables disagree"); } regions=a?std::move(r1):std::move(r2);
        const auto count=little(regions,8,4); if (!count || count>2047) { fail(EERROR_LIMIT,"invalid VHDX region count"); }
        std::vector<Region> occupied{{0,M},log}; std::set<Guid> ids;
        for (std::size_t i=0;i<count;++i) {
            const auto at=16+i*32; const auto g=guid(regions,at); Region r{little(regions,at+16,8),little(regions,at+24,4)};
            if (!ids.insert(g).second || r.offset<M || r.offset%M || !r.length || r.length%M) { fail(EERROR_INVALID,"invalid VHDX region"); }
            range(image->size(),r.offset,r.length); occupied.push_back(r);
            if (g==BAT) { batOffset=r.offset; batLength=r.length; }
            else if (g==META) { metadata=r; }
            else { fail(EERROR_UNSUPPORTED,"unknown VHDX region cannot be safely compacted"); }
        }
        if (!batLength || !metadata.length || metadata.length>options.maximumMetadataBytes || batLength>options.maximumMetadataBytes) { fail(EERROR_LIMIT,"VHDX metadata region exceeds policy or is absent"); }
        auto table=image->readBytes(metadata.offset,65536); if (!magic(table,"metadata",8)) { fail(EERROR_INVALID,"missing VHDX metadata table"); }
        const auto items=little(table,10,2); if (items>2047) { fail(EERROR_LIMIT,"too many VHDX metadata entries"); }
        std::map<Guid,SBytes> fields; std::set<std::pair<Guid,bool>> itemIds; std::vector<Region> ranges{{0,65536}};
        for (std::size_t i=0;i<items;++i) {
            const auto at=32+i*32; const auto g=guid(table,at); const auto offset=little(table,at+16,4),length=little(table,at+20,4),flags=little(table,at+24,4);
            if (flags&~UINT64_C(7) || !itemIds.emplace(g,(flags&1)!=0).second || !length || offset<65536 || length>M || little(table,at+28,4)) { fail(EERROR_INVALID,"invalid VHDX metadata entry"); }
            range(metadata.length,offset,length); ranges.push_back({offset,length});
            const bool known=g==PARAM || g==SIZE || g==ID || g==LOGICAL || g==PHYSICAL;
            if (known && flags!=(g==PARAM?4u:6u)) { fail(EERROR_INVALID,"invalid VHDX system metadata flags"); }
            if (!known && (flags&4)) { fail(EERROR_UNSUPPORTED,"unknown required VHDX metadata"); }
            if (known) { fields.emplace(g,image->readBytes(metadata.offset+offset,static_cast<std::size_t>(length))); }
            else { unknownMetadata=true; }
            if (g==PARAM) { parameterOffset=metadata.offset+offset; }
        } nonoverlap(std::move(ranges));
        for (const auto& item:{std::pair<Guid,std::size_t>{PARAM,8},{SIZE,8},{ID,16},{LOGICAL,4},{PHYSICAL,4}}) { if (!fields.contains(item.first) || fields.at(item.first).size()!=item.second) { fail(EERROR_INVALID,"missing VHDX mandatory metadata"); } }
        capacity=little(fields[SIZE],0,8); sector=static_cast<std::uint32_t>(little(fields[LOGICAL],0,4));
        const auto physical=little(fields[PHYSICAL],0,4); blockSize=little(fields[PARAM],0,4); const auto flags=little(fields[PARAM],4,4);
        if (flags&~UINT64_C(1)) { fail(EERROR_UNSUPPORTED,"parent/differencing VHDX unsupported"); } dynamic=!(flags&1);
        if ((sector!=512 && sector!=4096) || (physical!=512 && physical!=4096) || !capacity || capacity%sector || capacity>UINT64_C(64)*1024*1024*1024*1024 || blockSize<M || blockSize>256*M || (blockSize&(blockSize-1))) { fail(EERROR_RANGE,"invalid VHDX disk geometry"); }
        chunkRatio=multiply(UINT64_C(1)<<23,sector)/blockSize; const auto payload=(capacity+blockSize-1)/blockSize; budget(payload);
        const auto entries=payload+(payload-1)/chunkRatio; if (entries>batLength/8) { fail(EERROR_TRUNCATED,"short VHDX BAT region"); } blocks.resize(static_cast<std::size_t>(payload));
        for (std::uint64_t i=0;i<entries;++i) {
            const auto raw=little(image->readBytes(batOffset+i*8,8),0,8),state=raw&7,offset=raw&~(M-1);
            if (raw&UINT64_C(0xffff8)) { fail(EERROR_INVALID,"VHDX BAT reserved bits set"); }
            if (i%(chunkRatio+1)==chunkRatio) { if (state) { fail(EERROR_UNSUPPORTED,"standalone VHDX sector bitmap must not be allocated"); } continue; }
            const auto index=i-i/(chunkRatio+1);
            if (state==6) { if (!offset) { fail(EERROR_INVALID,"zero VHDX allocated block offset"); } range(image->size(),offset,blockSize); occupied.push_back({offset,blockSize}); blocks[static_cast<std::size_t>(index)]=offset; }
            else if (state>3) { fail(EERROR_UNSUPPORTED,"invalid or partially present VHDX block"); }
        } nonoverlap(std::move(occupied));
    }
};
CVirtualDisk::CVirtualDisk(std::unique_ptr<Impl> p) : _impl(std::move(p)) {}
CVirtualDisk::~CVirtualDisk()=default;
std::shared_ptr<CVirtualDisk> CVirtualDisk::open(std::shared_ptr<IBlockDevice> image,SVirtualDiskOpenOptions o) {
    if (!image) { throw std::invalid_argument("null virtual disk image"); }
    if (!o.maximumBlocks || o.maximumBlocks>16*1024*1024 || o.maximumMetadataBytes>1024*M) { fail(EERROR_LIMIT,"invalid virtual disk parsing policy"); }
    auto p=std::make_unique<Impl>(); p->image=std::move(image); p->options=o;
    if (p->image->size()>=8 && magic(p->image->readBytes(0,8),"vhdxfile",8)) { p->vhdx(); } else { p->vhd(); }
    return std::shared_ptr<CVirtualDisk>(new CVirtualDisk(std::move(p)));
}
std::shared_ptr<CVirtualDisk> CVirtualDisk::openFile(const std::filesystem::path& path,SVirtualDiskOpenOptions o) { return open(CFileDevice::open(path,false,o.mapping),o); }
EVirtualDiskFormat CVirtualDisk::virtualFormat() const noexcept { return _impl->format; }
bool CVirtualDisk::isDynamic() const noexcept { return _impl->dynamic; }
std::uint32_t CVirtualDisk::logicalSectorSize() const noexcept { return _impl->sector; }
std::uint64_t CVirtualDisk::size() const noexcept { return _impl->capacity; }
bool CVirtualDisk::isReadOnly() const noexcept { return true; }
void CVirtualDisk::writeAt(std::uint64_t,SReadOnlyByteSpan) { fail(EERROR_READ_ONLY,"virtual disk view is read-only; publish a separate compacted image"); }
void CVirtualDisk::flush() {}
void CVirtualDisk::readAt(std::uint64_t offset,SByteSpan out) const {
    range(size(),offset,out.size()); const auto& s=*_impl;
    if (s.format==EVIRTUAL_VHD && !s.dynamic) { s.image->readAt(offset,out); return; }
    while (!out.empty()) {
        const auto index=offset/s.blockSize,within=offset%s.blockSize,location=s.blocks[static_cast<std::size_t>(index)];
        auto count=static_cast<std::size_t>(std::min<std::uint64_t>(out.size(),s.blockSize-within));
        if (!location) { std::fill_n(out.begin(),count,0); }
        else if (s.format==EVIRTUAL_VHDX) { s.image->readAt(location+within,out.first(count)); }
        else {
            count=std::min(count,static_cast<std::size_t>(512-within%512)); const auto sector=within/512;
            const auto bitmap=s.image->readBytes(location+sector/8,1)[0];
            if (bitmap&(0x80>>(sector%8))) { s.image->readAt(location+s.bitmapSize+within,out.first(count)); }
            else { std::fill_n(out.begin(),count,0); }
        } offset+=count; out=out.subspan(count);
    }
}
SMappedIOOptions CVirtualDisk::mappingOptions() const {
    auto mapped=std::dynamic_pointer_cast<CFileDevice>(_impl->image); if (!mapped) { fail(EERROR_UNSUPPORTED,"virtual disk backend is not a mapped file"); } return mapped->mappingOptions();
}
void CVirtualDisk::setMappingOptions(SMappedIOOptions o) {
    auto mapped=std::dynamic_pointer_cast<CFileDevice>(_impl->image); if (!mapped) { fail(EERROR_UNSUPPORTED,"virtual disk backend is not a mapped file"); } mapped->setMappingOptions(o);
}
SCompactResult CVirtualDisk::compactToFile(const std::filesystem::path& path,SCompactOptions o) const {
    const auto& s=*_impl;
    if (!s.dynamic) { fail(EERROR_UNSUPPORTED,"compaction requires a dynamic image"); }
    if (s.unknownMetadata) { fail(EERROR_UNSUPPORTED,"cannot compact unknown optional VHDX metadata safely"); }
    if (o.mapping.windowBytes<4096 || o.mapping.windowBytes>64*M) { fail(EERROR_LIMIT,"invalid output mapping policy"); }
    std::vector<std::uint8_t> keep(s.blocks.size()); SBytes buffer(M); SCompactResult result; result.virtualBytes=size(); result.previousImageBytes=s.image->size();
    for (std::size_t i=0;i<keep.size();++i) {
        if (!s.blocks[i]) { continue; } keep[i]=1;
        if (o.scanZeroBlocks) {
            keep[i]=0; auto at=multiply(i,s.blockSize); const auto end=std::min(size(),add(at,s.blockSize));
            while (at<end) { auto span=SByteSpan(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(M,end-at))); readAt(at,span); if (!zero(span)) { keep[i]=1; break; } at+=span.size(); }
        }
        if (keep[i]) { ++result.retainedBlocks; } else { ++result.removedBlocks; }
    }
    std::uint64_t first=0,stride=0;
    SBytes bat;
    if (s.format==EVIRTUAL_VHD) {
        bat.assign(static_cast<std::size_t>(align(keep.size()*4,512)),0xff); first=1536+bat.size(); stride=s.bitmapSize+s.blockSize;
        result.imageBytes=add(add(first,multiply(result.retainedBlocks,stride)),512);
        if (result.imageBytes/512>UINT32_MAX) { fail(EERROR_RANGE,"compacted VHD physical offsets exceed BAT"); }
    } else {
        first=align(std::max({M,s.log.offset+s.log.length,s.metadata.offset+s.metadata.length,s.batOffset+s.batLength}),M); stride=s.blockSize;
        result.imageBytes=add(first,multiply(result.retainedBlocks,stride));
    }
    auto output=CFileDevice::create(path,result.imageBytes,o.mapping); // CREATE_NEW: never destroys a source/existing destination.
    std::uint64_t physical=first;
    if (s.format==EVIRTUAL_VHD) {
        auto h=s.header; put(h,16,1536,8,true); put(h,28,keep.size(),4,true); checksum(h,36);
        auto f=s.footer; put(f,16,512,8,true); checksum(f,64); output->writeAt(0,f); output->writeAt(512,h);
        for (std::size_t i=0;i<keep.size();++i) { if (keep[i]) {
            put(bat,i*4,physical/512,4,true); const auto logical=multiply(i,s.blockSize),length=std::min(s.blockSize,size()-logical);
            SBytes bitmap(static_cast<std::size_t>(s.bitmapSize)); const auto sectors=(length+511)/512;
            for (std::uint64_t bit=0;bit<sectors;++bit) { bitmap[static_cast<std::size_t>(bit/8)]|=static_cast<std::uint8_t>(0x80>>(bit%8)); }
            output->writeAt(physical,bitmap); copyRange(*this,*output,logical,physical+s.bitmapSize,length); physical+=stride;
        } }
        output->writeAt(1536,bat); output->writeAt(result.imageBytes-512,f);
    } else {
        copyRange(*s.image,*output,0,0,M); copyRange(*s.image,*output,s.metadata.offset,s.metadata.offset,s.metadata.length);
        // Preserve virtual disk identity and data-write GUID, generate a new file-write identity.
        auto h=s.header; std::random_device random; for (std::size_t i=16;i<32;++i) { h[i]=static_cast<std::uint8_t>(random()); }
        for (std::uint64_t copy=0;copy<2;++copy) { put(h,8,copy+1,8); put(h,4,0,4); put(h,4,crc32c(h),4); output->writeAt((copy+1)*65536,h); }
        auto params=s.image->readBytes(s.parameterOffset,8); put(params,4,0,4); output->writeAt(s.parameterOffset,params);
        for (std::size_t i=0;i<keep.size();++i) {
            const auto batIndex=i+i/s.chunkRatio; SBytes entry(8); put(entry,0,keep[i]?physical|6:2,8); output->writeAt(s.batOffset+batIndex*8,entry);
            if (keep[i]) { const auto logical=multiply(i,s.blockSize); copyRange(*this,*output,logical,physical,std::min(s.blockSize,size()-logical)); physical+=stride; }
        }
    }
    output->flush(); (void)CVirtualDisk::open(output,s.options); return result;
}
namespace {
asyncpp::task<std::shared_ptr<CVirtualDisk>> openOn(std::filesystem::path p,SVirtualDiskOpenOptions o,asyncpp::dispatcher* w) { co_await DispatchOn{w}; co_return CVirtualDisk::openFile(p,o); }
asyncpp::task<SCompactResult> compactOn(std::shared_ptr<CVirtualDisk> d,std::filesystem::path p,SCompactOptions o,asyncpp::dispatcher* w) { co_await DispatchOn{w}; co_return d->compactToFile(p,o); }
}
asyncpp::task<std::shared_ptr<CVirtualDisk>> CVirtualDisk::openFileAsync(std::filesystem::path p,asyncpp::dispatcher& w,SVirtualDiskOpenOptions o) { return openOn(std::move(p),o,&w); }
asyncpp::task<SCompactResult> CVirtualDisk::compactToFileAsync(std::filesystem::path p,asyncpp::dispatcher& w,SCompactOptions o) { return compactOn(shared_from_this(),std::move(p),o,&w); }
} // namespace hnfs
