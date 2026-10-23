#include <daemonizer.hpp>

#include <asyncpp/sync_wait.h>
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

DOCTEST_TEST_SUITE_BEGIN("daemonizer");

daemonz::SReadOnlyByteSpan bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/* Use a separate executable so Unix fork always precedes runtime threads. */
int runHelper(const std::string& scenario, bool replayArguments = false, const std::string& marker = {}) {
#if defined(_WIN32)
    const std::string path = DAEMONIZER_HELPER_PATH;
    const std::wstring executable(path.begin(), path.end());
    std::wstring command = L"\"" + executable + L"\" " + std::wstring(scenario.begin(), scenario.end());
    if (replayArguments) command += L" \"alpha beta\" \"quote\\\"inside\" \"C:\\path with space\\\\\" \"\"";
    if (!marker.empty()) {
        const auto wide = std::filesystem::path(std::u8string(marker.begin(), marker.end())).wstring();
        command += L" \"" + wide + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateProcessW(test helper)");
    }
    ::CloseHandle(process.hThread);
    const auto wait = ::WaitForSingleObject(process.hProcess, 15000);
    if (wait != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 99);
        ::WaitForSingleObject(process.hProcess, INFINITE);
        ::CloseHandle(process.hProcess);
        throw std::runtime_error("daemon test helper timed out");
    }
    DWORD code = 0;
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    const pid_t child = ::fork();
    if (child < 0) throw std::system_error(errno, std::generic_category(), "fork(test helper)");
    if (child == 0) {
        ::setpgid(0, 0);
        const char* executable = DAEMONIZER_HELPER_PATH;
        if (replayArguments) {
            ::execl(executable, executable, scenario.c_str(), "alpha beta", "quote\"inside",
                    "C:\\path with space\\", "", static_cast<char*>(nullptr));
        } else if (!marker.empty()) {
            ::execl(executable, executable, scenario.c_str(), marker.c_str(), static_cast<char*>(nullptr));
        } else {
            ::execl(executable, executable, scenario.c_str(), static_cast<char*>(nullptr));
        }
        ::_exit(98);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category(), "waitpid(test helper)");
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(-child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("daemon test helper timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

DOCTEST_TEST_CASE("options enforce process and per-process thread budgets") {
    daemonz::SDaemonOptions options;
    options.max_fork = 2;
    options.max_reactors = 2;
    options.max_workers = 2;
    DOCTEST_CHECK(options.validated().max_threads == 4);
    DOCTEST_CHECK(options.max_threads == -1);
    options.max_threads = 3;
    DOCTEST_CHECK_THROWS_AS(options.validated(), std::invalid_argument);
    options.max_threads = 4;
    DOCTEST_CHECK_NOTHROW(options.validated());
    options.max_threads = -2;
    DOCTEST_CHECK_THROWS_AS(options.validated(), std::invalid_argument);
    options.max_threads = -1;
    options.max_workers = (std::numeric_limits<int>::max)();
    DOCTEST_CHECK_THROWS_AS(options.validated(), std::invalid_argument);
    options.max_workers = 0;
    DOCTEST_CHECK(options.validated().max_threads == 2);
    options.max_fork = -1;
    DOCTEST_CHECK_THROWS_AS(options.validated(), std::invalid_argument);
    options.max_fork = 0;
    options.max_reactors = 0;
    DOCTEST_CHECK_THROWS_AS(options.validated(), std::invalid_argument);
    DOCTEST_CHECK(runHelper("invalid") == 1);
}

DOCTEST_TEST_CASE("signal values preserve native metadata and portable categories") {
#if defined(_WIN32)
    const auto signal = daemonz::SSignal::fromNative(CTRL_BREAK_EVENT, 42);
    DOCTEST_CHECK(signal.type() == daemonz::ESIG_INTERRUPT);
    DOCTEST_CHECK(signal.nativeCode() == CTRL_BREAK_EVENT);
#else
    const auto signal = daemonz::SSignal::fromNative(SIGTERM, 42);
    DOCTEST_CHECK(signal.type() == daemonz::ESIG_TERMINATE);
    DOCTEST_CHECK(signal.nativeCode() == SIGTERM);
#endif
    DOCTEST_CHECK(signal.sender() == 42);
}

DOCTEST_TEST_CASE("duplex pipes support concurrent large transfers in both directions") {
    std::vector<std::byte> outgoing(512 * 1024, std::byte{0x5a});
    std::vector<std::byte> incoming(outgoing.size());
    std::array<std::byte, 4> reply{};
    auto server = daemonz::IPipeServer::create(2);
    auto pair = server->createPair();
    DOCTEST_CHECK(server->pipeCount() == 2);
    auto received = asyncpp::as_promise(pair.child->readExact(incoming));
    auto sent = asyncpp::as_promise(pair.parent->writeAll(outgoing));
    auto replied = asyncpp::as_promise(pair.child->writeAll(bytes("pong")));
    auto replyReceived = asyncpp::as_promise(pair.parent->readExact(reply));
    sent.get();
    DOCTEST_CHECK(received.get());
    replied.get();
    DOCTEST_CHECK(replyReceived.get());
    DOCTEST_CHECK(incoming == outgoing);
    DOCTEST_CHECK(std::to_integer<char>(reply[0]) == 'p');
    server->close();
    DOCTEST_CHECK(server->pipeCount() == 0);
    DOCTEST_CHECK_FALSE(pair.parent->isOpen());
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(pair.parent->read(reply)).get(), std::system_error);
}

DOCTEST_TEST_CASE("pipe EOF cancellation and duplicate reads have distinct outcomes") {
    std::array<std::byte, 4> buffer{};
    std::array<std::byte, 4> other{};
    auto server = daemonz::IPipeServer::create();
    auto pair = server->createPair();
    auto pending = asyncpp::as_promise(pair.parent->read(buffer));
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(pair.parent->read(other)).get(), std::logic_error);
    pair.parent->close();
    DOCTEST_CHECK_THROWS_AS(pending.get(), std::system_error);
    auto eof = server->createPair();
    eof.child->close();
    DOCTEST_CHECK_FALSE(asyncpp::as_promise(eof.parent->readExact(buffer)).get());
    auto truncated = server->createPair();
    asyncpp::as_promise(truncated.child->writeAll(bytes("x"))).get();
    truncated.child->close();
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(truncated.parent->readExact(buffer)).get(), std::system_error);
}

