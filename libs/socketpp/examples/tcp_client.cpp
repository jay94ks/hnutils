#include <socketpp/tcp_client.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto listener = sockpp::CTcpListener::open();
        listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        listener.listen();
        auto client = sockpp::CTcpClient::open();
        client.setOption(sockpp::opts::SNoDelay{true});
        client.connect(listener.localAddress());
        auto server = listener.accept();
        client.sendAll(example::bytes("request"));
        std::cout << example::receiveText(server, 7) << '\n';
        server.sendAll(example::bytes("reply"));
        std::cout << example::receiveText(client, 5) << '\n';
        client.shutdown(sockpp::ESHUT_WRITE);
        std::array<std::byte, 1> tail{};
        if (server.receive(tail) != 0) {
            throw std::runtime_error("expected EOF after shutdown");
        }
    });
}
