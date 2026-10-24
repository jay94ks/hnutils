#include <hnhttp.hpp>
#include "../../hntls/examples/identity.hpp"
#include "../tests/console.hpp"
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>
#include <asyncpp/sync_wait.h>
#include <future>
#include <iostream>

namespace {
using namespace hnhttp;
asyncpp::task<ERetCode> respond(std::shared_ptr<IHttpServerContext> context) {
    auto body = co_await context->getRequest()->getBody()->collect(1024 * 1024);
    if (body.result.code != ERET_OK) {
        co_return body.result.code;
    }
    if (body.bytes.empty()) {
        const std::string text = "secure hnhttp\n";
        body.bytes.assign(text.begin(), text.end());
    }
    context->getResponse()->status(200).body(IHttpBodySource::fromBytes(std::move(body.bytes), "text/plain"));
    auto ret = co_await context->getResponse()->exec();
    if (ret != ERET_OK) {
        co_return ret;
    }
    co_return co_await context->finish();
}
asyncpp::task<ERetCode> serve(std::shared_ptr<sockpp::CSocketWatcher> watcher, EHttpVersion version) {
    co_await asyncpp::defer(*watcher);
    auto identity = exampleIdentity();
    hntls::STlsOptions tls;
    tls.role = hntls::EROLE_SERVER;
    tls.requirePeerAuthentication = false;
    tls.alpn = {version == HTTP_2 ? "h2" : "http/1.1"};
    SHttpConnectionOptions options;
    options.version = version;
    auto listener = IHttpServer::createTls(watcher, hntls::ITlsContext::create(tls, identity), options);
    auto ret = listener->listen(sockpp::CSocketAddress::loopbackIpv4(0));
    if (ret != ERET_OK) {
        co_return ret;
    }
    std::cout << "port=" << listener->localAddress().port()
              << "\ncertificate=" << exampleHex(identity.certificateChain.front()->rawData()) << std::endl;
    auto accepted = co_await listener->acceptConnection();
    if (accepted.result.code != ERET_OK) {
        listener->close();
        co_await listener->waitClosed();
        co_return accepted.result.code;
    }
    asyncpp::async_launch_scope pumps;
    auto mux = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(accepted.connection);
    std::shared_ptr<IHttpServerContext> context;
    if (mux) {
        pumps.launch(mux->run());
        ret = co_await mux->ready();
        if (ret == ERET_OK) {
            auto request = co_await mux->accept();
            ret = request.result.code;
            context = request.context;
        }
    } else {
        context =
            IHttpServerContext::create(std::dynamic_pointer_cast<IHttp1Connection>(accepted.connection));
        ret = co_await context->getRequest()->parseOnce();
    }
    if (ret == ERET_OK) {
        ret = co_await respond(context);
    }
    if (mux && ret == ERET_OK) {
        // --> This single-request wire fixture lets the peer consume END_STREAM and close.
        // Aborting immediately can reset TCP with a late unread SETTINGS ACK on Linux.
        co_await pumps.join();
    }
    accepted.connection->close();
    co_await pumps.join();
    auto closed = co_await accepted.connection->waitClosed();
    listener->close();
    co_await listener->waitClosed();
    co_return ret == ERET_OK ? closed : ret;
}
} // namespace
int main(int argc, char** argv) {
    configureFailures();
    try {
        const auto version = argc > 1 && std::string(argv[1]) == "2" ? hnhttp::HTTP_2 : hnhttp::HTTP_11;
        auto watcher = std::make_shared<sockpp::CSocketWatcher>();
        auto result = asyncpp::as_promise(serve(watcher, version));
        while (result.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
            watcher->runOne(std::chrono::milliseconds{100});
        }
        auto ret = result.get();
        if (ret != hnhttp::ERET_OK) {
            std::cerr << "secure HTTP operation failed: " << ret << std::endl;
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
