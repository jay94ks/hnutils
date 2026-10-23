#ifndef __INCLUDE_DAEMONIZER_CONTEXT_HPP__
#define __INCLUDE_DAEMONIZER_CONTEXT_HPP__

#include <daemonizer/options.hpp>
#include <daemonizer/pipe_server.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace daemonz {

namespace detail {
class Launcher;
} // namespace detail

class CDaemon;

/** Immutable per-process launch context with controlled runtime operations. */
class DAEMONIZER_API CDaemonContext {
private:
    struct Impl;
    std::shared_ptr<const Impl> _impl;

    /** Construct the launch-owned immutable implementation. */
    explicit CDaemonContext(std::shared_ptr<const Impl> impl);

    friend class detail::Launcher;
    friend class CDaemon;

public:
    /** Destroy the context snapshot. */
    ~CDaemonContext();

    /** Copy a context snapshot without exposing its implementation. */
    CDaemonContext(const CDaemonContext& other);

    /** Assign another immutable context snapshot. */
    CDaemonContext& operator=(const CDaemonContext& other);

    /** Return the validated options with internal launch flags removed. */
    const SDaemonOptions& options() const noexcept;

    /** Return the operating-system process identifier. */
    std::uint64_t processId() const noexcept;

    /** Return the launch parent's process identifier, or zero for a root. */
    std::uint64_t parentProcessId() const noexcept;

    /** Return zero for the root, or a child's one-based process slot. */
    std::size_t processIndex() const noexcept;

    /** Test whether this instance occupies a managed child slot. */
    bool isChild() const noexcept;

    /** Return the parent endpoint for a child, or nullptr for the root. */
    IPipePtr parentPipe() const noexcept;

    /** Return the root's child endpoints in process-slot order. */
    const std::vector<IPipePtr>& childPipes() const noexcept;

    /** Return the server managing this process's endpoints. */
    IPipeServerPtr pipeServer() const noexcept;

    /**
     * Run a callback on a dedicated worker and resume on the first reactor.
     * @param callback Work whose exceptions propagate through the task.
     * @throw std::logic_error if max_workers is zero.
     */
    asyncpp::task<void> work(std::function<void()> callback) const;
};

} // namespace daemonz

#endif
