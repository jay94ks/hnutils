#include <socketpp.hpp>

#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

sockpp::SReadOnlyByteSpan bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

void receiveExact(sockpp::CTcpClient& socket, sockpp::SByteSpan buffer) {
    while (!buffer.empty()) {
        const auto count = socket.receive(buffer);
        if (count == 0) {
            throw std::runtime_error("unexpected EOF");
        }
        buffer = buffer.subspan(count);
    }
}

asyncpp::task<void> receiveExact(sockpp::CTcpClientAsync& socket, sockpp::SByteSpan buffer) {
    while (!buffer.empty()) {
        const auto count = co_await socket.receive(buffer);
        if (count == 0) {
            throw std::runtime_error("unexpected EOF");
        }
        buffer = buffer.subspan(count);
    }
}

/* Drive futures on the calling thread, including already-ready operations. */
template<typename T>
T complete(sockpp::CSocketWatcher& watcher, std::future<T> future) {
    while (future.wait_for(0ms) != std::future_status::ready) {
        watcher.runOne(100ms);
    }
    return future.get();
}

template<typename T>
void checkOwnership(T socket) {
    static_assert(!std::is_copy_constructible_v<T>);
    static_assert(!std::is_copy_assignable_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    T closed;
    DOCTEST_CHECK_FALSE(closed.isOpen());
    DOCTEST_CHECK_FALSE(closed.close());
    DOCTEST_CHECK_THROWS_AS(closed.localAddress(), std::system_error);
    DOCTEST_CHECK(socket.isOpen());
    auto moved = std::move(socket);
    DOCTEST_CHECK_FALSE(socket.isOpen());
    DOCTEST_CHECK_FALSE(socket.close());
    DOCTEST_CHECK_THROWS_AS(socket.localAddress(), std::system_error);
    socket = std::move(moved);
    DOCTEST_CHECK(socket.isOpen());
    DOCTEST_CHECK_FALSE(moved.isOpen());
    DOCTEST_CHECK_FALSE(socket.close());
    DOCTEST_CHECK_FALSE(socket.isOpen());
    DOCTEST_CHECK_FALSE(socket.close());
    DOCTEST_CHECK_THROWS_AS(socket.localAddress(), std::system_error);
}

DOCTEST_TEST_SUITE_BEGIN("socketpp");

DOCTEST_TEST_CASE("protocol wrappers own sockets and reject invalid families") {
    sockpp::CSocketWatcher watcher;
    checkOwnership(sockpp::CTcpListener::open());
    checkOwnership(sockpp::CTcpClient::open());
    checkOwnership(sockpp::CUdpSocket::open());
    checkOwnership(sockpp::CUdpBroadcast::open());
    checkOwnership(sockpp::CTcpListenerAsync::open(watcher));
    checkOwnership(sockpp::CTcpClientAsync::open(watcher));
    checkOwnership(sockpp::CUdpSocketAsync::open(watcher));
    checkOwnership(sockpp::CUdpBroadcastAsync::open(watcher));

    DOCTEST_CHECK_THROWS_AS(sockpp::CTcpListener::open(sockpp::EAF_UNSPEC), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(sockpp::CTcpClient::open(sockpp::EAF_LOCAL), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(sockpp::CUdpSocket::open(sockpp::EAF_LOCAL), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(sockpp::CTcpListenerAsync::open(watcher, sockpp::EAF_LOCAL), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(sockpp::CTcpClientAsync::open(watcher, sockpp::EAF_UNSPEC), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(sockpp::CUdpSocketAsync::open(watcher, sockpp::EAF_LOCAL), std::invalid_argument);
    sockpp::CTcpClient closed;
    DOCTEST_CHECK_FALSE(closed.isOpen());
    DOCTEST_CHECK_THROWS_AS(closed.sendAll(bytes("x")), std::system_error);
    sockpp::CTcpListenerAsync closedAsync;
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(closedAsync.accept()).get(), std::system_error);
}

DOCTEST_TEST_CASE("TCP wrappers accept clients exchange streams and report EOF") {
    auto family = sockpp::EAF_INET;
    DOCTEST_SUBCASE("IPv4") {}
    DOCTEST_SUBCASE("IPv6") { family = sockpp::EAF_INET6; }
    auto listener = sockpp::CTcpListener::open(family);
    listener.setOption(sockpp::opts::SReuseAddress{true});
    DOCTEST_CHECK(sockpp::opts::SReuseAddress::read(listener).how);
    listener.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    listener.listen(8);

    for (int connection = 0; connection < 2; ++connection) {
        auto client = sockpp::CTcpClient::open(family);
        const sockpp::opts::SNoDelay noDelay{true};
        client.setOption(noDelay);
        DOCTEST_CHECK(sockpp::opts::SNoDelay::read(client).how);
        client.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
        client.connect(listener.localAddress());
        sockpp::CSocketAddress peer;
        auto accepted = listener.accept(connection == 0 ? &peer : nullptr);
        DOCTEST_CHECK(accepted.isOpen());
        DOCTEST_CHECK(accepted.peerAddress().port() == client.localAddress().port());
        DOCTEST_CHECK(client.peerAddress().port() == listener.localAddress().port());
        if (connection == 0) {
            DOCTEST_CHECK(peer.family() == family);
            DOCTEST_CHECK(peer.port() == client.localAddress().port());
        }
        client.sendAll(bytes("request"));
        std::array<std::byte, 7> request{};
        receiveExact(accepted, request);
        DOCTEST_CHECK(request == std::array<std::byte, 7>{
            std::byte{'r'}, std::byte{'e'}, std::byte{'q'}, std::byte{'u'},
            std::byte{'e'}, std::byte{'s'}, std::byte{'t'}});
        DOCTEST_CHECK(accepted.send(bytes("!")) == 1);
        std::array<std::byte, 1> reply{};
        receiveExact(client, reply);
        DOCTEST_CHECK(reply[0] == std::byte{'!'});
        client.sendAll({});
        client.shutdown(sockpp::ESHUT_WRITE);
        DOCTEST_CHECK(accepted.receive(reply) == 0);
    }
}

DOCTEST_TEST_CASE("async TCP wrappers retain watcher on accept and handle large sendAll") {
    auto family = sockpp::EAF_INET;
    DOCTEST_SUBCASE("IPv4") {}
    DOCTEST_SUBCASE("IPv6") { family = sockpp::EAF_INET6; }
    sockpp::CSocketWatcher watcher;
    auto listener = sockpp::CTcpListenerAsync::open(watcher, family);
    listener.setOption(sockpp::opts::SReuseAddress{true});
    DOCTEST_CHECK(sockpp::opts::SReuseAddress::read(listener).how);
    listener.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    listener.listen();
    auto client = sockpp::CTcpClientAsync::open(watcher, family);
    client.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    sockpp::opts::SNoDelay noDelay{true};
    client.setOption(noDelay);
    DOCTEST_CHECK(sockpp::opts::SNoDelay::read(client).how);
    sockpp::CSocketAddress peer;
    auto accepting = asyncpp::as_promise(listener.accept(&peer));
    complete(watcher, asyncpp::as_promise(client.connect(listener.localAddress())));
    auto accepted = complete(watcher, std::move(accepting));
    DOCTEST_CHECK(peer.port() == client.localAddress().port());
    DOCTEST_CHECK(accepted.peerAddress().port() == client.localAddress().port());
    DOCTEST_CHECK(client.peerAddress().port() == listener.localAddress().port());
    accepted.setOption(sockpp::opts::SSendBuffer{4096});
    DOCTEST_CHECK(sockpp::opts::SSendBuffer::read(accepted).size >= 4096);

    std::vector<std::byte> payload(256 * 1024, std::byte{0x5a});
    std::vector<std::byte> result(payload.size());
    auto receiving = asyncpp::as_promise(receiveExact(client, result));
    complete(watcher, asyncpp::as_promise(accepted.sendAll(payload)));
    complete(watcher, std::move(receiving));
    DOCTEST_CHECK(result == payload);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(client.send(bytes("!")))) == 1);
    std::array<std::byte, 1> reply{};
    complete(watcher, asyncpp::as_promise(receiveExact(accepted, reply)));
    DOCTEST_CHECK(reply[0] == std::byte{'!'});
    complete(watcher, asyncpp::as_promise(client.sendAll({})));
    client.shutdown(sockpp::ESHUT_WRITE);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(accepted.receive(reply))) == 0);
}

DOCTEST_TEST_CASE("UDP wrappers preserve datagrams empty payloads and connected peers") {
    auto family = sockpp::EAF_INET;
    DOCTEST_SUBCASE("IPv4") {}
    DOCTEST_SUBCASE("IPv6") { family = sockpp::EAF_INET6; }
    auto receiver = sockpp::CUdpSocket::open(family);
    receiver.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    auto sender = sockpp::CUdpSocket::open(family);
    sender.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    receiver.setOption(sockpp::opts::SReceiveBuffer{8192});
    DOCTEST_CHECK(sockpp::opts::SReceiveBuffer::read(receiver).size >= 8192);
    DOCTEST_CHECK(sender.sendTo(bytes("data"), receiver.localAddress()) == 4);
    DOCTEST_CHECK(sender.sendTo(bytes("next"), receiver.localAddress()) == 4);
    std::array<std::byte, 16> buffer{};
    sockpp::CSocketAddress source;
    DOCTEST_CHECK(receiver.receiveFrom(buffer, source) == 4);
    DOCTEST_CHECK(buffer[0] == std::byte{'d'});
    DOCTEST_CHECK(source.port() == sender.localAddress().port());
    DOCTEST_CHECK(receiver.receiveFrom(buffer, source) == 4);
    DOCTEST_CHECK(buffer[0] == std::byte{'n'});
    DOCTEST_CHECK(sender.sendTo(bytes(""), receiver.localAddress()) == 0);
    DOCTEST_CHECK(receiver.receiveFrom(buffer, source) == 0);
    sender.connect(receiver.localAddress());
    receiver.connect(sender.localAddress());
    DOCTEST_CHECK(sender.peerAddress().port() == receiver.localAddress().port());
    DOCTEST_CHECK(sender.send(bytes("peer")) == 4);
    DOCTEST_CHECK(receiver.receive(buffer) == 4);
    DOCTEST_CHECK(receiver.send(bytes("ok")) == 2);
    DOCTEST_CHECK(sender.receive(buffer) == 2);
    std::vector<std::byte> oversized(65536);
    DOCTEST_CHECK_THROWS_AS(sender.send(oversized), std::system_error);
}

DOCTEST_TEST_CASE("async UDP wrappers support connected I/O and empty datagrams") {
    auto family = sockpp::EAF_INET;
    DOCTEST_SUBCASE("IPv4") {}
    DOCTEST_SUBCASE("IPv6") { family = sockpp::EAF_INET6; }
    sockpp::CSocketWatcher watcher;
    auto receiver = sockpp::CUdpSocketAsync::open(watcher, family);
    auto sender = sockpp::CUdpSocketAsync::open(watcher, family);
    receiver.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    sender.bind(sockpp::CSocketAddress::loopbackAddress(0, family));
    receiver.setOption(sockpp::opts::SReceiveBuffer{8192});
    DOCTEST_CHECK(sockpp::opts::SReceiveBuffer::read(receiver).size >= 8192);
    std::array<std::byte, 16> buffer{};
    sockpp::CSocketAddress source;
    auto receiving = asyncpp::as_promise(receiver.receiveFrom(buffer, source));
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(sender.sendTo(bytes("data"), receiver.localAddress()))) == 4);
    DOCTEST_CHECK(complete(watcher, std::move(receiving)) == 4);
    DOCTEST_CHECK(buffer[0] == std::byte{'d'});
    DOCTEST_CHECK(source.port() == sender.localAddress().port());
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(sender.sendTo(bytes(""), receiver.localAddress()))) == 0);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(receiver.receiveFrom(buffer, source))) == 0);
    complete(watcher, asyncpp::as_promise(sender.connect(receiver.localAddress())));
    complete(watcher, asyncpp::as_promise(receiver.connect(sender.localAddress())));
    DOCTEST_CHECK(sender.peerAddress().port() == receiver.localAddress().port());
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(sender.send(bytes("peer")))) == 4);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(receiver.receive(buffer))) == 4);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(receiver.send(bytes("ok")))) == 2);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(sender.receive(buffer))) == 2);
    std::vector<std::byte> oversized(65536);
    DOCTEST_CHECK_THROWS_AS(complete(watcher, asyncpp::as_promise(sender.send(oversized))), std::system_error);
}

