#include <hntls.hpp>
#include "identity.hpp"
#include "../src/protocol.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/sync_wait.h>
#include <future>
#include <deque>

namespace {
hntls::SBytes hex(const char *text) {
    hntls::SBytes out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (; *text; text += 2) {
        out.push_back((nib(text[0]) << 4) | nib(text[1]));
    }
    return out;
}
template <class T> T drive(std::shared_ptr<sockpp::CSocketWatcher> w, asyncpp::task<T> task) {
    auto f = asyncpp::as_promise(std::move(task));
    while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        w->runOne(std::chrono::milliseconds{10});
    }
    return f.get();
}
struct Pair {
    std::shared_ptr<hntls::ITlsSession> client, server;
};
asyncpp::task<Pair> pair(std::shared_ptr<sockpp::CSocketWatcher> w, hntls::EMode mode, bool trust = true,
                         bool mutual = false, const char *hostname = "localhost") {
    co_await asyncpp::defer(*w);
    auto identity = testIdentity();
    hntls::STlsOptions client;
    client.serverName = hostname;
    client.alpn = {"h3"};
    hntls::STlsOptions server = client;
    server.role = hntls::EROLE_SERVER;
    server.requirePeerAuthentication = mutual;
    hntls::STrustOptions anchors;
    if (trust) {
        anchors.anchors = identity.certificateChain;
    }
    Pair p{hntls::ITlsContext::create(client, mutual ? identity : hntls::SIdentity{}, anchors)
               ->createSession(w, mode, mode == hntls::EMODE_QUIC ? hntls::SBytes{1, 0} : hntls::SBytes{}),
           hntls::ITlsContext::create(server, identity, anchors)
               ->createSession(w, mode, mode == hntls::EMODE_QUIC ? hntls::SBytes{2, 0} : hntls::SBytes{})};
    auto s = co_await p.server->start();
    DOCTEST_CHECK(s.result.code == hntls::ERET_OK);
    auto c = co_await p.client->start();
    DOCTEST_CHECK(c.result.code == hntls::ERET_OK);
    std::deque<std::pair<bool, hntls::SHandshakeFlight>> flights;
    for (auto &f : c.outgoing) {
        flights.emplace_back(false, std::move(f));
    }
    for (unsigned round = 0; !flights.empty() && round < 100; ++round) {
        auto [toClient, flight] = std::move(flights.front());
        flights.pop_front();
        auto dest = toClient ? p.client : p.server;
        hntls::SHandshakeResult r;
        auto midpoint = flight.bytes.size() / 2;
        hntls::SBytes first(flight.bytes.begin(), flight.bytes.begin() + midpoint),
            last(flight.bytes.begin() + midpoint, flight.bytes.end());
        r = co_await dest->receive(flight.level, std::move(first));
        DOCTEST_CHECK(r.result.code == hntls::ERET_OK);
        DOCTEST_CHECK(r.outgoing.empty());
        r = co_await dest->receive(flight.level, std::move(last));
        if (r.result.code != hntls::ERET_OK) {
            DOCTEST_INFO(r.result.message);
            if (trust && std::string(hostname) == "localhost") {
                DOCTEST_CHECK(r.result.code == hntls::ERET_OK);
            }
            break;
        }
        for (auto &f : r.outgoing) {
            flights.emplace_back(!toClient, std::move(f));
        }
    }
    co_return p;
}
asyncpp::task<bool> exercise(std::shared_ptr<sockpp::CSocketWatcher> w, hntls::EMode mode) {
    auto p = co_await pair(w, mode, true, true);
    DOCTEST_REQUIRE(p.client->state() == hntls::ESTATE_ESTABLISHED);
    DOCTEST_REQUIRE(p.server->state() == hntls::ESTATE_ESTABLISHED);
    DOCTEST_CHECK(p.client->peerInfo().authentication == hntls::EPEER_ACCEPTED);
    DOCTEST_CHECK(p.server->peerInfo().authentication == hntls::EPEER_ACCEPTED);
    DOCTEST_CHECK(p.client->peerInfo().alpn == "h3");
    if (mode == hntls::EMODE_RECORDS) {
        auto enc = co_await p.client->encrypt({1, 2, 3, 4});
        DOCTEST_REQUIRE(enc.result.code == hntls::ERET_OK);
        DOCTEST_CHECK(enc.bytes != hntls::SBytes{1, 2, 3, 4});
        auto plain = co_await p.server->receive(hntls::ELEVEL_INITIAL, std::move(enc.bytes));
        DOCTEST_REQUIRE(plain.result.code == hntls::ERET_OK);
        DOCTEST_CHECK(plain.plaintext == std::vector<hntls::SBytes>{{1, 2, 3, 4}});
        auto fin = co_await p.client->finishWrite();
        auto end = co_await p.server->receive(hntls::ELEVEL_APPLICATION, std::move(fin.bytes));
        DOCTEST_CHECK(end.result.code == hntls::ERET_OK);
        DOCTEST_CHECK(end.peerClosedWrite);
        auto bad = co_await p.server->encrypt({7});
        bad.bytes.back() ^= 1;
        auto reject = co_await p.client->receive(hntls::ELEVEL_APPLICATION, std::move(bad.bytes));
        DOCTEST_CHECK(reject.result.code == hntls::ERET_AUTHENTICATION);
        DOCTEST_CHECK(reject.plaintext.empty());
        DOCTEST_CHECK(p.client->state() == hntls::ESTATE_FAILED);
    } else {
        DOCTEST_CHECK(p.client->peerInfo().quicTransportParameters == hntls::SBytes{2, 0});
        auto enc = co_await p.client->encrypt({1});
        DOCTEST_CHECK(enc.result.code == hntls::ERET_INVALID_STATE);
    }
    p.client->close();
    p.server->close();
    DOCTEST_CHECK(co_await p.client->waitClosed() == hntls::ERET_OK);
    co_return true;
}
DOCTEST_TEST_CASE("TLS 1.3 record handshake mutual authentication encryption tamper close_notify") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(drive(w, exercise(w, hntls::EMODE_RECORDS)));
}
DOCTEST_TEST_CASE("TLS 1.3 QUIC handshake is record free and exports authenticated parameters") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(drive(w, exercise(w, hntls::EMODE_QUIC)));
}
asyncpp::task<bool> negative(std::shared_ptr<sockpp::CSocketWatcher> w) {
    for (auto [trusted, name] : {std::pair{false, "localhost"}, std::pair{true, "wrong.example"}}) {
        auto p = co_await pair(w, hntls::EMODE_RECORDS, trusted, false, name);
        DOCTEST_CHECK(p.client->state() == hntls::ESTATE_FAILED);
        DOCTEST_CHECK(p.client->peerInfo().authentication == hntls::EPEER_REJECTED);
    }
    co_return true;
}
DOCTEST_TEST_CASE("TLS rejects unknown trust anchors and wrong DNS identity") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(drive(w, negative(w)));
}
DOCTEST_TEST_CASE("TLS policy and separated client factories validate inputs") {
    auto w = std::make_shared<sockpp::CSocketWatcher>();
    auto context = hntls::ITlsContext::create();
    DOCTEST_CHECK(context->isImplemented());
    DOCTEST_CHECK(hntls::ITlsClient::create(w, context));
    hntls::STlsOptions o;
    o.enableEarlyData = true;
    DOCTEST_CHECK_THROWS_AS(hntls::ITlsContext::create(o), std::invalid_argument);
    o.enableEarlyData = false;
    o.alpn = {std::string(256, 'h')};
    DOCTEST_CHECK_THROWS_AS(hntls::ITlsContext::create(o), std::invalid_argument);
    o.alpn = {};
    o.role = hntls::EROLE_SERVER;
    DOCTEST_CHECK_THROWS_AS(hntls::ITlsClient::create(w, hntls::ITlsContext::create(o)),
                            std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(context->createSession({}, hntls::EMODE_RECORDS), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(context->createSession(w, hntls::EMODE_RECORDS, {1}), std::invalid_argument);
}
DOCTEST_TEST_CASE("RFC 8448 handshake traffic key and IV vectors") {
    auto secret = hex("b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21");
    auto key = hntls::detail::label(secret, "key", {}, 16);
    auto iv = hntls::detail::label(secret, "iv", {}, 12);
    DOCTEST_CHECK(hntls::SBytes(key.toPtr(), key.toPtr() + key.size()) ==
                  hex("dbfaa693d1762c5b666af5d950258d01"));
    DOCTEST_CHECK(hntls::SBytes(iv.toPtr(), iv.toPtr() + iv.size()) == hex("5bd3c71b836e0b76bb73265f"));
}
asyncpp::task<bool> forgedProof(std::shared_ptr<sockpp::CSocketWatcher> w, unsigned target) {
    co_await asyncpp::defer(*w);
    auto identity = testIdentity();
    hntls::STlsOptions c;
    c.serverName = "localhost";
    c.alpn = {"h3"};
    auto s = c;
    s.role = hntls::EROLE_SERVER;
    s.requirePeerAuthentication = false;
    hntls::STrustOptions anchors;
    anchors.anchors = identity.certificateChain;
    auto client = hntls::ITlsContext::create(c, {}, anchors)->createSession(w, hntls::EMODE_QUIC, {1, 0});
    auto server = hntls::ITlsContext::create(s, identity)->createSession(w, hntls::EMODE_QUIC, {2, 0});
    co_await server->start();
    auto hello = co_await client->start();
    auto reply = co_await server->receive(hntls::ELEVEL_INITIAL, std::move(hello.outgoing.front().bytes));
    DOCTEST_REQUIRE(reply.result.code == hntls::ERET_OK);
    bool rejected = false;
    for (auto &flight : reply.outgoing) {
        bool forged = flight.bytes.front() == target;
        if (forged) {
            flight.bytes.back() ^= 1;
        }
        auto result = co_await client->receive(flight.level, std::move(flight.bytes));
        if (forged) {
            DOCTEST_CHECK(result.result.code == hntls::ERET_AUTHENTICATION);
            DOCTEST_CHECK(result.secrets.empty());
            DOCTEST_CHECK(result.plaintext.empty());
            DOCTEST_CHECK_FALSE(result.handshakeComplete);
            rejected = true;
            break;
        }
        DOCTEST_REQUIRE(result.result.code == hntls::ERET_OK);
    }
    DOCTEST_CHECK(rejected);
    DOCTEST_CHECK(client->state() == hntls::ESTATE_FAILED);
    DOCTEST_CHECK(client->peerInfo().authentication == hntls::EPEER_REJECTED);
    co_return true;
}
DOCTEST_TEST_CASE("TLS rejects forged CertificateVerify and Finished before trust or application success") {
    for (unsigned type : {15u, 20u}) {
        auto w = std::make_shared<sockpp::CSocketWatcher>();
        DOCTEST_CHECK(drive(w, forgedProof(w, type)));
    }
}
} // namespace
