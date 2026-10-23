#include "runtime.hpp"

#include <coroutine>
#include <stdexcept>
#include <utility>

namespace daemonz {
namespace detail {

/* Construct dormant queues without creating threads. */
Runtime::Runtime(std::size_t reactors, std::size_t workers) : _workerCount(workers) {
    if (reactors == 0) {
        throw std::invalid_argument("at least one reactor is required");
    }
    for (std::size_t i = 0; i < reactors; ++i) {
        _reactors.push_back(std::make_unique<Reactor>());
    }
}

/* Stop and join threads before releasing queue storage. */
Runtime::~Runtime() { stop(); }

/* Start exactly the configured number of dedicated threads. */
void Runtime::start(bool callerReactor) {
    _callerReactor = callerReactor;
    auto self = shared_from_this();
    try {
        for (std::size_t i = callerReactor ? 1 : 0; i < _reactors.size(); ++i) {
            auto* target = _reactors[i].get();
            target->thread = std::thread([self, target] { self->runReactor(*target); });
        }
        for (std::size_t i = 0; i < _workerCount; ++i) {
            _workers.emplace_back([self] { self->runWorker(); });
        }
    } catch (...) {
        stop();
        throw;
    }
}

/* Cancel outstanding operations, drain queues, and join managed threads. */
void Runtime::stop() noexcept {
    _stopping.store(true);
    for (auto& reactor : _reactors) {
        reactor->wake.notify_all();
    }
    _workerWake.notify_all();
    if (_callerReactor) {
        auto& reactor = *_reactors.front();
        for (;;) {
            process(reactor);
            std::unique_lock lock(reactor.mutex);
            if (reactor.callbacks.empty() && reactor.operations.empty()) break;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    const auto current = std::this_thread::get_id();
    for (auto& reactor : _reactors) {
        if (reactor->thread.joinable()) {
            if (reactor->thread.get_id() == current) reactor->thread.detach();
            else reactor->thread.join();
        }
    }
    for (auto& worker : _workers) {
        if (worker.joinable()) {
            if (worker.get_id() == current) worker.detach();
            else worker.join();
        }
    }
}

/* Queue a callback on a selected reactor. */
void Runtime::post(std::function<void()> callback, std::size_t reactor) {
    auto& target = *_reactors.at(reactor);
    {
        std::lock_guard lock(target.mutex);
        if (_stopping.load()) throw std::logic_error("daemon runtime is stopping");
        target.callbacks.push_back(std::move(callback));
    }
    target.wake.notify_one();
}

/* Register an operation on a fixed reactor for its entire lifetime. */
void Runtime::submit(std::shared_ptr<Operation> operation) {
    const auto index = _nextReactor.fetch_add(1) % _reactors.size();
    auto* target = _reactors[index].get();
    post([target, operation = std::move(operation)] {
        target->operations.push_back(operation);
    }, index);
}

/* Route asyncpp dispatcher callbacks to the lifecycle reactor. */
void Runtime::push(std::function<void()> callback) { post(std::move(callback)); }

/* Yield a task to the first reactor without blocking its caller. */
asyncpp::task<void> Runtime::schedule() {
    struct Awaiter {
        Runtime* runtime;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> continuation) {
            runtime->post([continuation] { continuation.resume(); });
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{this};
}

/* Dispatch application work to the separate bounded worker queue. */
asyncpp::task<void> Runtime::work(std::function<void()> callback) {
    if (_workerCount == 0) throw std::logic_error("max_workers is zero");
    asyncpp::promise<void> result;
    {
        std::lock_guard lock(_workerMutex);
        if (_stopping.load()) throw std::logic_error("daemon runtime is stopping");
        _work.push_back([result, callback = std::move(callback)]() mutable {
            try {
                if (callback) callback();
                result.fulfill();
            } catch (...) {
                result.try_reject(std::current_exception());
            }
        });
    }
    _workerWake.notify_one();
    co_await result;
}

/* Process one batch without blocking the launching thread's signal supervision. */
void Runtime::process(Reactor& reactor) noexcept {
    auto* previous = asyncpp::dispatcher::current(this);
    std::deque<std::function<void()>> callbacks;
    {
        std::lock_guard lock(reactor.mutex);
        callbacks.swap(reactor.callbacks);
    }
    for (auto& callback : callbacks) callback();
    for (std::size_t i = 0; i < reactor.operations.size();) {
        const auto operation = reactor.operations[i];
        if (_stopping.load()) operation->cancel();
        if (operation->step()) {
            reactor.operations.erase(reactor.operations.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    asyncpp::dispatcher::current(previous);
}

/* Let the original launch thread serve as reactor zero within its thread budget. */
void Runtime::poll() noexcept { process(*_reactors.front()); }

/* Poll only non-blocking kernel operations; no thread is reserved per pipe. */
void Runtime::runReactor(Reactor& reactor) noexcept {
    for (;;) {
        process(reactor);
        std::unique_lock lock(reactor.mutex);
        if (_stopping.load() && reactor.callbacks.empty() && reactor.operations.empty()) break;
        if (!reactor.callbacks.empty()) continue;
        if (reactor.operations.empty()) {
            reactor.wake.wait(lock, [&] { return _stopping.load() || !reactor.callbacks.empty(); });
        } else {
            reactor.wake.wait_for(lock, std::chrono::milliseconds{5}, [&] {
                return !reactor.callbacks.empty();
            });
        }
    }
}

/* Execute ordinary work independently of I/O reactors. */
void Runtime::runWorker() noexcept {
    for (;;) {
        std::function<void()> callback;
        {
            std::unique_lock lock(_workerMutex);
            _workerWake.wait(lock, [&] { return _stopping.load() || !_work.empty(); });
            if (_work.empty() && _stopping.load()) break;
            callback = std::move(_work.front());
            _work.pop_front();
        }
        callback();
    }
}

} // namespace detail
} // namespace daemonz
