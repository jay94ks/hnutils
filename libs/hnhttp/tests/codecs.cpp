#include <hnhttp/qpack.hpp>
#include <doctest/doctest.h>

namespace {
using namespace hnhttp;
SBytes hex(std::string_view value) {
    SBytes bytes;
    for (std::size_t i = 0; i < value.size(); i += 2) { bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(value.substr(i,2)), nullptr, 16))); }
    return bytes;
}
DOCTEST_TEST_CASE("HPACK Huffman decodes RFC7541 C4 sequential requests and rejects EOS padding") {
    auto codec = IHeaderCodec::createHpack();
    auto first = codec->decode(hex("828684418cf1e3c2e5f23a6ba0ab90f4ff"));
    DOCTEST_REQUIRE(first.result.code == ERET_OK); DOCTEST_REQUIRE(first.fields.size() == 4);
    DOCTEST_CHECK(first.fields[3].value == "www.example.com");
    auto second = codec->decode(hex("828684be5886a8eb10649cbf"));
    DOCTEST_REQUIRE(second.result.code == ERET_OK); DOCTEST_CHECK(second.fields.back().value == "no-cache");
    auto third = codec->decode(hex("828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf"));
    DOCTEST_REQUIRE(third.result.code == ERET_OK); DOCTEST_CHECK(third.fields.back().value == "custom-value");
    for (const auto& wire : {"0084ffffffff00", "0081ff00", "00810000"}) {
        DOCTEST_CHECK(IHeaderCodec::createHpack()->decode(hex(wire)).result.code == ERET_MALFORMED);
    }
    SHeaderCodecOptions limits; limits.maximumDecodedBytes = 40;
    DOCTEST_CHECK(IHeaderCodec::createHpack(limits)->decode(hex("018cf1e3c2e5f23a6ba0ab90f4ff")).result.code == ERET_LIMIT);
}
DOCTEST_TEST_CASE("QPACK RFC9204 B dynamic postbase speculative duplicate cancellation eviction") {
    auto codec = IQpackCodec::create();
    const auto blocked = hex("03811011");
    DOCTEST_CHECK(codec->decodeSection(4, blocked).result.code == ERET_BLOCKED);
    auto instructions = hex("3fbd01c00f7777772e6578616d706c652e636f6dc10c2f73616d706c652f70617468");
    for (const auto byte : instructions) { DOCTEST_CHECK(codec->receiveEncoderInstructions(SBytes{byte}).code == ERET_OK); }
    DOCTEST_CHECK(codec->receivedInsertCount() == 2);
    auto decoded = codec->decodeSection(4, blocked); DOCTEST_REQUIRE(decoded.result.code == ERET_OK);
    DOCTEST_REQUIRE(decoded.fields.size() == 2); DOCTEST_CHECK(decoded.fields[0].value == "www.example.com");
    DOCTEST_CHECK(decoded.fields[1].value == "/sample/path");
    auto feedback = codec->takeDecoderInstructions(); DOCTEST_REQUIRE_FALSE(feedback.empty()); DOCTEST_CHECK(feedback.back() == 0x84);
    DOCTEST_CHECK(codec->receiveEncoderInstructions(hex("4a637573746f6d2d6b65790c637573746f6d2d76616c7565")).code == ERET_OK);
    DOCTEST_CHECK(codec->decodeSection(8, hex("050080c181")).result.code == ERET_BLOCKED);
    DOCTEST_CHECK(codec->cancelStream(8).bytes == hex("48"));
    DOCTEST_CHECK(codec->receiveEncoderInstructions(hex("02810d637573746f6d2d76616c756532")).code == ERET_OK);
    DOCTEST_CHECK(codec->receivedInsertCount() == 5);
    auto latest = codec->decodeSection(12, hex("060080"));
    DOCTEST_REQUIRE(latest.result.code == ERET_OK); DOCTEST_CHECK(latest.fields[0].value == "custom-value2");
}
DOCTEST_TEST_CASE("QPACK encoder protects referenced entries and validates decoder feedback") {
    auto sender = IQpackCodec::create(), receiver = IQpackCodec::create();
    DOCTEST_CHECK(sender->configurePeer(128,1) == ERET_OK);
    auto capacity = sender->setEncoderCapacity(128); DOCTEST_REQUIRE(capacity.result.code == ERET_OK);
    DOCTEST_CHECK(receiver->receiveEncoderInstructions(capacity.bytes).code == ERET_OK);
    auto insertion = sender->insert({"custom-key","custom-value"}); DOCTEST_REQUIRE(insertion.result.code == ERET_OK);
    std::vector<SHeaderField> fields{{"custom-key","custom-value"}};
    auto wire = sender->encodeSection(0, fields, true); DOCTEST_REQUIRE(wire.result.code == ERET_OK);
    DOCTEST_CHECK(receiver->decodeSection(0, wire.bytes).result.code == ERET_BLOCKED);
    DOCTEST_CHECK(sender->setEncoderCapacity(0).result.code == ERET_BUSY);
    DOCTEST_CHECK(receiver->receiveEncoderInstructions(insertion.bytes).code == ERET_OK);
    DOCTEST_CHECK(receiver->decodeSection(0, wire.bytes).result.code == ERET_OK);
    DOCTEST_CHECK(sender->receiveDecoderInstructions(receiver->takeDecoderInstructions()).code == ERET_OK);
    DOCTEST_CHECK(sender->setEncoderCapacity(0).result.code == ERET_OK);
    DOCTEST_CHECK(sender->insert({"authorization","secret"}).result.code == ERET_INVALID_ARGUMENT);
    DOCTEST_CHECK(sender->receiveDecoderInstructions(hex("80")).code == ERET_MALFORMED);
    DOCTEST_CHECK(sender->encode(fields).result.code == ERET_INVALID_STATE);
}
DOCTEST_TEST_CASE("bounded deterministic malformed compression inputs never fabricate partial fields") {
    std::uint32_t state = 0x16544a3u;
    for (unsigned iteration = 0; iteration < 4096; ++iteration) {
        SBytes bytes; const auto length = iteration%97;
        for (unsigned i = 0; i < length; ++i) { state ^= state<<13; state ^= state>>17; state ^= state<<5; bytes.push_back(static_cast<std::uint8_t>(state)); }
        for (auto codec : {IHeaderCodec::createHpack(),IHeaderCodec::createQpack()}) {
            auto result = codec->decode(bytes);
            if (result.result.code != ERET_OK) { DOCTEST_CHECK(result.fields.empty()); }
            else { DOCTEST_CHECK(result.fields.size() <= 128); }
        }
    }
}
}
