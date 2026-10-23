#include <hncomp.hpp>
#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>
#include <doctest/doctest.h>

#include <array>
#include <charconv>
#include <deque>
#include <functional>
#include <random>
#include <string>

namespace {

using namespace hncomp;
SBytes bytes(const std::string& text) { return SBytes(text.begin(), text.end()); }

#include "reference_vectors.inc"

SBytes hex(std::string_view text) {
    SBytes output;
    for (std::size_t pos = 0; pos < text.size(); pos += 2) {
        unsigned byte = 0;
        const auto parsed = std::from_chars(text.data() + pos, text.data() + pos + 2, byte, 16);
        if (parsed.ec != std::errc{}) { throw std::runtime_error("invalid reference fixture"); }
        output.push_back(static_cast<std::uint8_t>(byte));
    }
    return output;
}

DOCTEST_TEST_CASE("fixed independent reference streams decode without external codec dependencies") {
    for (const auto& fixture : REFERENCE_FIXTURES) {
        DOCTEST_INFO(fixture.name);
        SCompressionOptions options; options.algorithm = fixture.algorithm; options.format = fixture.format;
        DOCTEST_CHECK(IDecompressor::create(options)->decompress(hex(fixture.encoded)) == hex(fixture.plain));
    }
}

DOCTEST_TEST_CASE("all algorithms round trip empty binary repetitive and random messages") {
    std::minstd_rand random(123);
    SBytes randomData(4096); for (auto& value : randomData) { value = static_cast<std::uint8_t>(random()); }
    SBytes repetitive; for (unsigned i = 0; i < 1000; ++i) { const auto part = bytes("abcd0123abcd0123"); repetitive.insert(repetitive.end(), part.begin(), part.end()); }
    for (unsigned algorithm = 0; algorithm <= EALGORITHM_LZ4; ++algorithm) {
        SCompressionOptions options; options.algorithm = static_cast<EAlgorithm>(algorithm);
        DOCTEST_INFO(algorithm);
        auto compressor = ICompressor::create(options); auto decompressor = IDecompressor::create(options);
        for (const auto& input : {SBytes{}, bytes("a"), bytes("hello world"), SBytes(10000, 'A'), randomData, repetitive}) {
            auto compressed = compressor->compress(input);
            DOCTEST_CHECK(decompressor->decompress(compressed) == input);
        }
        DOCTEST_CHECK(compressor->compress(repetitive).size() < repetitive.size());
    }
}

DOCTEST_TEST_CASE("raw and wrapped deflate snappy and lz4 formats round trip") {
    for (auto algorithm : {EALGORITHM_DEFLATE, EALGORITHM_SNAPPY, EALGORITHM_LZ4}) {
        for (auto format : {EFORMAT_RAW, EFORMAT_DEFAULT}) {
            SCompressionOptions options; options.algorithm = algorithm; options.format = format;
            auto data = bytes("binary\r\nquoted pattern repeated repeated repeated");
            DOCTEST_CHECK(IDecompressor::create(options)->decompress(ICompressor::create(options)->compress(data)) == data);
        }
    }
    SCompressionOptions options; options.format = EFORMAT_GZIP;
    DOCTEST_CHECK(IDecompressor::create(options)->decompress(ICompressor::create(options)->compress(bytes("gzip"))) == bytes("gzip"));
}

DOCTEST_TEST_CASE("LZ78 recreates article dictionary indexes including the terminal prefix") {
    SCompressionOptions options; options.algorithm = EALGORITHM_LZ78;
    auto data = bytes("abccabcdabbcddab");
    const auto encoded = ICompressor::create(options)->compress(data);
    const SBytes records{0,1,'a',0,1,'b',0,1,'c',3,1,'a',2,1,'c',0,1,'d',1,1,'b',5,1,'d',6,1,'a',2,0};
    DOCTEST_CHECK(SBytes(encoded.begin() + 16, encoded.end()) == records);
    DOCTEST_CHECK(IDecompressor::create(options)->decompress(encoded) == data);
    options.maxDictionaryEntries = 2;
    DOCTEST_CHECK(IDecompressor::create(options)->decompress(ICompressor::create(options)->compress(data)) == data);
}

DOCTEST_TEST_CASE("LZ77 article offset length next triples support overlapping references") {
    SCompressionOptions options; options.algorithm = EALGORITHM_LZ77;
    const auto input = bytes("abbdabbdabc");
    const auto encoded = ICompressor::create(options)->compress(input);
    DOCTEST_CHECK(SBytes(encoded.end() - 6, encoded.end()) == SBytes{4,0,6,0,1,'c'});
    DOCTEST_CHECK(IDecompressor::create(options)->decompress(encoded) == input);
}

DOCTEST_TEST_CASE("all algorithms support explicit bounded HNCS streaming without claiming a native wire format") {
    for (unsigned algorithm = 0; algorithm <= EALGORITHM_LZ4; ++algorithm) {
        DOCTEST_INFO(algorithm);
        SCompressionOptions options; options.algorithm = static_cast<EAlgorithm>(algorithm); options.format = EFORMAT_CHUNKED; options.blockBytes = 1024;
        auto encoder = IStreamCompressor::create(options);
        auto decoder = IStreamDecompressor::create(options);
        SBytes input(10001); for (std::size_t i = 0; i < input.size(); ++i) { input[i] = static_cast<std::uint8_t>(i % 17); }
        SBytes encoded, decoded; std::array<std::uint8_t, 13> buffer{}; std::size_t pos = 0;
        for (;;) {
            auto result = encoder->process(SReadOnlyByteSpan(input).subspan(pos), buffer, EFLUSH_FINISH);
            pos += result.consumed; encoded.insert(encoded.end(), buffer.begin(), buffer.begin() + result.produced);
            if (result.status == ESTREAM_FINISHED) { break; }
            DOCTEST_REQUIRE((result.consumed || result.produced));
        }
        DOCTEST_CHECK(pos == input.size());
        DOCTEST_CHECK_FALSE(encoder->supportsFlush(EFLUSH_PARTIAL));
        pos = 0;
        while (pos < encoded.size()) {
            auto result = decoder->process(SReadOnlyByteSpan(encoded).subspan(pos, 1), buffer, EINPUT_MORE);
            pos += result.consumed; decoded.insert(decoded.end(), buffer.begin(), buffer.begin() + result.produced);
            if (result.status == ESTREAM_FINISHED) { break; }
            DOCTEST_REQUIRE((result.consumed || result.produced));
        }
        if (decoder->state() != ESTATE_FINISHED) {
            for (;;) {
                const auto result = decoder->process({}, buffer, EINPUT_FINAL);
                decoded.insert(decoded.end(), buffer.begin(), buffer.begin() + result.produced);
                if (result.status == ESTREAM_FINISHED) { break; }
                DOCTEST_REQUIRE(result.produced);
            }
        }
        DOCTEST_CHECK(decoded == input);
        DOCTEST_CHECK(IDecompressor::create(options)->decompress(ICompressor::create(options)->compress(input)) == input);
        encoded.pop_back(); decoder->reset();
        DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(encoded), CCompressionError);
    }
}

