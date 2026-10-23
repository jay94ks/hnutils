#include <daemonizer/context.hpp>

#include "runtime.hpp"

#include <utility>

namespace daemonz {

namespace {

/* Retain runtime ownership even if the original context snapshot is released. */
asyncpp::task<void> performWork(std::shared_ptr<detail::Runtime> runtime, std::function<void()> callback) {
    std::exception_ptr failure;
    try {
        co_await runtime->work(std::move(callback));
    } catch (...) {
        failure = std::current_exception();
    }
    co_await runtime->schedule();
    if (failure) std::rethrow_exception(failure);
}

} // namespace

/* Retain an immutable context implementation. */
CDaemonContext::CDaemonContext(std::shared_ptr<const Impl> impl) : _impl(std::move(impl)) {}

/* Destroy a context snapshot. */
CDaemonContext::~CDaemonContext() = default;

/* Copy a context snapshot. */
CDaemonContext::CDaemonContext(const CDaemonContext&) = default;

/* Assign a context snapshot. */
CDaemonContext& CDaemonContext::operator=(const CDaemonContext&) = default;

/* Return the validated launch options. */
const SDaemonOptions& CDaemonContext::options() const noexcept { return _impl->options; }

/* Return the native process ID. */
std::uint64_t CDaemonContext::processId() const noexcept { return _impl->processId; }

/* Return the launching parent's process ID. */
std::uint64_t CDaemonContext::parentProcessId() const noexcept { return _impl->parentId; }

/* Return the root or child process slot. */
std::size_t CDaemonContext::processIndex() const noexcept { return _impl->index; }

/* Test whether this is a managed child. */
bool CDaemonContext::isChild() const noexcept { return _impl->index != 0; }

/* Return the child's parent pipe. */
IPipePtr CDaemonContext::parentPipe() const noexcept { return _impl->parent; }

/* Return the root's child pipes. */
const std::vector<IPipePtr>& CDaemonContext::childPipes() const noexcept { return _impl->children; }

/* Return the launch-owned pipe server. */
IPipeServerPtr CDaemonContext::pipeServer() const noexcept { return _impl->server; }

/* Capture the runtime before suspending the worker task. */
asyncpp::task<void> CDaemonContext::work(std::function<void()> callback) const {
    return performWork(_impl->runtime, std::move(callback));
}

} // namespace daemonz
