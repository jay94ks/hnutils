#include <hnfs.hpp>
#include <doctest/doctest.h>
#include <asyncpp/thread_pool.h>
#include <asyncpp/sync_wait.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>
#include <type_traits>

namespace {
using namespace hnfs;
constexpr EFormat FORMATS[]{EFORMAT_CPIO_BINARY_LE,EFORMAT_CPIO_BINARY_BE,EFORMAT_CPIO_ODC,EFORMAT_CPIO_NEWC,EFORMAT_CPIO_CRC};
SBytes bytes(const std::string& text) { return {text.begin(),text.end()}; }
std::shared_ptr<CMemoryDevice> image(EFormat format,const SBytes& content) {
    auto device=std::make_shared<CMemoryDevice>(content.size()+8192);
    auto builder=IArchiveBuilder::create(device,{format});
    builder->beginEntry({"docs",ENODE_DIRECTORY,0,0755,123,456,1001});
    builder->beginEntry({"docs/data.bin",ENODE_FILE,content.size(),0640,123,456,1001});
    for (std::size_t at=0;at<content.size();) { const auto count=std::min<std::size_t>(37,content.size()-at); builder->write(SReadOnlyByteSpan(content).subspan(at,count)); at+=count; }
    builder->beginEntry({"empty",ENODE_FILE}); builder->finish(); builder->finish(); return device;
}
SBytes collect(const std::shared_ptr<IArchiveReader>& reader) {
    std::array<std::uint8_t,113> buffer{}; SBytes result;
    for (;;) { const auto count=reader->read(buffer); if (!count) { break; } result.insert(result.end(),buffer.begin(),buffer.begin()+count); } return result;
}
asyncpp::task<std::thread::id> resumeAfterRead(const CDeviceAsync& device) {
    co_await device.readAt(0,6); co_return std::this_thread::get_id();
}
struct Files {
    std::filesystem::path directory;
    Files() {
        static std::atomic<unsigned> serial{0};
        for (;;) {
            directory=std::filesystem::temp_directory_path()/("hnfs-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++));
            if (std::filesystem::create_directory(directory)) { break; }
        }
    }
    ~Files() { std::error_code error; for (const auto& file:std::filesystem::directory_iterator(directory,error)) { std::filesystem::remove(file.path(),error); } std::filesystem::remove(directory,error); }
};

class FailingDevice final : public IBlockDevice {
private:
    std::shared_ptr<CMemoryDevice> _memory;
    bool _fail=false;
public:
    explicit FailingDevice(std::shared_ptr<CMemoryDevice> memory) : _memory(std::move(memory)) {}
    void failIo(bool value) { _fail=value; }
    std::uint64_t size() const noexcept override { return _memory->size(); }
    bool isReadOnly() const noexcept override { return false; }
    void readAt(std::uint64_t offset,SByteSpan output) const override {
        if (_fail) { throw std::system_error(std::make_error_code(std::errc::io_error)); } _memory->readAt(offset,output);
    }
    void writeAt(std::uint64_t offset,SReadOnlyByteSpan input) override {
        if (_fail) { throw std::system_error(std::make_error_code(std::errc::io_error)); } _memory->writeAt(offset,input);
    }
    void flush() override { if (_fail) { throw std::system_error(std::make_error_code(std::errc::io_error)); } }
};

class RejectingWorker final : public asyncpp::dispatcher {
public:
    void push(std::function<void()>) override { throw std::runtime_error("worker rejected task"); }
};

DOCTEST_TEST_CASE("device ownership bounds views and immutable policy are enforced") {
    SBytes input{1,2,3,4,5,6}; auto owned=std::make_shared<CMemoryDevice>(input); input.clear();
    DOCTEST_CHECK(owned->readBytes(0,6)==SBytes{1,2,3,4,5,6});
    DOCTEST_CHECK_THROWS_AS(owned->writeAt(0,SBytes{9}),CFileSystemError);
    CDeviceView immutable(owned,2,3,false); DOCTEST_CHECK(immutable.isReadOnly());
    auto memory=std::make_shared<CMemoryDevice>(10); CDeviceView view(memory,2,4,false);
    view.writeAt(1,SBytes{7,8}); DOCTEST_CHECK(memory->readBytes(0,10)==SBytes{0,0,0,7,8,0,0,0,0,0});
    DOCTEST_CHECK_THROWS_AS(view.writeAt(3,SBytes{7,8}),CFileSystemError);
    DOCTEST_CHECK_THROWS_AS(memory->readBytes(UINT64_MAX,1),CFileSystemError);
    DOCTEST_CHECK_THROWS_AS(CDeviceView(memory,UINT64_MAX,1),CFileSystemError);
    std::array<std::uint8_t,1> byte{}; DOCTEST_CHECK_THROWS_AS(view.readAt(4,byte),CFileSystemError);
    memory->readAt(10,{}); view.writeAt(4,{}); DOCTEST_CHECK_THROWS_AS(memory->writeAt(11,{}),CFileSystemError);
    DOCTEST_CHECK_THROWS_AS(CDeviceView(nullptr,0,0),std::invalid_argument);
}
DOCTEST_TEST_CASE("regular file devices use exact positioned IO exclusive creation and Unicode paths") {
    Files files; const auto path=files.directory/std::filesystem::path(u8"\uD30C\uC77C.img");
    constexpr std::uint64_t size=1024*1024+43; auto device=CFileDevice::create(path,size);
    DOCTEST_CHECK(device->size()==size); DOCTEST_CHECK(device->readBytes(size-3,3)==SBytes(3));
    device->writeAt(size-3,SBytes{1,2,3}); device->flush();
    DOCTEST_CHECK_THROWS(CFileDevice::create(path,4)); device.reset();
    auto reader=CFileDevice::open(path); DOCTEST_CHECK(reader->isReadOnly());
    DOCTEST_CHECK(reader->readBytes(size-3,3)==SBytes{1,2,3}); DOCTEST_CHECK_THROWS_AS(reader->writeAt(0,SBytes{9}),CFileSystemError);
    DOCTEST_CHECK_THROWS(CFileDevice::open(files.directory));
#if !defined(_WIN32)
    reader.reset(); const auto link=files.directory/"symlink.img"; std::filesystem::create_symlink(path,link); DOCTEST_CHECK_THROWS(CFileDevice::open(link));
    const auto sparse=files.directory/"sparse.img"; auto large=CFileDevice::create(sparse,(UINT64_C(1)<<32)+13);
    large->writeAt((UINT64_C(1)<<32)+9,SBytes{4,5,6}); DOCTEST_CHECK(large->readBytes((UINT64_C(1)<<32)+9,3)==SBytes{4,5,6});
#endif
}
DOCTEST_TEST_CASE("format recognition never fabricates unavailable tree or mutable drivers") {
    static_assert(std::is_base_of_v<ITreeReader,ITreeEditor>);
    static_assert(std::is_base_of_v<IFileReader,IFileEditor>);
    static_assert(std::is_base_of_v<IFileSystem,IVolumeContainer>);
    static_assert(!std::is_base_of_v<ITreeReader,IArchiveReader>);
    for (int id=EFORMAT_FAT12;id<=EFORMAT_APFS;++id) {
        const auto format=static_cast<EFormat>(id); const auto support=IFileSystem::support(format);
        DOCTEST_CHECK(support.probe); DOCTEST_CHECK_FALSE(support.treeRead); DOCTEST_CHECK_FALSE(support.treeEdit);
        DOCTEST_CHECK(support.archiveRead==(format>=EFORMAT_CPIO_BINARY_LE && format<=EFORMAT_CPIO_CRC));
        DOCTEST_CHECK(support.archiveIndexRead==support.archiveRead);
    }
    auto raw=std::make_shared<CMemoryDevice>(128*1024); raw->writeAt(0,bytes("XFSB"));
    DOCTEST_CHECK(IFileSystem::probe(*raw).format==EFORMAT_XFS); auto before=raw->readBytes(0,16);
    DOCTEST_CHECK_THROWS_AS(IFileSystem::open(raw),CFileSystemError);
    DOCTEST_CHECK_THROWS_AS(IArchiveBuilder::create(raw,{EFORMAT_XFS,true}),CFileSystemError);
    DOCTEST_CHECK(raw->readBytes(0,16)==before);
    auto cpio=image(EFORMAT_CPIO_NEWC,bytes("data")); auto object=IFileSystem::open(cpio);
    DOCTEST_CHECK(std::dynamic_pointer_cast<IArchiveReader>(object)!=nullptr);
    DOCTEST_CHECK(std::dynamic_pointer_cast<ITreeReader>(object)==nullptr);
    DOCTEST_CHECK(std::dynamic_pointer_cast<ITreeEditor>(object)==nullptr);
    DOCTEST_CHECK(std::dynamic_pointer_cast<IVolumeContainer>(object)==nullptr);
    for (std::size_t n=0;n<1200;++n) { CMemoryDevice tiny(n); DOCTEST_CHECK(IFileSystem::probe(tiny).format==EFORMAT_UNKNOWN); }
}
DOCTEST_TEST_CASE("CPIO legacy and modern codecs stream metadata data and validated EOF") {
    SBytes payload(65549); for (std::size_t i=0;i<payload.size();++i) { payload[i]=static_cast<std::uint8_t>(i*37); }
    for (auto format:FORMATS) {
        DOCTEST_INFO(formatName(format)); auto device=image(format,payload); auto reader=IArchiveReader::open(device);
        DOCTEST_CHECK(reader->format()==format); DOCTEST_CHECK_THROWS_AS(reader->usedBytes(),CFileSystemError);
        auto dir=reader->next(); DOCTEST_REQUIRE(dir); DOCTEST_CHECK(dir->path=="docs"); DOCTEST_CHECK(dir->kind==ENODE_DIRECTORY);
        auto file=reader->next(); DOCTEST_REQUIRE(file); DOCTEST_CHECK(file->path=="docs/data.bin"); DOCTEST_CHECK(file->permissions==0640);
        DOCTEST_CHECK(file->uid==123); DOCTEST_CHECK(file->gid==456); DOCTEST_CHECK(file->modifiedTime==1001);
        DOCTEST_CHECK(collect(reader)==payload); auto empty=reader->next(); DOCTEST_REQUIRE(empty); DOCTEST_CHECK(collect(reader).empty());
        DOCTEST_CHECK_FALSE(reader->next()); DOCTEST_CHECK_FALSE(reader->next()); DOCTEST_CHECK(reader->usedBytes()<device->size());
        auto skipped=IArchiveReader::open(device); DOCTEST_REQUIRE(skipped->next()); DOCTEST_REQUIRE(skipped->next());
        DOCTEST_REQUIRE(skipped->next()); DOCTEST_CHECK_FALSE(skipped->next());
        SOpenOptions policy; policy.maximumDataBytes=payload.size()-1;
        auto limited=IArchiveReader::open(device,policy); DOCTEST_REQUIRE(limited->next()); DOCTEST_CHECK_THROWS_AS(limited->next(),CFileSystemError);
    }
}
DOCTEST_TEST_CASE("CPIO file images reopen read-only after the explicit builder finish") {
    Files files; const auto path=files.directory/"archive.cpio";
    auto file=CFileDevice::create(path,8192); auto builder=IArchiveBuilder::create(file,{EFORMAT_CPIO_CRC});
    builder->beginEntry({"data",ENODE_FILE,4}); builder->write(bytes("disk")); builder->finish();
    const auto length=builder->usedBytes(); builder.reset(); file.reset();
    auto disk=CFileDevice::open(path); auto view=std::make_shared<CDeviceView>(disk,0,length);
    auto reader=IArchiveReader::open(view); DOCTEST_REQUIRE(reader->next()); DOCTEST_CHECK(collect(reader)==bytes("disk"));
    DOCTEST_CHECK_FALSE(reader->next()); DOCTEST_CHECK(reader->usedBytes()==length);
}
DOCTEST_TEST_CASE("an explicit archive index supplies readonly tree and file views without changing native cursor capabilities") {
    for (auto format:FORMATS) {
        DOCTEST_INFO(format); auto device=image(format,bytes("payload")); auto tree=CIndexedArchive::open(device);
        DOCTEST_CHECK(tree->format()==format); DOCTEST_CHECK(tree->stat("/").kind==ENODE_DIRECTORY);
        DOCTEST_CHECK(tree->stat("/docs/data.bin").size==7);
        auto root=tree->list("/",0,1); DOCTEST_REQUIRE(root.size()==1); DOCTEST_CHECK(root[0].path=="docs");
        auto copied=tree->stat("docs/data.bin"); copied.path="changed"; DOCTEST_CHECK(tree->stat("docs/data.bin").path=="docs/data.bin");
        DOCTEST_CHECK(std::dynamic_pointer_cast<ITreeEditor>(tree)==nullptr);
        DOCTEST_CHECK(std::dynamic_pointer_cast<IArchiveReader>(tree)==nullptr);
        DOCTEST_CHECK_THROWS_AS(tree->openFile("docs"),CFileSystemError);
        DOCTEST_CHECK_THROWS_AS(tree->list("docs/data.bin",0,1),CFileSystemError);
        DOCTEST_CHECK_THROWS_AS(tree->stat("missing"),CFileSystemError);
        DOCTEST_CHECK_THROWS_AS(tree->stat("../bad"),CFileSystemError);
        auto file=tree->openFile("docs/data.bin"); DOCTEST_CHECK(file->size()==7);
        std::weak_ptr<IBlockDevice> weak=device; tree.reset(); device.reset();
        std::array<std::uint8_t,20> data{}; const auto count=file->readAt(2,data);
        DOCTEST_CHECK(count==5); DOCTEST_CHECK(SBytes(data.begin(),data.begin()+count)==bytes("yload"));
        DOCTEST_CHECK(file->readAt(UINT64_MAX,data)==0); file.reset(); DOCTEST_CHECK(weak.expired());
    }
}
DOCTEST_TEST_CASE("archive tree pagination and synthesized ancestor metadata stay bounded") {
    auto device=std::make_shared<CMemoryDevice>(256*1024); auto builder=IArchiveBuilder::create(device);
    for (unsigned i=0;i<600;++i) { const auto name="dir/sub/file"+std::to_string(10000+i).substr(1); builder->beginEntry({name,ENODE_FILE}); }
    builder->finish(); auto tree=CIndexedArchive::open(device);
    DOCTEST_CHECK(tree->stat("dir").kind==ENODE_DIRECTORY); DOCTEST_CHECK(tree->stat("dir/sub").kind==ENODE_DIRECTORY);
    DOCTEST_CHECK(tree->list("dir/sub",0).size()==128); DOCTEST_CHECK(tree->list("dir/sub",0,0).empty());
    DOCTEST_CHECK(tree->list("dir/sub",600,128).empty()); DOCTEST_CHECK(tree->list("dir/sub",UINT64_MAX,4096).empty());
    auto last=tree->list("dir/sub",599,128); DOCTEST_REQUIRE(last.size()==1); DOCTEST_CHECK(last[0].path=="dir/sub/file0599");
    last[0].path="mutated"; DOCTEST_CHECK(tree->list("dir/sub",599,1)[0].path=="dir/sub/file0599");
    DOCTEST_CHECK_THROWS_AS(tree->list("dir/sub",0,4097),CFileSystemError);
    auto nested=std::make_shared<CMemoryDevice>(8192); auto output=IArchiveBuilder::create(nested);
    output->beginEntry({"a/b/c",ENODE_FILE}); output->finish();
    SOpenOptions limits; limits.maximumEntries=2; DOCTEST_CHECK_THROWS_AS(CIndexedArchive::open(nested,limits),CFileSystemError);
    limits.maximumEntries=10; limits.maximumNameBytes=8; DOCTEST_CHECK_THROWS_AS(CIndexedArchive::open(nested,limits),CFileSystemError);
}
DOCTEST_TEST_CASE("archive tree indexing rejects ambiguous duplicates and validates payload checksums before returning") {
    auto device=std::make_shared<CMemoryDevice>(8192); auto writer=IArchiveBuilder::create(device,{EFORMAT_CPIO_CRC});
    writer->beginEntry({"a",ENODE_FILE,3}); writer->write(SBytes{1,2,3}); writer->finish();
    device->writeAt(112,SBytes{9}); DOCTEST_CHECK_THROWS_AS(CIndexedArchive::open(device),CFileSystemError);
    auto duplicate=std::make_shared<CMemoryDevice>(8192); auto output=IArchiveBuilder::create(duplicate);
    output->beginEntry({"a",ENODE_FILE}); output->beginEntry({"a",ENODE_FILE}); output->finish();
    auto cursor=IArchiveReader::open(duplicate); DOCTEST_REQUIRE(cursor->next()); DOCTEST_REQUIRE(cursor->next()); DOCTEST_CHECK_FALSE(cursor->next());
    DOCTEST_CHECK_THROWS_AS(CIndexedArchive::open(duplicate),CFileSystemError);
    asyncpp::thread_pool worker(1); auto valid=image(EFORMAT_CPIO_CRC,bytes("async tree"));
    auto tree=asyncpp::as_promise(CTreeReaderAsync::indexArchive(valid,worker)).get();
    auto async=std::make_unique<CTreeReaderAsync>(tree,worker);
    DOCTEST_CHECK(asyncpp::as_promise(async->stat("docs")).get().kind==ENODE_DIRECTORY);
    DOCTEST_CHECK(asyncpp::as_promise(async->list("docs",0,1)).get().size()==1);
    auto read=async->readFile("docs/data.bin",6,100); async.reset(); tree.reset(); valid.reset();
    DOCTEST_CHECK(asyncpp::as_promise(std::move(read)).get()==bytes("tree"));
}
DOCTEST_TEST_CASE("CPIO builder errors limits and overwrite policy fail before unsafe writes") {
    for (auto format:FORMATS) {
        DOCTEST_INFO(format); auto device=std::make_shared<CMemoryDevice>(8192);
        auto builder=IArchiveBuilder::create(device,{format});
        for (const auto& path:{"../bad","/root","a/../b","a//b","C:/file","a\\b","TRAILER!!!"}) {
            DOCTEST_CHECK_THROWS_AS(builder->beginEntry({path,ENODE_FILE}),CFileSystemError);
        }
        DOCTEST_CHECK(builder->usedBytes()==0); DOCTEST_CHECK(device->readBytes(0,128)==SBytes(128));
        DOCTEST_CHECK_THROWS_AS(builder->beginEntry({"too-large",ENODE_FILE,8192}),CFileSystemError);
        DOCTEST_CHECK(builder->usedBytes()==0);
        builder->beginEntry({"file",ENODE_FILE,2}); DOCTEST_CHECK_THROWS_AS(builder->finish(),CFileSystemError);
        DOCTEST_CHECK_THROWS_AS(builder->write(SBytes{1,2,3}),CFileSystemError); builder->write(SBytes{1,2}); builder->finish();
        DOCTEST_CHECK_THROWS_AS(builder->beginEntry({"late",ENODE_FILE}),CFileSystemError);
        const auto before=device->readBytes(0,8192);
        DOCTEST_CHECK_THROWS_AS(IArchiveBuilder::create(device,{format}),CFileSystemError); DOCTEST_CHECK(device->readBytes(0,8192)==before);
        auto replace=IArchiveBuilder::create(device,{format,true}); replace->finish(); auto empty=IArchiveReader::open(device); DOCTEST_CHECK_FALSE(empty->next());
        auto immutable=std::make_shared<CMemoryDevice>(8192,true); DOCTEST_CHECK_THROWS_AS(IArchiveBuilder::create(immutable,{format}),CFileSystemError);
    }
    auto device=std::make_shared<CMemoryDevice>(8192); auto legacy=IArchiveBuilder::create(device,{EFORMAT_CPIO_BINARY_LE});
    SEntry bad{"file",ENODE_FILE}; bad.uid=65536; DOCTEST_CHECK_THROWS_AS(legacy->beginEntry(bad),CFileSystemError);
    DOCTEST_CHECK(legacy->usedBytes()==0);
}
DOCTEST_TEST_CASE("CPIO CRC verifies unread skipped data and rejects corruption truncation and trailing records") {
    auto device=std::make_shared<CMemoryDevice>(8192); auto builder=IArchiveBuilder::create(device,{EFORMAT_CPIO_CRC});
    builder->beginEntry({"a",ENODE_FILE,3}); builder->write(SBytes{1,2,3}); builder->finish();
    const auto length=builder->usedBytes(); auto packed=device->readBytes(0,static_cast<std::size_t>(length));
    auto damaged=packed; damaged[112]^=1; auto corrupt=IArchiveReader::open(std::make_shared<CMemoryDevice>(damaged));
    DOCTEST_REQUIRE(corrupt->next());
    try { corrupt->next(); DOCTEST_FAIL("unread damaged data accepted"); }
    catch (const CFileSystemError& error) { DOCTEST_CHECK(error.code()==EERROR_CHECKSUM); }
    DOCTEST_CHECK_THROWS_AS(corrupt->next(),CFileSystemError);
    for (auto count:{std::size_t(6),std::size_t(110),std::size_t(113),packed.size()-1}) {
        auto reader=IArchiveReader::open(std::make_shared<CMemoryDevice>(SReadOnlyByteSpan(packed).first(count)));
        DOCTEST_CHECK_THROWS_AS(([&] { while (reader->next()) { collect(reader); } }()),CFileSystemError);
    }
    auto trailing=packed; trailing.push_back(1); auto reader=IArchiveReader::open(std::make_shared<CMemoryDevice>(trailing));
    DOCTEST_REQUIRE(reader->next()); DOCTEST_CHECK_THROWS_AS(reader->next(),CFileSystemError);
}
DOCTEST_TEST_CASE("async capabilities own input lifetimes propagate failures and resume on workers") {
    asyncpp::thread_pool worker(1); auto device=std::make_shared<CMemoryDevice>(8192);
    auto builder=asyncpp::as_promise(CArchiveBuilderAsync::create(device,{},worker)).get();
    auto adapter=std::make_unique<CArchiveBuilderAsync>(builder,worker);
    asyncpp::as_promise(adapter->beginEntry({"data",ENODE_FILE,3})).get();
    SBytes data{4,5,6}; auto write=adapter->write(data); data.clear(); adapter.reset(); builder.reset(); asyncpp::as_promise(std::move(write)).get();
    // The pending task retains the builder; retain one owner for the separate finish operation.
    auto second=std::make_shared<CMemoryDevice>(8192); auto owned=IArchiveBuilder::create(second);
    CArchiveBuilderAsync output(owned,worker); asyncpp::as_promise(output.beginEntry({"data",ENODE_FILE,3})).get();
    asyncpp::as_promise(output.write(SBytes{4,5,6})).get(); asyncpp::as_promise(output.finish()).get();
    auto reader=asyncpp::as_promise(CArchiveReaderAsync::open(second,worker)).get(); CArchiveReaderAsync input(reader,worker);
    auto entry=asyncpp::as_promise(input.next()).get(); DOCTEST_REQUIRE(entry); DOCTEST_CHECK(entry->path=="data");
    DOCTEST_CHECK(asyncpp::as_promise(input.read()).get()==SBytes{4,5,6}); DOCTEST_CHECK_FALSE(asyncpp::as_promise(input.next()).get());
    auto dev=std::make_unique<CDeviceAsync>(second,worker); auto operation=dev->readAt(0,6); dev.reset();
    DOCTEST_CHECK(asyncpp::as_promise(std::move(operation)).get()==bytes("070701"));
    CDeviceAsync readonly(std::make_shared<CMemoryDevice>(8,true),worker);
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(readonly.writeAt(0,SBytes{1})).get(),CFileSystemError);
    auto limits=CArchiveReaderAsync(IArchiveReader::open(second),worker);
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(limits.read(64*1024*1024+1)).get(),CFileSystemError);
    CDeviceAsync scheduled(second,worker);
    DOCTEST_CHECK(asyncpp::as_promise(resumeAfterRead(scheduled)).get()!=std::this_thread::get_id());
}
DOCTEST_TEST_CASE("CPIO I/O failures poison cursors and builders without hiding native errors") {
    auto memory=std::make_shared<CMemoryDevice>(8192); auto device=std::make_shared<FailingDevice>(memory);
    auto builder=IArchiveBuilder::create(device); builder->beginEntry({"data",ENODE_FILE,1}); device->failIo(true);
    DOCTEST_CHECK_THROWS_AS(builder->write(SBytes{1}),std::system_error); device->failIo(false);
    DOCTEST_CHECK_THROWS_AS(builder->finish(),CFileSystemError); DOCTEST_CHECK_THROWS_AS(builder->write(SBytes{1}),CFileSystemError);
    auto valid=image(EFORMAT_CPIO_CRC,bytes("contents")); auto input=std::make_shared<FailingDevice>(valid);
    auto reader=IArchiveReader::open(input); input->failIo(true); DOCTEST_CHECK_THROWS_AS(reader->next(),std::system_error);
    input->failIo(false); DOCTEST_CHECK_THROWS_AS(reader->next(),CFileSystemError);
    RejectingWorker worker; CDeviceAsync async(valid,worker);
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(async.readAt(0,6)).get(),std::runtime_error);
}
DOCTEST_TEST_CASE("CPIO trailing padding has an independent bound and accepts an exact image view") {
    auto memory=std::make_shared<CMemoryDevice>(8192); auto builder=IArchiveBuilder::create(memory);
    builder->beginEntry({"data",ENODE_FILE,1}); builder->write(SBytes{1}); builder->finish();
    SOpenOptions options; options.maximumTrailingPaddingBytes=0;
    auto reader=IArchiveReader::open(memory,options); DOCTEST_REQUIRE(reader->next());
    DOCTEST_CHECK_THROWS_AS(reader->next(),CFileSystemError);
    auto view=std::make_shared<CDeviceView>(memory,0,builder->usedBytes()); auto bounded=IArchiveReader::open(view,options);
    DOCTEST_REQUIRE(bounded->next()); DOCTEST_CHECK_FALSE(bounded->next());
    auto malformed=memory->readBytes(0,static_cast<std::size_t>(builder->usedBytes()));
    std::fill_n(malformed.begin()+94,8,'f'); auto invalid=IArchiveReader::open(std::make_shared<CMemoryDevice>(malformed));
    DOCTEST_CHECK_THROWS_AS(invalid->next(),CFileSystemError);
}
DOCTEST_TEST_CASE("bounded CPIO mutation corpus either parses or reports a format error") {
    std::minstd_rand random(14917);
    for (auto format:FORMATS) {
        const auto device=image(format,bytes("payload")); const auto valid=device->readBytes(0,512);
        for (unsigned iteration=0;iteration<100;++iteration) {
            auto input=valid; for (unsigned mutation=0;mutation<3;++mutation) { input[random()%input.size()]^=static_cast<std::uint8_t>(1u<<(random()%8)); }
            try {
                SOpenOptions policy; policy.maximumEntries=20; policy.maximumDataBytes=65536;
                auto reader=IArchiveReader::open(std::make_shared<CMemoryDevice>(input),policy); while (reader->next()) { collect(reader); }
            } catch (const CFileSystemError&) {}
        }
    }
}
} // namespace
