#include <socketpp/udp_socket_async.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        sockpp::CSocketWatcher watcher;
        auto receiver = sockpp::CUdpSocketAsync::open(watcher);
        auto sender = sockpp::CUdpSocketAsync::open(watcher);
        receiver.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        sender.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        std::array<std::byte, 64> buffer{};
        sockpp::CSocketAddress source;
        auto pending = asyncpp::as_promise(receiver.receiveFrom(buffer, source));
        example::run(watcher, sender.sendTo(example::bytes("query"), receiver.localAddress()));
        std::cout << "Query bytes: " << example::drive(watcher, std::move(pending)) << '\n';
        example::run(watcher, receiver.connect(source));
        example::run(watcher, sender.connect(receiver.localAddress()));
        example::run(watcher, receiver.send(example::bytes("reply")));
        std::cout << "Reply bytes: " << example::run(watcher, sender.receive(buffer)) << '\n';
    });
}
