#include <socketpp/tcp_listener.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto listener = sockpp::CTcpListener::open();
        listener.setOption(sockpp::opts::SReuseAddress{true});
        listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        listener.listen(16);
        auto client = sockpp::CTcpClient::open();
        client.connect(listener.localAddress());
        sockpp::CSocketAddress peer;
        auto connection = listener.accept(&peer);
        connection.sendAll(example::bytes("welcome"));
        std::cout << "Accepted " << peer.endpointString() << ": " << example::receiveText(client, 7) << '\n';
    });
}
