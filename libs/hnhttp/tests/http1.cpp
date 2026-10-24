#include <hnhttp.hpp>
#include <asyncpp/launch.h>
#include <asyncpp/defer.h>
#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>

namespace {
using namespace hnhttp;
SHttpError code(ERetCode ret) { SHttpError value; value.code = ret; return value; }
void ok(ERetCode value) { if (value != ERET_OK) { throw std::runtime_error("HTTP operation failed: "+std::to_string(value)); } }
template<typename T> T drive(std::shared_ptr<sockpp::CSocketWatcher> watcher, asyncpp::task<T> task) {
    auto future = asyncpp::as_promise(std::move(task));
    while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) { watcher->runOne(std::chrono::milliseconds{10}); }
    auto result = future.get();
    // --> Drain posted close requests before the final watcher reference is released.
    while (watcher->runOne(std::chrono::milliseconds{0})) {}
    return result;
}
class Script final : public IHttpStream {
private:
    std::shared_ptr<sockpp::CSocketWatcher> _watcher;
    std::string _input, _output;
    std::size_t _fragment, _position = 0;
    std::atomic<bool> _closed{false};
public:
    Script(std::shared_ptr<sockpp::CSocketWatcher> w, std::string input, std::size_t fragment = 3)
        : _watcher(std::move(w)), _input(std::move(input)), _fragment(fragment) {}
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _watcher; }
    SAsyncCapabilities capabilities() const noexcept override { return {}; }
    STransportSecurity security() const override { return {}; }
    void close() noexcept override { _closed = true; }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions) override { co_return _closed ? ERET_OK : ERET_INVALID_STATE; }
    asyncpp::task<ERetCode> finishWrite(SOperationOptions) override { co_return ERET_OK; }
    asyncpp::task<SReadResult> readSome(std::size_t maximum, SOperationOptions) override {
        if (_closed) { co_return SReadResult{code(ERET_CLOSED), {}}; }
        const auto count = (std::min)({maximum, _fragment, _input.size()-_position});
        SBytes value(count); if (count) { std::memcpy(value.data(), _input.data()+_position, count); } _position += count;
        co_await asyncpp::defer(*_watcher); co_return SReadResult{code(count ? ERET_OK : ERET_EOF), std::move(value)};
    }
    asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions) override {
        const auto count = (std::min<std::size_t>)(5, bytes.size()); _output.append(reinterpret_cast<const char*>(bytes.data()), count);
        co_await asyncpp::defer(*_watcher); co_return SWriteResult{code(ERET_OK), count};
    }
    std::string output() const { return _output; }
};
asyncpp::task<ERetCode> parseFixture(std::shared_ptr<sockpp::CSocketWatcher> watcher, std::string text, ERetCode expected, std::size_t fragment) {
    co_await asyncpp::defer(*watcher);
    auto stream = std::make_shared<Script>(watcher, std::move(text), fragment);
    auto context = IHttpServerContext::create(); ok(context->setStream(stream));
    auto result = co_await context->getRequest()->parseOnce();
    DOCTEST_CHECK(result == expected);
    if (result == ERET_OK) {
        DOCTEST_CHECK(context->getRequest()->path() == "/hello");
        DOCTEST_CHECK(context->getRequest()->queries().get("x") == "a+b c");
        auto body = co_await context->getRequest()->getBody()->collect(128); ok(body.result.code);
        DOCTEST_CHECK(std::string(body.bytes.begin(), body.bytes.end()) == "test");
        DOCTEST_CHECK(context->getRequest()->getBody()->trailers().get("x-end") == "yes");
        context->getResponse()->status(404).body(IHttpBodySource::fromBytes({'o','k'}, "text/plain"));
        ok(co_await context->getResponse()->exec()); ok(co_await context->finish());
        DOCTEST_CHECK(stream->output() == "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok");
    }
    auto connection = context->getConnection(); connection->close(); ok(co_await connection->waitClosed()); co_return result;
}
DOCTEST_TEST_CASE("HTTP1 fragmented chunked request trailers and independent response bytes") {
    const std::string text = "POST /hello?x=a+b%20c HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n0\r\nX-End: yes\r\n\r\n";
    for (const auto fragment : {1u, 3u, 4096u}) {
        auto watcher = std::make_shared<sockpp::CSocketWatcher>();
        DOCTEST_CHECK(drive(watcher, parseFixture(watcher, text, ERET_OK, fragment)) == ERET_OK);
    }
}
DOCTEST_TEST_CASE("HTTP1 rejects ambiguous framing smuggling syntax and truncation") {
    const std::vector<std::pair<std::string, ERetCode>> fixtures = {
        {"GET / HTTP/1.1\nHost: x\n\n", ERET_MALFORMED},
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", ERET_MALFORMED},
        {"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", ERET_MALFORMED},
        {"GET / HTTP/1.1\r\nHost: x\r\n bad: fold\r\n\r\n", ERET_MALFORMED},
        {"GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", ERET_MALFORMED},
        {"GET / HTTP/1.1\r\n\r\n", ERET_MALFORMED},
        {"GET / HTTP/1.1\r\nHost: x\r\n", ERET_TRUNCATED}
    };
    for (const auto& f : fixtures) {
        auto watcher = std::make_shared<sockpp::CSocketWatcher>();
        DOCTEST_CHECK(drive(watcher, parseFixture(watcher, f.first, f.second, 1)) == f.second);
    }
}
asyncpp::task<bool> readAhead(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher);
    auto stream = std::make_shared<Script>(watcher,
        "GET /one HTTP/1.1\r\nHost: x\r\n\r\nGET /two HTTP/1.1\r\nHost: x\r\n\r\n", 4096);
    auto first = IHttpServerContext::create(); ok(first->setStream(stream));
    ok(co_await first->getRequest()->parseOnce()); DOCTEST_CHECK(first->getRequest()->path() == "/one");
    ok(co_await first->getResponse()->exec()); ok(co_await first->finish());
    auto connection = std::dynamic_pointer_cast<IHttp1Connection>(first->getConnection());
    auto second = IHttpServerContext::create(connection);
    ok(co_await first->abort()); // A completed old context must not close the new exchange.
    ok(co_await second->getRequest()->parseOnce()); DOCTEST_CHECK(second->getRequest()->path() == "/two");
    // --> Old facades remain immutable snapshots rather than being reset for the second request.
    DOCTEST_CHECK(first->getRequest()->path() == "/one");
    ok(co_await second->getResponse()->exec()); ok(co_await second->finish());
    connection->close(); ok(co_await connection->waitClosed()); co_return true;
}
DOCTEST_TEST_CASE("HTTP1 reuses buffered pipelined bytes without replacing old facades") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, readAhead(watcher)));
}
asyncpp::task<bool> chunkedWire(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher);
    auto stream = std::make_shared<Script>(watcher,
        "GET /one HTTP/1.1\r\nHost: x\r\n\r\nGET /two HTTP/1.1\r\nHost: x\r\n\r\n", 1);
    auto first = IHttpServerContext::create(); ok(first->setStream(stream));
    ok(co_await first->getRequest()->parseOnce());
    SSendOptions options; options.framing = EFRAMING_CHUNKED;
    first->getResponse()->options(options).body(IHttpBodySource::fromBytes({'h','e','l','l','o'}));
    ok(co_await first->getResponse()->exec()); ok(co_await first->finish());
    auto connection = std::dynamic_pointer_cast<IHttp1Connection>(first->getConnection());
    auto second = IHttpServerContext::create(connection);
    ok(co_await second->getRequest()->parseOnce());
    ok(co_await second->getResponse()->exec()); ok(co_await second->finish());
    DOCTEST_CHECK(stream->output() ==
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    connection->close(); ok(co_await connection->waitClosed()); co_return true;
}
DOCTEST_TEST_CASE("HTTP1 outgoing chunked wire has exact delimiters terminal zero and next response boundary") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, chunkedWire(watcher)));
}
asyncpp::task<void> serve(std::shared_ptr<IHttpServer> server, std::exception_ptr& failure) {
    std::shared_ptr<IHttpConnection> connection;
    try {
        auto accepted = co_await server->accept(); ok(accepted.result.code); auto context = accepted.context;
        connection = context->getConnection();
        for (unsigned index = 0; index < 2; ++index) {
            if (index) { context = IHttpServerContext::create(std::dynamic_pointer_cast<IHttp1Connection>(connection)); ok(co_await context->getRequest()->parseOnce()); }
            auto body = co_await context->getRequest()->getBody()->collect(1024*1024); ok(body.result.code);
            DOCTEST_CHECK(context->getRequest()->path() == "/echo");
            DOCTEST_CHECK(context->getRequest()->queries().get("page") == std::to_string(index));
            context->getResponse()->status(200).body(IHttpBodySource::fromBytes(std::move(body.bytes)));
            if (index == 1 && context->getRequest()->version() == HTTP_11) { SSendOptions options; options.framing = EFRAMING_CHUNKED; context->getResponse()->options(options); }
            ok(co_await context->getResponse()->exec()); ok(co_await context->finish());
        }
    } catch (...) { failure = std::current_exception(); server->close(); }
    if (connection) { connection->close(); co_await connection->waitClosed(); }
}
asyncpp::task<bool> tcpScenario(std::shared_ptr<sockpp::CSocketWatcher> watcher, EHttpVersion version = HTTP_11) {
    co_await asyncpp::defer(*watcher);
    SHttpConnectionOptions options; options.version = version;
    options.http1.allowHttp10KeepAlive = version == HTTP_10;
    auto server = IHttpServer::create(watcher, options); ok(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)));
    auto client = IHttpClient::create(watcher, options);
    asyncpp::async_launch_scope scope; std::exception_ptr serverFailure, clientFailure;
    scope.launch(serve(server, serverFailure));
    try {
        ok(co_await client->connect(server->localAddress()));
        for (unsigned index = 0; index < 2; ++index) {
            auto context = client->createRequest(); auto request = context->getRequest();
            request->path("/echo").authority("localhost").method(HTTP_POST); request->queries().set("page", index);
            SBytes bytes(128*1024, static_cast<std::uint8_t>(index+1)); bytes[0] = 0; bytes.back() = 255;
            request->body(IHttpBodySource::fromBytes(bytes));
            if (index == 0 && version == HTTP_11) { SSendOptions options; options.framing = EFRAMING_CHUNKED; request->options(options); }
            ok(co_await request->exec()); DOCTEST_CHECK(context->getResponse()->status() == 200);
            auto result = co_await context->getResponse()->getBody()->collect(1024*1024); ok(result.result.code);
            DOCTEST_CHECK(result.bytes == bytes); ok(co_await request->waitSent()); ok(co_await context->finish());
        }
    } catch (...) { clientFailure = std::current_exception(); }
    client->close(); server->close(); co_await scope.join();
    ok(co_await client->waitClosed()); ok(co_await server->waitClosed());
    if (serverFailure) { std::rethrow_exception(serverFailure); } if (clientFailure) { std::rethrow_exception(clientFailure); }
    co_return true;
}
DOCTEST_TEST_CASE("plaintext TCP HTTP client server exchange binary bodies and reuse one connection") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, tcpScenario(watcher)));
}
DOCTEST_TEST_CASE("HTTP10 keep alive reuses one TCP connection for consecutive binary upload download") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, tcpScenario(watcher, HTTP_10)));
}
asyncpp::task<ERetCode> bodyCeiling(std::shared_ptr<sockpp::CSocketWatcher> watcher, std::string text, std::size_t maximum) {
    co_await asyncpp::defer(*watcher); auto stream = std::make_shared<Script>(watcher, std::move(text), 4096);
    auto context = IHttpServerContext::create(); ok(context->setStream(stream)); ok(co_await context->getRequest()->parseOnce());
    auto body = co_await context->getRequest()->getBody()->collect(maximum);
    auto connection = context->getConnection(); connection->close(); ok(co_await connection->waitClosed()); co_return body.result.code;
}
DOCTEST_TEST_CASE("HTTP1 body allocation ceilings distinguish exact boundaries limits and truncation") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>();
    const std::string fixed = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\ntest";
    const std::string chunked = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n0\r\n\r\n";
    DOCTEST_CHECK(drive(watcher, bodyCeiling(watcher, fixed, 4)) == ERET_OK);
    DOCTEST_CHECK(drive(watcher, bodyCeiling(watcher, chunked, 4)) == ERET_OK);
    DOCTEST_CHECK(drive(watcher, bodyCeiling(watcher, fixed, 3)) == ERET_LIMIT);
    DOCTEST_CHECK(drive(watcher, bodyCeiling(watcher, chunked, 3)) == ERET_LIMIT);
    DOCTEST_CHECK(drive(watcher, bodyCeiling(watcher, fixed.substr(0, fixed.size()-1), 4)) == ERET_TRUNCATED);
    DOCTEST_CHECK(drive(watcher, bodyCeiling(watcher, chunked.substr(0, chunked.size()-5), 4)) == ERET_TRUNCATED);
}
asyncpp::task<bool> missingLength(std::shared_ptr<sockpp::CSocketWatcher> watcher, bool strict,
                                std::string method, std::string framing, ERetCode expected) {
    co_await asyncpp::defer(*watcher);
    auto stream = std::make_shared<Script>(watcher, method +
        " / HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\n" + framing +
        "\r\n" + (framing.empty() ? "unframed bytes must never be accepted as this body" : ""), 1);
    SHttpConnectionOptions options; options.http1.requireExplicitRequestBodyLength = strict;
    auto context = IHttpServerContext::create(options); ok(context->setStream(stream));
    DOCTEST_CHECK(co_await context->getRequest()->parseOnce() == expected);
    if (expected == ERET_LENGTH_REQUIRED) {
        DOCTEST_CHECK(stream->output() == "HTTP/1.1 411 Length Required\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        DOCTEST_CHECK_FALSE(context->getConnection()->canStartExchange());
    } else if (expected == ERET_OK) {
        auto bytes = co_await context->getRequest()->getBody()->collect(1);
        DOCTEST_CHECK(bytes.result.code == ERET_OK); DOCTEST_CHECK(bytes.bytes.empty());
    }
    context->getConnection()->close(); ok(co_await context->getConnection()->waitClosed()); co_return true;
}
DOCTEST_TEST_CASE("HTTP1 missing body length rejects unframed uploads with 411 close but permits explicit empty bodies") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>();
    for (const auto& method : {"POST", "PUT", "PATCH", "GET"})
        DOCTEST_CHECK(drive(watcher, missingLength(watcher, true, method, "", ERET_LENGTH_REQUIRED)));
    DOCTEST_CHECK(drive(watcher, missingLength(watcher, true, "POST", "Content-Length: 0\r\n", ERET_OK)));
    DOCTEST_CHECK(drive(watcher, missingLength(watcher, false, "POST", "", ERET_OK)));
}
asyncpp::task<void> acceptUntilClosed(std::shared_ptr<IHttpServer> server, ERetCode& result) {
    auto accepted = co_await server->accept(); result = accepted.result.code;
}
asyncpp::task<bool> closeAccept(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); auto server = IHttpServer::create(watcher);
    ok(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)));
    ERetCode result = ERET_INVALID_STATE; asyncpp::async_launch_scope scope;
    scope.launch(acceptUntilClosed(server, result)); server->close();
    co_await scope.join(); ok(co_await server->waitClosed()); co_return result == ERET_CLOSED;
}
DOCTEST_TEST_CASE("plaintext listener close wakes and joins pending accept") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, closeAccept(watcher)));
}
asyncpp::task<bool> closeHeadRead(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); auto server = IHttpServer::create(watcher);
    ok(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)));
    auto peer = sockpp::CSocketAsync::open(*watcher, sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
    ERetCode result = ERET_INVALID_STATE; asyncpp::async_launch_scope scope;
    scope.launch(acceptUntilClosed(server, result));
    co_await peer.connect(server->localAddress());
    // --> Allow accept to complete and the accepted transport to wait for a request head.
    for (unsigned i = 0; i < 16; ++i) { co_await asyncpp::defer(*watcher); }
    server->close(); co_await scope.join(); ok(co_await server->waitClosed()); (void)peer.close();
    co_return result == ERET_CLOSED;
}
DOCTEST_TEST_CASE("plaintext listener close joins a peer stalled before its request head") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, closeHeadRead(watcher)));
}

