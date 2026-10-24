#include <hnhttp.hpp>
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>
#include <asyncpp/sync_wait.h>
#include <future>
#include <iostream>

namespace {
using namespace hnhttp;
asyncpp::task<ERetCode> handle(std::shared_ptr<IHttpMultiplexedConnection> connection) {
    auto result = co_await connection->ready(); if (result != ERET_OK) { co_return result; }
    auto accepted = co_await connection->accept(); if (accepted.result.code != ERET_OK) { co_return accepted.result.code; }
    auto bytes = co_await accepted.context->getRequest()->getBody()->collect(1024*1024);
    if (bytes.result.code != ERET_OK) { co_return bytes.result.code; }
    if (bytes.bytes.empty()) { const std::string text = "hnhttp HTTP/2\n"; bytes.bytes.assign(text.begin(),text.end()); }
    accepted.context->getResponse()->headers().add("x-hnhttp","independent-peer-example");
    accepted.context->getResponse()->body(IHttpBodySource::fromBytes(std::move(bytes.bytes),"text/plain"));
    result = co_await accepted.context->getResponse()->exec(); if (result != ERET_OK) { co_return result; }
    co_return co_await accepted.context->finish();
}
asyncpp::task<ERetCode> serve(std::shared_ptr<sockpp::CSocketWatcher> watcher, std::uint16_t port) {
    co_await asyncpp::defer(*watcher); SHttpConnectionOptions options; options.version = HTTP_2;
    auto listener = IHttpServer::create(watcher,options);
    auto result = listener->listen(sockpp::CSocketAddress::loopbackIpv4(port)); if (result != ERET_OK) { co_return result; }
    std::cout << "port=" << listener->localAddress().port() << std::endl;
    auto accepted = co_await listener->acceptConnection(); if (accepted.result.code != ERET_OK) { co_return accepted.result.code; }
    auto connection = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(accepted.connection); asyncpp::async_launch_scope tasks;
    tasks.launch(connection->run()); std::exception_ptr exception;
    try { result = co_await handle(connection); } catch (...) { exception = std::current_exception(); result = ERET_IO; }
    if (result == ERET_OK) { result = co_await connection->gracefulShutdown(); } else { connection->close(); }
    co_await tasks.join(); const auto joined = co_await connection->waitClosed();
    listener->close(); (void)co_await listener->waitClosed(); if (exception) { std::rethrow_exception(exception); }
    co_return result == ERET_OK ? joined : result;
}
}
/** One plaintext HTTP/2 prior-knowledge connection and request; no TLS or HTTP/1 upgrade. */
int main(int argc, char** argv) {
    try {
        const auto port = argc == 2 ? std::stoul(argv[1]) : 0ul; if (port > 65535) { throw std::invalid_argument("invalid port"); }
        auto watcher = std::make_shared<sockpp::CSocketWatcher>(); auto result = asyncpp::as_promise(serve(watcher,static_cast<std::uint16_t>(port)));
        while (result.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) { watcher->runOne(std::chrono::milliseconds{10}); }
        const auto code = result.get(); if (code != ERET_OK) { std::cerr << "HTTP/2 operation failed: " << code << '\n'; return 1; }
        return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