DOCTEST_TEST_CASE("DEFLATE streaming preserves a dictionary across partial sync full and final flushes") {
    for (auto mode : {EFLUSH_PARTIAL, EFLUSH_SYNC, EFLUSH_FULL}) {
        auto encoder = IStreamCompressor::create(); auto decoder = IStreamDecompressor::create();
        SBytes encoded, decoded; std::array<std::uint8_t, 3> buffer{};
        auto encode = [&](SReadOnlyByteSpan input, EFlushMode flush) {
            std::size_t pos = 0;
            for (unsigned calls = 0; calls < 10000; ++calls) {
                const auto result = encoder->process(input.subspan(pos), buffer, flush);
                pos += result.consumed; encoded.insert(encoded.end(), buffer.begin(), buffer.begin() + result.produced);
                if (result.status == ESTREAM_FLUSHED || result.status == ESTREAM_FINISHED) { DOCTEST_CHECK(pos == input.size()); return; }
            }
            DOCTEST_FAIL("encoder failed to terminate");
        };
        const auto chunk = bytes("abcabcabcabcabcabcabcabcabcabc");
        encode(chunk, mode);
        for (auto value : encoded) {
            const std::array<std::uint8_t, 1> input{value}; std::size_t pos = 0;
            for (;;) {
                const auto result = decoder->process(SReadOnlyByteSpan(input).subspan(pos), buffer);
                pos += result.consumed; decoded.insert(decoded.end(), buffer.begin(), buffer.begin() + result.produced);
                if (result.status == ESTREAM_NEED_INPUT) { break; }
            }
        }
        DOCTEST_CHECK(decoded == chunk);
        encoded.clear(); encode(chunk, EFLUSH_FINISH);
        std::size_t pos = 0;
        for (;;) {
            const auto result = decoder->process(SReadOnlyByteSpan(encoded).subspan(pos), buffer, EINPUT_FINAL);
            pos += result.consumed; decoded.insert(decoded.end(), buffer.begin(), buffer.begin() + result.produced);
            if (result.status == ESTREAM_FINISHED) { break; }
        }
        auto expected = chunk; expected.insert(expected.end(), chunk.begin(), chunk.end());
        DOCTEST_CHECK(decoded == expected);
        DOCTEST_CHECK(encoder->state() == ESTATE_FINISHED);
        decoder->reset(); DOCTEST_CHECK(decoder->state() == ESTATE_RUNNING);
    }
}

