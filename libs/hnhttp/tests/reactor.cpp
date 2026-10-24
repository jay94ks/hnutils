#include <hnhttp.hpp>
#include <socketpp/watcher.hpp>
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>
#include <asyncpp/thread_pool.h>
#include <doctest/doctest.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using namespace hnhttp;

static_assert(std::is_base_of_v<asyncpp::dispatcher, sockpp::CSocketWatcher>);
static_assert(std::is_convertible_v<std::shared_ptr<sockpp::CSocketWatcher>,
    std::shared_ptr<asyncpp::dispatcher>>);

struct ProbeResult {
    bool enteredReactor = false;
    bool enteredWorker = false;
    bool returnedToReactor = false;
    std::thread::id reactorThread;
    std::thread::id workerThread;
    unsigned copiedValue = 0;
    bool workerExceptionReturned = false;
    std::exception_ptr failure;
};

/* Scheduler integration only: no fake HTTP parser, context or socket adapter. */
asyncpp::task<void> roundTrip(std::shared_ptr<sockpp::CSocketWatcher> reactor,
    std::shared_ptr<asyncpp::thread_pool> workers, bool failWorker, ProbeResult& result) {
    try {
        std::shared_ptr<asyncpp::dispatcher> protocol = reactor;
        co_await asyncpp::defer(*protocol);
        result.enteredReactor = asyncpp::dispatcher::current() == protocol.get();
        result.reactorThread = std::this_thread::get_id();
        unsigned copiedInput = 21;
        co_await asyncpp::defer(*workers);
        result.enteredWorker = asyncpp::dispatcher::current() == workers.get();
        result.workerThread = std::this_thread::get_id();
        std::exception_ptr workerFailure;
        try {
            if (failWorker) { throw std::runtime_error("expected worker failure"); }
            result.copiedValue = copiedInput*2;
        } catch (...) { workerFailure = std::current_exception(); }
        co_await asyncpp::defer(*protocol);
        result.returnedToReactor = asyncpp::dispatcher::current() == protocol.get()
            && std::this_thread::get_id() == result.reactorThread;
        result.workerExceptionReturned = static_cast<bool>(workerFailure);
    } catch (...) { result.failure = std::current_exception(); }
}

DOCTEST_TEST_CASE("socket watcher and worker support supervised coroutine round trips") {
    bool failWorker = false;
    DOCTEST_SUBCASE("worker success") {}
    DOCTEST_SUBCASE("worker exception returns to reactor") { failWorker = true; }
    auto reactor = std::make_shared<sockpp::CSocketWatcher>();
    auto workers = std::make_shared<asyncpp::thread_pool>(1);
    ProbeResult result;
    asyncpp::async_launch_scope scope;
    scope.launch(roundTrip(reactor, workers, failWorker, result));
    // --> No blocking future.get on the reactor; CTest supplies the external 30-second bound.
    while (!scope.all_done()) { reactor->runOne(std::chrono::milliseconds{10}); }
    DOCTEST_CHECK_FALSE(static_cast<bool>(result.failure));
    DOCTEST_CHECK(result.enteredReactor);
    DOCTEST_CHECK(result.enteredWorker);
    DOCTEST_CHECK(result.returnedToReactor);
    DOCTEST_CHECK(result.reactorThread != result.workerThread);
    DOCTEST_CHECK(result.workerExceptionReturned == failWorker);
    DOCTEST_CHECK(result.copiedValue == (failWorker ? 0 : 42));
    DOCTEST_CHECK(asyncpp::dispatcher::current() == nullptr);
    // --> All continuations have completed before stopping or destroying the event loop.
    reactor->stop();
}
} // namespace
