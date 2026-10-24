#include "test_transport.hpp"
#include <asyncpp/defer.h>

namespace {
using namespace hnhttp;
class CMemoryBody final : public IHttpBodyReader {
private:
    SBytes _bytes; std::size_t _at = 0, _fragment;
    class CEmptyHeaders final : public IReadOnlyHttpHeaders {
    public:
        std::size_t size() const noexcept override { return 0; }
        std::optional<std::string> get(const std::string&) const override { return {}; }
        std::vector<SHeaderField> entries(std::uint64_t, std::size_t) const override { return {}; }
        std::vector<std::string> values(const std::string&, std::uint64_t, std::size_t) const override { return {}; }
    } _headers;
public:
    CMemoryBody(SBytes bytes, std::size_t fragment) : _bytes(std::move(bytes)),_fragment(fragment) {}
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions) override {
        if (_at == _bytes.size()) { co_return SReadResult{testhttp::error(ERET_EOF),{}}; }
        const auto count = (std::min)({n,_fragment,_bytes.size()-_at}); auto begin = _bytes.begin()+static_cast<std::ptrdiff_t>(_at); _at += count;
        co_return SReadResult{testhttp::error(ERET_OK),SBytes(begin,begin+static_cast<std::ptrdiff_t>(count))};
    }
    asyncpp::task<ERetCode> discard(std::uint64_t n, SOperationOptions) override { if (_bytes.size()-_at > n) { co_return ERET_LIMIT; } _at = _bytes.size(); co_return ERET_OK; }
    asyncpp::task<SReadResult> collect(std::size_t n, SOperationOptions o) override { co_return co_await readSome(n,o); }
    bool isComplete() const noexcept override { return _at == _bytes.size(); }
    const IReadOnlyHttpHeaders& trailers() const override { return _headers; }
    SHttpError lastError() const override { return {}; }
};
std::shared_ptr<IHttpBodyReader> body(std::string value, std::size_t fragment = 1) { return std::make_shared<CMemoryBody>(SBytes(value.begin(),value.end()),fragment); }
std::string string(const SBytes& bytes) { return {bytes.begin(),bytes.end()}; }
asyncpp::task<bool> forms(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher);
    std::string wire = "preamble\r\n--boundary\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\none\r\n--boundaryX\r\n--boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a\\\".bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    wire.append("\0\1\2",3); wire += "\r\n--boundary--\r\nepilogue";
    auto form = IHttpFormReader::create(body(wire),"multipart/form-data; boundary=\"boundary\"");
    auto first = co_await form->next(); DOCTEST_REQUIRE(first.result.code == ERET_OK); DOCTEST_CHECK(first.part->name() == "a");
    DOCTEST_CHECK((co_await form->next()).result.code == ERET_BODY_PENDING);
    auto firstBody = co_await first.part->body()->collect(64); DOCTEST_REQUIRE(firstBody.result.code == ERET_OK); DOCTEST_CHECK(string(firstBody.bytes) == "one\r\n--boundaryX");
    auto second = co_await form->next(); DOCTEST_REQUIRE(second.result.code == ERET_OK); DOCTEST_CHECK(second.part->filename() == std::optional<std::string>{"a\".bin"});
    auto bytes = co_await second.part->body()->collect(3); DOCTEST_CHECK(bytes.result.code == ERET_OK); DOCTEST_CHECK(bytes.bytes == SBytes{0,1,2});
    DOCTEST_CHECK((co_await form->next()).result.code == ERET_EOF);
    auto url = IHttpFormReader::create(body("a=one+two&a=%00%ff&empty="),"application/x-www-form-urlencoded; charset=UTF-8");
    for (const auto& expected : {std::string("one two"),std::string("\0\xff",2),std::string{}}) {
        auto part = co_await url->next(); DOCTEST_REQUIRE(part.result.code == ERET_OK);
        auto data = co_await part.part->body()->collect(8); DOCTEST_CHECK(data.result.code == ERET_OK); DOCTEST_CHECK(string(data.bytes) == expected);
    }
    DOCTEST_CHECK((co_await url->next()).result.code == ERET_EOF);
    auto empty = IHttpFormReader::create(body("--empty--"),"multipart/form-data; boundary=empty");
    DOCTEST_CHECK((co_await empty->next()).result.code == ERET_EOF);
    co_return true;
}
DOCTEST_TEST_CASE("multipart and urlencoded incremental readers preserve binary repeated fields and delimiters") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,forms(watcher)));
}
asyncpp::task<bool> source(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); SFormOptions options; options.boundary = "fixed-boundary";
    auto output = IFormData::create(EFORM_MULTIPART,options); output->addField("a","123").addField("a",42);
    output->addFile("upload","../not-a-host-path.bin",IHttpBodySource::fromBytes({0,1,2}));
    DOCTEST_CHECK(co_await output->prepare() == ERET_OK); DOCTEST_CHECK_THROWS_AS(output->addField("x","y"),std::logic_error);
    SBytes wire;
    while (true) { auto bytes = co_await output->readSome(7); if (bytes.result.code == ERET_EOF) { break; }
        DOCTEST_REQUIRE(bytes.result.code == ERET_OK); wire.insert(wire.end(),bytes.bytes.begin(),bytes.bytes.end());
    }
    DOCTEST_CHECK(output->metadata().contentLength == wire.size());
    auto input = IHttpFormReader::create(std::make_shared<CMemoryBody>(wire,1),output->metadata().contentType);
    for (const auto& expected : {SBytes{'1','2','3'},SBytes{'4','2'},SBytes{0,1,2}}) {
        auto part = co_await input->next(); DOCTEST_REQUIRE(part.result.code == ERET_OK); auto bytes = co_await part.part->body()->collect(expected.size());
        DOCTEST_CHECK(bytes.result.code == ERET_OK); DOCTEST_CHECK(bytes.bytes == expected);
    }
    DOCTEST_CHECK((co_await input->next()).result.code == ERET_EOF);
    DOCTEST_CHECK(co_await output->rewind() == ERET_OK); DOCTEST_CHECK((co_await output->readSome(7)).result.code == ERET_OK);
    auto collision = IFormData::create(EFORM_MULTIPART,options); collision->addField("a","--fixed-boundary");
    DOCTEST_CHECK(co_await collision->prepare() == ERET_BOUNDARY_COLLISION);
    auto truncated = IHttpFormReader::create(body("--b\r\nContent-Disposition: form-data; name=x\r\n\r\nmissing"),"multipart/form-data; boundary=b");
    auto part = co_await truncated->next(); DOCTEST_REQUIRE(part.result.code == ERET_OK);
    DOCTEST_CHECK((co_await part.part->body()->collect()).result.code == ERET_TRUNCATED);
    co_return true;
}
DOCTEST_TEST_CASE("multipart source preflights collisions streams replayably and rejects truncation") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,source(watcher)));
}
}
