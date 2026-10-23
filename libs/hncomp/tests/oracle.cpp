#include <hncomp.hpp>
#include "console.hpp"

#include <charconv>
#include <array>
#include <iostream>
#include <iterator>
#include <string>

/* Binary stdin/stdout bridge for independent reference verification, never a runtime dependency. */
int main(int argc, char** argv) {
    configureTestConsole();
    try {
        if (argc < 3) { throw std::invalid_argument("oracle algorithm encode|decode [format]"); }
        hncomp::SCompressionOptions options;
        int algorithm = 0, format = 0;
        auto readNumber = [](const char* value, int& result) {
            const std::string text(value); const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { throw std::invalid_argument("bad number"); }
        };
        readNumber(argv[1], algorithm); if (argc > 3) { readNumber(argv[3], format); }
        options.algorithm = static_cast<hncomp::EAlgorithm>(algorithm); options.format = static_cast<hncomp::EFormat>(format);
        const std::string direction(argv[2]);
        auto streamEncoder = direction == "stream-encode" ? hncomp::IStreamCompressor::create(options) : nullptr;
        auto streamDecoder = direction == "stream-decode" ? hncomp::IStreamDecompressor::create(options) : nullptr;
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                if (line.size() % 2) { throw std::invalid_argument("invalid hex input"); }
                hncomp::SBytes input;
                for (std::size_t pos = 0; pos < line.size(); pos += 2) {
                    unsigned byte = 0; const auto parsed = std::from_chars(line.data() + pos, line.data() + pos + 2, byte, 16);
                    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + pos + 2) { throw std::invalid_argument("invalid hex input"); }
                    input.push_back(static_cast<std::uint8_t>(byte));
                }
                hncomp::SBytes output;
                if (streamEncoder || streamDecoder) {
                    std::array<std::uint8_t, 4096> buffer{}; std::size_t pos = 0;
                    for (;;) {
                        const auto result = streamEncoder ? streamEncoder->process(hncomp::SReadOnlyByteSpan(input).subspan(pos), buffer, hncomp::EFLUSH_PARTIAL) :
                            streamDecoder->process(hncomp::SReadOnlyByteSpan(input).subspan(pos), buffer, hncomp::EINPUT_MORE);
                        pos += result.consumed; output.insert(output.end(), buffer.begin(), buffer.begin() + result.produced);
                        if (result.status == hncomp::ESTREAM_FLUSHED || result.status == hncomp::ESTREAM_NEED_INPUT) { break; }
                    }
                    if (pos != input.size()) { throw std::runtime_error("partial stream consumption"); }
                } else if (direction == "archive-encode" || direction=="archive-aes-encode" || direction=="archive-filter-encode") {
                    const auto container = static_cast<hncomp::EArchiveFormat>(algorithm);
                    hncomp::SArchiveMemoryOptions archiveOptions; archiveOptions.format=container;
                    if (direction=="archive-aes-encode") { archiveOptions.zip={static_cast<hncomp::EZipEncryption>(format),"test-password"}; }
                    if (direction=="archive-filter-encode") {
                        int check=1,parameter=0; if (argc>4) { readNumber(argv[4],check); } if (argc>5) { readNumber(argv[5],parameter); }
                        archiveOptions.xz.check=static_cast<hncomp::EXzCheck>(check);
                        if (format) { archiveOptions.xz.filters={{static_cast<hncomp::EXzFilter>(format),format==3?0u:static_cast<std::uint32_t>(parameter),format==3?static_cast<unsigned>(parameter?parameter:1):1u}}; }
                    }
                    auto archive = hncomp::IArchive::createInMemory(32 * 1024 * 1024, archiveOptions, [&](auto bytes) { output.assign(bytes.begin(),bytes.end()); });
                    if (container != hncomp::EARCHIVE_XZ) { archive->addDirectory("docs"); }
                    archive->addFile(container == hncomp::EARCHIVE_XZ ? "data" : "docs/data.bin",input); archive->finish();
                } else if (direction == "archive-decode" || direction=="archive-aes-decode") {
                    auto archive = hncomp::IArchive::openFromMemory(input,256*1024*1024,direction=="archive-aes-decode"?"test-password":"");
                    const auto entries = archive->entries(0,128);
                    for (const auto& entry : entries) { if (!entry.directory) { output = archive->readFile(entry.name); break; } }
                } else { output = direction == "encode" ? hncomp::ICompressor::create(options)->compress(input) : hncomp::IDecompressor::create(options)->decompress(input); }
                const char hex[] = "0123456789abcdef";
                for (auto byte : output) { std::cout << hex[byte >> 4] << hex[byte & 15]; }
                std::cout << '\n';
            } catch (const std::exception& error) { std::cout << "ERROR " << error.what() << '\n'; }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
