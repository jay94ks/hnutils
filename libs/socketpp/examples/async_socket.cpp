#include <socketpp/async_socket.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        sockpp::CSocketWatcher watcher;
        auto receiver = sockpp::CSocketAsync::open(watcher, sockpp::EAF_INET, sockpp::ESOCK_DATAGRAM, sockpp::EPROTO_UDP);
        auto sender = sockpp::CSocketAsync::open(watcher, sockpp::EAF_INET, sockpp::ESOCK_DATAGRAM, sockpp::EPROTO_UDP);
        receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        std::array<std::byte, 64> buffer{};
        sockpp::CSocketAddress source;
        auto pending = asyncpp::as_promise(receiver.receiveFrom(buffer, source));
        example::run(watcher, sender.sendTo(example::bytes("hello"), receiver.localAddress()));
        const auto count = example::drive(watcher, std::move(pending));
        std::cout << "Received " << count << " bytes from " << source.endpointString() << '\n';
    });
}
