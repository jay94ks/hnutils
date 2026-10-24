#ifndef __INCLUDE_SOCKETPP_WATCHER_HPP__
#define __INCLUDE_SOCKETPP_WATCHER_HPP__

#include <socketpp/common.hpp>

#include <asyncpp/dispatcher.h>
#include <asyncpp/promise.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace sockpp {

class CSocket;
class CSocketAsync;

/**
 * Runs socket completions and asyncpp callbacks on a platform event backend.
 */
class SOCKETPP_API CSocketWatcher final : public asyncpp::dispatcher {
private:
    /** Remove and reject POSIX readiness waits after the native handle has closed. */
    void cancelWaits(std::uintptr_t closedHandle) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /**
     * Create a watcher using the current platform's native backend.
     */
    CSocketWatcher();

    /**
     * Destroy the watcher.
     *
     * Every attached CSocketAsync and pending operation must be destroyed or
     * completed before the watcher is destroyed.
     */
    ~CSocketWatcher();

    CSocketWatcher(const CSocketWatcher&) = delete;
    CSocketWatcher(CSocketWatcher&&) = delete;
    CSocketWatcher& operator=(const CSocketWatcher&) = delete;
    CSocketWatcher& operator=(CSocketWatcher&&) = delete;

public:
    /**
     * Return the selected native event backend.
     *
     * @return IOCP, epoll, or kqueue for the current platform.
     */
    ESocketWatcherBackend backend() const noexcept;

    /**
     * Queue a callback for the watcher thread.
     *
     * @param callback The callback to execute.
     */
    void push(std::function<void()> callback) override;

    /**
     * Schedule a one-shot monotonic timer on this reactor, without a helper thread.
     * Call on the owning executor. Callback runs no earlier than deadline during runOne().
     * @return Nonzero cancellation ID, valid until firing or cancellation.
     */
    std::uint64_t schedule(std::chrono::steady_clock::time_point deadline,std::function<void()> callback);

    /** Cancel a scheduled callback on the owning executor; false if already dispatched. */
    bool cancelTimer(std::uint64_t id);

    /**
     * Process callbacks and at most one batch of native events.
     *
     * @param timeout Maximum time to wait; a negative value waits forever.
     * @return true when at least one callback or native event was processed.
     */
    bool runOne(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

    /**
     * Process callbacks and native events until stop() is called.
     */
    void run();

    /**
     * Request event-loop termination and wake a blocked run() call.
     */
    void stop() noexcept;

    /**
     * Test whether stop() has been requested.
     *
     * @return true when the watcher is stopped.
     */
    bool isStopped() const noexcept;

private:
    /** Register a one-shot readiness wait. */
    asyncpp::promise<void> wait(CSocket& socket, bool write);

    /** Associate a Windows socket with this watcher's completion port. */
    void associate(CSocket& socket);

    /** Register a Windows overlapped operation completion callback. */
    void submit(
        CSocket& socket,
        void* overlapped,
        std::function<void(std::size_t, std::error_code)> callback);

    /** Remove an overlapped operation that failed before submission. */
    void cancel(void* overlapped) noexcept;

    friend class CSocketAsync;
};

} // namespace sockpp

#endif
