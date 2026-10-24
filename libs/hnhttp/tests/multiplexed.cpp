#include "test_transport.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>

namespace {
using namespace hnhttp;
void ok(ERetCode code) { DOCTEST_CHECK(code == ERET_OK); }
asyncpp::task<void> serve(std::shared_ptr<IHttpServerContext> context) {
    auto reader = context->getRequest()->getBody(); auto body = co_await reader->collect(1024*1024); ok(body.result.code);
    std::vector<SHeaderField> informational{{"link","</style.css>; rel=preload"}};
    ok(co_await context->getResponse()->sendInformational(103,std::move(informational)));
    context->getResponse()->headers().add("x-custom","dynamic-table-value");
    context->getResponse()->body(IHttpBodySource::fromBytes(std::move(body.bytes)));
    ok(co_await context->getResponse()->exec()); ok(co_await context->finish());
}
asyncpp::task<void> acceptMany(std::shared_ptr<IHttpMultiplexedConnection> server, unsigned count) {
    asyncpp::async_launch_scope handlers;
    for (unsigned i = 0; i < count; ++i) { auto accepted = co_await server->accept(); ok(accepted.result.code); if (accepted.context) { handlers.launch(serve(std::move(accepted.context))); } }
    co_await handlers.join();
}
asyncpp::task<void> request(std::shared_ptr<IHttpConnection> connection, std::size_t n, std::uint8_t value) {
    auto context = IHttpClientContext::create(connection); auto req = context->getRequest();
    req->method(HTTP_POST).authority("example.test").path("/echo"); req->queries().set("request",static_cast<std::int64_t>(value));
    req->body(IHttpBodySource::fromBytes(SBytes(n,value)));
    ok(co_await req->exec()); DOCTEST_CHECK(context->getResponse()->status() == 200);
    auto body = co_await context->getResponse()->getBody()->collect(1024*1024); ok(body.result.code);
    DOCTEST_CHECK(body.bytes == SBytes(n,value)); ok(co_await req->waitSent()); ok(co_await context->finish());
}
asyncpp::task<ERetCode> conversation(std::shared_ptr<sockpp::CSocketWatcher> watcher, EHttpVersion version) {
    co_await asyncpp::defer(*watcher); SHttpConnectionOptions options; options.version = version; options.http2.initialStreamWindow = 1024;
    if (version == HTTP_3) { options.limits.maximumStreamBufferedBytes = 1024; options.limits.maximumConnectionBufferedBytes = 4096; }
    std::shared_ptr<IHttpConnection> client, server;
    if (version == HTTP_2) {
        auto pair = testhttp::bytePair(watcher);
        client = IHttpConnection::createStream(pair.first,HTTP_CLIENT,options); server = IHttpConnection::createStream(pair.second,HTTP_SERVER,options);
        DOCTEST_CHECK_THROWS_AS(IHttpConnection::createStream(pair.first,HTTP_CLIENT,options),std::logic_error);
    } else {
        auto pair = testhttp::nativePair(watcher);
        client = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.first),HTTP_CLIENT,options);
        server = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.second),HTTP_SERVER,options);
    }
    auto c = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(client), s = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(server);
    asyncpp::async_launch_scope engines; engines.launch(c->run()); engines.launch(s->run());
    ok(co_await c->ready()); ok(co_await s->ready());
    asyncpp::async_launch_scope work; work.launch(acceptMany(s,version == HTTP_3 ? 5 : 2)); work.launch(request(client,180000,17)); work.launch(request(client,90000,29));
    if (version == HTTP_3) { work.launch(request(client,4096,31)); work.launch(request(client,4096,32)); work.launch(request(client,4096,33)); }
    co_await work.join();
    if (version == HTTP_2) { auto h2 = std::dynamic_pointer_cast<IHttp2Connection>(client); ok(co_await h2->ping({1,2,3,4,5,6,7,8}));
        std::vector<SProtocolSetting> settings{{3,10}}; ok(co_await h2->updateSettings(std::move(settings))); DOCTEST_CHECK(h2->flowState().connectionSendWindow > 0); }
    client->close(); server->close(); co_await engines.join(); ok(co_await client->waitClosed()); ok(co_await server->waitClosed()); co_return ERET_OK;
}
DOCTEST_TEST_CASE("HTTP2 concurrent streams fragment frames apply flow control and ACK ping settings") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); ok(testhttp::drive(watcher,conversation(watcher,HTTP_2)));
}
DOCTEST_TEST_CASE("HTTP3 native hnquic interface streams carry concurrent bodies control and dynamic QPACK") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); ok(testhttp::drive(watcher,conversation(watcher,HTTP_3)));
}
asyncpp::task<void> serveTcp(std::shared_ptr<IHttpServer> listener) {
    auto accepted = co_await listener->acceptConnection(); DOCTEST_REQUIRE(accepted.result.code == ERET_OK);
    auto engine = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(accepted.connection); asyncpp::async_launch_scope tasks;
    tasks.launch(engine->run()); ok(co_await engine->ready());
    auto request = co_await engine->accept(); DOCTEST_REQUIRE(request.result.code == ERET_OK);
    co_await serve(std::move(request.context));
    // --> The peer closes after collecting the response, allowing socket callbacks to quiesce.
    co_await tasks.join(); ok(co_await engine->waitClosed());
}
asyncpp::task<bool> tcp(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); SHttpConnectionOptions options; options.version = HTTP_2;
    auto listener = IHttpServer::create(watcher,options); ok(listener->listen(sockpp::CSocketAddress::loopbackIpv4(0)));
    asyncpp::async_launch_scope tasks; tasks.launch(serveTcp(listener));
    auto client = IHttpClient::create(watcher,options); ok(co_await client->connect(listener->localAddress()));
    auto engine = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(client->getConnection()); tasks.launch(engine->run()); ok(co_await engine->ready());
    co_await request(client->getConnection(),100000,42); client->close(); co_await tasks.join(); ok(co_await client->waitClosed());
    listener->close(); ok(co_await listener->waitClosed()); co_return true;
}
DOCTEST_TEST_CASE("HTTP2 prior knowledge client server use socketpp TCP and caller owned pumps") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,tcp(watcher)));
}
class CTrailerSource final : public IHttpBodySource {
private:
    std::shared_ptr<IHttpBodySource> _source = IHttpBodySource::fromBytes({'t','e','s','t'});
public:
    asyncpp::task<ERetCode> prepare(SOperationOptions o) override { return _source->prepare(o); }
    SBodyMetadata metadata() const override { auto metadata = _source->metadata(); metadata.trailerNames = {"x-check"}; return metadata; }
    asyncpp::task<SReadResult> readSome(std::size_t n, SOperationOptions o) override { return _source->readSome(n,o); }
    asyncpp::task<ERetCode> rewind(SOperationOptions o) override { return _source->rewind(o); }
    std::vector<SHeaderField> trailers() const override { return {{"x-check","complete"}}; }
    SHttpError lastError() const override { return {}; }
};
asyncpp::task<void> respondWithTrailers(std::shared_ptr<IHttpMultiplexedConnection> server) {
    auto accepted = co_await server->accept(); DOCTEST_REQUIRE(accepted.result.code == ERET_OK);
    ok(co_await accepted.context->getRequest()->getBody()->discard(0));
    accepted.context->getResponse()->body(std::make_shared<CTrailerSource>());
    ok(co_await accepted.context->getResponse()->exec()); ok(co_await accepted.context->finish());
}
asyncpp::task<bool> trailers(std::shared_ptr<sockpp::CSocketWatcher> watcher, EHttpVersion version) {
    co_await asyncpp::defer(*watcher); SHttpConnectionOptions options; options.version = version;
    std::shared_ptr<IHttpConnection> client, server;
    if (version == HTTP_2) { auto pair = testhttp::bytePair(watcher); client = IHttpConnection::createStream(pair.first,HTTP_CLIENT,options); server = IHttpConnection::createStream(pair.second,HTTP_SERVER,options); }
    else { auto pair = testhttp::nativePair(watcher); client = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.first),HTTP_CLIENT,options); server = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.second),HTTP_SERVER,options); }
    auto c = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(client), s = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(server);
    asyncpp::async_launch_scope tasks; tasks.launch(c->run()); tasks.launch(s->run()); ok(co_await c->ready()); ok(co_await s->ready()); tasks.launch(respondWithTrailers(s));
    auto context = IHttpClientContext::create(client); context->getRequest()->authority("example.test"); ok(co_await context->getRequest()->exec());
    auto reader = context->getResponse()->getBody(); auto body = co_await reader->collect(4); ok(body.result.code); DOCTEST_CHECK(body.bytes == SBytes{'t','e','s','t'});
    DOCTEST_CHECK(reader->trailers().get("x-check") == std::optional<std::string>{"complete"}); ok(co_await context->finish());
    client->close(); server->close(); co_await tasks.join(); ok(co_await client->waitClosed()); ok(co_await server->waitClosed()); co_return true;
}
DOCTEST_TEST_CASE("HTTP2 and HTTP3 trailers validate exact body limits and terminal boundaries") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,trailers(watcher,HTTP_2))); DOCTEST_CHECK(testhttp::drive(watcher,trailers(watcher,HTTP_3)));
}
class CGatedSource final : public IHttpBodySource {
public:
    asyncpp::multi_consumer_event release;
    asyncpp::task<ERetCode> prepare(SOperationOptions) override { co_return ERET_OK; }
    SBodyMetadata metadata() const override { return {}; }
    asyncpp::task<SReadResult> readSome(std::size_t, SOperationOptions) override { co_await release; co_return SReadResult{testhttp::error(ERET_IO),{}}; }
    asyncpp::task<ERetCode> rewind(SOperationOptions) override { co_return ERET_UNSUPPORTED; }
    std::vector<SHeaderField> trailers() const override { return {}; }
    SHttpError lastError() const override { return testhttp::error(ERET_IO); }
};
asyncpp::task<void> startRequest(std::shared_ptr<IHttpClientRequest> request, asyncpp::multi_consumer_event& head) { ok(co_await request->exec()); head.set(); }
asyncpp::task<bool> earlyResponse(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); auto pair = testhttp::nativePair(watcher); SHttpConnectionOptions options; options.version = HTTP_3;
    auto client = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.first),HTTP_CLIENT,options), server = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.second),HTTP_SERVER,options);
    auto c = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(client), s = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(server);
    asyncpp::async_launch_scope tasks; tasks.launch(c->run()); tasks.launch(s->run()); ok(co_await c->ready()); ok(co_await s->ready());
    auto context = IHttpClientContext::create(client); auto request = context->getRequest(); auto source = std::make_shared<CGatedSource>();
    request->method(HTTP_POST).authority("example.test").body(source); asyncpp::multi_consumer_event head; tasks.launch(startRequest(request,head));
    auto accepted = co_await s->accept(); DOCTEST_REQUIRE(accepted.result.code == ERET_OK);
    accepted.context->getResponse()->body(IHttpBodySource::fromBytes({'o','k'})); ok(co_await accepted.context->getResponse()->exec()); co_await head;
    auto reader = context->getResponse()->getBody(); auto body = co_await reader->collect(2); ok(body.result.code); DOCTEST_CHECK(body.bytes == SBytes{'o','k'});
    source->release.set(); DOCTEST_CHECK(co_await request->waitSent() == ERET_IO); DOCTEST_CHECK(reader->isComplete());
    DOCTEST_CHECK(request->lastError().code == ERET_IO);
    DOCTEST_CHECK((co_await reader->readSome()).result.code == ERET_EOF); DOCTEST_CHECK(co_await context->finish() == ERET_IO);
    client->close(); server->close(); co_await tasks.join(); ok(co_await client->waitClosed()); ok(co_await server->waitClosed()); co_return true;
}
DOCTEST_TEST_CASE("HTTP3 complete early response survives a later failed request upload") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,earlyResponse(watcher)));
}
asyncpp::task<std::shared_ptr<IHttpConnection>> closedConnection(std::shared_ptr<sockpp::CSocketWatcher> watcher, EHttpVersion version) {
    co_await asyncpp::defer(*watcher); SHttpConnectionOptions options; options.version = version; std::shared_ptr<IHttpConnection> connection;
    if (version == HTTP_2) { auto pair = testhttp::bytePair(watcher); connection = IHttpConnection::createStream(pair.first,HTTP_CLIENT,options); }
    else { auto pair = testhttp::nativePair(watcher); connection = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.first),HTTP_CLIENT,options); }
    connection->close(); ok(co_await connection->waitClosed()); co_return connection;
}
DOCTEST_TEST_CASE("closed multiplexed connection release outside executor queues no retained cleanup") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>();
    for (auto version : {HTTP_2,HTTP_3}) {
        auto connection = testhttp::drive(watcher,closedConnection(watcher,version)); connection.reset();
        DOCTEST_CHECK_FALSE(watcher->runOne(std::chrono::milliseconds{0}));
    }
}
}
