#include <hnquic.hpp>
#include "../src/engine.hpp"
#include "../../hntls/tests/identity.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/sync_wait.h>
#include <future>

namespace {
using namespace hnquic;
SBytes hex(const char *text) {
    SBytes out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (; *text; text += 2) {
        out.push_back((nib(text[0]) << 4) | nib(text[1]));
    }
    return out;
}
template <class T> T drive(std::shared_ptr<sockpp::CSocketWatcher> w, asyncpp::task<T> t) {
    auto f = asyncpp::as_promise(std::move(t));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready &&
           std::chrono::steady_clock::now() < until) {
        w->runOne(std::chrono::milliseconds{10});
    }
    DOCTEST_REQUIRE(f.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    return f.get();
}
struct Policies {
    std::shared_ptr<hntls::ITlsContext> client, server;
    Policies() {
        auto id = testIdentity();
        hntls::STlsOptions c;
        c.serverName = "localhost";
        c.alpn = {"h3"};
        auto s = c;
        s.role = hntls::EROLE_SERVER;
        s.requirePeerAuthentication = false;
        hntls::STrustOptions trust;
        trust.anchors = id.certificateChain;
        client = hntls::ITlsContext::create(c, {}, trust);
        server = hntls::ITlsContext::create(s, id);
    }
};
DOCTEST_TEST_CASE("RFC 9001 server Initial packet matches independent encrypted wire vector") {
    using namespace detail;
    auto secret = initialSecret(hex("8394c8f03e515708"), false);
    DOCTEST_CHECK(SBytes(secret.toPtr(), secret.toPtr() + secret.size()) ==
                  hex("3c199828fd139efd216c155ad844cc81fb82fa8d7446fa7d78be803acdda951b"));
    auto payload = hex(
        "02000000000600405a020000560303eefce7f7b37ba1d1632e96677825ddf73988cfc79825df566dc5430b9a045a12001301"
        "00002e00330024001d00209d3c940d89690b84d08a60993c144eca684d1081287c834d5311bcf32bb9da1a002b00020304");
    auto header = hex("c1000000010008f067a5502a4262b50040750001");
    auto expected =
        hex("cf000000010008f067a5502a4262b5004075c0d95a482cd0991cd25b0aac406a5816b6394100f37a1c69797554780bb3"
            "8cc5a99f5ede4cf73c3ec2493a1839b3dbcba3f6ea46c5b7684df3548e7ddeb9c3bf9c73cc3f3bded74b562bfb19fb84"
            "022f8ef4cdd93795d77d06edbb7aaf2f58891850abbdca3d20398c276456cbc42158407dd074ee");
    PacketKey key;
    key.install(view(secret));
    auto packet = key.protect(header, payload, header.size() - 2, 1);
    DOCTEST_CHECK(packet == expected);
    SBytes plaintext;
    std::uint64_t pn = 0;
    DOCTEST_CHECK(key.open(expected, header.size() - 2, 0, plaintext, pn));
    DOCTEST_CHECK(pn == 1);
    DOCTEST_CHECK(plaintext == payload);
    expected.back() ^= 1;
    DOCTEST_CHECK_FALSE(key.open(expected, header.size() - 2, 0, plaintext, pn));
    DOCTEST_CHECK(plaintext.empty());
}
asyncpp::task<bool> engineTest(std::shared_ptr<sockpp::CSocketWatcher> w, bool chaos) {
    co_await asyncpp::defer(*w);
    Policies p;
    SQuicOptions clientOptions;
    clientOptions.maximumStreamData = 4096;
    clientOptions.maximumConnectionData = 8192;
    auto serverOptions = clientOptions;
    serverOptions.role = EROLE_SERVER;
    detail::Engine client(w, p.client, clientOptions), server(w, p.server, serverOptions);
    auto now = std::chrono::steady_clock::now();
    co_await client.start(now);
    co_await server.start(now);
    bool dropped = false;
    unsigned packets = 0;
    for (unsigned round = 0; round < 1000 && (!client.confirmed() || !server.confirmed()); ++round) {
        now += std::chrono::milliseconds{20};
        client.tick(now);
        server.tick(now);
        auto outgoing = client.datagrams(now);
        for (auto &packet : outgoing) {
            ++packets;
            if (chaos && !dropped) {
                dropped = true;
                continue;
            }
            co_await server.receive(packet, now);
            if (chaos) {
                co_await server.receive(packet, now);
            }
        }
        outgoing = server.datagrams(now);
        if (chaos) {
            std::reverse(outgoing.begin(), outgoing.end());
        }
        for (auto &packet : outgoing) {
            ++packets;
            co_await client.receive(packet, now);
            if (chaos) {
                co_await client.receive(packet, now);
            }
        }
    }
    DOCTEST_INFO(client.error().message, server.error().message, packets);
    DOCTEST_REQUIRE(client.confirmed());
    DOCTEST_REQUIRE(server.confirmed());
    DOCTEST_CHECK(client.peerInfo().authentication == hntls::EPEER_ACCEPTED);
    auto id = client.open(ESTREAM_BIDIRECTIONAL);
    DOCTEST_REQUIRE(id);
    DOCTEST_CHECK(*id == 0);
    SBytes data(2400);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i);
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        offset += client.write(*id, detail::View(data).subspan(offset));
    }
    client.write(*id, {}, true);
    auto sent = client.datagrams(now);
    DOCTEST_REQUIRE(sent.size() >= 3);
    if (chaos) {
        sent.erase(sent.begin() + 1);
        std::reverse(sent.begin(), sent.end());
    }
    for (auto &packet : sent) {
        co_await server.receive(packet, now);
        co_await server.receive(packet, now);
    }
    SBytes received;
    for (unsigned i = 0; i < 300 && received.size() < data.size(); ++i) {
        now += std::chrono::milliseconds{20};
        client.tick(now);
        server.tick(now);
        for (auto &packet : server.datagrams(now)) {
            co_await client.receive(packet, now);
        }
        for (auto &packet : client.datagrams(now)) {
            co_await server.receive(packet, now);
        }
        auto chunk = server.read(*id, 500);
        detail::append(received, chunk);
    }
    DOCTEST_CHECK(received == data);
    DOCTEST_CHECK(server.at(*id).finalSize == data.size());
    DOCTEST_CHECK(server.at(*id).receive.offset() == data.size());
    co_return true;
}
DOCTEST_TEST_CASE("QUIC actual packet engine handshakes and reassembles encrypted streams") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(drive(w, engineTest(w, false)));
}
DOCTEST_TEST_CASE("QUIC packet loss PTO retransmission duplicate and reordering preserve bytes") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(drive(w, engineTest(w, true)));
}
asyncpp::task<bool> udpTest(std::shared_ptr<sockpp::CSocketWatcher> w) {
    co_await asyncpp::defer(*w);
    Policies p;
    SQuicOptions serverOptions;
    serverOptions.role = EROLE_SERVER;
    auto server = IQuicServer::create(w, p.server, serverOptions);
    DOCTEST_REQUIRE(server->listen(sockpp::CSocketAddress::loopbackIpv4(0)) == ERET_OK);
    auto address = server->localAddress();
    auto serverRun = asyncpp::as_promise(server->run());
    auto client = IQuicClient::create(w, p.client);
    auto clientRun = asyncpp::as_promise(client->run());
    auto incoming = asyncpp::as_promise(server->accept());
    auto connected = co_await client->connect(address);
    DOCTEST_INFO(connected.result.message);
    DOCTEST_REQUIRE(connected.result.code == ERET_OK);
    while (incoming.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        co_await asyncpp::defer(*w);
    }
    auto accepted = incoming.get();
    DOCTEST_REQUIRE(accepted.result.code == ERET_OK);
    auto peer = accepted.connection;
    auto localStream = co_await connected.connection->openStream(ESTREAM_BIDIRECTIONAL);
    DOCTEST_REQUIRE(localStream.result.code == ERET_OK);
    auto acceptStream = asyncpp::as_promise(peer->acceptStream());
    auto written = co_await localStream.stream->writeSome({1, 3, 5, 7});
    DOCTEST_CHECK(written.acceptedBytes == 4);
    DOCTEST_CHECK(co_await localStream.stream->finishWrite() == ERET_OK);
    while (acceptStream.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        co_await asyncpp::defer(*w);
    }
    auto stream = acceptStream.get();
    DOCTEST_REQUIRE(stream.result.code == ERET_OK);
    auto read = co_await stream.stream->readSome(10);
    DOCTEST_CHECK(read.bytes == SBytes{1, 3, 5, 7});
    auto end = co_await stream.stream->readSome(10);
    DOCTEST_CHECK(end.result.code == ERET_EOF);
    peer->close();
    client->close();
    server->close();
    DOCTEST_CHECK(co_await client->waitClosed() == ERET_OK);
    DOCTEST_CHECK(co_await server->waitClosed() == ERET_OK);
    DOCTEST_CHECK(co_await peer->waitClosed() == ERET_OK);
    DOCTEST_CHECK(clientRun.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    DOCTEST_CHECK(serverRun.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    clientRun.get();
    serverRun.get();
    co_return true;
}
DOCTEST_TEST_CASE("QUIC client and server use real socketpp UDP and native FIN streams") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(drive(w, udpTest(w)));
}
DOCTEST_TEST_CASE("QUIC factories validate roles ALPN resources and preserve private policies") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    Policies p;
    DOCTEST_CHECK_THROWS_AS(IQuicClient::create(w, p.server), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(IQuicConnection::create(w, {}), std::invalid_argument);
    SQuicOptions o;
    o.maximumConnectionData = UINT64_MAX;
    DOCTEST_CHECK_THROWS_AS(IQuicClient::create(w, p.client, o), std::invalid_argument);
    o = {};
    o.maximumBidirectionalStreams = 7;
    auto c = IQuicConnection::create(w, p.client, o);
    o.maximumBidirectionalStreams = 9;
    auto copy = c->options();
    copy.maximumBidirectionalStreams = 15;
    DOCTEST_CHECK(c->options().maximumBidirectionalStreams == 7);
    DOCTEST_CHECK(c->state() == ESTATE_IDLE);
    DOCTEST_CHECK_FALSE(c->handshakeConfirmed());
}
DOCTEST_TEST_CASE("QUIC reassembly bounds reject conflicting overlaps and sparse range floods") {
    detail::Reassembly r;
    r.insert(2, SBytes{2, 3}, 16, 2);
    r.insert(0, SBytes{0, 1, 2}, 16, 2);
    DOCTEST_CHECK(r.take(16) == SBytes{0, 1, 2, 3});
    r.insert(9, SBytes{1}, 16, 2);
    DOCTEST_CHECK_THROWS_AS(r.insert(9, SBytes{2}, 16, 2), SQuicError);
    r.insert(11, SBytes{1}, 16, 2);
    DOCTEST_CHECK_THROWS_AS(r.insert(13, SBytes{1}, 16, 2), SQuicError);
}
DOCTEST_TEST_CASE("QUIC deterministic malformed headers remain length bounded") {
    std::uint32_t seed = 0x12345678;
    for (unsigned i = 0; i < 4000; ++i) {
        SBytes packet(i % 128);
        for (auto &b : packet) {
            seed = seed * 1664525u + 1013904223u;
            b = static_cast<std::uint8_t>(seed >> 24);
        }
        try {
            auto h = detail::parseHeader(packet);
            DOCTEST_CHECK(h.pnOffset <= h.length);
            DOCTEST_CHECK(h.length <= packet.size());
            DOCTEST_CHECK(h.destination.size() <= 20);
            DOCTEST_CHECK(h.source.size() <= 20);
        } catch (const SQuicError &) {
        }
    }
}
} // namespace
