#include <hnfs.hpp>
#include <asyncpp/thread_pool.h>
#include <asyncpp/sync_wait.h>
#include "../tests/console.hpp"
#include <iostream>

/* Sequential creation/read does not require a synthetic mutable-tree interface. */
int main() {
    configureFailures();
    try {
        asyncpp::thread_pool worker(1); auto device=std::make_shared<hnfs::CMemoryDevice>(8192);
        auto builder=asyncpp::as_promise(hnfs::CArchiveBuilderAsync::create(device,{hnfs::EFORMAT_CPIO_CRC},worker)).get();
        hnfs::CArchiveBuilderAsync output(builder,worker);
        asyncpp::as_promise(output.beginEntry({"hello.txt",hnfs::ENODE_FILE,5})).get();
        asyncpp::as_promise(output.write({'h','e','l','l','o'})).get(); asyncpp::as_promise(output.finish()).get();
        auto filesystem=hnfs::IFileSystem::open(device);
        auto reader=std::dynamic_pointer_cast<hnfs::IArchiveReader>(filesystem);
        if (!reader || std::dynamic_pointer_cast<hnfs::ITreeEditor>(filesystem)) { throw std::runtime_error("incorrect capability discovery"); }
        hnfs::CArchiveReaderAsync input(reader,worker);
        while (auto entry=asyncpp::as_promise(input.next()).get()) {
            hnfs::SBytes content;
            for (;;) { auto chunk=asyncpp::as_promise(input.read()).get(); if (chunk.empty()) { break; } content.insert(content.end(),chunk.begin(),chunk.end()); }
            if (content!=hnfs::SBytes{'h','e','l','l','o'}) { throw std::runtime_error("CPIO example mismatch"); }
            std::cout<<entry->path<<": "<<std::string(content.begin(),content.end())<<'\n';
        }
        std::cout<<hnfs::formatName(filesystem->format())<<": "<<reader->usedBytes()<<" image bytes\n";
        // --> A tree is a separate opt-in adapter, not a property of the native cursor.
        auto tree=asyncpp::as_promise(hnfs::CTreeReaderAsync::indexArchive(device,worker)).get();
        hnfs::CTreeReaderAsync query(tree,worker);
        const auto contents=asyncpp::as_promise(query.readFile("hello.txt",0,64)).get();
        if (contents!=hnfs::SBytes{'h','e','l','l','o'} || std::dynamic_pointer_cast<hnfs::ITreeEditor>(tree)) { throw std::runtime_error("tree adapter mismatch"); }
        std::cout<<"Optional readonly tree: "<<tree->list("/",0,128).size()<<" entry\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
