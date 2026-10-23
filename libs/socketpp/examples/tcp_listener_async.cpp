#include <socketpp/tcp_listener_async.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        sockpp::CSocketWatcher watcher;
        auto listener = sockpp::CTcpListenerAsync::open(watcher);
        listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        listener.listen();
        sockpp::CSocketAddress peer;
        auto pending = asyncpp::as_promise(listener.accept(&peer));
        auto client = sockpp::CTcpClient::open();
        client.connect(listener.localAddress());
        auto connection = example::drive(watcher, std::move(pending));
        example::run(watcher, connection.sendAll(example::bytes("welcome")));
        std::cout << "Accepted " << peer.endpointString() << ": " << example::receiveText(client, 7) << '\n';
    });
}
