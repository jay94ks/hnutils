#include <hnhttp.hpp>
#include <doctest/doctest.h>

namespace {
using namespace hnhttp;
SResourceMetadata metadata() {
    SResourceMetadata r;
    r.size = 100;
    r.entityTag = "\"v1\"";
    r.lastModified = std::chrono::sys_seconds{std::chrono::seconds{784111777}};
    r.strongLastModified = true;
    return r;
}
SResourceDecision evaluate(std::vector<SHeaderField> fields, EHttpMethod method = HTTP_GET,
                          SResourceMetadata resource = metadata()) {
    auto context = IHttpClientContext::create();
    auto request = context->getRequest();
    for (auto& field : fields) request->headers().add(field.name, field.value);
    return CHttpResource::evaluate(method, request->headers(), resource);
}
DOCTEST_TEST_CASE("resource entity tags use strong If-Match weak If-None-Match lists wildcard and precedence") {
    DOCTEST_CHECK(evaluate({{"If-Match", "\"old\", \"v1\""}}).status == 200);
    DOCTEST_CHECK(evaluate({{"If-Match", "W/\"v1\""}}).status == 412);
    DOCTEST_CHECK(evaluate({{"If-Match", "*"}}).status == 200);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "W/\"v1\""}}).status == 304);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "\"old\""}, {"If-None-Match", "\"v1\""}}).status == 304);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "*"}}, HTTP_HEAD).status == 304);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "*"}}, HTTP_PUT).status == 412);
    DOCTEST_CHECK(evaluate({{"If-Match", "\"old\""}, {"If-None-Match", "\"v1\""}}).status == 412);
    DOCTEST_CHECK(evaluate({{"If-Match", "\"v1\""}, {"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT"}}).status == 200);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "\"old\""}, {"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT"}}).status == 200);
    for (const auto& value : {"\"v1\" garbage", "*, \"v1\"", "\"unterminated", "w/\"v1\"", ""})
        DOCTEST_CHECK(evaluate({{"If-Match", value}}).status == 400);
    auto r = metadata(); r.entityTag.clear();
    DOCTEST_CHECK(evaluate({{"If-Match", "  *  "}}, HTTP_GET, r).status == 200);
    DOCTEST_CHECK(evaluate({{"If-Match", "\"\""}}, HTTP_GET, r).status == 412);
    r.exists = false;
    DOCTEST_CHECK(evaluate({{"If-Match", "*"}}, HTTP_PUT, r).status == 412);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "*"}}, HTTP_PUT, r).status == 200);
    r = metadata(); r.entityTag = "W/\"v1\"";
    DOCTEST_CHECK(evaluate({{"If-Match", "\"v1\""}}, HTTP_GET, r).status == 412);
    DOCTEST_CHECK(evaluate({{"If-None-Match", "\"v1\""}}, HTTP_GET, r).status == 304);
    r.entityTag = "bad";
    DOCTEST_CHECK_THROWS_AS(evaluate({}, HTTP_GET, r), std::invalid_argument);
}
DOCTEST_TEST_CASE("resource date preconditions accept HTTP date forms and ignore invalid dates") {
    for (const auto& date : {"Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994"}) {
        DOCTEST_CHECK(evaluate({{"If-Modified-Since", date}}).status == 304);
        DOCTEST_CHECK(evaluate({{"If-Unmodified-Since", date}}).status == 200);
    }
    DOCTEST_CHECK(evaluate({{"If-Modified-Since", "Sun, 06 Nov 1994 08:49:36 GMT"}}).status == 200);
    DOCTEST_CHECK(evaluate({{"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:36 GMT"}}).status == 412);
    DOCTEST_CHECK(evaluate({{"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT"}}, HTTP_POST).status == 200);
    for (const auto& date : {"invalid", "Sun, 31 Feb 1994 08:49:37 GMT", "Sun, 06 Nov 1994 99:49:37 GMT", "Bad, 06 Nov 1994 08:49:37 GMT"})
        DOCTEST_CHECK(evaluate({{"If-Modified-Since", date}}).status == 200);
}
DOCTEST_TEST_CASE("resource byte ranges cover suffix open end overflow empty files and If-Range validators") {
    struct Case { const char* range; int status; std::uint64_t offset, length; };
    for (const auto& c : {Case{"bytes=0-9",206,0,10}, {"Bytes=90-",206,90,10},
             {"bytes=-12",206,88,12}, {"bytes=-999",206,0,100},
             {"bytes=99-999999999999999999999999",206,99,1}, {"bytes=100-",416,0,0},
             {"bytes=999999999999999999999999-",416,0,0}, {"bytes=-0",416,0,0},
             {"bytes=9-1",200,0,100}, {"items=0-9",200,0,100}, {"bytes=0-1,3-4",200,0,100}}) {
        auto result = evaluate({{"Range", c.range}});
        DOCTEST_CHECK(result.status == c.status);
        DOCTEST_CHECK(result.length == c.length);
        if (c.status != 416) DOCTEST_CHECK(result.offset == c.offset);
    }
    DOCTEST_CHECK(evaluate({{"Range", "bytes=0-9"}}, HTTP_HEAD).status == 200);
    DOCTEST_CHECK(evaluate({{"Range", "bytes=0-9"}}, HTTP_POST).status == 200);
    for (const auto& validator : {"\"v1\"", "Sun, 06 Nov 1994 08:49:37 GMT"})
        DOCTEST_CHECK(evaluate({{"Range", "bytes=0-9"}, {"If-Range", validator}}).status == 206);
    for (const auto& validator : {"W/\"v1\"", "\"old\"", "Sun, 06 Nov 1994 08:49:38 GMT", "bad"})
        DOCTEST_CHECK(evaluate({{"Range", "bytes=0-9"}, {"If-Range", validator}}).status == 200);
    auto r = metadata(); r.strongLastModified = false;
    DOCTEST_CHECK(evaluate({{"Range", "bytes=0-9"}, {"If-Range", "Sun, 06 Nov 1994 08:49:37 GMT"}}, HTTP_GET, r).status == 200);
    r.size = 0;
    DOCTEST_CHECK(evaluate({{"Range", "bytes=0-0"}}, HTTP_GET, r).status == 416);
    DOCTEST_CHECK(evaluate({{"Range", "bytes=999-"}, {"If-None-Match", "\"v1\""}}).status == 304);
}
} // namespace