asyncpp::task<bool> rejectionPolicies(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    auto stream = std::make_shared<Script>(watcher, ""); auto context = IHttpServerContext::create();
    DOCTEST_CHECK(context->setStream(stream) == ERET_WRONG_EXECUTOR);
    co_await asyncpp::defer(*watcher); ok(context->setStream(stream));
    std::stop_source cancel; SOperationOptions options; options.cancellation = cancel.get_token();
    DOCTEST_CHECK(co_await context->getRequest()->parseOnce(options) == ERET_UNSUPPORTED);
    cancel.request_stop(); DOCTEST_CHECK(co_await context->getRequest()->parseOnce(options) == ERET_CANCELLED);
    auto connection = context->getConnection(); connection->close(); ok(co_await connection->waitClosed());
    SHttpConnectionOptions h2; h2.version = HTTP_2;
    auto h2Client = IHttpClient::create(watcher,h2); h2Client->close(); ok(co_await h2Client->waitClosed());
    auto wrongWatcher = std::make_shared<sockpp::CSocketWatcher>();
    auto udp = sockpp::CSocketAsync::open(*watcher, sockpp::EAF_INET, sockpp::ESOCK_DATAGRAM, sockpp::EPROTO_UDP);
    DOCTEST_CHECK(udp.usesWatcher(*watcher)); DOCTEST_CHECK_FALSE(udp.usesWatcher(*wrongWatcher)); DOCTEST_CHECK_FALSE(udp.isByteStream());
    DOCTEST_CHECK_THROWS_AS(CSocketHttpStream::create(watcher, std::move(udp)), std::invalid_argument);
    co_return true;
}
DOCTEST_TEST_CASE("HTTP factories reject unsupported policies versions and datagram adapters before wire effects") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(drive(watcher, rejectionPolicies(watcher)));
}

