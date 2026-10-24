#include <hnfs.hpp>
#include <doctest/doctest.h>
#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>
#include <algorithm>
#include <chrono>
#include <map>

namespace {
using namespace hnfs;
SPartition part(unsigned index,std::uint64_t first,std::uint64_t count,unsigned type=0x83) {
    SPartition p; p.index=index; p.firstLba=first; p.sectorCount=count; p.mbrType=static_cast<std::uint8_t>(type); p.typeGuid[0]=1; p.uniqueGuid[0]=static_cast<std::uint8_t>(index); return p;
}
void put(SBytes& b,std::size_t at,std::uint64_t value,unsigned n,bool be=false) {
    for (unsigned i=0;i<n;++i) { b.at(at+(be?n-1-i:i))=static_cast<std::uint8_t>(value>>(8*i)); }
}
void both(SBytes& b,std::size_t at,std::uint64_t value,unsigned n) { put(b,at,value,n); put(b,at+n,value,n,true); }
SBytes isoRecord(std::uint32_t lba,std::uint32_t size,const SBytes& name,std::uint8_t flags=0) {
    SBytes b(33+name.size()+((name.size()%2)?0:1)); b[0]=static_cast<std::uint8_t>(b.size()); both(b,2,lba,4); both(b,10,size,4);
    b[18]=124; b[19]=1; b[20]=2; b[25]=flags; both(b,28,1,2); b[32]=static_cast<std::uint8_t>(name.size()); std::copy(name.begin(),name.end(),b.begin()+33); return b;
}
std::shared_ptr<CMemoryDevice> iso() {
    auto d=std::make_shared<CMemoryDevice>(40*2048); SBytes descriptor(2048); descriptor[0]=1; std::copy_n("CD001",5,descriptor.begin()+1); descriptor[6]=1;
    both(descriptor,80,40,4); both(descriptor,120,1,2); both(descriptor,124,1,2); both(descriptor,128,2048,2);
    auto root=isoRecord(20,2048,{0},2); std::copy(root.begin(),root.end(),descriptor.begin()+156); d->writeAt(16*2048,descriptor);
    descriptor[0]=255; d->writeAt(17*2048,descriptor);
    auto dot=isoRecord(20,2048,{0},2),parent=isoRecord(20,2048,{1},2),file=isoRecord(21,5,{'H','E','L','L','O','.','T','X','T',';','1'});
    std::uint64_t at=20*2048; for (const auto& r:{dot,parent,file}) { d->writeAt(at,r); at+=r.size(); } d->writeAt(21*2048,SBytes{'h','e','l','l','o'}); return d;
}
class SparseDisk final : public IBlockDevice {
    std::uint64_t _size;
    std::map<std::uint64_t,std::uint8_t> _bytes;
public:
    explicit SparseDisk(std::uint64_t length) : _size(length) {}
    std::uint64_t size() const noexcept override { return _size; }
    bool isReadOnly() const noexcept override { return false; }
    void readAt(std::uint64_t offset,SByteSpan output) const override {
        if (offset>_size || output.size()>_size-offset) { throw CFileSystemError(EERROR_RANGE,"test disk range"); }
        std::fill(output.begin(),output.end(),0);
        for (auto i=_bytes.lower_bound(offset);i!=_bytes.end() && i->first-offset<output.size();++i) { output[static_cast<std::size_t>(i->first-offset)]=i->second; }
    }
    void writeAt(std::uint64_t offset,SReadOnlyByteSpan input) override {
        if (offset>_size || input.size()>_size-offset) { throw CFileSystemError(EERROR_RANGE,"test disk range"); }
        for (auto byte:input) { if (byte) { _bytes[offset]=byte; } else { _bytes.erase(offset); } ++offset; }
    }
    void flush() override {}
};
void sum(SBytes& b,std::size_t at) { put(b,at,0,4,true); std::uint32_t total=0; for (auto c:b) { total+=c; } put(b,at,~total,4,true); }
std::shared_ptr<CMemoryDevice> vhd(bool dynamic) {
    constexpr std::uint32_t block=2*1024*1024; SBytes f(512); std::copy_n("conectix",8,f.begin()); put(f,8,2,4,true); put(f,12,0x10000,4,true);
    put(f,16,dynamic?512:UINT64_MAX,8,true); put(f,40,block*2,8,true); put(f,48,block*2,8,true); put(f,60,dynamic?3:2,4,true); f[68]=1; sum(f,64);
    const auto capacity=dynamic?std::size_t(2048+2*(512+block)+512):std::size_t(2*block+512); auto d=std::make_shared<CMemoryDevice>(capacity);
    d->writeAt(capacity-512,f);
    if (!dynamic) { d->writeAt(3,SBytes{1,2,3}); return d; }
    d->writeAt(0,f); SBytes h(1024); std::copy_n("cxsparse",8,h.begin()); put(h,8,UINT64_MAX,8,true); put(h,16,1536,8,true); put(h,24,0x10000,4,true); put(h,28,2,4,true); put(h,32,block,4,true); sum(h,36); d->writeAt(512,h);
    SBytes bat(512,0xff); put(bat,0,4,4,true); put(bat,4,(2048+512+block)/512,4,true); d->writeAt(1536,bat);
    // Both blocks allocated, but only the first sector in each is logically present.
    d->writeAt(2048,SBytes{0x80}); d->writeAt(2048+512+3,SBytes{1,2,3}); d->writeAt(2048+512+block,SBytes{0x80});
    // Host bytes for an absent virtual sector must not leak through a read or compaction.
    d->writeAt(2048+512+512,SBytes{99}); return d;
}
std::uint32_t castagnoli(SReadOnlyByteSpan input) {
    std::uint32_t crc=UINT32_MAX; for (auto byte:input) { crc^=byte; for (unsigned bit=0;bit<8;++bit) { crc=(crc&1)?(crc>>1)^0x82f63b78:crc>>1; } } return ~crc;
}
std::shared_ptr<CMemoryDevice> vhdx() {
    constexpr std::size_t meg=1024*1024; auto disk=std::make_shared<CMemoryDevice>(12*meg); disk->writeAt(0,SBytes{'v','h','d','x','f','i','l','e'});
    SBytes header(4096); std::copy_n("head",4,header.begin()); header[16]=1; header[32]=2; put(header,66,1,2); put(header,68,meg,4); put(header,72,meg,8);
    for (unsigned copy=1;copy<=2;++copy) { put(header,8,copy,8); put(header,4,0,4); put(header,4,castagnoli(header),4); disk->writeAt(copy*65536,header); }
    SBytes regions(65536); std::copy_n("regi",4,regions.begin()); put(regions,8,2,4);
    const SBytes batGuid{0x66,0x77,0xc2,0x2d,0x23,0xf6,0,0x42,0x9d,0x64,0x11,0x5e,0x9b,0xfd,0x4a,8},metaGuid{6,0xa2,0x7c,0x8b,0x90,0x47,0x9a,0x4b,0xb8,0xfe,0x57,0x5f,5,0xf,0x88,0x6e};
    std::copy(batGuid.begin(),batGuid.end(),regions.begin()+16); put(regions,32,3*meg,8); put(regions,40,meg,4); put(regions,44,1,4);
    std::copy(metaGuid.begin(),metaGuid.end(),regions.begin()+48); put(regions,64,2*meg,8); put(regions,72,meg,4); put(regions,76,1,4);
    put(regions,4,castagnoli(regions),4); disk->writeAt(192*1024,regions); disk->writeAt(256*1024,regions);
    const std::array<SBytes,5> ids{{
        {0x37,0x67,0xa1,0xca,0x36,0xfa,0x43,0x4d,0xb3,0xb6,0x33,0xf0,0xaa,0x44,0xe7,0x6b},
        {0x24,0x42,0xa5,0x2f,0x1b,0xcd,0x76,0x48,0xb2,0x11,0x5d,0xbe,0xd8,0x3b,0xf4,0xb8},
        {0xab,0x12,0xca,0xbe,0xe6,0xb2,0x23,0x45,0x93,0xef,0xc3,9,0xe0,0,0xc7,0x46},
        {0x1d,0xbf,0x41,0x81,0x6f,0xa9,9,0x47,0xba,0x47,0xf2,0x33,0xa8,0xfa,0xab,0x5f},
        {0xc7,0x48,0xa3,0xcd,0x5d,0x44,0x71,0x44,0x9c,0xc9,0xe9,0x88,0x52,0x51,0xc5,0x56}}};
    SBytes metadata(65536+80); std::copy_n("metadata",8,metadata.begin()); put(metadata,10,5,2);
    for (std::size_t i=0;i<5;++i) { const auto at=32+i*32; std::copy(ids[i].begin(),ids[i].end(),metadata.begin()+at); put(metadata,at+16,65536+i*16,4); put(metadata,at+20,i==0 || i==1?8:i==2?16:4,4); put(metadata,at+24,i==0?4:6,4); }
    put(metadata,65536,2*meg,4); put(metadata,65552,4*meg,8); metadata[65568]=3; put(metadata,65584,512,4); put(metadata,65600,4096,4); disk->writeAt(2*meg,metadata);
    SBytes bat(16); put(bat,0,(4*meg)|6,8); put(bat,8,(8*meg)|6,8); disk->writeAt(3*meg,bat); disk->writeAt(4*meg+7,SBytes{4,5,6}); return disk;
}
DOCTEST_TEST_CASE("GPT writes validate before mutation and preserve data with redundant CRC protected tables") {
    for (unsigned sector:{512u,4096u}) {
        auto d=std::make_shared<CMemoryDevice>(sector*2048); SPartitionLayout layout; layout.logicalSectorSize=sector; layout.diskGuid[0]=19;
        layout.partitions={part(3,128,256),part(1,512,256)}; layout.partitions[0].name="data \xED\x95\x9C\xEA\xB8\x80";
        d->writeAt(128*sector,SBytes{4,5,6}); CPartitionTable::write(d,layout);
        SPartitionOpenOptions options; options.logicalSectorSize=sector; auto table=CPartitionTable::open(d,options);
        DOCTEST_CHECK(table->scheme()==EPARTITION_GPT); DOCTEST_CHECK(table->list(0).size()==2); DOCTEST_CHECK(table->list(0,1)[0].index==1);
        DOCTEST_CHECK(table->list(1,1)[0].name==layout.partitions[0].name); DOCTEST_CHECK(table->list(UINT64_MAX).empty());
        auto view=table->openPartition(3); DOCTEST_CHECK(view->isReadOnly()); DOCTEST_CHECK(view->readBytes(0,3)==SBytes{4,5,6});
        table->openPartition(3,true)->writeAt(1,SBytes{8}); DOCTEST_CHECK(view->readBytes(0,3)==SBytes{4,8,6});
        DOCTEST_CHECK_THROWS(CPartitionTable::write(d,layout)); auto before=d->readBytes(0,sector*40);
        auto bad=layout; bad.partitions[0].firstLba=510; DOCTEST_CHECK_THROWS(CPartitionTable::write(d,bad,{true})); DOCTEST_CHECK(d->readBytes(0,sector*40)==before);
        d->writeAt(sector+16,SBytes{0}); DOCTEST_CHECK_THROWS(CPartitionTable::open(d,options));
        options.useBackupGpt=true; auto backup=CPartitionTable::open(d,options); DOCTEST_CHECK(backup->usesBackupGpt());
        asyncpp::thread_pool worker(1); asyncpp::as_promise(CPartitionTable::writeAsync(d,layout,worker,{true})).get(); options.useBackupGpt=false;
        DOCTEST_CHECK(asyncpp::as_promise(CPartitionTable::openAsync(d,worker,options)).get()->list(0).size()==2);
    }
}
DOCTEST_TEST_CASE("MBR EBR creation and traversal use correct relative bases and reject cycles and overlap") {
    auto d=std::make_shared<CMemoryDevice>(4096*512); SPartitionLayout l; l.scheme=EPARTITION_MBR;
    auto one=part(5,1025,300); one.ebrLba=1024; auto two=part(6,2049,300); two.ebrLba=2048;
    l.partitions={part(1,128,512),part(2,1024,2048,15),one,two}; CPartitionTable::write(d,l); auto t=CPartitionTable::open(d);
    auto records=t->list(0); DOCTEST_REQUIRE(records.size()==4); DOCTEST_CHECK(records[1].extendedContainer); DOCTEST_CHECK(records[3].ebrLba==2048);
    DOCTEST_CHECK(records[3].firstLba==2049); DOCTEST_CHECK_THROWS(t->openPartition(2));
    auto invalid=l; invalid.partitions[3].ebrLba=1024; auto before=d->readBytes(0,2048*512); DOCTEST_CHECK_THROWS(CPartitionTable::write(d,invalid,{true})); DOCTEST_CHECK(d->readBytes(0,2048*512)==before);
    // Second EBR links back to the extended base, not to the current EBR.
    SBytes cycle(16); cycle[4]=15; put(cycle,12,2048,4); d->writeAt(2048*512+462,cycle); DOCTEST_CHECK_THROWS(CPartitionTable::open(d));
}
DOCTEST_TEST_CASE("GPT sixty four bit LBAs preserve partitions beyond the MBR two TiB boundary") {
    auto d=std::make_shared<SparseDisk>((UINT64_C(1)<<42)+1024*1024); SPartitionLayout l; l.diskGuid[0]=7;
    l.partitions={part(1,(UINT64_C(1)<<32)+128,256)}; CPartitionTable::write(d,l); auto table=CPartitionTable::open(d);
    DOCTEST_CHECK(table->list(0)[0].firstLba==(UINT64_C(1)<<32)+128); DOCTEST_CHECK(table->list(0)[0].byteOffset>((UINT64_C(1)<<41)));
    auto view=table->openPartition(1,true); view->writeAt(1,SBytes{1,2,3}); DOCTEST_CHECK(view->readBytes(1,3)==SBytes{1,2,3});
    DOCTEST_CHECK_THROWS(view->writeAt(view->size(),SBytes{4}));
}
DOCTEST_TEST_CASE("ISO tree exposes lazy paged files and async reads without mutable capabilities") {
    auto d=iso(); auto tree=std::dynamic_pointer_cast<ITreeReader>(IFileSystem::open(d)); DOCTEST_REQUIRE(tree);
    DOCTEST_CHECK(tree->format()==EFORMAT_ISO9660); DOCTEST_CHECK(tree->list("/",0,1)[0].path=="HELLO.TXT"); DOCTEST_CHECK(tree->list("/",1,1).empty());
    DOCTEST_CHECK(tree->stat("HELLO.TXT").size==5); auto f=tree->openFile("HELLO.TXT"); SBytes out(9); DOCTEST_CHECK(f->readAt(2,out)==3); DOCTEST_CHECK(out[0]=='l');
    DOCTEST_CHECK(f->readAt(UINT64_MAX,out)==0); DOCTEST_CHECK_THROWS(tree->stat("../HELLO.TXT")); DOCTEST_CHECK_THROWS(tree->list("/",0,4097));
    DOCTEST_CHECK(std::dynamic_pointer_cast<ITreeEditor>(tree)==nullptr); asyncpp::thread_pool worker(1);
    auto asyncTree=asyncpp::as_promise(CTreeReaderAsync::open(d,worker)).get(); CTreeReaderAsync async(asyncTree,worker);
    DOCTEST_CHECK(asyncpp::as_promise(async.readFile("HELLO.TXT",0,5)).get()==SBytes{'h','e','l','l','o'});
    d->writeAt(16*2048+84,SBytes{1}); DOCTEST_CHECK_THROWS(IFileSystem::open(d));
}
DOCTEST_TEST_CASE("mapped file windows change at runtime and cross small windows without data loss") {
    const auto path=std::filesystem::temp_directory_path()/("hnfs-mapping-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".img");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove(path,e); } } cleanup{path};
    auto d=CFileDevice::create(path,3*1024*1024+19,{4096}); SBytes b(2*1024*1024+9);
    for (std::size_t i=0;i<b.size();++i) { b[i]=static_cast<std::uint8_t>(i*37); }
    d->writeAt(4091,b); d->setMappingOptions({65537}); DOCTEST_CHECK(d->mappingOptions().windowBytes==65537); DOCTEST_CHECK(d->readBytes(4091,b.size())==b);
    DOCTEST_CHECK_THROWS(d->setMappingOptions({0})); DOCTEST_CHECK(d->mappingOptions().windowBytes==65537); d->flush(); d.reset();
    auto r=CFileDevice::open(path,false,{8192}); DOCTEST_CHECK(r->readBytes(4091,b.size())==b);
}
DOCTEST_TEST_CASE("ISO level three reads multi extent files larger than four GiB without image allocation") {
    auto disk=std::make_shared<SparseDisk>(UINT64_C(6)*1024*1024*1024); auto base=iso(); disk->writeAt(0,base->readBytes(0,static_cast<std::size_t>(base->size())));
    auto descriptor=disk->readBytes(16*2048,2048); both(descriptor,80,disk->size()/2048,4); disk->writeAt(16*2048,descriptor);
    constexpr std::uint32_t firstSize=0xfffff800,firstLba=30,secondLba=firstLba+firstSize/2048+5;
    SBytes records(2048); auto a=isoRecord(firstLba,firstSize,{'B','I','G',';','1'},0x80),b=isoRecord(secondLba,4096,{'B','I','G',';','1'});
    std::copy(a.begin(),a.end(),records.begin()); std::copy(b.begin(),b.end(),records.begin()+a.size()); disk->writeAt(20*2048,records);
    disk->writeAt(std::uint64_t(firstLba)*2048+firstSize-2,SBytes{1,2}); disk->writeAt(std::uint64_t(secondLba)*2048,SBytes{3,4,5});
    auto tree=COpticalFileSystem::open(disk); auto file=tree->openFile("BIG"); DOCTEST_CHECK(file->size()==std::uint64_t(firstSize)+4096);
    SBytes data(5); DOCTEST_CHECK(file->readAt(std::uint64_t(firstSize)-2,data)==5); DOCTEST_CHECK(data==SBytes{1,2,3,4,5});
    SOpenOptions options; options.maximumExtents=1; auto limited=COpticalFileSystem::open(disk,options); DOCTEST_CHECK_THROWS(limited->stat("BIG"));
    DOCTEST_CHECK_THROWS(disk->readBytes(UINT64_MAX,5));
}
DOCTEST_TEST_CASE("virtual VHD bitmap holes checksums fixed images and copy out compaction preserve virtual bytes") {
    auto dynamic=vhd(true); auto disk=CVirtualDisk::open(dynamic); DOCTEST_CHECK(disk->isDynamic()); DOCTEST_CHECK(disk->isReadOnly());
    DOCTEST_CHECK(disk->readBytes(3,3)==SBytes{1,2,3}); DOCTEST_CHECK(disk->readBytes(512,1)==SBytes{0});
    DOCTEST_CHECK_THROWS(disk->writeAt(0,SBytes{3})); DOCTEST_CHECK_THROWS(disk->readBytes(disk->size(),1));
    auto fixed=CVirtualDisk::open(vhd(false)); DOCTEST_CHECK_FALSE(fixed->isDynamic()); DOCTEST_CHECK(fixed->readBytes(3,3)==SBytes{1,2,3});
    const auto path=std::filesystem::temp_directory_path()/("hnfs-compact-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".vhd");
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code e; std::filesystem::remove(p,e); } } cleanup{path};
    asyncpp::thread_pool worker(1); auto result=asyncpp::as_promise(disk->compactToFileAsync(path,worker)).get(); DOCTEST_CHECK(result.imageBytes<dynamic->size()); DOCTEST_CHECK(result.removedBlocks==1);
    auto packed=CVirtualDisk::openFile(path); packed->setMappingOptions({4096}); DOCTEST_CHECK(packed->mappingOptions().windowBytes==4096);
    DOCTEST_CHECK(packed->size()==disk->size()); DOCTEST_CHECK(packed->readBytes(0,1024)==disk->readBytes(0,1024));
    DOCTEST_CHECK(packed->readBytes(2*1024*1024,1024)==SBytes(1024)); DOCTEST_CHECK_THROWS(disk->compactToFile(path));
    dynamic->writeAt(dynamic->size()-512+64,SBytes{0}); DOCTEST_CHECK_THROWS(CVirtualDisk::open(dynamic));
}
DOCTEST_TEST_CASE("VHDX headers BAT metadata checksums and zero block compaction work across mapped windows") {
    const SBytes vector{'1','2','3','4','5','6','7','8','9'}; DOCTEST_CHECK(castagnoli(vector)==0xe3069283);
    auto image=vhdx(); auto disk=CVirtualDisk::open(image); DOCTEST_CHECK(disk->virtualFormat()==EVIRTUAL_VHDX); DOCTEST_CHECK(disk->logicalSectorSize()==512);
    DOCTEST_CHECK(disk->readBytes(7,3)==SBytes{4,5,6}); DOCTEST_CHECK(disk->readBytes(2*1024*1024-2,4)==SBytes(4));
    const auto path=std::filesystem::temp_directory_path()/("hnfs-compact-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".vhdx");
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code e; std::filesystem::remove(p,e); } } cleanup{path};
    auto result=disk->compactToFile(path,{true,{4096}}); DOCTEST_CHECK(result.imageBytes==6*1024*1024); DOCTEST_CHECK(result.retainedBlocks==1); DOCTEST_CHECK(result.removedBlocks==1);
    asyncpp::thread_pool worker(1); auto packed=asyncpp::as_promise(CVirtualDisk::openFileAsync(path,worker)).get(); DOCTEST_CHECK(packed->readBytes(0,1024)==disk->readBytes(0,1024));
    image->writeAt(131072,SBytes{'B'}); DOCTEST_CHECK(CVirtualDisk::open(image)->readBytes(7,3)==SBytes{4,5,6});
    image->writeAt(65536,SBytes{'B'}); DOCTEST_CHECK_THROWS(CVirtualDisk::open(image));
    auto malformed=vhdx(); SBytes entry(8); put(entry,0,(2*1024*1024)|6,8); malformed->writeAt(3*1024*1024,entry); DOCTEST_CHECK_THROWS(CVirtualDisk::open(malformed));
}
} // namespace
