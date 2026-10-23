#ifndef __INCLUDE_DAEMONIZER_DAEMON_HPP__
#define __INCLUDE_DAEMONIZER_DAEMON_HPP__

#include <daemonizer/context.hpp>
#include <daemonizer/signal.hpp>

#include <asyncpp/fire_and_forget.h>
#include <asyncpp/promise.h>

#include <memory>

namespace daemonz {

/** Process lifecycle and signal-delivery contract for a daemon instance. */
class DAEMONIZER_API IDaemon {
public:
    /** Destroy the daemon interface. */
    virtual ~IDaemon();

    /**
     * Validate options, launch processes, and drive asynchronous lifecycles.
     * @param daemon Instance used by the root and each child invocation.
     * @param options Original arguments and per-process resource limits.
     * @return Exit status; configuration and startup errors print to stderr.
     * @note On Unix call before starting application-owned threads.
     */
    static int launch(IDaemon& daemon, const SDaemonOptions& options);

    /** Run one process lifecycle and return its exit status asynchronously. */
    virtual asyncpp::task<int> run(CDaemonContext context) = 0;

    /** Queue a signal for delivery outside the kernel's signal handler. */
    virtual void postSignal(SSignal signal) = 0;

    /** Request cooperative termination with a process exit status. */
    virtual void requestStop(int exitCode = 0) = 0;

    /** Test whether cooperative termination was requested. */
    virtual bool stopRequested() const noexcept = 0;
};

/** Default daemon lifecycle with private state and overridable async hooks. */
class DAEMONIZER_API CDaemon : public IDaemon {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    /** Run the application body while the lifecycle independently handles signals. */
    asyncpp::eager_fire_and_forget_task<> runApplication(
        CDaemonContext context, asyncpp::promise<void> completed);

public:
    /** Construct an idle daemon. */
    CDaemon();

    /** Destroy a daemon after launch() and all its tasks have finished. */
    ~CDaemon() override;

    /** Daemon identity and lifecycle state cannot be copied. */
    CDaemon(const CDaemon&) = delete;

    /** Daemon identity and lifecycle state cannot be assigned. */
    CDaemon& operator=(const CDaemon&) = delete;

    /** Drive start, signal processing, and stop hooks for one launch. */
    asyncpp::task<int> run(CDaemonContext context) final;

    /** Queue an event and wake the lifecycle on its reactor. */
    void postSignal(SSignal signal) final;

    /** Request stop and wake the lifecycle on its reactor. */
    void requestStop(int exitCode = 0) final;

    /** Test whether the current or most recent launch requested stop. */
    bool stopRequested() const noexcept final;

protected:
    /** Perform asynchronous startup; default implementation does nothing. */
    virtual asyncpp::task<void> onStart(const CDaemonContext& context);

    /** Run application work concurrently with signal delivery; default waits for stop. */
    virtual asyncpp::task<void> onRun(const CDaemonContext& context);

    /** Suspend until requestStop() is called without blocking a runtime thread. */
    asyncpp::task<void> waitForStop();

    /** Handle an event; interrupt, terminate, and close request stop by default. */
    virtual asyncpp::task<void> onSignal(SSignal signal);

    /** Perform cleanup, including when startup or signal handling throws. */
    virtual asyncpp::task<void> onStop();
};

} // namespace daemonz

#endif
