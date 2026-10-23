#include <socketpp/socket.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto listener = sockpp::CSocket::open(sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
        listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        listener.listen();
        auto client = sockpp::CSocket::open(sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
        client.connect(listener.localAddress());
        auto accepted = listener.accept();
        auto remaining = example::bytes("hello");
        while (!remaining.empty()) {
            const auto count = client.send(remaining);
            if (count == 0) {
                throw std::runtime_error("send made no progress");
            }
            remaining = remaining.subspan(count);
        }
        std::cout << example::receiveText(accepted, 5) << '\n';
    });
}