DOCTEST_TEST_CASE("launch exchanges data with two children and preserves original argv") {
    DOCTEST_CHECK(runHelper("ipc", true) == 0);
}

DOCTEST_TEST_CASE("parent shutdown reaches child signal hooks and reaps every child") {
    DOCTEST_CHECK(runHelper("graceful") == 0);
}

DOCTEST_TEST_CASE("shutdown timeout terminates and reaps uncooperative owned children") {
    DOCTEST_CHECK(runHelper("stubborn") == 1);
}

DOCTEST_TEST_CASE("kernel signals reach the asynchronous daemon signal hook") {
    DOCTEST_CHECK(runHelper("signal") == 0);
    DOCTEST_CHECK(runHelper("body") == 0);
}

DOCTEST_TEST_CASE("startup and worker failures still run daemon cleanup") {
    DOCTEST_CHECK(runHelper("failure") == 1);
    DOCTEST_CHECK(runHelper("worker-failure") == 1);
    DOCTEST_CHECK(runHelper("no-workers") == 1);
    DOCTEST_CHECK(runHelper("body-failure") == 1);
    DOCTEST_CHECK(runHelper("signal-failure") == 1);
}

DOCTEST_TEST_CASE("detached root releases its terminal and completes cleanup") {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() / ("daemonizer-detach-" + unique + ".txt");
    const auto encoded = path.u8string();
    const std::string marker(encoded.begin(), encoded.end());
    DOCTEST_REQUIRE(runHelper("detach", false, marker) == 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    std::string completed;
    std::uint64_t pid = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(path);
        input >> completed >> pid;
        if (completed == "done" && pid != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (std::filesystem::exists(path)) std::filesystem::remove(path);
    DOCTEST_CHECK(completed == "done");
    DOCTEST_REQUIRE(pid != 0);
#if defined(_WIN32)
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process) {
        const DWORD result = ::WaitForSingleObject(process, 5000);
        ::CloseHandle(process);
        DOCTEST_CHECK(result == WAIT_OBJECT_0);
    }
#endif
}

DOCTEST_TEST_CASE("launch preserves root and child failure exit statuses") {
    DOCTEST_CHECK(runHelper("exit-code") == 7);
    DOCTEST_CHECK(runHelper("child-failure") == 3);
}

DOCTEST_TEST_SUITE_END();

} // namespace
