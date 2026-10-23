#include <socketpp.hpp>

#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <exception>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace {

DOCTEST_TEST_SUITE_BEGIN("socketpp");

sockpp::SReadOnlyByteSpan bytes(std::string_view text) {
    return {
        reinterpret_cast<const std::byte*>(text.data()),
        text.size(),
    };
}

DOCTEST_TEST_CASE("addresses resolve and format IPv4 IPv6 and local endpoints") {
    auto ipv4 = sockpp::CSocketAddress::resolveOne("127.0.0.1", 8443, sockpp::EAF_INET);
    DOCTEST_CHECK(ipv4.isValid());
    DOCTEST_CHECK(ipv4.family() == sockpp::EAF_INET);
    DOCTEST_CHECK(ipv4.port() == 8443);
    DOCTEST_CHECK(ipv4.isLoopback());
    DOCTEST_CHECK(ipv4.toString() == "127.0.0.1");
    DOCTEST_CHECK(ipv4.endpointString() == "127.0.0.1:8443");

    ipv4.port(9443);
    DOCTEST_CHECK(ipv4.port() == 9443);

    auto ipv6 = sockpp::CSocketAddress::loopbackAddress(443, sockpp::EAF_INET6);
    DOCTEST_CHECK(ipv6.family() == sockpp::EAF_INET6);
    DOCTEST_CHECK(ipv6.port() == 443);
    DOCTEST_CHECK(ipv6.isLoopback());

    auto local = sockpp::CSocketAddress::local("socketpp-test.sock");
    DOCTEST_CHECK(local.family() == sockpp::EAF_LOCAL);
    DOCTEST_CHECK(local.port() == -1);
    DOCTEST_CHECK(local.toString() == "socketpp-test.sock");
}

DOCTEST_TEST_CASE("synchronous TCP exchanges data and applies typed options") {
    auto listener = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    listener.setOption(sockpp::opts::SReuseAddress{});
    DOCTEST_CHECK(sockpp::opts::SReuseAddress::read(listener).how);
#if defined(_WIN32)
    auto optionProbe = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    optionProbe.setOption(sockpp::opts::SExclusiveAddressUse{});
    DOCTEST_CHECK(sockpp::opts::SExclusiveAddressUse::read(optionProbe).how);
#else
    listener.setOption(sockpp::opts::SReusePort{});
    DOCTEST_CHECK(sockpp::opts::SReusePort::read(listener).how);
#endif
    listener.bind(sockpp::CSocketAddress::loopbackAddress());
    listener.listen();
    const auto endpoint = listener.localAddress();

    std::exception_ptr serverError;
    std::thread server([&]() {
        try {
            auto accepted = listener.accept();
            std::array<std::byte, 4> request{};
            const auto received = accepted.receive(request);
            DOCTEST_CHECK(received == request.size());
            DOCTEST_CHECK(accepted.send(bytes("pong")) == 4);
        } catch (...) {
            serverError = std::current_exception();
        }
    });

    auto client = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    const sockpp::opts::SNoDelay noDelay{true};
    client.setOption(noDelay);
    DOCTEST_CHECK(sockpp::opts::SNoDelay::read(client).how);
    client.setOption(sockpp::opts::SNoDelay{false});
    DOCTEST_CHECK_FALSE(sockpp::opts::SNoDelay::read(client).how);
    sockpp::opts::SKeepAlive keepAlive{true};
    client.setOption(keepAlive);
    DOCTEST_CHECK(sockpp::opts::SKeepAlive::read(client).how);
    client.connect(endpoint);
    DOCTEST_CHECK(client.send(bytes("ping")) == 4);
    std::array<std::byte, 4> response{};
    DOCTEST_CHECK(client.receive(response) == response.size());
    DOCTEST_CHECK(std::to_integer<char>(response[0]) == 'p');

    server.join();
    if (serverError) {
        std::rethrow_exception(serverError);
    }
}

DOCTEST_TEST_CASE("synchronous UDP preserves the datagram source address") {
    auto receiver = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_DATAGRAM,
        sockpp::EPROTO_UDP);
    receiver.bind(sockpp::CSocketAddress::loopbackAddress());
    const auto endpoint = receiver.localAddress();

    auto sender = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_DATAGRAM,
        sockpp::EPROTO_UDP);
    DOCTEST_CHECK(sender.sendTo(bytes("data"), endpoint) == 4);

    std::array<std::byte, 4> message{};
    sockpp::CSocketAddress source;
    DOCTEST_CHECK(receiver.receiveFrom(message, source) == message.size());
    DOCTEST_CHECK(source.family() == sockpp::EAF_INET);
    DOCTEST_CHECK(std::to_integer<char>(message[0]) == 'd');
}

