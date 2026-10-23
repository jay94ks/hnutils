#ifndef __SRC_DAEMONIZER_RUNTIME_HPP__
#define __SRC_DAEMONIZER_RUNTIME_HPP__

#include <daemonizer/context.hpp>

#include <asyncpp/promise.h>
#include <asyncpp/dispatcher.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace daemonz {
namespace detail {

/** One non-blocking operation owned by a reactor until kernel completion. */
class Operation {
public:
    virtual ~Operation() = default;
    virtual bool step() noexcept = 0;
    virtual void cancel() noexcept = 0;
};

/** Bounded reactor and worker queues, created only after process creation. */
class Runtime final : public asyncpp::dispatcher, public std::enable_shared_from_this<Runtime> {
private:
    struct Reactor {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::function<void()>> callbacks;
        std::vector<std::shared_ptr<Operation>> operations;
        std::thread thread;
    };

    std::vector<std::unique_ptr<Reactor>> _reactors;
    std::vector<std::thread> _workers;
    std::mutex _workerMutex;
    std::condition_variable _workerWake;
    std::deque<std::function<void()>> _work;
    std::atomic<bool> _stopping{false};
    std::atomic<std::size_t> _nextReactor{0};
    std::size_t _workerCount;
    bool _callerReactor = false;

    void process(Reactor& reactor) noexcept;
    void runReactor(Reactor& reactor) noexcept;
    void runWorker() noexcept;

public:
    Runtime(std::size_t reactors, std::size_t workers);
    ~Runtime();
    void start(bool callerReactor = false);
    void poll() noexcept;
    void stop() noexcept;
    void post(std::function<void()> callback, std::size_t reactor = 0);
    void push(std::function<void()> callback) override;
    void submit(std::shared_ptr<Operation> operation);
    asyncpp::task<void> schedule();
    asyncpp::task<void> work(std::function<void()> callback);
};

class NativeEndpoint;
using NativeEndpointPtr = std::shared_ptr<NativeEndpoint>;

/** Endpoints created before fork/re-exec, without starting runtime threads. */
struct NativePair {
    NativeEndpointPtr parent;
    NativeEndpointPtr child;
};

NativePair createNativePair();
IPipeServerPtr makePipeServer(std::shared_ptr<Runtime> runtime, bool ownsRuntime);
IPipePtr adoptPipe(const IPipeServerPtr& server, NativeEndpointPtr endpoint);

#if defined(_WIN32)
std::uintptr_t endpointHandle(const NativeEndpointPtr& endpoint);
NativeEndpointPtr adoptEndpoint(std::uintptr_t handle);
#endif

} // namespace detail

struct CDaemonContext::Impl {
    SDaemonOptions options;
    std::uint64_t processId = 0;
    std::uint64_t parentId = 0;
    std::size_t index = 0;
    IPipePtr parent;
    std::vector<IPipePtr> children;
    IPipeServerPtr server;
    std::shared_ptr<detail::Runtime> runtime;
};

} // namespace daemonz

#endif
