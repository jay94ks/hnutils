#include <hncomp.hpp>
#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>
#include <iostream>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

/* Create a memory ZIP; the completion span is borrowed only for the callback. */
int main() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    for (const auto kind : {_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}) {
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE); _CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try {
        asyncpp::thread_pool worker(1); hncomp::SBytes content;
        auto archive = hncomp::IArchive::createInMemory(1024*1024, {hncomp::EARCHIVE_ZIP,5},
            [&](auto bytes) { content.assign(bytes.begin(),bytes.end()); });
        archive->addDirectory("docs");
        asyncpp::as_promise(archive->addFileAsync("docs/hello.txt", {'h','e','l','l','o'}, worker)).get();
        asyncpp::as_promise(archive->finishAsync(worker)).get();
        auto reader = hncomp::IArchive::openFromMemory(content);
        const auto decoded = reader->readFile("docs/hello.txt");
        if (decoded != hncomp::SBytes{'h','e','l','l','o'}) { throw std::runtime_error("archive example mismatch"); }
        std::cout << "ZIP: " << content.size() << " bytes, " << reader->entryCount() << " entries\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
