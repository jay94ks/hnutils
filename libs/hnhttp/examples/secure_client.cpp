#include <hnhttp.hpp>
#include <asyncpp/launch.h>

namespace hnhttp_examples {
/** All tasks are supervised on watcher; HTTPS and HTTP/3 never fall back to plaintext. */
asyncpp::task<hnhttp::ERetCode> requestSecure(std::shared_ptr<sockpp::CSocketWatcher> watcher,
                                              std::shared_ptr<hntls::ITlsContext> policy,
                                              sockpp::CSocketAddress address, hnhttp::EHttpVersion version) {
    using namespace hnhttp;
    SHttpConnectionOptions options;
    options.version = version;
    auto client = version == HTTP_3 ? IHttpClient::createQuic(watcher, policy, options)
                                    : IHttpClient::createTls(watcher, policy, options);
    asyncpp::async_launch_scope native, http;
    ERetCode result = ERET_OK;
    std::exception_ptr failure;
    try {
        if (version == HTTP_3) {
            native.launch(client->run());
        }
        result = co_await client->connect(address);
        if (result == ERET_OK) {
            if (auto mux = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(client->getConnection())) {
                http.launch(mux->run());
                result = co_await mux->ready();
            }
        }
        if (result == ERET_OK) {
            auto context = client->createRequest();
            context->getRequest()->path("/");
            result = co_await context->getRequest()->exec();
            if (result == ERET_OK) {
                auto body = co_await context->getResponse()->getBody()->collect(1024 * 1024);
                result = body.result.code;
            }
            if (result == ERET_OK) {
                result = co_await context->finish();
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    client->close();
    co_await http.join();
    co_await native.join();
    auto closed = co_await client->waitClosed();
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return result == ERET_OK ? closed : result;
}
} // namespace hnhttp_examples