DOCTEST_TEST_CASE("header codecs decode independent RFC vectors and reject unsupported features") {
    auto hpack = IHeaderCodec::createHpack();
    const std::string key = "custom-key", value = "custom-header";
    SBytes literal{0x40, 10}; literal.insert(literal.end(), key.begin(), key.end()); literal.push_back(13); literal.insert(literal.end(), value.begin(), value.end());
    auto first = hpack->decode(literal); DOCTEST_REQUIRE(first.result.code == ERET_OK); DOCTEST_REQUIRE(first.fields.size() == 1);
    DOCTEST_CHECK(first.fields[0].name == key); DOCTEST_CHECK(first.fields[0].value == value);
    auto indexed = hpack->decode(SBytes{0xbe}); DOCTEST_REQUIRE(indexed.result.code == ERET_OK); DOCTEST_CHECK(indexed.fields[0].value == value);
    auto qpack = IHeaderCodec::createQpack();
    const std::string path = "/index.html"; SBytes q{0,0,0x51,11}; q.insert(q.end(), path.begin(), path.end());
    auto decoded = qpack->decode(q); DOCTEST_REQUIRE(decoded.result.code == ERET_OK); DOCTEST_CHECK(decoded.fields[0].name == ":path"); DOCTEST_CHECK(decoded.fields[0].value == path);
    std::vector<SHeaderField> fields{{"password","secret",EINDEX_NEVER}};
    auto wire = hpack->encode(fields); DOCTEST_REQUIRE(wire.result.code == ERET_OK); DOCTEST_CHECK(wire.bytes[0] == 0x10);
    auto roundtrip = IHeaderCodec::createHpack()->decode(wire.bytes);
    DOCTEST_REQUIRE(roundtrip.result.code == ERET_OK); DOCTEST_CHECK(roundtrip.fields[0].value == "secret");
    DOCTEST_CHECK(roundtrip.fields[0].indexing == EINDEX_NEVER);
    auto fresh = IHeaderCodec::createHpack(); DOCTEST_CHECK(fresh->decode(SBytes{0x00, 0x81, 0}).result.code == ERET_MALFORMED);
    DOCTEST_CHECK(fresh->decode(SBytes{0x82}).result.code == ERET_INVALID_STATE);
    DOCTEST_CHECK(IHeaderCodec::createQpack()->decode(SBytes{1,0}).result.code == ERET_MALFORMED);
    DOCTEST_CHECK(IHeaderCodec::createHpack()->decode(SBytes{0x80}).result.code == ERET_MALFORMED);
}
} // namespace
