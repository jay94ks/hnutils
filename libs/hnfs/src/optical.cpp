#include <hnfs/optical.hpp>
#include "disk_internal.hpp"
#include <chrono>
#include <functional>
#include <set>
#include <map>

namespace hnfs {
namespace {
using namespace detail;
struct Extent { std::uint64_t offset,length; bool hole=false; std::uint64_t partitionBase=0; };
struct Node { SEntry entry; std::vector<Extent> extents; std::uint64_t id=0; std::uint16_t partition=0; };
class ExtentFile final : public IFileReader {
    std::shared_ptr<IBlockDevice> _device;
    Node _node;
public:
    ExtentFile(std::shared_ptr<IBlockDevice> device,Node node) : _device(std::move(device)),_node(std::move(node)) {}
    std::uint64_t size() const override { return _node.entry.size; }
    std::size_t readAt(std::uint64_t offset,SByteSpan out) const override {
        if (offset>=size() || out.empty()) { return 0; }
        const auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(out.size(),size()-offset)); out=out.first(amount);
        for (const auto& e:_node.extents) {
            if (offset>=e.length) { offset-=e.length; continue; }
            const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(out.size(),e.length-offset));
            if (e.hole) { std::fill_n(out.begin(),n,0); } else { _device->readAt(add(e.offset,offset),out.first(n)); }
            out=out.subspan(n); offset=0; if (out.empty()) { return amount; }
        } fail(EERROR_TRUNCATED,"file extents do not cover information length");
    }
};
std::uint64_t date(unsigned year,unsigned month,unsigned day,unsigned hour,unsigned minute,unsigned second,int zone) {
    using namespace std::chrono;
    if (year<1970 || year>32767) { fail(EERROR_RANGE,"optical timestamp year is not representable"); }
    const year_month_day ymd{std::chrono::year(static_cast<int>(year)),std::chrono::month(month),std::chrono::day(day)};
    if (!ymd.ok() || hour>23 || minute>59 || second>59) { fail(EERROR_INVALID,"invalid optical timestamp"); }
    const auto result=duration_cast<seconds>(sys_days(ymd).time_since_epoch()).count()+hour*3600+minute*60+second-zone*60;
    if (result<0) { fail(EERROR_RANGE,"optical timestamp predates Unix epoch"); } return static_cast<std::uint64_t>(result);
}
void nameCheck(const std::string& name) {
    if (name.empty() || name=="." || name==".." || name.find_first_of("/\\:\0",0,4)!=std::string::npos) { fail(EERROR_INVALID,"unsafe optical path component"); }
}
std::string canonical(std::string path,const SOpenOptions& o) {
    if (!path.empty() && path.front()=='/') { path.erase(0,1); }
    if (path.size()>=o.maximumPathBytes) { fail(EERROR_LIMIT,"optical path exceeds policy"); }
    std::size_t start=0,depth=0;
    while (start<path.size()) { const auto slash=path.find('/',start),end=slash==std::string::npos?path.size():slash; nameCheck(path.substr(start,end-start)); if (++depth>o.maximumDepth) { fail(EERROR_LIMIT,"optical path depth exceeds policy"); } start=end+1; }
    if (!path.empty() && path.back()=='/') { fail(EERROR_INVALID,"empty path component"); } return path;
}
class OpticalTree {
protected:
    std::shared_ptr<IBlockDevice> device;
    SOpenOptions options;
    Node root;
    using Visitor=std::function<bool(Node)>;
    virtual void visit(const Node& directory,const Visitor& visitor) const =0;
    void directory(const Node& node) const {
        if (node.entry.kind!=ENODE_DIRECTORY) { fail(EERROR_NOT_DIRECTORY,"optical node is not a directory"); }
        if (node.entry.size>options.maximumDirectoryBytes) { fail(EERROR_LIMIT,"optical directory exceeds scan policy"); }
    }
    void extent(Node& node,std::uint64_t offset,std::uint64_t length,bool hole=false,std::uint64_t partitionBase=0) const {
        if (node.extents.size()>=options.maximumExtents) { fail(EERROR_LIMIT,"too many optical file extents"); }
        if (!hole) { range(device->size(),offset,length); } node.extents.push_back({offset,length,hole,partitionBase});
    }
    struct Budget {
        const SOpenOptions& options; std::uint64_t entries=0,names=0,data=0; std::set<std::string> seen;
        void accept(const Node& n) {
            nameCheck(n.entry.path);
            if (++entries>options.maximumEntries || n.entry.path.size()>=options.maximumPathBytes || n.entry.path.size()>options.maximumNameBytes-names || n.entry.size>options.maximumDataBytes-data) { fail(EERROR_LIMIT,"optical directory metadata exceeds policy"); }
            names+=n.entry.path.size(); data+=n.entry.size;
            if (!seen.insert(n.entry.path).second) { fail(EERROR_INVALID,"ambiguous duplicate optical name"); }
        }
    };
public:
    OpticalTree(std::shared_ptr<IBlockDevice> d,SOpenOptions o) : device(std::move(d)),options(o) {}
    virtual ~OpticalTree()=default;
    virtual EFormat format() const noexcept=0;
    Node lookup(const std::string& input) const {
        auto path=canonical(input,options); Node node=root; std::set<std::uint64_t> ancestors;
        std::size_t start=0;
        while (start<path.size()) {
            directory(node); if (!ancestors.insert(node.id).second) { fail(EERROR_INVALID,"optical directory cycle"); }
            const auto slash=path.find('/',start),end=slash==std::string::npos?path.size():slash;
            const auto name=path.substr(start,end-start); std::optional<Node> found;
            visit(node,[&](Node child) { if (child.entry.path==name) { found=std::move(child); return false; } return true; });
            if (!found) { fail(EERROR_NOT_FOUND,"optical path does not exist"); }
            node=std::move(*found); start=end+1;
        } node.entry.path=path; return node;
    }
    std::vector<SEntry> list(const std::string& path,std::uint64_t offset,std::size_t count) const {
        if (count>4096) { fail(EERROR_LIMIT,"optical page exceeds 4096 entries"); }
        auto node=lookup(path); directory(node); std::vector<SEntry> out; if (!count) { return out; }
        std::uint64_t ordinal=0;
        visit(node,[&](Node child) {
            if (ordinal++>=offset) { child.entry.path=node.entry.path.empty()?child.entry.path:node.entry.path+"/"+child.entry.path; if (child.entry.path.size()>=options.maximumPathBytes) { fail(EERROR_LIMIT,"optical full path exceeds policy"); } out.push_back(std::move(child.entry)); }
            return out.size()<count;
        }); return out;
    }
    std::shared_ptr<IFileReader> file(const std::string& path) const {
        auto node=lookup(path);
        if (node.entry.kind==ENODE_DIRECTORY) { fail(EERROR_IS_DIRECTORY,"cannot read directory as a regular file"); }
        if (node.entry.kind!=ENODE_FILE) { fail(EERROR_UNSUPPORTED,"optical special files and links are not followed"); }
        return std::make_shared<ExtentFile>(device,std::move(node));
    }
};
class IsoTree final : public OpticalTree {
    std::uint32_t block=2048;
    std::uint64_t volumeBytes=0;
    bool joliet=false;
    std::uint64_t both(SReadOnlyByteSpan b,std::size_t at,unsigned width) const {
        const auto value=little(b,at,width); if (value!=big(b,at+width,width)) { fail(EERROR_INVALID,"ISO endian copies disagree"); } return value;
    }
    Node record(SReadOnlyByteSpan b,bool& continuation,std::string& raw) const {
        if (b.size()<34 || b[0]!=b.size() || b[32]>b.size()-33 || !b[32]) { fail(EERROR_INVALID,"invalid ISO directory record"); }
        if (b[26] || b[27] || (b[25]&0x60) || both(b,28,2)!=1) { fail(EERROR_UNSUPPORTED,"ISO interleave, volume sets or reserved flags unsupported"); }
        Node n; const auto start=multiply(add(both(b,2,4),b[1]),block),length=both(b,10,4);
        range(volumeBytes,start,length); n.entry.size=length; n.id=start; continuation=(b[25]&0x80)!=0;
        n.entry.kind=(b[25]&2)?ENODE_DIRECTORY:ENODE_FILE; n.entry.permissions=(b[25]&2)?0555:0444;
        const int zone=b[24]<128?b[24]:int(b[24])-256;
        if (zone<-48 || zone>52) { fail(EERROR_INVALID,"invalid ISO timezone"); }
        n.entry.modifiedTime=date(1900+b[18],b[19],b[20],b[21],b[22],b[23],zone*15);
        raw.assign(reinterpret_cast<const char*>(b.data()+33),b[32]);
        if (b[32]==1 && (b[33]==0 || b[33]==1)) { n.entry.path=b[33]?"..":"."; }
        else {
            n.entry.path=joliet?utf16(b.subspan(33,b[32]),true,false):raw;
            auto semicolon=n.entry.path.rfind(';');
            if (semicolon!=std::string::npos) {
                const auto version=n.entry.path.substr(semicolon+1);
                if (version.empty() || !std::all_of(version.begin(),version.end(),[](auto c) { return c>='0' && c<='9'; })) { fail(EERROR_INVALID,"invalid ISO version suffix"); }
                n.entry.path.resize(semicolon);
            }
            if (!joliet && !n.entry.path.empty() && n.entry.path.back()=='.') { n.entry.path.pop_back(); }
            nameCheck(n.entry.path);
        }
        if (continuation && (n.entry.kind!=ENODE_FILE || !length || length%block)) { fail(EERROR_INVALID,"invalid ISO multi-extent section"); }
        extent(n,start,length); return n;
    }
    void visit(const Node& dir,const Visitor& visitor) const override {
        directory(dir); ExtentFile stream(device,dir); std::uint64_t pos=0; Budget budget{options,0,0,0,{}};
        std::optional<Node> pending; std::string pendingRaw;
        while (pos<stream.size()) {
            std::array<std::uint8_t,1> length{}; stream.readAt(pos,length);
            const auto remaining=2048-(dir.extents[0].offset+pos)%2048;
            if (!length[0]) { pos=std::min(stream.size(),add(pos,remaining)); continue; }
            if (length[0]>remaining || length[0]>stream.size()-pos) { fail(EERROR_TRUNCATED,"ISO record crosses logical sector or directory end"); }
            auto b=SBytes(length[0]); stream.readAt(pos,b); pos+=b.size(); bool more=false; std::string raw;
            auto n=record(b,more,raw);
            if (n.entry.path=="." || n.entry.path=="..") { if (pending) { fail(EERROR_INVALID,"interrupted ISO extent sequence"); } continue; }
            if (pending) {
                if (raw!=pendingRaw || n.entry.kind!=pending->entry.kind || n.entry.modifiedTime!=pending->entry.modifiedTime) { fail(EERROR_INVALID,"mismatched ISO multi-extent records"); }
                if (n.entry.size>options.maximumDataBytes-pending->entry.size) { fail(EERROR_LIMIT,"ISO file exceeds data policy"); }
                extent(*pending,n.extents[0].offset,n.entry.size); pending->entry.size+=n.entry.size;
                if (more) { continue; } n=std::move(*pending); pending.reset();
            } else if (more) { if (n.entry.size>options.maximumDataBytes) { fail(EERROR_LIMIT,"ISO file exceeds data policy"); } pending=std::move(n); pendingRaw=raw; continue; }
            budget.accept(n); if (!visitor(std::move(n))) { return; }
        }
        if (pending) { fail(EERROR_TRUNCATED,"unterminated ISO multi-extent file"); }
    }
public:
    IsoTree(std::shared_ptr<IBlockDevice> d,SOpenOptions o) : OpticalTree(std::move(d),o) {
        SBytes primary,selected; bool terminated=false; int selectedLevel=-1;
        for (std::uint32_t i=0;i<options.maximumDescriptors;++i) {
            auto b=device->readBytes((std::uint64_t(16)+i)*2048,2048);
            if (!std::equal(b.begin()+1,b.begin()+6,"CD001") || b[6]!=1) { fail(EERROR_INVALID,"invalid ISO volume descriptor"); }
            if (b[0]==255) { terminated=true; break; }
            if (b[0]==1) { if (!primary.empty()) { fail(EERROR_UNSUPPORTED,"multiple ISO primary descriptors"); } primary=b; }
            if (b[0]==2 && b[88]=='%' && b[89]=='/' && (b[90]=='@' || b[90]=='C' || b[90]=='E') && b[90]>selectedLevel) { selected=b; selectedLevel=b[90]; }
        }
        if (!terminated || primary.empty()) { fail(EERROR_INVALID,"ISO descriptor set is incomplete or exceeds policy"); }
        auto& b=selected.empty()?primary:selected; joliet=!selected.empty();
        block=static_cast<std::uint32_t>(both(b,128,2));
        if (block<512 || block>2048 || (block&(block-1)) || both(b,120,2)!=1 || both(b,124,2)!=1) { fail(EERROR_UNSUPPORTED,"ISO block size or multi-volume set unsupported"); }
        volumeBytes=multiply(both(b,80,4),block); if (volumeBytes>device->size() || volumeBytes<18*2048) { fail(EERROR_TRUNCATED,"ISO volume exceeds device"); }
        bool more=false; std::string raw; root=record(SReadOnlyByteSpan(b).subspan(156,b[156]),more,raw);
        if (root.entry.kind!=ENODE_DIRECTORY || more || root.entry.path!=".") { fail(EERROR_INVALID,"invalid ISO root record"); } root.entry.path.clear(); directory(root);
    }
    EFormat format() const noexcept override { return EFORMAT_ISO9660; }
};

