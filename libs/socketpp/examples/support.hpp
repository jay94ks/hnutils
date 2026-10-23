#ifndef __SOCKETPP_EXAMPLES_SUPPORT_HPP__
#define __SOCKETPP_EXAMPLES_SUPPORT_HPP__

#include <socketpp.hpp>
#include <asyncpp/sync_wait.h>

#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace example {

/* Keep the data, not merely its span descriptor, alive during I/O. */
inline sockpp::SReadOnlyByteSpan bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/* A caller-owned loop can drive asyncpp tasks without another thread. */
template<typename T>
T drive(sockpp::CSocketWatcher& watcher, std::future<T> future) {
    using namespace std::chrono_literals;
    while (future.wait_for(0ms) != std::future_status::ready) {
        watcher.runOne(100ms);
    }
    return future.get();
}

template<typename T>
T run(sockpp::CSocketWatcher& watcher, asyncpp::task<T> task) {
    return drive(watcher, asyncpp::as_promise(std::move(task)));
}

/* TCP is a byte stream: one receive need not fill the requested buffer. */
template<typename TSocket>
std::string receiveText(TSocket& socket, std::size_t length) {
    std::string result(length, '\0');
    auto remaining = std::as_writable_bytes(std::span(result));
    while (!remaining.empty()) {
        const auto count = socket.receive(remaining);
        if (count == 0) {
            throw std::runtime_error("unexpected TCP EOF");
        }
        remaining = remaining.subspan(count);
    }
    return result;
}

template<typename TSocket>
asyncpp::task<std::string> receiveTextAsync(TSocket& socket, std::size_t length) {
    std::string result(length, '\0');
    auto remaining = std::as_writable_bytes(std::span(result));
    while (!remaining.empty()) {
        const auto count = co_await socket.receive(remaining);
        if (count == 0) {
            throw std::runtime_error("unexpected TCP EOF");
        }
        remaining = remaining.subspan(count);
    }
    co_return result;
}

/* Report example failures through stderr, without assertions or debugger breaks. */
template<typename TFunction>
int report(TFunction&& function) {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try {
        std::forward<TFunction>(function)();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "socketpp example: " << error.what() << '\n';
        return 1;
    }
}

} // namespace example

#endif
