#include <hncomp.hpp>
#include "../src/archive_crypto.hpp"
#include <doctest/doctest.h>
#include <asyncpp/thread_pool.h>
#include <asyncpp/sync_wait.h>

namespace {
using namespace hncomp;
SBytes fromHex(const std::string& text) {
    SBytes bytes; for (std::size_t i=0;i<text.size();i+=2) { bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i,2),nullptr,16))); } return bytes;
}
#if !defined(HNCOMP_SHARED)
DOCTEST_TEST_CASE("archive libcertpp primitives match published known answers") {
    const SBytes abc{'a','b','c'},password{'p','a','s','s','w','o','r','d'},salt{'s','a','l','t'};
    detail::ArchiveHash hash(true); hash.update(abc);
    DOCTEST_CHECK(hash.finish()==fromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    DOCTEST_CHECK(detail::archivePbkdf2(password,salt,1,20)==fromHex("0c60c80f961f0e71f3a9b524af6012062fe037a6"));
    DOCTEST_CHECK(detail::archivePbkdf2(password,salt,2,20)==fromHex("ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"));
    SBytes key(20,0x0b),message{'H','i',' ','T','h','e','r','e'}; detail::ArchiveHmac mac(key); mac.update(message);
    DOCTEST_CHECK(mac.finish()==fromHex("b617318655057264e28bc0b6fb378c8ef146be00"));
    // AES-128(01 00...00), independently checked with OpenSSL ECB, no padding.
    SBytes aesKey(16),plain(16); detail::ZipAesCtr cipher(aesKey); cipher.process(plain);
    DOCTEST_CHECK(plain==fromHex("47711816e91d6ff059bbbf2bf58e0fd3"));
    detail::ZipAesCtr split(aesKey); SBytes segmented(16); split.process(SByteSpan(segmented).first(7)); split.process(SByteSpan(segmented).subspan(7));
    DOCTEST_CHECK(segmented==plain);
}
#endif
DOCTEST_TEST_CASE("archive ZIP AES authenticates before output across all strengths") {
    SBytes payload(130019); for (std::size_t i=0;i<payload.size();++i) { payload[i]=static_cast<std::uint8_t>((i*53)^(i/271)); }
    for (auto mode:{EZIP_AES128,EZIP_AES192,EZIP_AES256}) {
        DOCTEST_INFO(mode); SArchiveMemoryOptions options; options.zip={mode,"test-password"}; SBytes packed;
        auto writer=IArchive::createInMemory(1024*1024,options,[&](auto bytes) { packed.assign(bytes.begin(),bytes.end()); });
        writer->addDirectory("docs"); writer->addFile("docs/data.bin",payload); writer->addFile("empty",{});
        DOCTEST_CHECK(writer->readFile("docs/data.bin")==payload); writer->finish();
        auto reader=IArchive::openFromMemory(packed,1024*1024,"test-password");
        DOCTEST_CHECK(reader->entries(1,1).front().encrypted); DOCTEST_CHECK_FALSE(reader->entries(0,1).front().encrypted);
        DOCTEST_CHECK(reader->readFile("docs/data.bin")==payload); DOCTEST_CHECK(reader->readFile("empty").empty());
        unsigned calls=0; auto wrong=IArchive::openFromMemory(packed,1024*1024,"wrong");
        DOCTEST_CHECK_THROWS_AS(wrong->readFile("docs/data.bin",[&](auto) { ++calls; }),CCompressionError); DOCTEST_CHECK(calls==0);
        // Locate the second local record and corrupt ciphertext, not its metadata.
        auto damaged=packed;
        for (std::size_t i=4;i+60<damaged.size();++i) {
            if (damaged[i]=='P' && damaged[i+1]=='K' && damaged[i+2]==3 && damaged[i+3]==4) {
                const auto name=unsigned(damaged[i+26])|(unsigned(damaged[i+27])<<8),extra=unsigned(damaged[i+28])|(unsigned(damaged[i+29])<<8);
                const auto salt=mode==EZIP_AES128?8u:mode==EZIP_AES192?12u:16u;
                damaged[i+30+name+extra+salt+2]^=1; break;
            }
        }
        auto tampered=IArchive::openFromMemory(damaged,1024*1024,"test-password");
        DOCTEST_CHECK_THROWS_AS(tampered->readFile("docs/data.bin",[&](auto) { ++calls; }),CCompressionError); DOCTEST_CHECK(calls==0);
        asyncpp::thread_pool pool(1);
        auto async=asyncpp::as_promise(IArchive::openFromMemoryAsync(packed,1024*1024,pool,"test-password")).get();
        DOCTEST_CHECK(asyncpp::as_promise(async->readFileAsync("docs/data.bin",1024*1024,pool)).get()==payload);
    }
    SArchiveMemoryOptions bad; bad.zip={EZIP_AES256,{}};
    DOCTEST_CHECK_THROWS_AS(IArchive::createInMemory(4096,bad,[](auto) {}),std::invalid_argument);
    bad.format=EARCHIVE_TAR; bad.zip.password="secret";
    DOCTEST_CHECK_THROWS_AS(IArchive::createInMemory(4096,bad,[](auto) {}),std::invalid_argument);
}
DOCTEST_TEST_CASE("archive range queries are bounded independent copied pages") {
    SBytes packed; auto writer=IArchive::createInMemory(1024*1024,{},[&](auto bytes) { packed.assign(bytes.begin(),bytes.end()); });
    for (unsigned i=0;i<600;++i) { writer->addDirectory("dir"+std::to_string(i)); }
    writer->finish(); auto reader=IArchive::openFromMemory(packed);
    for (auto archive:{writer,reader}) {
        DOCTEST_CHECK(archive->entryCount()==600); DOCTEST_CHECK(archive->entries(0).size()==128);
        auto page=archive->entries(599,128); DOCTEST_REQUIRE(page.size()==1); DOCTEST_CHECK(page[0].name=="dir599/");
        page[0].name="changed"; DOCTEST_CHECK(archive->entries(599,1)[0].name=="dir599/");
        DOCTEST_CHECK(archive->entries(600,1).empty()); DOCTEST_CHECK(archive->entries(UINT64_MAX,4096).empty());
        DOCTEST_CHECK(archive->entries(0,0).empty()); DOCTEST_CHECK_THROWS_AS(archive->entries(0,4097),std::invalid_argument);
    }
}
DOCTEST_TEST_CASE("archive XZ filters checks and chunk boundary chains roundtrip") {
    SBytes payload(131081); for (std::size_t i=0;i<payload.size();++i) { payload[i]=static_cast<std::uint8_t>((i*73)^(i/997)); }
    for (auto filter:{EXZ_DELTA,EXZ_POWERPC,EXZ_ARM,EXZ_ARMTHUMB,EXZ_SPARC,EXZ_ARM64}) {
        for (auto check:{EXZ_CHECK_NONE,EXZ_CHECK_CRC32,EXZ_CHECK_CRC64,EXZ_CHECK_SHA256}) {
            DOCTEST_INFO(filter,check); SArchiveMemoryOptions options; options.format=EARCHIVE_XZ;
            options.xz.check=check; options.xz.filters={{filter,filter==EXZ_DELTA?0u:0xfffff000u,filter==EXZ_DELTA?256u:1u}};
            SBytes packed; auto writer=IArchive::createInMemory(1024*1024,options,[&](auto bytes) { packed.assign(bytes.begin(),bytes.end()); });
            writer->addFile("data",payload); writer->finish(); auto reader=IArchive::openFromMemory(packed); DOCTEST_CHECK(reader->readFile("data")==payload);
        }
    }
    SArchiveMemoryOptions options; options.format=EARCHIVE_XZ; options.xz.filters={{EXZ_ARMTHUMB,2,1},{EXZ_DELTA,0,3},{EXZ_DELTA,0,256}};
    SBytes packed; auto writer=IArchive::createInMemory(1024*1024,options,[&](auto bytes) { packed.assign(bytes.begin(),bytes.end()); });
    writer->addFile("data",payload); writer->finish(); DOCTEST_CHECK(IArchive::openFromMemory(packed)->readFile("data")==payload);
    options.xz.filters={{EXZ_ARM,1,1}}; DOCTEST_CHECK_THROWS_AS(IArchive::createInMemory(4096,options,[](auto) {}),std::invalid_argument);
    options.xz.filters={{EXZ_DELTA,0,257}}; DOCTEST_CHECK_THROWS_AS(IArchive::createInMemory(4096,options,[](auto) {}),std::invalid_argument);
}
} // namespace
