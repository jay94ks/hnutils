#include <hnhttp.hpp>
#include <asyncpp/launch.h>
#include <exception>

namespace hnhttp_examples {
using namespace hnhttp;

/* Supervised child: async_launch_scope must never receive an escaping exception. */
asyncpp::task<void> drive(std::shared_ptr<IHttpMultiplexedConnection> connection,
    ERetCode& result, std::exception_ptr& failure) {
    try { result = co_await connection->run(); }
    catch (...) { failure = std::current_exception(); }
}

/* One accepted request; production dispatch may run bounded concurrent handlers. */
asyncpp::task<ERetCode> handleOne(std::shared_ptr<IHttpMultiplexedConnection> connection) {
    auto accepted = co_await connection->accept();
    if (accepted.result.code != ERET_OK) { co_return accepted.result.code; }
    auto context = std::move(accepted.context);
    auto request = context->getRequest();
    // --> Headers are already decoded by the shared protocol pump, without buffering the body.
    if (co_await request->parseOnce() != ERET_OK) { co_return ERET_MALFORMED; }
    context->getResponse()->status(200).body(IHttpBodySource::fromBytes(SBytes{'o','k'}, "text/plain"));
    auto sent = co_await context->getResponse()->exec();
    if (sent != ERET_OK) { co_return sent; }
    co_return co_await context->finish({true, 1024*1024});
}

/* Caller must enter its serial protocol executor and keep the external transport event loop alive. */
asyncpp::task<ERetCode> serveOne(std::shared_ptr<IHttpConnection> connection) {
    auto multiplexed = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    if (!multiplexed) { co_return ERET_UNSUPPORTED; }
    asyncpp::async_launch_scope scope;
    ERetCode pumpResult = ERET_OK;
    std::exception_ptr pumpFailure, handlerFailure;
    scope.launch(drive(multiplexed, pumpResult, pumpFailure));
    ERetCode handled = ERET_INVALID_STATE;
    try {
        auto ready = co_await multiplexed->ready();
        handled = ready == ERET_OK ? co_await handleOne(multiplexed) : ready;
    } catch (...) { handlerFailure = std::current_exception(); }
    connection->close();
    co_await scope.join(); // No future.get() that could block the same event loop.
    auto closed = co_await connection->waitClosed();
    if (handlerFailure) { std::rethrow_exception(handlerFailure); }
    if (pumpFailure) { std::rethrow_exception(pumpFailure); }
    (void)pumpResult; // Explicit close can end the pump with CLOSED/CANCELLED after successful handling.
    co_return handled == ERET_OK ? closed : handled;
}

/* HTTP/2 uses one ordered byte transport; hnhttp owns framing/HPACK/multiplexing. */
asyncpp::task<ERetCode> serveHttp2(std::shared_ptr<IHttpStream> stream) {
    SHttpConnectionOptions options;
    options.version = HTTP_2;
    auto connection = IHttpConnection::createStream(std::move(stream), HTTP_SERVER, options);
    co_return co_await serveOne(std::move(connection));
}

/* HTTP/3 consumes native QUIC uni/bidirectional streams, never a fake TCP-like byte transport. */
asyncpp::task<ERetCode> serveHttp3(std::unique_ptr<IHttpQuicTransport> transport) {
    SHttpConnectionOptions options;
    options.version = HTTP_3;
    options.http3.qpackBlockedStreams = 8;
    auto connection = IHttpConnection::createQuic(std::move(transport), HTTP_SERVER, options);
    co_return co_await serveOne(std::move(connection));
}

} // namespace hnhttp_examples