DOCTEST_TEST_CASE("truncation checksum damage and configured output limits fail explicitly") {
    for (auto algorithm : {EALGORITHM_LZ77, EALGORITHM_LZ78, EALGORITHM_LZMA, EALGORITHM_DEFLATE,
            EALGORITHM_BZIP2, EALGORITHM_ZSTD, EALGORITHM_BROTLI, EALGORITHM_SNAPPY, EALGORITHM_LZ4}) {
        SCompressionOptions options; options.algorithm = algorithm;
        auto compressed = ICompressor::create(options)->compress(bytes("a long enough payload a long enough payload"));
        compressed.pop_back();
        DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(compressed), CCompressionError);
        compressed = ICompressor::create(options)->compress(SBytes(10000, 42));
        options.maxOutputBytes = 10;
        DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(compressed), CCompressionError);
    }
    for (auto algorithm : {EALGORITHM_DEFLATE, EALGORITHM_BZIP2, EALGORITHM_ZSTD, EALGORITHM_SNAPPY, EALGORITHM_LZ4}) {
        SCompressionOptions options; options.algorithm = algorithm;
        auto compressed = ICompressor::create(options)->compress(bytes("checksummed payload checksummed payload"));
        const auto pos = algorithm == EALGORITHM_BZIP2 ? 10u : algorithm == EALGORITHM_SNAPPY ? 14u :
            algorithm == EALGORITHM_LZ4 ? 14u : compressed.size() - 1;
        compressed[pos] ^= 1;
        DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(compressed), CCompressionError);
    }
}

DOCTEST_TEST_CASE("incremental gzip optional headers remain bounded and preserve header checksums") {
    SCompressionOptions options; options.format = EFORMAT_GZIP;
    const auto plain = bytes("gzip extended header payload");
    const auto encoded = ICompressor::create(options)->compress(plain);
    SBytes extended(encoded.begin(), encoded.begin() + 10); extended[3] = 30;
    extended.insert(extended.end(), {3,0,'x','y','z'});
    extended.insert(extended.end(), 200000, 'f'); extended.push_back(0);
    extended.insert(extended.end(), {'c','o','m','m','e','n','t',0});
    std::uint32_t crc = 0xffffffffu;
    for (auto value : extended) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit) { crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    }
    const auto checksumPos = extended.size();
    extended.push_back(static_cast<std::uint8_t>(~crc)); extended.push_back(static_cast<std::uint8_t>(~crc >> 8));
    extended.insert(extended.end(), encoded.begin() + 10, encoded.end());
    DOCTEST_CHECK(IDecompressor::create(options)->decompress(extended) == plain);
    extended[checksumPos] ^= 1;
    DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(extended), CCompressionError);
    extended[checksumPos] ^= 1; options.maxWindowBytes = 32768;
    DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(extended), CCompressionError);
    options.format = EFORMAT_RAW;
    // --> A single code of length one is not a complete code-length alphabet.
    SBytes incomplete; unsigned pos = 0;
    auto put = [&](unsigned value, unsigned count) {
        for (unsigned bit = 0; bit < count; ++bit, ++pos) {
            if (pos / 8 == incomplete.size()) { incomplete.push_back(0); }
            incomplete[pos / 8] |= static_cast<std::uint8_t>(((value >> bit) & 1) << (pos % 8));
        }
    };
    put(1, 1); put(2, 2); put(0, 5); put(0, 5); put(0, 4);
    put(0, 3); put(0, 3); put(0, 3); put(1, 3);
    try { static_cast<void>(IDecompressor::create(options)->decompress(incomplete)); DOCTEST_FAIL("incomplete alphabet accepted"); }
    catch (const CCompressionError& error) { DOCTEST_CHECK(error.code() == EERROR_INVALID_DATA); }
}

