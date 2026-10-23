#include <socketpp/tcp_client_async.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        auto listener = sockpp::CTcpListener::open();
        listener.bind(sockpp::CSocketAddress::loopbackIpv4(0));
        listener.listen();
        sockpp::CSocketWatcher watcher;
        auto client = sockpp::CTcpClientAsync::open(watcher);
        client.setOption(sockpp::opts::SNoDelay{true});
        example::run(watcher, client.connect(listener.localAddress()));
        auto server = listener.accept();
        example::run(watcher, client.sendAll(example::bytes("request")));
        std::cout << example::receiveText(server, 7) << '\n';
        server.sendAll(example::bytes("reply"));
        std::cout << example::run(watcher, example::receiveTextAsync(client, 5)) << '\n';
    });
}
