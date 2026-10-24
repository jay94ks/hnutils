#include <hncomp.hpp>
#include <asyncpp/thread_pool.h>
#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>

namespace {
using namespace hncomp;
SBytes dataBytes(const std::string& text) { return {text.begin(), text.end()}; }
struct Files {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> paths;
    Files() {
        static std::atomic<unsigned> serial{0};
        for (;;) {
            directory = std::filesystem::temp_directory_path() / ("hncomp-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++));
            if (std::filesystem::create_directory(directory)) { break; }
        }
    }
    std::filesystem::path path(const std::filesystem::path& name) { auto value = directory / name; paths.push_back(value); return value; }
    ~Files() { std::error_code error; for (const auto& path : paths) { std::filesystem::remove(path, error); } std::filesystem::remove(directory, error); }
};
SBytes makeArchive(EArchiveFormat format, const SBytes& data) {
    SBytes result;
    auto archive = IArchive::createInMemory(8 * 1024 * 1024, {format,5}, [&](auto bytes) { result.assign(bytes.begin(), bytes.end()); });
    if (format != EARCHIVE_XZ) { archive->addDirectory("docs"); }
    archive->addFile(format == EARCHIVE_XZ ? "data" : "docs/data.bin", data); archive->finish(); return result;
}
DOCTEST_TEST_CASE("archive mapping policy is configurable at open and adjustable between operations") {
    Files files; auto path=files.path("window.tar"); auto source=files.path("input.bin");
    SBytes payload(2*1024*1024+9); for (std::size_t i=0;i<payload.size();++i) { payload[i]=static_cast<std::uint8_t>(i*37); }
    { std::ofstream file(source,std::ios::binary); file.write(reinterpret_cast<const char*>(payload.data()),static_cast<std::streamsize>(payload.size())); }
    SArchiveOpenOptions options; options.mode=EARCHIVE_CREATE; options.mapping.windowBytes=4096; auto output=IArchive::createFromFile(path,options);
    DOCTEST_CHECK(output->mappingOptions().windowBytes==4096); output->addFileFromFile("payload",source); output->setMappingOptions({65537}); output->finish(); output.reset();
    options.mode=EARCHIVE_OPEN; options.mapping.windowBytes=8192; auto input=IArchive::createFromFile(path,options);
    DOCTEST_CHECK(input->readFile("payload")==payload); input->setMappingOptions({4096}); DOCTEST_CHECK(input->readFile("payload")==payload);
    DOCTEST_CHECK_THROWS(input->setMappingOptions({0})); DOCTEST_CHECK(input->mappingOptions().windowBytes==4096);
}
DOCTEST_TEST_CASE("archive memory modes ownership and exactly once completion cover all containers") {
    const auto data = dataBytes("archive payload repeated repeated repeated");
    for (auto format : {EARCHIVE_ZIP, EARCHIVE_TAR, EARCHIVE_XZ, EARCHIVE_TAR_XZ, EARCHIVE_TAR_GZIP}) {
        DOCTEST_INFO(format); SBytes completed; unsigned calls = 0;
        auto writer = IArchive::createInMemory(1024 * 1024, {format,5}, [&](auto bytes) { ++calls; completed.assign(bytes.begin(), bytes.end()); });
        if (format != EARCHIVE_XZ) { writer->addDirectory("docs"); }
        const auto name = format == EARCHIVE_XZ ? "data" : "docs/data.bin";
        writer->addFile(name,data); DOCTEST_CHECK_FALSE(writer->isReadOnly()); DOCTEST_CHECK(writer->readFile(name) == data);
        DOCTEST_CHECK(calls == 0); writer->finish(); writer->finish(); DOCTEST_CHECK(calls == 1);
        DOCTEST_CHECK(writer->readFile(name) == data); DOCTEST_CHECK_THROWS_AS(writer->addFile("late",{}),CCompressionError);
        auto reader = IArchive::openFromMemory(completed); completed.clear(); completed.shrink_to_fit();
        DOCTEST_CHECK(reader->isReadOnly()); DOCTEST_CHECK(reader->format() == format); DOCTEST_CHECK(reader->readFile(name) == data);
        DOCTEST_CHECK_THROWS_AS(reader->addFile("new",data),CCompressionError);
        DOCTEST_CHECK_THROWS_AS(reader->addDirectory("new"),CCompressionError);
        DOCTEST_CHECK_THROWS_AS(reader->addFileFromFile("new","missing"),CCompressionError);
        DOCTEST_CHECK_THROWS_AS(reader->finish(),CCompressionError); DOCTEST_CHECK_THROWS_AS(reader->readFile(name,2),CCompressionError);
        const auto packed = makeArchive(format,data); DOCTEST_CHECK_THROWS_AS(IArchive::openFromMemory(packed,1),CCompressionError);
    }
}
DOCTEST_TEST_CASE("archive mapped files cross windows preserve Unicode paths and detect content") {
    Files files; const auto source = files.path("source.bin");
    SBytes data(1024 * 1024 + 123); for (std::size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<std::uint8_t>((i * 113) ^ (i / 997)); }
    { std::ofstream stream(source, std::ios::binary); stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())); if (!stream) { throw std::runtime_error("test source write failed"); } }
    for (const auto& suffix : {".zip", ".tar", ".xz", ".tar.xz", ".tar.gz"}) {
        DOCTEST_INFO(suffix); auto filename = std::filesystem::path(u8"\uD14C\uC2A4\uD2B8"); filename += suffix;
        const auto path = files.path(filename); auto writer = IArchive::createFromFile(path,{3,EARCHIVE_CREATE});
        const auto name = std::string(suffix) == ".xz" ? "data" : "dir/\xc3\xa9.bin";
        writer->addFileFromFile(name,source); std::uint64_t count = 0;
        writer->readFile(name,[&](auto bytes) { DOCTEST_CHECK(std::equal(bytes.begin(),bytes.end(),data.begin() + count)); count += bytes.size(); });
        DOCTEST_CHECK(count == data.size()); writer->finish(); writer.reset();
        DOCTEST_CHECK_THROWS(IArchive::createFromFile(path,{5,EARCHIVE_CREATE}));
        const auto renamed = files.path(std::string(suffix) + ".unknown"); std::filesystem::rename(path,renamed);
        auto reader = IArchive::createFromFile(renamed); DOCTEST_CHECK(reader->readFile(name) == data);
    }
}
DOCTEST_TEST_CASE("archive empty files unsafe names duplicate paths and memory ceilings fail explicitly") {
    for (auto format : {EARCHIVE_ZIP,EARCHIVE_TAR,EARCHIVE_XZ,EARCHIVE_TAR_XZ,EARCHIVE_TAR_GZIP}) {
        auto empty = makeArchive(format,{}); auto reader = IArchive::openFromMemory(empty);
        DOCTEST_CHECK(reader->readFile(format == EARCHIVE_XZ ? "data" : "docs/data.bin").empty());
    }
    auto writer = IArchive::createInMemory(4096,{},[](auto) {});
    for (const auto& name : {"../bad","/absolute","C:/drive","a/../bad","a\\b","a//b"}) { DOCTEST_CHECK_THROWS_AS(writer->addFile(name,{}),CCompressionError); }
    writer->addFile("file",{}); DOCTEST_CHECK_THROWS_AS(writer->addFile("file",{}),CCompressionError);
    DOCTEST_CHECK_THROWS_AS(writer->addDirectory("file"),CCompressionError);
    auto limited = IArchive::createInMemory(2,{},[](auto) { DOCTEST_FAIL("failed archive invoked completion"); });
    DOCTEST_CHECK_THROWS_AS(limited->addFile("file",{}),CCompressionError); DOCTEST_CHECK_THROWS_AS(limited->finish(),CCompressionError);
    auto xz = IArchive::createInMemory(4096,{EARCHIVE_XZ,5},[](auto) {});
    DOCTEST_CHECK_THROWS_AS(xz->addDirectory("dir"),CCompressionError); xz->addFile("data",{});
    DOCTEST_CHECK_THROWS_AS(xz->addFile("two",{}),CCompressionError);
    unsigned calls = 0;
    { auto abandoned = IArchive::createInMemory(4096,{},[&](auto) { ++calls; }); abandoned->addFile("data",{}); }
    DOCTEST_CHECK(calls == 0);
    auto throwing = IArchive::createInMemory(4096,{},[&](auto) { ++calls; throw std::runtime_error("callback"); });
    DOCTEST_CHECK_THROWS_AS(throwing->finish(),std::runtime_error); DOCTEST_CHECK(calls == 1);
    DOCTEST_CHECK_THROWS_AS(throwing->finish(),CCompressionError); DOCTEST_CHECK(calls == 1);
}
DOCTEST_TEST_CASE("archive truncation footer damage and payload CRC errors are rejected") {
    const auto data = dataBytes("metadata and checksum corruption payload payload payload");
    for (auto format : {EARCHIVE_ZIP,EARCHIVE_TAR,EARCHIVE_XZ,EARCHIVE_TAR_XZ,EARCHIVE_TAR_GZIP}) {
        auto valid = makeArchive(format,data);
        for (auto length : {std::size_t(0),std::size_t(1),valid.size()/2,valid.size()-1}) { DOCTEST_CHECK_THROWS_AS(IArchive::openFromMemory(SReadOnlyByteSpan(valid).first(length)),CCompressionError); }
        if (format == EARCHIVE_XZ || format == EARCHIVE_TAR_XZ) { valid[valid.size()-12] ^= 1; DOCTEST_CHECK_THROWS_AS(IArchive::openFromMemory(valid),CCompressionError); }
    }
    auto zip = makeArchive(EARCHIVE_ZIP,data);
    for (std::size_t i = 4; i + 60 < zip.size(); ++i) {
        if (zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 3 && zip[i+3] == 4) {
            const auto n = unsigned(zip[i+26]) | unsigned(zip[i+27]) << 8, e = unsigned(zip[i+28]) | unsigned(zip[i+29]) << 8;
            zip[i+30+n+e] ^= 255; break;
        }
    }
    auto damaged = IArchive::openFromMemory(zip); DOCTEST_CHECK_THROWS_AS(damaged->readFile("docs/data.bin"),CCompressionError);
}
DOCTEST_TEST_CASE("archive async tasks own arguments retain instances and complete on worker") {
    asyncpp::thread_pool worker(1); SBytes completed; const auto caller = std::this_thread::get_id(); bool offCaller = false;
    auto archive = IArchive::createInMemory(4096,{},[&](auto bytes) { completed.assign(bytes.begin(),bytes.end()); offCaller = std::this_thread::get_id() != caller; });
    auto add = archive->addFileAsync("data",dataBytes("owned asynchronous payload"),worker); asyncpp::as_promise(std::move(add)).get();
    DOCTEST_CHECK(asyncpp::as_promise(archive->readFileAsync("data",4096,worker)).get() == dataBytes("owned asynchronous payload"));
    auto finish = archive->finishAsync(worker); archive.reset(); asyncpp::as_promise(std::move(finish)).get();
    DOCTEST_CHECK(offCaller); DOCTEST_CHECK(IArchive::openFromMemory(completed)->readFile("data") == dataBytes("owned asynchronous payload"));
    auto open = IArchive::openFromMemoryAsync(completed,4096,worker); completed.clear();
    auto reader = asyncpp::as_promise(std::move(open)).get(); SBytes streamed;
    auto task = reader->readFileAsync("data",[&](auto bytes) { streamed.insert(streamed.end(),bytes.begin(),bytes.end()); },worker);
    reader.reset(); asyncpp::as_promise(std::move(task)).get(); DOCTEST_CHECK(streamed == dataBytes("owned asynchronous payload"));
    Files files; const auto source = files.path("async-source.bin"), destination = files.path("async.zip");
    { std::ofstream file(source,std::ios::binary); file << "source file"; if (!file) { throw std::runtime_error("async fixture write failed"); } }
    auto disk = asyncpp::as_promise(IArchive::createFromFileAsync(destination,{5,EARCHIVE_CREATE},worker)).get();
    asyncpp::as_promise(disk->addFileFromFileAsync("source",source,worker)).get();
    auto seal = disk->finishAsync(worker); disk.reset(); asyncpp::as_promise(std::move(seal)).get();
    auto reopened = asyncpp::as_promise(IArchive::createFromFileAsync(destination,{},worker)).get();
    DOCTEST_CHECK(reopened->readFile("source") == dataBytes("source file"));
}
DOCTEST_TEST_CASE("archive PAX long paths nested directories empty files and copied metadata round trip") {
    const auto name = "folder/sub/" + std::string(300,'n') + ".bin"; const auto data = dataBytes("long name contents");
    for (auto format : {EARCHIVE_ZIP,EARCHIVE_TAR,EARCHIVE_TAR_XZ,EARCHIVE_TAR_GZIP}) {
        SBytes packed; auto writer = IArchive::createInMemory(65536,{format,5},[&](auto bytes) { packed.assign(bytes.begin(),bytes.end()); });
        writer->addDirectory("folder"); writer->addDirectory("folder/sub"); writer->addFile("folder/empty",{}); writer->addFile(name,data);
        auto copied = writer->entries(0,4); copied.clear(); DOCTEST_CHECK(writer->entryCount() == 4);
        writer->finish(); auto reader = IArchive::openFromMemory(packed);
        DOCTEST_CHECK(reader->entryCount() == 4); DOCTEST_CHECK(reader->readFile(name) == data); DOCTEST_CHECK(reader->readFile("folder/empty").empty());
        DOCTEST_CHECK_THROWS_AS(reader->readFile("folder/"),CCompressionError);
        DOCTEST_CHECK_THROWS_AS(reader->readFile("missing"),std::out_of_range);
    }
}
DOCTEST_TEST_CASE("archive ZIP AES file factories preserve passwords and reject invalid settings before creation") {
    Files files; const auto path=files.path("encrypted.zip"),source=files.path("payload.bin"),invalid=files.path("invalid.tar");
    { std::ofstream output(source,std::ios::binary); output<<"mapped AES file payload"; if (!output) { throw std::runtime_error("write AES source fixture"); } }
    SArchiveOpenOptions options; options.mode=EARCHIVE_CREATE; options.zip={EZIP_AES256,"file-password"};
    asyncpp::thread_pool worker(1); auto writer=asyncpp::as_promise(IArchive::createFromFileAsync(path,options,worker)).get();
    asyncpp::as_promise(writer->addFileFromFileAsync("data",source,worker)).get();
    DOCTEST_CHECK(writer->readFile("data")==dataBytes("mapped AES file payload"));
    asyncpp::as_promise(writer->finishAsync(worker)).get(); writer.reset();
    options.mode=EARCHIVE_OPEN; auto reader=asyncpp::as_promise(IArchive::createFromFileAsync(path,options,worker)).get();
    DOCTEST_CHECK(reader->readFile("data")==dataBytes("mapped AES file payload"));
    auto noPassword=IArchive::createFromFile(path); unsigned calls=0;
    try { noPassword->readFile("data",[&](auto) { ++calls; }); DOCTEST_FAIL("missing password accepted"); }
    catch (const CCompressionError& error) { DOCTEST_CHECK(error.code()==EERROR_AUTHENTICATION); }
    DOCTEST_CHECK(calls==0); options.mode=EARCHIVE_CREATE;
    DOCTEST_CHECK_THROWS_AS(IArchive::createFromFile(invalid,options),std::invalid_argument);
    DOCTEST_CHECK_FALSE(std::filesystem::exists(invalid));
}
DOCTEST_TEST_CASE("archive mutated metadata and random signatures remain bounded") {
    std::minstd_rand random(87613);
    for (auto format : {EARCHIVE_ZIP,EARCHIVE_TAR,EARCHIVE_XZ,EARCHIVE_TAR_XZ,EARCHIVE_TAR_GZIP}) {
        const auto valid = makeArchive(format,dataBytes("mutated archive corpus repeated repeated repeated"));
        for (unsigned test = 0; test < 80; ++test) {
            SBytes input;
            if (test & 1) { input = valid; for (unsigned mutation = 0; mutation < 3; ++mutation) { input[random()%input.size()] ^= static_cast<std::uint8_t>(1u << (random()%8)); } }
            else { input.resize(random()%512); for (auto& byte : input) { byte = static_cast<std::uint8_t>(random()); } }
            try { auto archive = IArchive::openFromMemory(input,1024*1024); for (const auto& entry : archive->entries(0,128)) { if (!entry.directory) { static_cast<void>(archive->readFile(entry.name,1024*1024)); } } }
            catch (const CCompressionError&) {}
        }
    }
}
#if !defined(_WIN32)
DOCTEST_TEST_CASE("archive ZIP64 locates a sparse central directory beyond four GiB with bounded mappings") {
    Files files; const auto path = files.path("sparse.zip");
    auto put = [](SBytes& bytes, std::uint64_t value, unsigned count) { for (unsigned i = 0; i < count; ++i) { bytes.push_back(static_cast<std::uint8_t>(value >> (i*8))); } };
    std::uint32_t crc = 0xffffffffu ^ 'x'; for (unsigned i = 0; i < 8; ++i) { crc = (crc >> 1) ^ (0xedb88320u & (0u-(crc&1))); } crc = ~crc;
    SBytes local; put(local,0x04034b50,4); put(local,20,2); put(local,0,8); put(local,crc,4); put(local,1,4); put(local,1,4); put(local,1,2); put(local,0,2); local.push_back('x'); local.push_back('x');
    SBytes central; put(central,0x02014b50,4); put(central,20,2); put(central,20,2); put(central,0,8); put(central,crc,4); put(central,1,4); put(central,1,4);
    put(central,1,2); put(central,0,8); put(central,0,4); put(central,0,4); central.push_back('x');
    const std::uint64_t offset = (UINT64_C(1)<<32) + 4096; SBytes end;
    put(end,0x06064b50,4); put(end,44,8); put(end,45,2); put(end,45,2); put(end,0,8); put(end,1,8); put(end,1,8); put(end,central.size(),8); put(end,offset,8);
    put(end,0x07064b50,4); put(end,0,4); put(end,offset+central.size(),8); put(end,1,4);
    put(end,0x06054b50,4); put(end,0,4); put(end,65535,2); put(end,65535,2); put(end,UINT32_MAX,4); put(end,UINT32_MAX,4); put(end,0,2);
    { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(local.data()),static_cast<std::streamsize>(local.size()));
      file.seekp(static_cast<std::streamoff>(offset)); file.write(reinterpret_cast<const char*>(central.data()),static_cast<std::streamsize>(central.size())); file.write(reinterpret_cast<const char*>(end.data()),static_cast<std::streamsize>(end.size()));
      if (!file) { throw std::runtime_error("sparse archive fixture write failed"); } }
    auto reader = IArchive::createFromFile(path); DOCTEST_CHECK(reader->readFile("x") == SBytes{'x'});
}
#endif
} // namespace
