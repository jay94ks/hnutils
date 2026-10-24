#include <hntls.hpp>
#include "identity.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/sync_wait.h>
#include <asyncpp/event.h>
#include <future>

namespace {
asyncpp::task<hntls::ERetCode> server(std::shared_ptr<sockpp::CSocketWatcher> w,
                                      sockpp::CSocketAsync &listener,
                                      std::shared_ptr<hntls::ITlsContext> context) {
    auto socket = co_await listener.accept();
    auto raw = hntls::CSocketByteStream::create(w, std::move(socket));
    auto tls = hntls::ITlsStream::create(raw, context);
    auto code = co_await tls->handshake();
    DOCTEST_REQUIRE(code == hntls::ERET_OK);
    auto request = co_await tls->readSome(1024);
    DOCTEST_CHECK(request.bytes == hntls::SBytes{4, 5, 6});
    auto write = co_await tls->writeSome(request.bytes);
    DOCTEST_CHECK(write.acceptedBytes == 3);
    DOCTEST_CHECK(co_await tls->finishWrite() == hntls::ERET_OK);
    auto eof = co_await tls->readSome(1);
    DOCTEST_CHECK(eof.result.code == hntls::ERET_EOF);
    tls->close();
    co_return co_await tls->waitClosed();
}
asyncpp::task<bool> loopback(std::shared_ptr<sockpp::CSocketWatcher> w,
                             certpp::crypto::EAsymmetrics algorithm) {
    co_await asyncpp::defer(*w);
    auto id = testIdentity("localhost", algorithm);
    hntls::STlsOptions c;
    c.serverName = "localhost";
    c.alpn = {"h2"};
    auto s = c;
    s.role = hntls::EROLE_SERVER;
    s.requirePeerAuthentication = false;
    hntls::STrustOptions trust;
    trust.anchors = id.certificateChain;
    auto clientContext = hntls::ITlsContext::create(c, {}, trust);
    auto serverContext = hntls::ITlsContext::create(s, id);
    auto listener =
        sockpp::CSocketAsync::open(*w, sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
    listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
    listener.listen();
    auto task = asyncpp::as_promise(server(w, listener, serverContext));
    auto client = hntls::ITlsClient::create(w, clientContext);
    auto connected = co_await client->connect(listener.localAddress());
    DOCTEST_INFO(connected.result.message);
    DOCTEST_REQUIRE(connected.result.code == hntls::ERET_OK);
    DOCTEST_CHECK(connected.stream->peerInfo().authentication == hntls::EPEER_ACCEPTED);
    DOCTEST_CHECK(connected.stream->peerInfo().alpn == "h2");
    auto write = co_await connected.stream->writeSome({4, 5, 6});
    DOCTEST_CHECK(write.acceptedBytes == 3);
    auto read = co_await connected.stream->readSome(1024);
    DOCTEST_CHECK(read.bytes == hntls::SBytes{4, 5, 6});
    DOCTEST_CHECK(co_await connected.stream->finishWrite() == hntls::ERET_OK);
    auto eof = co_await connected.stream->readSome(1);
    DOCTEST_CHECK(eof.result.code == hntls::ERET_EOF);
    client->close();
    DOCTEST_CHECK(co_await client->waitClosed() == hntls::ERET_OK);
    while (task.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        co_await asyncpp::defer(*w);
    }
    DOCTEST_CHECK(task.get() == hntls::ERET_OK);
    listener.close();
    co_return true;
}
DOCTEST_TEST_CASE("TLS TCP client server encrypted echo Ed25519 P256 and RSA PSS") {
    for (auto algorithm :
         {certpp::crypto::EASYM_ED25519, certpp::crypto::EASYM_P256, certpp::crypto::EASYM_RSA}) {
        auto w = std::make_shared<sockpp::CSocketWatcher>();
        auto f = asyncpp::as_promise(loopback(w, algorithm));
        while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
            w->runOne(std::chrono::milliseconds{10});
        }
        DOCTEST_CHECK(f.get());
    }
}
asyncpp::task<bool> timers(std::shared_ptr<sockpp::CSocketWatcher> w) {
    co_await asyncpp::defer(*w);
    asyncpp::single_consumer_event event;
    bool cancelled = false;
    auto id = w->schedule(std::chrono::steady_clock::now() + std::chrono::milliseconds{1},
                          [&] { cancelled = true; });
    DOCTEST_CHECK(w->cancelTimer(id));
    DOCTEST_CHECK_FALSE(w->cancelTimer(id));
    auto start = std::chrono::steady_clock::now();
    w->schedule(start + std::chrono::milliseconds{5}, [&] { event.set(); });
    co_await event;
    DOCTEST_CHECK(std::chrono::steady_clock::now() >= start + std::chrono::milliseconds{5});
    DOCTEST_CHECK_FALSE(cancelled);
    co_return true;
}
DOCTEST_TEST_CASE("socketpp monotonic timers fire on owner reactor and cancel without hidden threads") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    auto f = asyncpp::as_promise(timers(w));
    while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        w->runOne(std::chrono::milliseconds{20});
    }
    DOCTEST_CHECK(f.get());
}
} // namespace