DOCTEST_TEST_CASE("broadcast wrapper enables broadcast and handles unicast replies") {
    auto sender = sockpp::CUdpBroadcast::open();
    auto receiver = sockpp::CUdpBroadcast::open();
    DOCTEST_CHECK(sockpp::opts::SBroadcast::read(sender).how);
    DOCTEST_CHECK(sockpp::opts::SBroadcast::read(receiver).how);
    sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
    receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
    const sockpp::opts::SReuseAddress reuse{true};
    receiver.setOption(reuse);
    DOCTEST_CHECK(sockpp::opts::SReuseAddress::read(receiver).how);
    DOCTEST_CHECK(sender.sendTo(bytes("find"), receiver.localAddress()) == 4);
    std::array<std::byte, 16> buffer{};
    sockpp::CSocketAddress source;
    DOCTEST_CHECK(receiver.receiveFrom(buffer, source) == 4);
    DOCTEST_CHECK(source.port() == sender.localAddress().port());
    DOCTEST_CHECK(receiver.sendTo(bytes("here"), source) == 4);
    DOCTEST_CHECK(sender.receiveFrom(buffer, source) == 4);
    DOCTEST_CHECK(buffer[0] == std::byte{'h'});
}

DOCTEST_TEST_CASE("async broadcast wrapper enables broadcast and handles unicast replies") {
    sockpp::CSocketWatcher watcher;
    auto sender = sockpp::CUdpBroadcastAsync::open(watcher);
    auto receiver = sockpp::CUdpBroadcastAsync::open(watcher);
    DOCTEST_CHECK(sockpp::opts::SBroadcast::read(sender).how);
    DOCTEST_CHECK(sockpp::opts::SBroadcast::read(receiver).how);
    sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
    receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
    receiver.setOption(sockpp::opts::SReuseAddress{true});
    DOCTEST_CHECK(sockpp::opts::SReuseAddress::read(receiver).how);
    std::array<std::byte, 16> buffer{};
    sockpp::CSocketAddress source;
    auto receiving = asyncpp::as_promise(receiver.receiveFrom(buffer, source));
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(sender.sendTo(bytes("find"), receiver.localAddress()))) == 4);
    DOCTEST_CHECK(complete(watcher, std::move(receiving)) == 4);
    DOCTEST_CHECK(source.port() == sender.localAddress().port());
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(receiver.sendTo(bytes("here"), source))) == 4);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(sender.receiveFrom(buffer, source))) == 4);
    DOCTEST_CHECK(buffer[0] == std::byte{'h'});
}

