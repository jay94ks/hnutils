#include <hncomp.hpp>
#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>

#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

/* A caller can use the same byte API without introducing HTTP or SSH dependencies. */
int main() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    for (const auto kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE); _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try {
        const std::string text = "reusable compression reusable compression reusable compression";
        const hncomp::SBytes input(text.begin(), text.end());
        hncomp::SCompressionOptions options; options.algorithm = hncomp::EALGORITHM_ZSTD;
        auto compressor = hncomp::ICompressor::create(options);
        auto decompressor = hncomp::IDecompressor::create(options);
        asyncpp::thread_pool worker(1);
        const auto compressed = asyncpp::as_promise(compressor->compressAsync(input, worker)).get();
        const auto decoded = asyncpp::as_promise(decompressor->decompressAsync(compressed, worker)).get();
        if (decoded != input) { throw std::runtime_error("round trip failed"); }
        std::cout << "Zstd: " << input.size() << " -> " << compressed.size() << " bytes\n";

        options.format = hncomp::EFORMAT_CHUNKED;
        auto stream = hncomp::IStreamCompressor::create(options);
        std::size_t pos = 0, total = 0;
        for (;;) {
            const auto result = asyncpp::as_promise(stream->processAsync(
                hncomp::SBytes(input.begin() + pos, input.end()), 16, hncomp::EFLUSH_FINISH, worker)).get();
            pos += result.progress.consumed; total += result.output.size();
            if (result.progress.status == hncomp::ESTREAM_FINISHED) { break; }
        }
        std::cout << "Explicit HNCS chunked stream: " << total << " bytes\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
