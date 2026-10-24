#include "test_transport.hpp"
#include "../../hntls/tests/identity.hpp"
#include <hnquic/client.hpp>
#include <hnquic/server.hpp>
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>

namespace {
using namespace hnhttp;
asyncpp::task<void> echo(std::shared_ptr<IHttpMultiplexedConnection> server) {
    auto incoming = co_await server->accept();
    DOCTEST_REQUIRE(incoming.result.code == ERET_OK);
    auto body = co_await incoming.context->getRequest()->getBody()->collect(1024 * 1024);
    DOCTEST_REQUIRE(body.result.code == ERET_OK);
    incoming.context->getResponse()->body(IHttpBodySource::fromBytes(std::move(body.bytes)));
    DOCTEST_CHECK(co_await incoming.context->getResponse()->exec() == ERET_OK);
    DOCTEST_CHECK(co_await incoming.context->finish() == ERET_OK);
}
asyncpp::task<bool> http3(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher);
    auto identity = testIdentity();
    hntls::STlsOptions tc;
    tc.serverName = "localhost";
    tc.alpn = {"h3"};
    auto ts = tc;
    ts.role = hntls::EROLE_SERVER;
    ts.requirePeerAuthentication = false;
    hntls::STrustOptions trust;
    trust.anchors = identity.certificateChain;
    hnquic::SQuicOptions qs;
    qs.role = hnquic::EROLE_SERVER;
    auto endpoint = hnquic::IQuicServer::create(watcher, hntls::ITlsContext::create(ts, identity), qs);
    DOCTEST_REQUIRE(endpoint->listen(sockpp::CSocketAddress::loopbackIpv4(0)) == hnquic::ERET_OK);
    auto endpointRun = asyncpp::as_promise(endpoint->run());
    auto nativeClient = hnquic::IQuicClient::create(watcher, hntls::ITlsContext::create(tc, {}, trust));
    auto clientRun = asyncpp::as_promise(nativeClient->run());
    auto incoming = asyncpp::as_promise(endpoint->accept());
    auto connected = co_await nativeClient->connect(endpoint->localAddress());
    DOCTEST_REQUIRE(connected.result.code == hnquic::ERET_OK);
    while (incoming.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        co_await asyncpp::defer(*watcher);
    }
    auto accepted = incoming.get();
    DOCTEST_REQUIRE(accepted.result.code == hnquic::ERET_OK);
    SHttpConnectionOptions options;
    options.version = HTTP_3;
    auto client = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(connected.connection), HTTP_CLIENT,
                                              options),
         server = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(accepted.connection), HTTP_SERVER,
                                              options);
    auto c = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(client),
         s = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(server);
    asyncpp::async_launch_scope tasks;
    tasks.launch(c->run());
    tasks.launch(s->run());
    DOCTEST_REQUIRE(co_await c->ready() == ERET_OK);
    DOCTEST_REQUIRE(co_await s->ready() == ERET_OK);
    tasks.launch(echo(s));
    auto context = IHttpClientContext::create(client);
    auto request = context->getRequest();
    request->method(HTTP_POST).authority("localhost").path("/encrypted-udp");
    request->body(IHttpBodySource::fromBytes(SBytes(400000, 37)));
    DOCTEST_REQUIRE(co_await request->exec() == ERET_OK);
    auto reply = co_await context->getResponse()->getBody()->collect(1024 * 1024);
    DOCTEST_CHECK(reply.result.code == ERET_OK);
    DOCTEST_CHECK(reply.bytes == SBytes(400000, 37));
    DOCTEST_CHECK(co_await request->waitSent() == ERET_OK);
    DOCTEST_CHECK(co_await context->finish() == ERET_OK);
    client->close();
    server->close();
    co_await tasks.join();
    DOCTEST_CHECK(co_await client->waitClosed() == ERET_OK);
    DOCTEST_CHECK(co_await server->waitClosed() == ERET_OK);
    nativeClient->close();
    endpoint->close();
    DOCTEST_CHECK(co_await nativeClient->waitClosed() == hnquic::ERET_OK);
    DOCTEST_CHECK(co_await endpoint->waitClosed() == hnquic::ERET_OK);
    clientRun.get();
    endpointRun.get();
    co_return true;
}
DOCTEST_TEST_CASE("HTTP3 runs over actual hnquic UDP hntls handshake and native stream flow control") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, http3(w)));
}
} // namespace