/** ECMA-167 descriptor tag checksum and CRC-16/CCITT, never CRC-32. */
std::uint16_t tag(SReadOnlyByteSpan b,std::uint64_t location) {
    if (b.size()<16 || location>UINT32_MAX || little(b,12,4)!=location || (little(b,2,2)!=2 && little(b,2,2)!=3)) { fail(EERROR_INVALID,"invalid UDF descriptor tag location/version"); }
    std::uint8_t sum=0; for (std::size_t i=0;i<16;++i) { if (i!=4) { sum=static_cast<std::uint8_t>(sum+b[i]); } }
    if (sum!=b[4]) { fail(EERROR_CHECKSUM,"UDF tag checksum mismatch"); }
    const auto length=little(b,10,2); if (length>b.size()-16) { fail(EERROR_TRUNCATED,"UDF descriptor CRC range exceeds record"); }
    std::uint16_t crc=0; for (auto c:b.subspan(16,static_cast<std::size_t>(length))) { crc^=std::uint16_t(c)<<8; for (unsigned i=0;i<8;++i) { crc=static_cast<std::uint16_t>((crc<<1)^((crc&0x8000)?0x1021:0)); } }
    if (crc!=little(b,8,2)) { fail(EERROR_CHECKSUM,"UDF descriptor CRC16 mismatch"); } return static_cast<std::uint16_t>(little(b,0,2));
}
void covered(SReadOnlyByteSpan b,std::uint64_t end) {
    if (end>16+little(b,10,2)) { fail(EERROR_INVALID,"UDF interpreted fields are not CRC-covered"); }
}
class UdfTree final : public OpticalTree {
    struct Partition { std::uint64_t start,length; };
    std::vector<Partition> maps;
    static constexpr std::uint32_t block=2048;
    std::uint64_t address(std::uint16_t partition,std::uint64_t lba,std::uint64_t length) const {
        if (partition>=maps.size()) { fail(EERROR_RANGE,"invalid UDF partition reference"); }
        const auto& p=maps[partition]; range(multiply(p.length,block),multiply(lba,block),length); return multiply(add(p.start,lba),block);
    }
    Node icb(SReadOnlyByteSpan ad) const {
        const auto length=little(ad,0,4),lba=little(ad,4,4); const auto part=static_cast<std::uint16_t>(little(ad,8,2));
        if (length<block || length>block) { fail(EERROR_UNSUPPORTED,"UDF indirect or multi-block ICB unsupported"); }
        const auto at=address(part,lba,block); auto b=device->readBytes(at,block); const auto type=tag(b,lba);
        if (type!=261 && type!=266) { fail(EERROR_UNSUPPORTED,"UDF indirect ICB or file entry type unsupported"); }
        if (little(b,20,2)!=4 || little(b,24,2)!=1) { fail(EERROR_UNSUPPORTED,"UDF ICB strategy unsupported"); }
        Node n; n.id=at; n.partition=part; n.entry.size=little(b,56,8); n.entry.uid=static_cast<std::uint32_t>(little(b,36,4)); n.entry.gid=static_cast<std::uint32_t>(little(b,40,4));
        n.entry.kind=b[27]==4?ENODE_DIRECTORY:b[27]==5?ENODE_FILE:b[27]==12?ENODE_SYMLINK:ENODE_SPECIAL;
        const auto permissions=little(b,44,4); n.entry.permissions=static_cast<std::uint32_t>((permissions&7)|((permissions>>5&7)<<3)|((permissions>>10&7)<<6));
        const auto timeAt=type==266?92u:84u; const auto tz=little(b,timeAt,2); int zone=static_cast<int>(tz&4095); if (zone&2048) { zone-=4096; }
        if (zone==-2047) { zone=0; } if (zone<-1440 || zone>1440) { fail(EERROR_INVALID,"invalid UDF timezone"); }
        n.entry.modifiedTime=date(static_cast<unsigned>(little(b,timeAt+2,2)),b[timeAt+4],b[timeAt+5],b[timeAt+6],b[timeAt+7],b[timeAt+8],zone);
        if (n.entry.size>options.maximumDataBytes) { fail(EERROR_LIMIT,"UDF file exceeds data policy"); }
        const auto base=type==266?216u:176u; const auto ea=little(b,base-8,4),ads=little(b,base-4,4),start=add(base,ea);
        range(b.size(),start,ads); if (start+ads>16+little(b,10,2)) { fail(EERROR_INVALID,"UDF allocation descriptors are not CRC-covered"); }
        const auto flags=little(b,34,2)&7;
        if (flags==3) { if (ads<n.entry.size) { fail(EERROR_TRUNCATED,"short embedded UDF content"); } extent(n,at+start,n.entry.size,false,multiply(maps[part].start,block)); return n; }
        const auto width=flags==0?8u:flags==1?16u:0u; if (!width || ads%width) { fail(EERROR_UNSUPPORTED,"UDF allocation descriptor type unsupported"); }
        std::uint64_t covered=0;
        for (auto pos=start;pos<start+ads;pos+=width) {
            const auto encoded=little(b,static_cast<std::size_t>(pos),4),len=encoded&0x3fffffff,state=encoded>>30;
            if (!len) { break; } if (state==3) { fail(EERROR_UNSUPPORTED,"UDF allocation continuation unsupported"); }
            const auto ref=flags==0?part:static_cast<std::uint16_t>(little(b,static_cast<std::size_t>(pos+8),2));
            const auto location=little(b,static_cast<std::size_t>(pos+4),4);
            const auto physical=state==2?0:address(ref,location,len);
            extent(n,physical,len,state!=0,ref<maps.size()?multiply(maps[ref].start,block):0); covered=add(covered,len);
        }
        if (covered<n.entry.size) { fail(EERROR_TRUNCATED,"UDF extents do not cover file"); } return n;
    }
    void visit(const Node& dir,const Visitor& visitor) const override {
        directory(dir); ExtentFile stream(device,dir); std::uint64_t pos=0; Budget budget{options,0,0,0,{}};
        while (pos<stream.size()) {
            if (stream.size()-pos<38) { fail(EERROR_TRUNCATED,"truncated UDF file identifier"); }
            auto head=SBytes(38); stream.readAt(pos,head); const auto nameLength=head[19]; const auto implementation=little(head,36,2);
            const auto length=(38+implementation+nameLength+3)&~UINT64_C(3);
            if (length>65536 || length>stream.size()-pos) { fail(EERROR_LIMIT,"UDF file identifier exceeds bounds"); }
            auto b=SBytes(static_cast<std::size_t>(length)); stream.readAt(pos,b);
            std::uint64_t local=pos,tagLocation=0; bool located=false;
            for (const auto& e:dir.extents) { if (local>=e.length) { local-=e.length; continue; } if (e.hole) { fail(EERROR_INVALID,"UDF directory has an unrecorded extent"); }
                if (e.offset<e.partitionBase) { fail(EERROR_RANGE,"UDF extent precedes partition"); }
                tagLocation=(e.offset-e.partitionBase+local)/block; located=true; break; }
            if (!located || tag(b,tagLocation)!=257 || 16+little(b,10,2)<38+implementation+nameLength) { fail(EERROR_INVALID,"invalid UDF file identifier tag"); }
            pos+=length; if (b[18]&12) { continue; } // Deleted and parent entries are not namespace children.
            auto encoded=SReadOnlyByteSpan(b).subspan(static_cast<std::size_t>(38+implementation),nameLength);
            if (encoded.empty()) { fail(EERROR_INVALID,"empty UDF child name"); }
            std::string name;
            if (encoded[0]==8) { for (auto c:encoded.subspan(1)) { utf8(name,c); } }
            else if (encoded[0]==16) { name=utf16(encoded.subspan(1),true,false); }
            else { fail(EERROR_UNSUPPORTED,"unknown UDF CS0 compression ID"); }
            auto n=icb(SReadOnlyByteSpan(b).subspan(20,16)); n.entry.path=std::move(name);
            if (((b[18]&2)!=0)!=(n.entry.kind==ENODE_DIRECTORY)) { fail(EERROR_INVALID,"UDF directory flag disagrees with ICB"); }
            budget.accept(n); if (!visitor(std::move(n))) { return; }
        }
    }
public:
    UdfTree(std::shared_ptr<IBlockDevice> d,SOpenOptions o) : OpticalTree(std::move(d),o) {
        // Read-only physical-partition UDF (DVD-ROM); no VAT, sparing or metadata remapping.
        auto anchor=device->readBytes(UINT64_C(256)*block,block);
        if (tag(anchor,256)!=2) { fail(EERROR_INVALID,"missing UDF anchor at sector 256"); }
        covered(anchor,32);
        const auto sequenceBytes=little(anchor,16,4),sequence=little(anchor,20,4);
        if (!sequenceBytes || sequenceBytes%block || sequenceBytes/block>options.maximumDescriptors) { fail(EERROR_LIMIT,"UDF descriptor sequence exceeds policy"); }
        range(device->size(),multiply(sequence,block),sequenceBytes);
        std::map<std::uint16_t,Partition> partitions; SBytes logical; bool terminated=false;
        for (std::uint64_t i=0;i<sequenceBytes/block;++i) {
            auto b=device->readBytes(multiply(sequence+i,block),block); const auto type=tag(b,sequence+i);
            if (type==8) { terminated=true; break; }
            if (type==3) { fail(EERROR_UNSUPPORTED,"UDF continuation volume descriptor sequence unsupported"); }
            if (type==5) {
                covered(b,196);
                const auto number=static_cast<std::uint16_t>(little(b,22,2));
                Partition p{little(b,188,4),little(b,192,4)}; range(device->size(),multiply(p.start,block),multiply(p.length,block));
                if (!p.length || !partitions.emplace(number,p).second) { fail(EERROR_UNSUPPORTED,"multiple UDF partition generations unsupported"); }
            }
            if (type==6) { covered(b,440); if (!logical.empty()) { fail(EERROR_UNSUPPORTED,"multiple UDF logical volumes unsupported"); } logical=std::move(b); }
        }
        if (!terminated || logical.empty() || little(logical,212,4)!=block) { fail(EERROR_UNSUPPORTED,"incomplete UDF sequence or unsupported block size"); }
        if (logical[20] || !std::equal(logical.begin()+21,logical.begin()+44,"OSTA Compressed Unicode") ||
            !std::equal(logical.begin()+217,logical.begin()+236,"*OSTA UDF Compliant") || little(logical,240,2)>0x260) {
            fail(EERROR_UNSUPPORTED,"unsupported UDF domain or character set");
        }
        const auto mapLength=little(logical,264,4),count=little(logical,268,4);
        if (!count || count>16 || mapLength!=count*6 || 440+mapLength>16+little(logical,10,2)) { fail(EERROR_UNSUPPORTED,"UDF virtual/sparable/metadata partition maps unsupported"); }
        for (std::size_t i=0;i<count;++i) {
            const auto at=440+i*6;
            if (logical[at]!=1 || logical[at+1]!=6 || little(logical,at+2,2)!=1) { fail(EERROR_UNSUPPORTED,"UDF requires physical type-1 partition maps"); }
            auto p=partitions.find(static_cast<std::uint16_t>(little(logical,at+4,2))); if (p==partitions.end()) { fail(EERROR_INVALID,"UDF partition map has no descriptor"); } maps.push_back(p->second);
        }
        const auto fsd=SReadOnlyByteSpan(logical).subspan(248,16); const auto location=little(fsd,4,4);
        if (little(fsd,0,4)<block) { fail(EERROR_INVALID,"missing UDF file set descriptor"); }
        const auto at=address(static_cast<std::uint16_t>(little(fsd,8,2)),location,block); auto b=device->readBytes(at,block);
        if (tag(b,location)!=256 || little(b,448,4)) { fail(EERROR_UNSUPPORTED,"UDF file set sequence unsupported"); }
        covered(b,464);
        if (b[240] || !std::equal(b.begin()+241,b.begin()+264,"OSTA Compressed Unicode")) { fail(EERROR_UNSUPPORTED,"unsupported UDF file set character set"); }
        root=icb(SReadOnlyByteSpan(b).subspan(400,16)); root.entry.path.clear(); directory(root);
    }
    EFormat format() const noexcept override { return EFORMAT_UDF; }
};
} // namespace
struct COpticalFileSystem::Impl { std::shared_ptr<OpticalTree> tree; };
COpticalFileSystem::COpticalFileSystem(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
COpticalFileSystem::~COpticalFileSystem()=default;
std::shared_ptr<COpticalFileSystem> COpticalFileSystem::open(std::shared_ptr<IBlockDevice> d,SOpenOptions o,EFormat f) {
    if (!d) { throw std::invalid_argument("null optical image"); } validate(o);
    if (!o.maximumDepth || o.maximumDepth>4096 || !o.maximumExtents || o.maximumExtents>1048576 || !o.maximumDescriptors || o.maximumDescriptors>4096) { fail(EERROR_LIMIT,"invalid optical parsing policy"); }
    if (f==EFORMAT_UNKNOWN) { f=IFileSystem::probe(*d).format; } auto impl=std::make_unique<Impl>();
    if (f==EFORMAT_ISO9660) { impl->tree=std::make_shared<IsoTree>(std::move(d),o); }
    else if (f==EFORMAT_UDF) { impl->tree=std::make_shared<UdfTree>(std::move(d),o); }
    else { fail(EERROR_UNSUPPORTED,"not an implemented optical format"); }
    return std::shared_ptr<COpticalFileSystem>(new COpticalFileSystem(std::move(impl)));
}
EFormat COpticalFileSystem::format() const noexcept { return _impl->tree->format(); }
SEntry COpticalFileSystem::stat(const std::string& path) const { return _impl->tree->lookup(path).entry; }
std::vector<SEntry> COpticalFileSystem::list(const std::string& path,std::uint64_t offset,std::size_t count) const { return _impl->tree->list(path,offset,count); }
std::shared_ptr<IFileReader> COpticalFileSystem::openFile(const std::string& path) const { return _impl->tree->file(path); }
} // namespace hnfs
