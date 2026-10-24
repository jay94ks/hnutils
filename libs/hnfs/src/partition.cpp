#include <hnfs/partition.hpp>
#include "disk_internal.hpp"
#include "async_internal.hpp"
#include <set>

namespace hnfs {
namespace {
using namespace detail;
bool extended(std::uint8_t type) { return type==5 || type==15 || type==0x85; }
bool zero(SReadOnlyByteSpan b) { return std::all_of(b.begin(),b.end(),[](auto c) { return c==0; }); }
void signature(SReadOnlyByteSpan b) {
    if (b.size()<512 || b[510]!=0x55 || b[511]!=0xaa) { fail(EERROR_INVALID,"missing MBR/EBR signature"); }
}
void overlaps(const std::vector<SPartition>& entries,bool ignoreContainers) {
    std::vector<const SPartition*> sorted;
    for (const auto& p:entries) { if (!ignoreContainers || !p.extendedContainer) { sorted.push_back(&p); } }
    std::sort(sorted.begin(),sorted.end(),[](auto a,auto b) { return a->firstLba<b->firstLba; });
    for (std::size_t i=1;i<sorted.size();++i) {
        if (sorted[i]->firstLba-sorted[i-1]->firstLba<sorted[i-1]->sectorCount) { fail(EERROR_INVALID,"overlapping partitions"); }
    }
}
SPartition mbrEntry(SReadOnlyByteSpan b,std::size_t at,std::uint32_t index,std::uint64_t base,
    std::uint64_t total,std::uint32_t sector) {
    SPartition p; p.index=index; p.mbrType=b[at+4];
    const auto length=little(b,at+12,4),relative=little(b,at+8,4);
    if (!p.mbrType || !length) { return {}; }
    if (b[at]!=0 && b[at]!=0x80) { fail(EERROR_INVALID,"invalid MBR boot indicator"); }
    p.bootable=b[at]==0x80; p.firstLba=add(base,relative); p.sectorCount=length;
    if (!p.firstLba || p.firstLba>=total || length>total-p.firstLba) { fail(EERROR_RANGE,"MBR partition outside device"); }
    p.byteOffset=multiply(p.firstLba,sector); p.byteSize=multiply(length,sector); p.extendedContainer=extended(p.mbrType); return p;
}
struct Gpt { SBytes header; std::vector<SPartition> entries; std::uint64_t arrayLba=0,arrayBytes=0; };
Gpt readGpt(const IBlockDevice& d,const SPartitionOpenOptions& o,std::uint64_t lba) {
    const auto total=d.size()/o.logicalSectorSize; Gpt g;
    g.header=d.readBytes(multiply(lba,o.logicalSectorSize),o.logicalSectorSize); auto& h=g.header;
    if (!std::equal(h.begin(),h.begin()+8,"EFI PART")) { fail(EERROR_INVALID,"missing GPT header"); }
    const auto size=little(h,12,4);
    if (little(h,8,4)!=0x10000 || size<92 || size>h.size() || little(h,20,4)) { fail(EERROR_UNSUPPORTED,"unsupported GPT header revision or size"); }
    const auto expected=little(h,16,4); std::fill(h.begin()+16,h.begin()+20,0);
    if ((crc32(SReadOnlyByteSpan(h).first(static_cast<std::size_t>(size)))^UINT32_MAX)!=expected) { fail(EERROR_CHECKSUM,"GPT header CRC32 mismatch"); }
    if (little(h,24,8)!=lba || little(h,32,8)!=(lba==1?total-1:1)) { fail(EERROR_INVALID,"GPT reciprocal header location mismatch"); }
    const auto first=little(h,40,8),last=little(h,48,8),slots=little(h,80,4),width=little(h,84,4);
    if (first<2 || first>last || last>=total-1 || zero(SReadOnlyByteSpan(h).subspan(56,16))) { fail(EERROR_RANGE,"invalid GPT usable geometry or disk GUID"); }
    if (!slots || slots>o.maximumEntries || width<128 || (width&(width-1)) || width>65536) { fail(EERROR_LIMIT,"GPT entry count or size exceeds policy"); }
    g.arrayLba=little(h,72,8); g.arrayBytes=multiply(slots,width);
    if (g.arrayBytes>o.maximumEntryArrayBytes || g.arrayBytes<16384) { fail(EERROR_LIMIT,"GPT array size violates policy or minimum size"); }
    const auto blocks=(g.arrayBytes+o.logicalSectorSize-1)/o.logicalSectorSize;
    if (g.arrayLba<2 || g.arrayLba>=total || blocks>total-g.arrayLba ||
        (lba==1?(g.arrayLba>=first || blocks>first-g.arrayLba):(g.arrayLba<=last || g.arrayLba>=lba || blocks>lba-g.arrayLba))) {
        fail(EERROR_RANGE,"GPT entry array overlaps usable space or headers");
    }
    auto crc=UINT32_MAX; std::set<std::array<std::uint8_t,16>> identities;
    for (std::uint32_t i=0;i<slots;++i) {
        auto b=d.readBytes(add(multiply(g.arrayLba,o.logicalSectorSize),multiply(i,width)),static_cast<std::size_t>(width)); crc=crc32(b,crc);
        if (zero(SReadOnlyByteSpan(b).first(16))) { continue; }
        SPartition p; p.index=i+1; std::copy_n(b.begin(),16,p.typeGuid.begin()); std::copy_n(b.begin()+16,16,p.uniqueGuid.begin());
        if (zero(p.uniqueGuid) || !identities.insert(p.uniqueGuid).second) { fail(EERROR_INVALID,"zero or duplicate GPT partition GUID"); }
        p.firstLba=little(b,32,8); const auto end=little(b,40,8);
        if (p.firstLba<first || end<p.firstLba || end>last) { fail(EERROR_RANGE,"GPT partition outside usable LBAs"); }
        p.sectorCount=end-p.firstLba+1; p.byteOffset=multiply(p.firstLba,o.logicalSectorSize); p.byteSize=multiply(p.sectorCount,o.logicalSectorSize);
        p.attributes=little(b,48,8); p.name=utf16(SReadOnlyByteSpan(b).subspan(56,72),false,true); g.entries.push_back(std::move(p));
    }
    if ((crc^UINT32_MAX)!=little(h,88,4)) { fail(EERROR_CHECKSUM,"GPT entry array CRC32 mismatch"); }
    overlaps(g.entries,false); return g;
}
asyncpp::task<std::shared_ptr<CPartitionTable>> openOn(std::shared_ptr<IBlockDevice> d,SPartitionOpenOptions o,asyncpp::dispatcher* w) {
    co_await DispatchOn{w}; co_return CPartitionTable::open(std::move(d),o);
}
} // namespace

struct CPartitionTable::Impl {
    std::shared_ptr<IBlockDevice> device;
    SPartitionOpenOptions options;
    EPartitionScheme scheme=EPARTITION_MBR;
    std::vector<SPartition> entries;
};
CPartitionTable::CPartitionTable(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
CPartitionTable::~CPartitionTable()=default;
std::shared_ptr<CPartitionTable> CPartitionTable::open(std::shared_ptr<IBlockDevice> device,SPartitionOpenOptions o) {
    if (!device) { throw std::invalid_argument("null partition device"); }
    if (o.logicalSectorSize<512 || o.logicalSectorSize>4096 || (o.logicalSectorSize&(o.logicalSectorSize-1)) ||
        !o.maximumEntries || o.maximumEntries>1048576 || o.maximumEbrRecords>1048576) { fail(EERROR_LIMIT,"invalid partition geometry or limits"); }
    if (device->size()%o.logicalSectorSize || device->size()/o.logicalSectorSize<2) { fail(EERROR_RANGE,"device is not an integral set of sectors"); }
    auto impl=std::make_unique<Impl>(); impl->device=device; impl->options=o;
    auto b=device->readBytes(0,512); signature(b); const auto total=device->size()/o.logicalSectorSize;
    std::optional<SPartition> container; bool protective=false;
    for (std::uint32_t i=0;i<4;++i) {
        auto p=mbrEntry(b,446+i*16,i+1,0,total,o.logicalSectorSize); if (!p.index) { continue; }
        if (p.mbrType==0xee) { if (protective) { fail(EERROR_INVALID,"multiple protective MBR entries"); } protective=true; }
        if (p.extendedContainer) { if (container) { fail(EERROR_UNSUPPORTED,"multiple extended MBR containers"); } container=p; }
        impl->entries.push_back(p);
    }
    if (protective) {
        if (impl->entries.size()!=1 || impl->entries[0].firstLba!=1 || impl->entries[0].sectorCount!=std::min<std::uint64_t>(total-1,UINT32_MAX)) { fail(EERROR_UNSUPPORTED,"hybrid or malformed protective MBR"); }
        impl->scheme=EPARTITION_GPT;
        auto g=readGpt(*device,o,o.useBackupGpt?total-1:1);
        if (!o.useBackupGpt) {
            const auto backup=readGpt(*device,o,total-1);
            for (auto pair:{std::pair<std::size_t,std::size_t>{40,32},{80,12}}) {
                if (!std::equal(g.header.begin()+pair.first,g.header.begin()+pair.first+pair.second,backup.header.begin()+pair.first)) { fail(EERROR_INVALID,"primary and backup GPT disagree"); }
            }
            // A matching CRC alone is not sufficient to assert byte-for-byte redundancy.
            for (std::uint64_t at=0;at<g.arrayBytes;) {
                const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(65536,g.arrayBytes-at));
                if (device->readBytes(add(multiply(g.arrayLba,o.logicalSectorSize),at),n)!=device->readBytes(add(multiply(backup.arrayLba,o.logicalSectorSize),at),n)) { fail(EERROR_INVALID,"GPT redundant arrays differ"); } at+=n;
            }
        }
        impl->entries=std::move(g.entries);
    } else {
        if (o.useBackupGpt) { fail(EERROR_INVALID,"backup GPT requested for an MBR disk"); }
        overlaps(impl->entries,false);
        if (container) {
            std::set<std::uint64_t> visited; auto current=container->firstLba; std::uint32_t index=5;
            for (;;) {
                if (visited.size()>=o.maximumEbrRecords || !visited.insert(current).second) { fail(EERROR_LIMIT,"EBR chain cycle or limit"); }
                if (current<container->firstLba || current-container->firstLba>=container->sectorCount) { fail(EERROR_RANGE,"EBR outside extended container"); }
                auto e=device->readBytes(multiply(current,o.logicalSectorSize),512); signature(e);
                for (std::size_t at:{478u,494u}) { if (!zero(SReadOnlyByteSpan(e).subspan(at,16))) { fail(EERROR_UNSUPPORTED,"extra EBR partition records"); } }
                auto data=mbrEntry(e,446,index++,current,total,o.logicalSectorSize);
                if (!data.index || data.extendedContainer || data.mbrType==0xee || data.firstLba<=current ||
                    data.firstLba-container->firstLba>=container->sectorCount || data.sectorCount>container->sectorCount-(data.firstLba-container->firstLba)) { fail(EERROR_RANGE,"invalid logical partition"); }
                data.logical=true; data.ebrLba=current; impl->entries.push_back(data);
                if (impl->entries.size()>o.maximumEntries) { fail(EERROR_LIMIT,"too many MBR partitions"); }
                auto link=mbrEntry(e,462,1,container->firstLba,total,o.logicalSectorSize);
                if (!link.index) { break; }
                if (!link.extendedContainer || link.firstLba<container->firstLba || link.firstLba-container->firstLba>=container->sectorCount ||
                    link.sectorCount>container->sectorCount-(link.firstLba-container->firstLba)) { fail(EERROR_RANGE,"invalid EBR link"); } current=link.firstLba;
            }
            overlaps(impl->entries,true);
            for (auto lba:visited) { for (const auto& p:impl->entries) { if (!p.extendedContainer && lba>=p.firstLba && lba-p.firstLba<p.sectorCount) { fail(EERROR_INVALID,"EBR overlaps partition data"); } } }
        }
        if (impl->entries.size()>o.maximumEntries) { fail(EERROR_LIMIT,"too many MBR partitions"); }
    }
    return std::shared_ptr<CPartitionTable>(new CPartitionTable(std::move(impl)));
}
asyncpp::task<std::shared_ptr<CPartitionTable>> CPartitionTable::openAsync(std::shared_ptr<IBlockDevice> d,asyncpp::dispatcher& w,SPartitionOpenOptions o) { return openOn(std::move(d),o,&w); }
EPartitionScheme CPartitionTable::scheme() const noexcept { return _impl->scheme; }
bool CPartitionTable::usesBackupGpt() const noexcept { return _impl->scheme==EPARTITION_GPT && _impl->options.useBackupGpt; }
std::uint32_t CPartitionTable::logicalSectorSize() const noexcept { return _impl->options.logicalSectorSize; }
std::vector<SPartition> CPartitionTable::list(std::uint64_t offset,std::size_t count) const {
    if (count>4096) { fail(EERROR_LIMIT,"partition page exceeds 4096 entries"); }
    if (offset>=_impl->entries.size()) { return {}; }
    auto first=_impl->entries.begin()+static_cast<std::size_t>(offset); return {first,first+std::min(count,static_cast<std::size_t>(_impl->entries.end()-first))};
}
std::shared_ptr<CDeviceView> CPartitionTable::openPartition(std::uint32_t index,bool allowWrites) const {
    for (const auto& p:_impl->entries) { if (p.index==index) {
        if (p.extendedContainer) { fail(EERROR_UNSUPPORTED,"extended partition is a container, not a filesystem"); }
        if (allowWrites) { writable(_impl->device->isReadOnly()); }
        return std::make_shared<CDeviceView>(_impl->device,p.byteOffset,p.byteSize,!allowWrites);
    } } fail(EERROR_NOT_FOUND,"partition slot not found");
}

namespace {
void put(SBytes& b,std::size_t p,std::uint64_t value,unsigned width) {
    for (unsigned i=0;i<width;++i) { b.at(p+i)=static_cast<std::uint8_t>(value>>(i*8)); }
}
struct Edit { std::uint64_t offset; SBytes bytes; };
class ProposedDisk final : public IBlockDevice {
    std::shared_ptr<IBlockDevice> _base;
    const std::vector<Edit>& _edits;
public:
    ProposedDisk(std::shared_ptr<IBlockDevice> base,const std::vector<Edit>& edits) : _base(std::move(base)),_edits(edits) {}
    std::uint64_t size() const noexcept override { return _base->size(); }
    bool isReadOnly() const noexcept override { return true; }
    void readAt(std::uint64_t offset,SByteSpan out) const override {
        _base->readAt(offset,out);
        for (const auto& e:_edits) {
            const auto first=std::max(offset,e.offset),last=std::min(add(offset,out.size()),add(e.offset,e.bytes.size()));
            if (first<last) { std::copy_n(e.bytes.begin()+static_cast<std::size_t>(first-e.offset),static_cast<std::size_t>(last-first),out.begin()+static_cast<std::size_t>(first-offset)); }
        }
    }
    void writeAt(std::uint64_t,SReadOnlyByteSpan) override { fail(EERROR_READ_ONLY,"proposed partition disk is immutable"); }
    void flush() override {}
};
void record(SBytes& b,std::size_t at,const SPartition& p,std::uint64_t base) {
    if (!p.mbrType || p.firstLba<base || p.firstLba-base>UINT32_MAX || !p.sectorCount || p.sectorCount>UINT32_MAX) { fail(EERROR_RANGE,"MBR record is not representable"); }
    b[at]=p.bootable?0x80:0; b[at+1]=b[at+5]=0xfe; b[at+2]=b[at+6]=0xff; b[at+3]=b[at+7]=0xff; b[at+4]=p.mbrType;
    put(b,at+8,p.firstLba-base,4); put(b,at+12,p.sectorCount,4);
}
std::vector<std::uint16_t> encodeName(const std::string& name) {
    std::vector<std::uint16_t> result;
    for (std::size_t i=0;i<name.size();) {
        const auto lead=static_cast<std::uint8_t>(name[i++]); unsigned extra=0; std::uint32_t c=lead,min=0;
        if (lead>=0xc2 && lead<=0xdf) { extra=1; c=lead&31; min=0x80; }
        else if (lead>=0xe0 && lead<=0xef) { extra=2; c=lead&15; min=0x800; }
        else if (lead>=0xf0 && lead<=0xf4) { extra=3; c=lead&7; min=0x10000; }
        else if (lead>=0x80) { fail(EERROR_INVALID,"invalid UTF-8 GPT name"); }
        for (unsigned n=0;n<extra;++n) { if (i==name.size() || (static_cast<std::uint8_t>(name[i])&0xc0)!=0x80) { fail(EERROR_INVALID,"truncated UTF-8 GPT name"); } c=(c<<6)|(static_cast<std::uint8_t>(name[i++])&63); }
        if (!c || c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) { fail(EERROR_INVALID,"invalid UTF-8 GPT name"); }
        if (c<0x10000) { result.push_back(static_cast<std::uint16_t>(c)); }
        else { c-=0x10000; result.push_back(static_cast<std::uint16_t>(0xd800+(c>>10))); result.push_back(static_cast<std::uint16_t>(0xdc00+(c&1023))); }
    }
    if (result.size()>36) { fail(EERROR_LIMIT,"GPT name exceeds 36 UTF-16 code units"); } return result;
}
asyncpp::task<void> writeOn(std::shared_ptr<IBlockDevice> d,SPartitionLayout l,SPartitionWriteOptions o,asyncpp::dispatcher* w) {
    co_await DispatchOn{w}; CPartitionTable::write(std::move(d),std::move(l),o);
}
} // namespace

void CPartitionTable::write(std::shared_ptr<IBlockDevice> device,SPartitionLayout l,SPartitionWriteOptions o) {
    if (!device) { throw std::invalid_argument("null partition target"); } writable(device->isReadOnly());
    if (l.logicalSectorSize<512 || l.logicalSectorSize>4096 || (l.logicalSectorSize&(l.logicalSectorSize-1)) || device->size()%l.logicalSectorSize) { fail(EERROR_RANGE,"invalid partition target geometry"); }
    const auto total=device->size()/l.logicalSectorSize;
    if (total<2 || l.partitions.size()>16384) { fail(EERROR_LIMIT,"partition layout too large or target too small"); }
    std::sort(l.partitions.begin(),l.partitions.end(),[](const auto& a,const auto& b) { return a.index<b.index; });
    std::set<std::uint32_t> slots; for (const auto& p:l.partitions) { if (!p.index || !slots.insert(p.index).second) { fail(EERROR_INVALID,"duplicate or zero partition slot"); } }
    std::vector<Edit> edits; SBytes mbr(l.logicalSectorSize); mbr[510]=0x55; mbr[511]=0xaa;
    if (o.preserveBootCode) { device->readAt(0,SByteSpan(mbr).first(446)); }
    if (l.scheme==EPARTITION_GPT) {
        if (zero(l.diskGuid) || l.gptEntryCount<128 || l.gptEntryCount>16384) { fail(EERROR_LIMIT,"GPT requires disk GUID and 128..16384 slots"); }
        const auto arrayBytes=std::uint64_t(l.gptEntryCount)*128,blocks=(arrayBytes+l.logicalSectorSize-1)/l.logicalSectorSize;
        if (total<2*blocks+4) { fail(EERROR_RANGE,"device too small for redundant GPT"); }
        const auto first=2+blocks,last=total-blocks-2,backupArray=last+1;
        SBytes array(static_cast<std::size_t>(blocks*l.logicalSectorSize));
        for (const auto& p:l.partitions) {
            if (p.index>l.gptEntryCount || p.firstLba<first || p.firstLba>last || !p.sectorCount || p.sectorCount-1>last-p.firstLba) { fail(EERROR_RANGE,"GPT partition does not fit"); }
            const auto at=static_cast<std::size_t>(p.index-1)*128;
            std::copy(p.typeGuid.begin(),p.typeGuid.end(),array.begin()+at); std::copy(p.uniqueGuid.begin(),p.uniqueGuid.end(),array.begin()+at+16);
            if (zero(p.typeGuid) || zero(p.uniqueGuid)) { fail(EERROR_INVALID,"GPT partition GUID must be nonzero"); }
            put(array,at+32,p.firstLba,8); put(array,at+40,p.firstLba+p.sectorCount-1,8); put(array,at+48,p.attributes,8);
            const auto name=encodeName(p.name); for (std::size_t j=0;j<name.size();++j) { put(array,at+56+2*j,name[j],2); }
        }
        auto header=[&](std::uint64_t at,std::uint64_t peer,std::uint64_t arrayLba) {
            SBytes h(l.logicalSectorSize); std::copy_n("EFI PART",8,h.begin()); put(h,8,0x10000,4); put(h,12,92,4);
            put(h,24,at,8); put(h,32,peer,8); put(h,40,first,8); put(h,48,last,8); std::copy(l.diskGuid.begin(),l.diskGuid.end(),h.begin()+56);
            put(h,72,arrayLba,8); put(h,80,l.gptEntryCount,4); put(h,84,128,4); put(h,88,crc32(SReadOnlyByteSpan(array).first(static_cast<std::size_t>(arrayBytes)))^UINT32_MAX,4);
            put(h,16,crc32(SReadOnlyByteSpan(h).first(92))^UINT32_MAX,4); return h;
        };
        // Publish backup before primary; a device failure can still leave a partial update.
        auto primaryHeader=header(1,total-1,2),backupHeader=header(total-1,1,backupArray);
        edits.push_back({multiply(backupArray,l.logicalSectorSize),array}); edits.push_back({multiply(total-1,l.logicalSectorSize),std::move(backupHeader)});
        edits.push_back({multiply(2,l.logicalSectorSize),std::move(array)}); edits.push_back({l.logicalSectorSize,std::move(primaryHeader)});
        SPartition protect; protect.mbrType=0xee; protect.firstLba=1; protect.sectorCount=std::min<std::uint64_t>(total-1,UINT32_MAX); record(mbr,446,protect,0);
    } else if (l.scheme==EPARTITION_MBR) {
        std::optional<SPartition> container; std::vector<SPartition> logical;
        for (const auto& p:l.partitions) {
            if (p.index<=4) { if (p.mbrType==0xee) { fail(EERROR_UNSUPPORTED,"protective MBR must be generated as GPT"); } record(mbr,446+(p.index-1)*16,p,0); if (extended(p.mbrType)) { if (container) { fail(EERROR_INVALID,"multiple extended containers"); } container=p; } }
            else { logical.push_back(p); }
        }
        if (!logical.empty() && !container) { fail(EERROR_INVALID,"logical partitions require an extended container"); }
        if (container && logical.empty()) { fail(EERROR_INVALID,"extended container requires a logical partition"); }
        for (std::size_t i=0;i<logical.size();++i) {
            const auto& p=logical[i]; if (p.index!=5+i || !p.ebrLba || (i==0 && p.ebrLba!=container->firstLba) || extended(p.mbrType) || p.mbrType==0xee) { fail(EERROR_INVALID,"invalid logical MBR layout"); }
            SBytes e(l.logicalSectorSize); e[510]=0x55; e[511]=0xaa; record(e,446,p,p.ebrLba);
            if (i+1<logical.size()) {
                SPartition link; link.mbrType=container->mbrType; link.firstLba=logical[i+1].ebrLba;
                if (link.firstLba<container->firstLba || link.firstLba-container->firstLba>=container->sectorCount) { fail(EERROR_RANGE,"EBR outside extended partition"); }
                link.sectorCount=container->sectorCount-(link.firstLba-container->firstLba); record(e,462,link,container->firstLba);
            } edits.push_back({multiply(p.ebrLba,l.logicalSectorSize),std::move(e)});
        }
    } else { fail(EERROR_INVALID,"unknown partition scheme"); }
    edits.push_back({0,std::move(mbr)});
    // Reject colliding metadata write ranges before creating the validation overlay.
    std::vector<const Edit*> sorted; for (const auto& e:edits) { range(device->size(),e.offset,e.bytes.size()); sorted.push_back(&e); }
    std::sort(sorted.begin(),sorted.end(),[](auto a,auto b) { return a->offset<b->offset; });
    for (std::size_t i=1;i<sorted.size();++i) { if (sorted[i]->offset-sorted[i-1]->offset<sorted[i-1]->bytes.size()) { fail(EERROR_INVALID,"overlapping metadata writes"); } }
    SPartitionOpenOptions check; check.logicalSectorSize=l.logicalSectorSize;
    (void)open(std::make_shared<ProposedDisk>(device,edits),check);
    if (!o.allowOverwrite) { for (const auto& e:edits) { if (!zero(device->readBytes(e.offset,e.bytes.size()))) { fail(EERROR_STATE,"nonzero partition metadata requires allowOverwrite"); } } }
    for (const auto& e:edits) { device->writeAt(e.offset,e.bytes); device->flush(); }
}
asyncpp::task<void> CPartitionTable::writeAsync(std::shared_ptr<IBlockDevice> d,SPartitionLayout l,asyncpp::dispatcher& w,SPartitionWriteOptions o) { return writeOn(std::move(d),std::move(l),o,&w); }
} // namespace hnfs
