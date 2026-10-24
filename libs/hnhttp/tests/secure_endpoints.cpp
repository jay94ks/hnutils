#include "test_transport.hpp"
#include "resource_scenario.hpp"
#include "../../hntls/tests/identity.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>

namespace {
using namespace hnhttp;
struct Policy {
    hntls::SIdentity identity = testIdentity();
    std::shared_ptr<hntls::ITlsContext> client, server;
    explicit Policy(EHttpVersion v, bool trusted = true) {
        hntls::STlsOptions c;
        c.serverName = "localhost";
        c.alpn = {v == HTTP_3 ? "h3" : v == HTTP_2 ? "h2" : "http/1.1"};
        auto s = c;
        s.role = hntls::EROLE_SERVER;
        s.requirePeerAuthentication = false;
        hntls::STrustOptions anchors;
        if (trusted) {
            anchors.anchors = identity.certificateChain;
        }
        client = hntls::ITlsContext::create(c, {}, anchors);
        server = hntls::ITlsContext::create(s, identity);
    }
};
asyncpp::task<void> echo(std::shared_ptr<IHttpServerContext> context) {
    auto body = co_await context->getRequest()->getBody()->collect(1024 * 1024);
    DOCTEST_REQUIRE(body.result.code == ERET_OK);
    DOCTEST_CHECK(context->getConnection()->security().tls);
    context->getResponse()->status(200).body(IHttpBodySource::fromBytes(std::move(body.bytes)));
    DOCTEST_CHECK(co_await context->getResponse()->exec() == ERET_OK);
    DOCTEST_CHECK(co_await context->finish() == ERET_OK);
}
asyncpp::task<void> serve(std::shared_ptr<IHttpConnection> connection, unsigned count) {
    if (connection->version() == HTTP_10 || connection->version() == HTTP_11) {
        for (unsigned i = 0; i < count; ++i) {
            auto context =
                IHttpServerContext::create(std::dynamic_pointer_cast<IHttp1Connection>(connection));
            DOCTEST_REQUIRE(co_await context->getRequest()->parseOnce() == ERET_OK);
            co_await echo(context);
        }
        co_return;
    }
    auto mux = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    DOCTEST_REQUIRE(co_await mux->ready() == ERET_OK);
    asyncpp::async_launch_scope handlers;
    for (unsigned i = 0; i < count; ++i) {
        auto r = co_await mux->accept();
        DOCTEST_REQUIRE(r.result.code == ERET_OK);
        handlers.launch(echo(r.context));
    }
    co_await handlers.join();
    co_return;
}
asyncpp::task<void> request(std::shared_ptr<IHttpClient> client, std::size_t size, std::uint8_t byte) {
    auto context = client->createRequest();
    auto req = context->getRequest();
    req->method(HTTP_POST).path("/secure-echo");
    req->body(IHttpBodySource::fromBytes(SBytes(size, byte)));
    DOCTEST_REQUIRE(co_await req->exec() == ERET_OK);
    auto body = co_await context->getResponse()->getBody()->collect(1024 * 1024);
    DOCTEST_REQUIRE(body.result.code == ERET_OK);
    DOCTEST_CHECK(body.bytes == SBytes(size, byte));
    DOCTEST_CHECK(co_await req->waitSent() == ERET_OK);
    DOCTEST_CHECK(co_await context->finish() == ERET_OK);
}
asyncpp::task<bool> conversation(std::shared_ptr<sockpp::CSocketWatcher> w, EHttpVersion version, bool resources = false) {
    co_await asyncpp::defer(*w);
    Policy p(version);
    SHttpConnectionOptions options;
    options.version = version;
    std::shared_ptr<IHttpClient> client;
    std::shared_ptr<IHttpServer> server;
    asyncpp::async_launch_scope native;
    if (version == HTTP_3) {
        hnquic::SQuicOptions q;
        q.role = hnquic::EROLE_SERVER;
        server = IHttpServer::createQuic(w, p.server, options, q);
        client = IHttpClient::createQuic(w, p.client, options);
    } else {
        server = IHttpServer::createTls(w, p.server, options);
        client = IHttpClient::createTls(w, p.client, options);
    }
    DOCTEST_REQUIRE(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)) == ERET_OK);
    auto address = server->localAddress();
    if (version == HTTP_3) {
        native.launch(server->run());
        native.launch(client->run());
    }
    auto incoming = asyncpp::as_promise(server->acceptConnection());
    DOCTEST_REQUIRE(co_await client->connect(address) == ERET_OK);
    while (incoming.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        co_await asyncpp::defer(*w);
    }
    auto accepted = incoming.get();
    DOCTEST_REQUIRE(accepted.result.code == ERET_OK);
    auto connection = client->getConnection();
    auto security = connection->security();
    DOCTEST_CHECK(security.tls);
    DOCTEST_CHECK(security.peerAuthentication == EPEER_TRANSPORT_ACCEPTED);
    DOCTEST_CHECK(security.peerCertificates.size() == 1);
    DOCTEST_CHECK(security.alpn == p.client->options().alpn.front());
    if (version != HTTP_3) {
        server->close();
        DOCTEST_CHECK(co_await server->waitClosed() == ERET_OK);
    }
    asyncpp::async_launch_scope work;
    auto mux = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    auto serverMux = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(accepted.connection);
    asyncpp::async_launch_scope pumps;
    if (mux) {
        pumps.launch(serverMux->run());
        pumps.launch(mux->run());
        DOCTEST_REQUIRE(co_await mux->ready() == ERET_OK);
        DOCTEST_REQUIRE(co_await serverMux->ready() == ERET_OK);
    }
    if (resources) {
        work.launch(testresource::serve(accepted.connection));
        co_await testresource::requests(client);
    } else if (version == HTTP_10 || version == HTTP_11) {
        work.launch(serve(accepted.connection, 2));
        co_await request(client, 30000, 11);
        DOCTEST_CHECK(client->getConnection() == connection);
        co_await request(client, 60000, 22);
        accepted.connection->close();
    } else {
        work.launch(serve(accepted.connection, 2));
        work.launch(request(client, 300000, 31));
        work.launch(request(client, 150000, 42));
    }
    co_await work.join();
    client->close();
    accepted.connection->close();
    server->close();
    co_await pumps.join();
    co_await native.join();
    DOCTEST_CHECK(co_await client->waitClosed() == ERET_OK);
    DOCTEST_CHECK(co_await accepted.connection->waitClosed() == ERET_OK);
    DOCTEST_CHECK(co_await server->waitClosed() == ERET_OK);
    co_return true;
}
DOCTEST_TEST_CASE("HTTPS endpoint HTTP1 reuses one authenticated TLS connection after listener close") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, conversation(w, HTTP_11)));
}
DOCTEST_TEST_CASE("HTTPS endpoint HTTP2 negotiates h2 and multiplexes authenticated requests") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, conversation(w, HTTP_2)));
}
DOCTEST_TEST_CASE("HTTP3 endpoint factories supervise native UDP and concurrent HTTP request engines") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, conversation(w, HTTP_3)));
}
DOCTEST_TEST_CASE("HTTPS HTTP1 uploads forms downloads chunked ranges cache validators and reuses connection") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, conversation(w, HTTP_11, true)));
}
DOCTEST_TEST_CASE("HTTPS HTTP2 uploads forms downloads ranges cache validators and reuses connection streams") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, conversation(w, HTTP_2, true)));
}
DOCTEST_TEST_CASE("HTTP3 uploads forms downloads ranges cache validators and reuses native UDP connection streams") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, conversation(w, HTTP_3, true)));
}
asyncpp::task<bool> rejected(std::shared_ptr<sockpp::CSocketWatcher> w, EHttpVersion version = HTTP_11) {
    co_await asyncpp::defer(*w);
    Policy p(version, false);
    SHttpConnectionOptions options;
    options.version = version;
    hnquic::SQuicOptions q;
    q.role = hnquic::EROLE_SERVER;
    auto server = version == HTTP_3 ? IHttpServer::createQuic(w, p.server, options, q)
                                    : IHttpServer::createTls(w, p.server);
    auto client = version == HTTP_3 ? IHttpClient::createQuic(w, p.client, options)
                                    : IHttpClient::createTls(w, p.client);
    DOCTEST_REQUIRE(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)) == ERET_OK);
    asyncpp::async_launch_scope native;
    if (version == HTTP_3) {
        native.launch(server->run());
        native.launch(client->run());
    }
    auto incoming = asyncpp::as_promise(server->acceptConnection());
    DOCTEST_CHECK(co_await client->connect(server->localAddress()) == ERET_AUTHENTICATION);
    DOCTEST_CHECK_FALSE(client->getConnection());
    DOCTEST_CHECK_THROWS_AS(client->createRequest(), std::logic_error);
    client->close();
    server->close();
    co_await native.join();
    DOCTEST_CHECK(co_await client->waitClosed() == ERET_OK);
    DOCTEST_CHECK(co_await server->waitClosed() == ERET_OK);
    while (incoming.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        co_await asyncpp::defer(*w);
    }
    DOCTEST_CHECK(incoming.get().result.code != ERET_OK);
    co_return true;
}
DOCTEST_TEST_CASE("HTTPS authentication failure never publishes a usable HTTP connection") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, rejected(w)));
}
DOCTEST_TEST_CASE("HTTP3 authentication failure keeps native TLS error and never publishes HTTP connection") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, rejected(w, HTTP_3)));
}
asyncpp::task<bool> stalled(std::shared_ptr<sockpp::CSocketWatcher> w) {
    co_await asyncpp::defer(*w);
    Policy p(HTTP_11);
    auto server = IHttpServer::createTls(w, p.server);
    DOCTEST_REQUIRE(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)) == ERET_OK);
    auto incoming = asyncpp::as_promise(server->acceptConnection());
    auto socket = sockpp::CSocketAsync::open(*w, sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
    co_await socket.connect(server->localAddress());
    const std::array<std::byte, 6> partial{std::byte{22}, std::byte{3},  std::byte{3},
                                           std::byte{0},  std::byte{10}, std::byte{1}};
    co_await socket.sendAll(partial);
    co_await asyncpp::defer(*w);
    DOCTEST_CHECK(incoming.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready);
    server->close();
    DOCTEST_CHECK(co_await server->waitClosed() == ERET_OK);
    DOCTEST_CHECK(incoming.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    DOCTEST_CHECK(incoming.get().result.code == ERET_CLOSED);
    socket.close();
    co_return true;
}
DOCTEST_TEST_CASE("HTTPS listener close aborts and joins a peer stalled inside the TLS handshake") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, stalled(w)));
}
DOCTEST_TEST_CASE(
    "Secure endpoint factories reject roles unauthenticated clients and ALPN downgrade before IO") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    Policy p(HTTP_2);
    SHttpConnectionOptions options;
    options.version = HTTP_2;
    DOCTEST_CHECK_THROWS_AS(IHttpClient::createTls(w, p.server, options), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(IHttpServer::createTls(w, p.client, options), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(IHttpClient::createTls(w, p.client), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(IHttpClient::createQuic(w, p.client, options), std::invalid_argument);
    hntls::STlsOptions weak;
    weak.alpn = {"h2"};
    weak.serverName = "localhost";
    weak.requirePeerAuthentication = false;
    DOCTEST_CHECK_THROWS_AS(IHttpClient::createTls(w, hntls::ITlsContext::create(weak), options),
                            std::invalid_argument);
    DOCTEST_CHECK(IHttpClient::createTls(w, p.client, options));
    DOCTEST_CHECK(IHttpServer::createTls(w, p.server, options));
}
asyncpp::task<bool> closeClient(std::shared_ptr<sockpp::CSocketWatcher> w) {
    Policy p(HTTP_11);
    auto client = IHttpClient::createTls(w, p.client);
    DOCTEST_CHECK(co_await client->connect(sockpp::CSocketAddress::loopbackIpv4(443)) == ERET_WRONG_EXECUTOR);
    co_await asyncpp::defer(*w);
    std::stop_source stopped;
    stopped.request_stop();
    DOCTEST_CHECK(co_await client->connect(sockpp::CSocketAddress::loopbackIpv4(443),
                                           {{}, stopped.get_token()}) == ERET_CANCELLED);
    DOCTEST_CHECK(co_await client->run() == ERET_UNSUPPORTED);
    auto listener =
        sockpp::CSocketAsync::open(*w, sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
    listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
    listener.listen();
    auto connecting = asyncpp::as_promise(client->connect(listener.localAddress()));
    auto socket = co_await listener.accept();
    client->close();
    DOCTEST_CHECK(co_await client->waitClosed() == ERET_OK);
    DOCTEST_CHECK(connecting.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    DOCTEST_CHECK(connecting.get() == ERET_CLOSED);
    DOCTEST_CHECK_FALSE(client->getConnection());
    socket.close();
    listener.close();
    co_return true;
}
DOCTEST_TEST_CASE("HTTPS client close joins stalled handshake and rejects foreign cancelled operations") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(w, closeClient(w)));
}
} // namespace
