#include <hnhttp.hpp>
#include <socketpp/async_socket.hpp>
#include <asyncpp/defer.h>

#include <exception>
#include <functional>
#include <span>
#include <type_traits>

namespace hnhttp_examples {
using namespace hnhttp;

/* Application-provided adapter, not a transport implementation supplied by hnhttp.
 * It owns socket buffers through completion, maps native errors, reports optional policies
 * honestly, and implements abort/wakeup/join before exposing an IHttpStream.
 */
using SSocketStreamFactory = std::function<std::shared_ptr<IHttpStream>(
    std::shared_ptr<sockpp::CSocketWatcher>, sockpp::CSocketAsync)>;

// --> The bridge can pass views of an owned SBytes frame to socketpp without exposing native handles.
static_assert(std::is_same_v<decltype(std::declval<sockpp::CSocketAsync&>().receive(
    std::as_writable_bytes(std::span<std::uint8_t>{}))), asyncpp::task<std::size_t>>);
static_assert(std::is_same_v<decltype(std::declval<sockpp::CSocketAsync&>().send(
    std::as_bytes(std::span<const std::uint8_t>{}))), asyncpp::task<std::size_t>>);

/* Compile-only, supervised by the caller. The watcher has exactly one runner and remains
 * active until this handler, connection waitClosed and any other handlers/pumps are joined.
 * Admission to workers is caller-bounded; a raw worker pool is not the protocol executor.
 */
asyncpp::task<ERetCode> serveTwoRequests(std::shared_ptr<sockpp::CSocketWatcher> watcher,
    sockpp::CSocket accepted, std::shared_ptr<asyncpp::dispatcher> workers,
    SSocketStreamFactory adapt) {
    if (!watcher || !workers || !adapt) { co_return ERET_INVALID_ARGUMENT; }
    co_await asyncpp::defer(*watcher);
    // --> Move the synchronous accepted socket into reactor-backed asynchronous ownership.
    auto stream = adapt(watcher, sockpp::CSocketAsync(*watcher, std::move(accepted)));
    if (!stream || stream->executor().get() != watcher.get()) { co_return ERET_INVALID_ARGUMENT; }
    auto context = IHttpServerContext::create();
    auto bound = context->setStream(stream); // Caller may retain this shared lifetime, not perform I/O.
    if (bound != ERET_OK) { co_return bound; }
    auto connection = std::dynamic_pointer_cast<IHttp1Connection>(context->getConnection());
    auto protocol = connection->executor();
    std::exception_ptr failure;
    ERetCode result = ERET_OK;
    try {
        for (unsigned index = 0; index < 2; ++index) {
            auto request = context->getRequest();
            result = co_await request->parseOnce();
            if (result != ERET_OK) { break; }
            // --> Workers receive copies, never references to executor-affine facades.
            auto path = request->path();
            SBytes reply;
            std::exception_ptr workerFailure;
            co_await asyncpp::defer(*workers);
            try {
                reply.assign(path.begin(), path.end()); // Example application-only computation.
            } catch (...) { workerFailure = std::current_exception(); }
            // --> Return even on a worker exception before touching HTTP state or propagating it.
            co_await asyncpp::defer(*protocol);
            if (workerFailure) { std::rethrow_exception(workerFailure); }
            context->getResponse()->status(200).body(IHttpBodySource::fromBytes(std::move(reply)));
            result = co_await context->getResponse()->exec();
            if (result != ERET_OK) { break; }
            result = co_await context->finish({true, 1024*1024});
            if (result != ERET_OK || !connection->isReusable() || index == 1) { break; }
            // --> Keep the read-ahead owner: do not setStream(stream) on a second connection.
            context = IHttpServerContext::create(connection);
        }
    } catch (...) { failure = std::current_exception(); }
    // --> A scheduling failure can leave us on the worker; marshal cleanup as well.
    co_await asyncpp::defer(*protocol);
    connection->close();
    auto closed = co_await connection->waitClosed();
    if (failure) { std::rethrow_exception(failure); }
    co_return result == ERET_OK ? closed : result;
}

} // namespace hnhttp_examples
