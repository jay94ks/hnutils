#include <socketpp/address.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto ipv4 = sockpp::CSocketAddress::parse("127.0.0.1", 8080);
        auto ipv6 = sockpp::CSocketAddress::loopbackIpv6(8080);
        auto addresses = sockpp::CSocketAddress::resolve("localhost", 8080);
        std::cout << ipv4.endpointString() << '\n' << ipv6.endpointString() << '\n';
        for (const auto& address : addresses) {
            std::cout << address.endpointString() << '\n';
        }
    });
}