DOCTEST_TEST_CASE("asynchronous TCP connects and exchanges data") {
    auto listener = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    listener.bind(sockpp::CSocketAddress::loopbackAddress());
    listener.listen();
    const auto endpoint = listener.localAddress();

    std::exception_ptr serverError;
    std::thread server([&]() {
        try {
            auto accepted = listener.accept();
            std::array<std::byte, 4> request{};
            DOCTEST_CHECK(accepted.receive(request) == request.size());
            DOCTEST_CHECK(accepted.send(bytes("done")) == 4);
        } catch (...) {
            serverError = std::current_exception();
        }
    });

    sockpp::CSocketWatcher watcher;
    DOCTEST_CHECK((watcher.backend() == sockpp::EWATCH_IOCP ||
                   watcher.backend() == sockpp::EWATCH_EPOLL ||
                   watcher.backend() == sockpp::EWATCH_KQUEUE));
    std::exception_ptr watcherError;
    std::thread eventLoop([&]() {
        try {
            watcher.run();
        } catch (...) {
            watcherError = std::current_exception();
        }
    });

    auto client = sockpp::CSocketAsync::open(
        watcher,
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    try {
        const sockpp::opts::SNoDelay noDelay{true};
        client.setOption(noDelay);
        DOCTEST_CHECK(sockpp::opts::SNoDelay::read(client).how);
        client.setOption(sockpp::opts::SNoDelay{false});
        DOCTEST_CHECK_FALSE(sockpp::opts::SNoDelay::read(client).how);
        sockpp::opts::SKeepAlive keepAlive{true};
        client.setOption(keepAlive);
        DOCTEST_CHECK(sockpp::opts::SKeepAlive::read(client).how);
        asyncpp::as_promise(client.connect(endpoint)).get();
        asyncpp::as_promise(client.sendAll(bytes("work"))).get();
        std::array<std::byte, 4> response{};
        DOCTEST_CHECK(asyncpp::as_promise(client.receive(response)).get() == response.size());
        DOCTEST_CHECK(std::to_integer<char>(response[0]) == 'd');
    } catch (...) {
        (void)client.close();
        (void)listener.close();
        watcher.stop();
        eventLoop.join();
        server.join();
        throw;
    }

    watcher.stop();
    eventLoop.join();
    server.join();
    if (watcherError) {
        std::rethrow_exception(watcherError);
    }
    if (serverError) {
        std::rethrow_exception(serverError);
    }
}

DOCTEST_TEST_CASE("asynchronous accept returns a watcher-attached socket") {
    auto rawListener = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    rawListener.bind(sockpp::CSocketAddress::loopbackAddress());
    rawListener.listen();

    sockpp::CSocketWatcher watcher;
    sockpp::CSocketAsync listener(watcher, std::move(rawListener));
    const auto endpoint = listener.localAddress();
    std::exception_ptr watcherError;
    std::thread eventLoop([&]() {
        try {
            watcher.run();
        } catch (...) {
            watcherError = std::current_exception();
        }
    });

    auto client = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_STREAM,
        sockpp::EPROTO_TCP);
    try {
        sockpp::CSocketAddress peer;
        auto acceptedFuture = asyncpp::as_promise(listener.accept(&peer));
        client.connect(endpoint);
        auto accepted = acceptedFuture.get();
        DOCTEST_CHECK(peer.family() == sockpp::EAF_INET);
        DOCTEST_CHECK(client.send(bytes("take")) == 4);

        std::array<std::byte, 4> message{};
        DOCTEST_CHECK(asyncpp::as_promise(accepted.receive(message)).get() == message.size());
        DOCTEST_CHECK(std::to_integer<char>(message[0]) == 't');
    } catch (...) {
        (void)client.close();
        (void)listener.close();
        watcher.stop();
        eventLoop.join();
        throw;
    }

    watcher.stop();
    eventLoop.join();
    if (watcherError) {
        std::rethrow_exception(watcherError);
    }
}

DOCTEST_TEST_CASE("asynchronous UDP receives a datagram and its source address") {
    sockpp::CSocketWatcher watcher;
    auto receiver = sockpp::CSocketAsync::open(
        watcher,
        sockpp::EAF_INET,
        sockpp::ESOCK_DATAGRAM,
        sockpp::EPROTO_UDP);
    receiver.bind(sockpp::CSocketAddress::loopbackAddress());
    const auto endpoint = receiver.localAddress();

    std::exception_ptr watcherError;
    std::thread eventLoop([&]() {
        try {
            watcher.run();
        } catch (...) {
            watcherError = std::current_exception();
        }
    });

    auto sender = sockpp::CSocket::open(
        sockpp::EAF_INET,
        sockpp::ESOCK_DATAGRAM,
        sockpp::EPROTO_UDP);
    try {
        std::array<std::byte, 4> message{};
        sockpp::CSocketAddress source;
        auto receiveFuture = asyncpp::as_promise(receiver.receiveFrom(message, source));
        DOCTEST_CHECK(sender.sendTo(bytes("gram"), endpoint) == 4);
        DOCTEST_CHECK(receiveFuture.get() == message.size());
        DOCTEST_CHECK(source.family() == sockpp::EAF_INET);
        DOCTEST_CHECK(std::to_integer<char>(message[0]) == 'g');
    } catch (...) {
        (void)sender.close();
        (void)receiver.close();
        watcher.stop();
        eventLoop.join();
        throw;
    }

    watcher.stop();
    eventLoop.join();
    if (watcherError) {
        std::rethrow_exception(watcherError);
    }
}

DOCTEST_TEST_SUITE_END();

} // namespace
