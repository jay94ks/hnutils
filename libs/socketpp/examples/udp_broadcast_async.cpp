#include <socketpp/udp_broadcast_async.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        sockpp::CSocketWatcher watcher;
        auto sender = sockpp::CUdpBroadcastAsync::open(watcher);
        auto receiver = sockpp::CUdpBroadcastAsync::open(watcher);
        sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        std::cout << "SO_BROADCAST=" << sockpp::opts::SBroadcast::read(sender).how << '\n';
        // --> Unicast makes this example safe to run without broadcasting externally.
        std::array<std::byte, 64> buffer{};
        sockpp::CSocketAddress source;
        auto pending = asyncpp::as_promise(receiver.receiveFrom(buffer, source));
        example::run(watcher, sender.sendTo(example::bytes("discovery"), receiver.localAddress()));
        example::drive(watcher, std::move(pending));
        example::run(watcher, receiver.sendTo(example::bytes("found"), source));
        std::cout << "Reply bytes: " << example::run(watcher, sender.receiveFrom(buffer, source)) << '\n';
        // Real broadcasts require a non-loopback-bound socket:
        // auto network = sockpp::CUdpBroadcastAsync::open(watcher);
        // example::run(watcher, network.send(example::bytes("discovery"), 37020));
    });
}
