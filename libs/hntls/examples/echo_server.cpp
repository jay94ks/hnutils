#include <hntls.hpp>
#include "identity.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/sync_wait.h>
#include <iostream>
#include <future>

namespace {
asyncpp::task<int> run(std::shared_ptr<sockpp::CSocketWatcher> w, std::uint16_t port) {
    co_await asyncpp::defer(*w);
    auto id = exampleIdentity();
    hntls::STlsOptions options;
    options.role = hntls::EROLE_SERVER;
    options.requirePeerAuthentication = false;
    options.alpn = {"echo/1"};
    auto context = hntls::ITlsContext::create(options, id);
    auto listener =
        sockpp::CSocketAsync::open(*w, sockpp::EAF_INET, sockpp::ESOCK_STREAM, sockpp::EPROTO_TCP);
    listener.bind(sockpp::CSocketAddress::loopbackIpv4(port));
    listener.listen();
    std::cout << "port=" << listener.localAddress().port()
              << "\ncertificate=" << exampleHex(id.certificateChain.front()->rawData()) << std::endl;
    auto socket = co_await listener.accept();
    auto tls = hntls::ITlsStream::create(hntls::CSocketByteStream::create(w, std::move(socket)), context);
    auto result = co_await tls->handshake();
    if (result != hntls::ERET_OK) {
        std::cerr << "handshake=" << result << std::endl;
        co_return 1;
    }
    auto input = co_await tls->readSome(16384);
    if (input.result.code != hntls::ERET_OK) {
        co_return 2;
    }
    auto output = co_await tls->writeSome(std::move(input.bytes));
    if (output.result.code != hntls::ERET_OK) {
        co_return 3;
    }
    co_await tls->finishWrite();
    auto eof = co_await tls->readSome(1);
    tls->close();
    co_await tls->waitClosed();
    listener.close();
    co_return eof.result.code == hntls::ERET_EOF ? 0 : 4;
}
} // namespace
int main(int argc, char **argv) {
    try {
        auto w = std::make_shared<sockpp::CSocketWatcher>();
        auto f = asyncpp::as_promise(run(w, argc > 1 ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 0));
        while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
            w->runOne(std::chrono::milliseconds{100});
        }
        return f.get();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
