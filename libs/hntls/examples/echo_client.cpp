#include <hntls.hpp>
#include <certpp/x509/cert.hpp>
#include <asyncpp/defer.h>
#include <asyncpp/sync_wait.h>
#include <iostream>
#include <future>

namespace {
asyncpp::task<int> run(std::shared_ptr<sockpp::CSocketWatcher> w, std::uint16_t port, hntls::SBytes der) {
    co_await asyncpp::defer(*w);
    auto anchor = std::make_shared<certpp::x509::CCert>();
    if (anchor->importFrom({der.data(), der.size()}) != certpp::ERET_OK) {
        co_return 1;
    }
    hntls::STlsOptions options;
    options.serverName = "localhost";
    options.alpn = {"echo/1"};
    hntls::STrustOptions trust;
    trust.anchors = {anchor};
    auto client = hntls::ITlsClient::create(w, hntls::ITlsContext::create(options, {}, trust));
    auto result = co_await client->connect(sockpp::CSocketAddress::loopbackIpv4(port));
    if (result.result.code != hntls::ERET_OK) {
        std::cerr << result.result.message << " code=" << result.result.code << std::endl;
        client->close();
        co_await client->waitClosed();
        co_return 2;
    }
    auto stream = result.stream;
    auto write = co_await stream->writeSome({'h', 'n', 't', 'l', 's'});
    auto reply = co_await stream->readSome(16384);
    std::cout << std::string(reply.bytes.begin(), reply.bytes.end()) << std::endl;
    co_await stream->finishWrite();
    auto eof = co_await stream->readSome(1);
    client->close();
    co_await client->waitClosed();
    co_return write.result.code == hntls::ERET_OK &&reply.bytes == hntls::SBytes{'h', 'n', 't', 'l', 's'} &&
            eof.result.code == hntls::ERET_EOF
        ? 0
        : 3;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "usage: hntls_echo_client port trusted-DER-hex\n";
            return 1;
        }
        std::string text = argv[2];
        if (text.size() % 2) {
            return 1;
        }
        hntls::SBytes der;
        for (std::size_t i = 0; i < text.size(); i += 2) {
            der.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
        }
        auto w = std::make_shared<sockpp::CSocketWatcher>();
        auto f = asyncpp::as_promise(run(w, static_cast<std::uint16_t>(std::stoul(argv[1])), std::move(der)));
        while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
            w->runOne(std::chrono::milliseconds{100});
        }
        return f.get();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
