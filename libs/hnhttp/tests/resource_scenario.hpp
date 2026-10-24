#ifndef __HNHTTP_TEST_RESOURCE_SCENARIO_HPP__
#define __HNHTTP_TEST_RESOURCE_SCENARIO_HPP__
#include "test_transport.hpp"

namespace testresource {
using namespace hnhttp;
inline SBytes fileBytes() {
    SBytes bytes(100000);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 37);
    return bytes;
}
/** Test-only unknown-length source exercises chunked H1 and END_STREAM/FIN on H2/H3. */
class CUnknownLength final : public IHttpBodySource {
private:
    std::shared_ptr<IHttpBodySource> _source;
public:
    explicit CUnknownLength(SBytes bytes) : _source(IHttpBodySource::fromBytes(std::move(bytes))) {}
    asyncpp::task<ERetCode> prepare(SOperationOptions o) override { return _source->prepare(o); }
    SBodyMetadata metadata() const override { auto m = _source->metadata(); m.contentLength.reset(); return m; }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return _source->readSome(n, o); }
    asyncpp::task<ERetCode> rewind(SOperationOptions o) override { return _source->rewind(o); }
    std::vector<SHeaderField> trailers() const override { return {}; }
    SHttpError lastError() const override { return _source->lastError(); }
};
struct SCase {
    const char* path;
    EHttpMethod method = HTTP_GET;
    std::vector<SHeaderField> headers;
    unsigned status = 200;
    std::uint64_t offset = 0, length = 100000;
};
inline std::vector<SCase> cases() {
    return {{"/multipart", HTTP_POST}, {"/urlencoded", HTTP_POST}, {"/upload", HTTP_POST},
        {"/stream", HTTP_POST}, {"/download"},
        {"/download", HTTP_GET, {{"Range", "bytes=0-1023"}}, 206, 0, 1024},
        {"/download", HTTP_GET, {{"Range", "bytes=-19"}}, 206, 99981, 19},
        {"/download", HTTP_GET, {{"Range", "bytes=99990-"}}, 206, 99990, 10},
        {"/download", HTTP_GET, {{"Range", "bytes=100000-"}}, 416, 0, 0},
        {"/download", HTTP_GET, {{"If-None-Match", "W/\"v1\""}}, 304, 0, 0},
        {"/download", HTTP_GET, {{"If-Match", "W/\"v1\""}}, 412, 0, 0},
        {"/download", HTTP_GET, {{"If-Match", "\"old\", \"v1\""}}},
        {"/download", HTTP_GET, {{"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT"}}, 304, 0, 0},
        {"/download", HTTP_GET, {{"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT"}}, 412, 0, 0},
        {"/download", HTTP_GET, {{"Range", "bytes=10-19"}, {"If-Range", "\"v1\""}}, 206, 10, 10},
        {"/download", HTTP_GET, {{"Range", "bytes=10-19"}, {"If-Range", "\"old\""}}},
        {"/download", HTTP_GET, {{"If-None-Match", "\"old\""}, {"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT"}}},
        {"/download", HTTP_PUT, {{"If-None-Match", "*"}}, 412, 0, 0},
        {"/download", HTTP_HEAD, {{"Range", "bytes=0-9"}}, 200, 0, 0},
        {"/download"}};
}
inline asyncpp::task<void> handle(std::shared_ptr<IHttpServerContext> context) {
    auto req = context->getRequest(); auto res = context->getResponse();
    auto file = fileBytes();
    if (req->path() == "/multipart" || req->path() == "/urlencoded") {
        DOCTEST_CHECK(req->queries().get("q") == "a+b /?=&");
        DOCTEST_CHECK(req->queries().get("page") == "42");
        auto reader = IHttpFormReader::create(req->getBody(), *req->headers().get("content-type"));
        const std::string names[]{"name +&=", "name +&=", "empty", "file"};
        const SBytes expected[]{SBytes{'o','n','e',' ','+','&','='}, SBytes{0,255}, {}, file};
        for (unsigned i = 0; i < 4; ++i) {
            auto part = co_await reader->next(); DOCTEST_REQUIRE(part.result.code == ERET_OK);
            DOCTEST_CHECK(part.part->name() == names[i]);
            if (req->path() == "/multipart" && i == 3)
                DOCTEST_CHECK(part.part->filename() == std::optional<std::string>{"../untrusted.bin"});
            else DOCTEST_CHECK_FALSE(part.part->filename());
            auto bytes = co_await part.part->body()->collect(200000);
            DOCTEST_REQUIRE(bytes.result.code == ERET_OK);
            DOCTEST_CHECK(bytes.bytes == expected[i]);
        }
        DOCTEST_CHECK((co_await reader->next()).result.code == ERET_EOF);
        res->body(IHttpBodySource::fromBytes(file));
    } else if (req->path() == "/upload" || req->path() == "/stream") {
        auto body = co_await req->getBody()->collect(200000);
        DOCTEST_REQUIRE(body.result.code == ERET_OK); DOCTEST_CHECK(body.bytes == file);
        if (req->path() == "/stream") {
            DOCTEST_CHECK_FALSE(req->headers().get("content-length"));
            if (req->version() == HTTP_11) DOCTEST_CHECK(req->headers().get("transfer-encoding") == "chunked");
            res->body(std::make_shared<CUnknownLength>(file));
        } else res->body(IHttpBodySource::fromBytes(file));
    } else {
        DOCTEST_CHECK((co_await req->getBody()->collect(16)).result.code == ERET_OK);
        SResourceMetadata metadata;
        metadata.size = file.size(); metadata.entityTag = "\"v1\"";
        metadata.lastModified = std::chrono::sys_seconds{std::chrono::seconds{784111777}};
        auto decision = CHttpResource::evaluate(req->method(), req->headers(), metadata);
        res->status(decision.status);
        for (auto& header : decision.headers) res->headers().set(header.name, header.value);
        res->headers().set("Cache-Control", "public, max-age=60").set("Vary", "Accept-Encoding")
            .set("Expires", "Sun, 06 Nov 1994 08:50:37 GMT").set("Content-Disposition", "attachment; filename=sample.bin");
        if (decision.sendBody) {
            SBytes selected(file.begin() + static_cast<std::ptrdiff_t>(decision.offset),
                file.begin() + static_cast<std::ptrdiff_t>(decision.offset + decision.length));
            res->body(IHttpBodySource::fromBytes(std::move(selected)));
        } else if (req->method() == HTTP_HEAD && decision.status == 200) {
            res->body(IHttpBodySource::fromBytes(file)); // Metadata only; serializer must suppress bytes.
        }
    }
    DOCTEST_REQUIRE(co_await res->exec() == ERET_OK);
    DOCTEST_CHECK(co_await context->finish() == ERET_OK);
}
inline asyncpp::task<void> serve(std::shared_ptr<IHttpConnection> connection) {
    auto mux = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    for (std::size_t i = 0; i < cases().size(); ++i) {
        std::shared_ptr<IHttpServerContext> context;
        if (mux) {
            auto result = co_await mux->accept(); DOCTEST_REQUIRE(result.result.code == ERET_OK);
            context = result.context;
        } else {
            context = IHttpServerContext::create(std::dynamic_pointer_cast<IHttp1Connection>(connection));
            DOCTEST_REQUIRE(co_await context->getRequest()->parseOnce() == ERET_OK);
        }
        co_await handle(context);
    }
}
inline asyncpp::task<void> requests(std::shared_ptr<IHttpClient> client) {
    const auto original = client->getConnection();
    auto file = fileBytes();
    for (const auto& item : cases()) {
        auto context = client->createRequest(); auto req = context->getRequest();
        req->method(item.method).path(item.path);
        for (auto& header : item.headers) req->headers().add(header.name, header.value);
        if (std::string{item.path} == "/multipart" || std::string{item.path} == "/urlencoded") {
            auto multipart = std::string{item.path} == "/multipart";
            auto form = IFormData::create(multipart ? EFORM_MULTIPART : EFORM_URLENCODED);
            form->addField("name +&=", "one +&=").addField("name +&=", std::string{"\0\xff", 2}).addField("empty", "");
            if (multipart) form->addFile("file", "../untrusted.bin", IHttpBodySource::fromBytes(file));
            else {
                DOCTEST_CHECK_THROWS_AS(form->addFile("file", "x.bin", IHttpBodySource::fromBytes(file)), std::system_error);
                form->addField("file", std::string{file.begin(), file.end()});
            }
            req->queries().set("q", "a+b /?=&").set("page", 42);
            req->body(form);
        } else if (std::string{item.path} == "/upload") req->body(IHttpBodySource::fromBytes(file));
        else if (std::string{item.path} == "/stream") req->body(std::make_shared<CUnknownLength>(file));
        DOCTEST_REQUIRE(co_await req->exec() == ERET_OK);
        auto res = context->getResponse(); DOCTEST_CHECK(res->status() == item.status);
        if (std::string{item.path} == "/download") {
            DOCTEST_CHECK(res->headers().get("etag") == "\"v1\"");
            DOCTEST_CHECK(res->headers().get("cache-control") == "public, max-age=60");
            DOCTEST_CHECK(res->headers().get("vary") == "Accept-Encoding");
            DOCTEST_CHECK(res->headers().get("last-modified") == "Sun, 06 Nov 1994 08:49:37 GMT");
            if (item.status == 206) DOCTEST_CHECK(res->headers().get("content-range") ==
                "bytes " + std::to_string(item.offset) + "-" + std::to_string(item.offset + item.length - 1) + "/100000");
            if (item.status == 416) DOCTEST_CHECK(res->headers().get("content-range") == "bytes */100000");
        }
        if (std::string{item.path} == "/stream") {
            DOCTEST_CHECK_FALSE(res->headers().get("content-length"));
            if (res->version() == HTTP_11) DOCTEST_CHECK(res->headers().get("transfer-encoding") == "chunked");
        }
        auto body = co_await res->getBody()->collect(200000);
        DOCTEST_REQUIRE(body.result.code == ERET_OK);
        SBytes expected(file.begin() + static_cast<std::ptrdiff_t>(item.offset),
            file.begin() + static_cast<std::ptrdiff_t>(item.offset + item.length));
        DOCTEST_CHECK(body.bytes == expected);
        DOCTEST_CHECK(co_await req->waitSent() == ERET_OK);
        DOCTEST_CHECK(co_await context->finish() == ERET_OK);
        DOCTEST_CHECK(client->getConnection() == original);
        DOCTEST_CHECK(original->canStartExchange());
    }
}
} // namespace testresource
#endif
