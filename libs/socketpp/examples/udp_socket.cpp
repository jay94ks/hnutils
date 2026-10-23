#include <socketpp/udp_socket.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto receiver = sockpp::CUdpSocket::open();
        auto sender = sockpp::CUdpSocket::open();
        receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        sender.sendTo(example::bytes("query"), receiver.localAddress());
        std::array<std::byte, 64> buffer{};
        sockpp::CSocketAddress source;
        const auto count = receiver.receiveFrom(buffer, source);
        std::cout << "Received " << count << " bytes from " << source.endpointString() << '\n';
        // --> Connected UDP selects a default destination; it performs no handshake.
        receiver.connect(source);
        sender.connect(receiver.localAddress());
        receiver.send(example::bytes("reply"));
        std::cout << "Reply bytes: " << sender.receive(buffer) << '\n';
    });
}