DOCTEST_TEST_CASE("async operations retain inputs instances and execute on worker") {
    asyncpp::thread_pool workers(1);
    auto compressor = ICompressor::create(); auto task = compressor->compressAsync(bytes("async payload"), workers);
    compressor.reset(); const auto encoded = asyncpp::as_promise(std::move(task)).get();
    DOCTEST_CHECK(asyncpp::as_promise(IDecompressor::create()->decompressAsync(encoded, workers)).get() == bytes("async payload"));
}

DOCTEST_TEST_CASE("random malformed inputs and mutated streams remain bounded") {
    std::minstd_rand random(7654321);
    for (unsigned algorithm = 0; algorithm <= EALGORITHM_LZ4; ++algorithm) {
        SCompressionOptions options; options.algorithm = static_cast<EAlgorithm>(algorithm); options.maxOutputBytes = 32768;
        const auto valid = ICompressor::create(options)->compress(bytes("random mutation corpus repeated repeated repeated"));
        auto decoder = IDecompressor::create(options);
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            SBytes input;
            if (iteration & 1) {
                input = valid;
                for (unsigned i = 0; i < 3 && !input.empty(); ++i) { input[random() % input.size()] ^= static_cast<std::uint8_t>(1u << (random() % 8)); }
            } else { input.resize(random() % 128); for (auto& value : input) { value = static_cast<std::uint8_t>(random()); } }
            try { const auto decoded = decoder->decompress(input); DOCTEST_CHECK(decoded.size() <= options.maxOutputBytes); }
            catch (const CCompressionError&) {}
        }
    }
}

DOCTEST_TEST_CASE("stream zero buffers lifecycle errors reset and suffix ownership are explicit") {
    for (auto format : {EFORMAT_ZLIB, EFORMAT_CHUNKED}) {
        SCompressionOptions options; options.format = format;
        auto encoder = IStreamCompressor::create(options); auto decoder = IStreamDecompressor::create(options);
        auto input = bytes("payload");
        const auto zero = encoder->process(input, {}, EFLUSH_FINISH);
        DOCTEST_CHECK(zero.consumed == 0); DOCTEST_CHECK(zero.produced == 0); DOCTEST_CHECK(zero.status == ESTREAM_NEED_OUTPUT);
        auto compressed = ICompressor::create(options)->compress(input); compressed.push_back(255);
        std::array<std::uint8_t, 100> output{};
        const auto result = decoder->process(compressed, output, EINPUT_FINAL);
        DOCTEST_CHECK(result.status == ESTREAM_FINISHED);
        DOCTEST_CHECK(result.consumed == compressed.size() - 1);
        DOCTEST_CHECK(result.produced == input.size());
        DOCTEST_CHECK_THROWS_AS(IDecompressor::create(options)->decompress(compressed), CCompressionError);
        decoder->reset(); compressed.resize(2);
        DOCTEST_CHECK_THROWS_AS(decoder->process(compressed, output, EINPUT_FINAL), CCompressionError);
        DOCTEST_CHECK(decoder->state() == ESTATE_FAILED);
        DOCTEST_CHECK_THROWS_AS(decoder->process({}, output), CCompressionError);
        decoder->reset(); DOCTEST_CHECK(decoder->state() == ESTATE_RUNNING);
    }
    SCompressionOptions policy; policy.level = 0;
    DOCTEST_CHECK_THROWS_AS(ICompressor::create(policy), std::invalid_argument);
    policy = {}; policy.algorithm = EALGORITHM_SNAPPY; policy.format = EFORMAT_GZIP;
    DOCTEST_CHECK_THROWS_AS(ICompressor::create(policy), CCompressionError);
}

DOCTEST_TEST_CASE("async stream tasks preserve unconsumed bytes and do not borrow caller lifetime") {
    class ManualWorker final : public asyncpp::dispatcher {
    private:
        std::deque<std::function<void()>> _queue;
    public:
        void push(std::function<void()> fn) override { _queue.push_back(std::move(fn)); }
        void run() { while (!_queue.empty()) { auto fn = std::move(_queue.front()); _queue.pop_front(); fn(); } }
    } worker;
    auto encoder = IStreamCompressor::create();
    const auto input = bytes("this input must survive the suspended operation");
    auto task = encoder->processAsync(input, 1, EFLUSH_FINISH, worker);
    encoder.reset();
    auto promise = asyncpp::as_promise(std::move(task)); worker.run();
    const auto result = promise.get();
    DOCTEST_CHECK(result.output.size() == 1);
    DOCTEST_CHECK(result.progress.consumed == 0);
    DOCTEST_CHECK(result.remainingInput == input);
    DOCTEST_CHECK(result.progress.status == ESTREAM_NEED_OUTPUT);
}

} // namespace
