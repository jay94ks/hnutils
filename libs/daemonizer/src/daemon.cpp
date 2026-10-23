#include <daemonizer/daemon.hpp>

#include "runtime.hpp"

#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace daemonz {

struct CDaemon::Impl {
    mutable std::mutex mutex;
    bool running = false;
    bool stopped = false;
    int exitCode = 0;
    std::deque<SSignal> signals;
    std::optional<asyncpp::promise<void>> wake;
    asyncpp::promise<void> stopCompletion;
    std::shared_ptr<detail::Runtime> runtime;
};

/* Destroy the daemon interface. */
IDaemon::~IDaemon() = default;

/* Construct an idle daemon without starting threads. */
CDaemon::CDaemon() : _impl(std::make_unique<Impl>()) {}

/* Destroy the encapsulated lifecycle state. */
CDaemon::~CDaemon() = default;

/* Run startup, serialized signal callbacks, and guaranteed cleanup. */
asyncpp::task<int> CDaemon::run(CDaemonContext context) {
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->running) throw std::logic_error("daemon is already running");
        _impl->running = true;
        _impl->stopped = false;
        _impl->exitCode = 0;
        _impl->stopCompletion = asyncpp::promise<void>{};
        _impl->runtime = context._impl->runtime;
    }
    std::exception_ptr failure;
    std::optional<asyncpp::promise<void>> applicationDone;
    std::optional<asyncpp::eager_fire_and_forget_task<>> application;
    try {
        co_await onStart(context);
        applicationDone.emplace();
        application.emplace(runApplication(context, *applicationDone));
        for (;;) {
            std::optional<SSignal> signal;
            std::optional<asyncpp::promise<void>> wait;
            {
                std::lock_guard lock(_impl->mutex);
                if (_impl->stopped) break;
                if (!_impl->signals.empty()) {
                    signal = _impl->signals.front();
                    _impl->signals.pop_front();
                } else {
                    wait.emplace();
                    _impl->wake = *wait;
                }
            }
            if (signal) co_await onSignal(*signal);
            else co_await *wait;
            {
                std::lock_guard lock(_impl->mutex);
                _impl->wake.reset();
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    if (applicationDone) {
        requestStop(failure ? 1 : 0);
        try {
            co_await *applicationDone;
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    try {
        co_await onStop();
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    int exitCode = 0;
    {
        std::lock_guard lock(_impl->mutex);
        exitCode = _impl->exitCode;
        _impl->stopped = true;
        _impl->running = false;
        _impl->wake.reset();
        _impl->runtime.reset();
    }
    if (failure) std::rethrow_exception(failure);
    co_return exitCode;
}

/* Enqueue a signal and settle a waiting lifecycle on its reactor. */
void CDaemon::postSignal(SSignal signal) {
    std::optional<asyncpp::promise<void>> wake;
    std::shared_ptr<detail::Runtime> runtime;
    {
        std::lock_guard lock(_impl->mutex);
        _impl->signals.push_back(signal);
        wake = _impl->wake;
        runtime = _impl->runtime;
    }
    if (wake && runtime) runtime->post([wake = *wake]() mutable { (void)wake.try_fulfill(); });
}

/* Request cooperative termination without invoking user code on this thread. */
void CDaemon::requestStop(int exitCode) {
    std::optional<asyncpp::promise<void>> wake;
    std::shared_ptr<detail::Runtime> runtime;
    asyncpp::promise<void> completion;
    {
        std::lock_guard lock(_impl->mutex);
        if (!_impl->stopped) {
            _impl->stopped = true;
            _impl->exitCode = exitCode;
        }
        wake = _impl->wake;
        runtime = _impl->runtime;
        completion = _impl->stopCompletion;
    }
    if (runtime) runtime->post([wake, completion]() mutable {
        if (wake) (void)wake->try_fulfill();
        (void)completion.try_fulfill();
    });
}

/* Read the lifecycle stop flag through its synchronization boundary. */
bool CDaemon::stopRequested() const noexcept {
    std::lock_guard lock(_impl->mutex);
    return _impl->stopped;
}

/* Default startup has no application-specific work. */
asyncpp::task<void> CDaemon::onStart(const CDaemonContext&) { co_return; }

/* Keep the application body alive independently of the signal-processing loop. */
asyncpp::eager_fire_and_forget_task<> CDaemon::runApplication(
    CDaemonContext context, asyncpp::promise<void> completed) {
    std::exception_ptr failure;
    try {
        co_await onRun(context);
        requestStop();
    } catch (...) {
        failure = std::current_exception();
        requestStop(1);
    }
    if (failure) completed.reject(failure);
    else completed.fulfill();
}

/* Default application behavior remains alive until cooperative shutdown. */
asyncpp::task<void> CDaemon::onRun(const CDaemonContext&) { co_await waitForStop(); }

/* Copy the current launch's stop completion before suspending. */
asyncpp::task<void> CDaemon::waitForStop() {
    asyncpp::promise<void> completion;
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->stopped) co_return;
        completion = _impl->stopCompletion;
    }
    co_await completion;
}

/* Apply portable default shutdown behavior to termination signals. */
asyncpp::task<void> CDaemon::onSignal(SSignal signal) {
    if (signal.type() == ESIG_INTERRUPT || signal.type() == ESIG_TERMINATE || signal.type() == ESIG_CLOSE) {
        requestStop();
    }
    co_return;
}

/* Default cleanup has no application-specific work. */
asyncpp::task<void> CDaemon::onStop() { co_return; }

} // namespace daemonz
