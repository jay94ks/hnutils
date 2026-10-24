#ifndef __SRC_HNFS_ASYNC_INTERNAL_HPP__
#define __SRC_HNFS_ASYNC_INTERNAL_HPP__
#include <asyncpp/dispatcher.h>
#include <coroutine>

namespace hnfs {
namespace detail {

struct DispatchOn {
    asyncpp::dispatcher* worker;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> continuation) const {
        // --> Always queue: dispatcher TLS may differ across Windows DLL boundaries.
        worker->push([continuation] { continuation.resume(); });
    }
    void await_resume() const noexcept {}
};

} // namespace detail
} // namespace hnfs
#endif