/* Exercise the actual 255.255.255.255 path with a bounded nonblocking receiver. */
DOCTEST_TEST_CASE("broadcast wrappers send real limited broadcast datagrams") {
    auto receiver = sockpp::CSocket::open(sockpp::EAF_INET, sockpp::ESOCK_DATAGRAM, sockpp::EPROTO_UDP);
    receiver.bind(sockpp::CSocketAddress::anyIpv4(0));
    receiver.setBlocking(false);
    const auto port = static_cast<std::uint16_t>(receiver.localAddress().port());
    sockpp::CSocketWatcher watcher;
    auto sender = sockpp::CUdpBroadcast::open();
    auto asyncSender = sockpp::CUdpBroadcastAsync::open(watcher);
    DOCTEST_CHECK(sender.send(bytes("sync-broadcast"), port) == 14);
    DOCTEST_CHECK(complete(watcher, asyncpp::as_promise(asyncSender.send(bytes("async-broadcast"), port))) == 15);
    bool gotSync = false;
    bool gotAsync = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((!gotSync || !gotAsync) && std::chrono::steady_clock::now() < deadline) {
        std::array<std::byte, 64> buffer{};
        sockpp::CSocketAddress source;
        try {
            const auto count = receiver.receiveFrom(buffer, source);
            const std::string_view message(reinterpret_cast<const char*>(buffer.data()), count);
            gotSync = gotSync || message == "sync-broadcast";
            gotAsync = gotAsync || message == "async-broadcast";
        } catch (const std::system_error& error) {
            if (!sockpp::IsSocketWouldBlock(error.code())) {
                throw;
            }
            std::this_thread::sleep_for(2ms);
        }
    }
    DOCTEST_CHECK(gotSync);
    DOCTEST_CHECK(gotAsync);
}

DOCTEST_TEST_SUITE_END();

} // namespace
