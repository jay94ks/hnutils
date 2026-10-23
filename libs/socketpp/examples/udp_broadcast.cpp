#include <socketpp/udp_broadcast.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto sender = sockpp::CUdpBroadcast::open();
        auto receiver = sockpp::CUdpBroadcast::open();
        sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        std::cout << "SO_BROADCAST=" << sockpp::opts::SBroadcast::read(sender).how << '\n';
        // --> Use unicast locally so the example does not emit network broadcasts.
        sender.sendTo(example::bytes("discovery"), receiver.localAddress());
        std::array<std::byte, 64> buffer{};
        sockpp::CSocketAddress source;
        receiver.receiveFrom(buffer, source);
        receiver.sendTo(example::bytes("found"), source);
        std::cout << "Reply bytes: " << sender.receiveFrom(buffer, source) << '\n';
        // To discover on a real network, use an unbound socket (not loopback-bound):
        // auto network = sockpp::CUdpBroadcast::open();
        // network.send(example::bytes("discovery"), 37020); // 255.255.255.255
        // network.sendTo(example::bytes("discovery"),
        //     sockpp::CSocketAddress::parse("192.168.1.255", 37020));
    });
}
