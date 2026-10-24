#include <hnhttp.hpp>
#include <asyncpp/defer.h>

namespace hnhttp_examples {
using namespace hnhttp;

/* Run under a caller-owned task scope while watcher remains active. No TLS or DNS is used. */
asyncpp::task<ERetCode> postPlaintext(std::shared_ptr<sockpp::CSocketWatcher> watcher,
    sockpp::CSocketAddress endpoint) {
    co_await asyncpp::defer(*watcher);
    auto client = IHttpClient::create(watcher);
    auto result = co_await client->connect(endpoint);
    std::exception_ptr failure;
    try {
        if (result == ERET_OK) {
            auto context = client->createRequest();
            auto form = IFormData::create(EFORM_URLENCODED);
            form->addField("message", "hello world").addField("page", 1);
            auto request = context->getRequest();
            request->method(HTTP_POST).path("/echo").authority(endpoint.endpointString()).body(form);
            result = co_await request->exec();
            if (result == ERET_OK) {
                auto response = co_await context->getResponse()->getBody()->collect(1024*1024);
                result = response.result.code;
                if (result == ERET_OK) { result = co_await request->waitSent(); }
                if (result == ERET_OK) { result = co_await context->finish(); }
            }
        }
    } catch (...) { failure = std::current_exception(); }
    client->close(); auto closed = co_await client->waitClosed();
    if (failure) { std::rethrow_exception(failure); }
    co_return result == ERET_OK ? closed : result;
}

/* The caller creates/listens on IHttpServer and admits a bounded number of these handlers. */
asyncpp::task<ERetCode> echoOne(std::shared_ptr<IHttpServer> server) {
    auto accepted = co_await server->accept();
    if (accepted.result.code != ERET_OK) { co_return accepted.result.code; }
    auto context = accepted.context;
    auto connection = context->getConnection();
    ERetCode result = ERET_OK;
    std::exception_ptr failure;
    try {
        auto incoming = co_await context->getRequest()->getBody()->collect(1024*1024);
        result = incoming.result.code;
        if (result == ERET_OK) {
            context->getResponse()->status(200).body(IHttpBodySource::fromBytes(std::move(incoming.bytes)));
            result = co_await context->getResponse()->exec();
            if (result == ERET_OK) { result = co_await context->finish(); }
        }
    } catch (...) { failure = std::current_exception(); }
    connection->close(); auto closed = co_await connection->waitClosed();
    if (failure) { std::rethrow_exception(failure); }
    co_return result == ERET_OK ? closed : result;
}
} // namespace hnhttp_examples
