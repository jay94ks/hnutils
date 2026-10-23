#include <daemonizer.hpp>

#include <array>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#else
#include <unistd.h>
#endif

namespace {

daemonz::SReadOnlyByteSpan bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

class TestDaemon final : public daemonz::CDaemon {
private:
    std::string _scenario;
    bool _child = false;
    bool _signalSeen = false;
    bool _cleaned = false;
    std::string _markerFile;
    std::uint64_t _processId = 0;

public:
    explicit TestDaemon(std::string scenario) : _scenario(std::move(scenario)) {}
    bool cleaned() const noexcept { return _cleaned; }

protected:
    asyncpp::task<void> onStart(const daemonz::CDaemonContext& context) override {
        _child = context.isChild();
        _processId = context.processId();
        if (context.options().max_threads != context.options().max_reactors + context.options().max_workers) {
            throw std::runtime_error("normalized thread budget mismatch");
        }
#if defined(__linux__)
        std::size_t threadCount = 0;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
            (void)entry;
            ++threadCount;
        }
        if (threadCount != static_cast<std::size_t>(context.options().max_threads)) {
            throw std::runtime_error("actual process thread count exceeded the runtime budget");
        }
#endif
        for (const auto& arg : context.options().args) {
            if (arg.starts_with("--daemonz-internal-")) throw std::runtime_error("internal flag leaked into application arguments");
        }
        if (_scenario == "no-workers") {
            co_await context.work([] {});
            throw std::runtime_error("disabled worker queue accepted work");
        }
        std::atomic<bool> worked{false};
        const auto reactorThread = std::this_thread::get_id();
        co_await context.work([&] {
            if (std::this_thread::get_id() == reactorThread) throw std::runtime_error("worker ran on the reactor");
            worked.store(true);
        });
        if (!worked.load()) throw std::runtime_error("worker did not run");
        if (std::this_thread::get_id() != reactorThread) throw std::runtime_error("worker continuation did not return to its reactor");
        if (_scenario == "detach") {
#if defined(_WIN32)
            if (::GetConsoleWindow() != nullptr) throw std::runtime_error("detached root still owns a console");
#else
            if (::getsid(0) == ::getpid() || ::isatty(STDIN_FILENO)) {
                throw std::runtime_error("double-fork detachment did not release terminal ownership");
            }
#endif
            _markerFile = context.options().args.at(2);
            requestStop();
            co_return;
        }
        if (_scenario == "failure") throw std::runtime_error("expected startup failure");
        if (_scenario == "worker-failure") {
            co_await context.work([] { throw std::runtime_error("expected worker failure"); });
        }
        if (_scenario == "exit-code") { requestStop(7); co_return; }
        if (_scenario == "body" || _scenario == "body-failure") co_return;
        if (_scenario == "signal" || _scenario == "signal-failure") {
#if defined(_WIN32)
            postSignal(daemonz::SSignal::fromNative(CTRL_C_EVENT));
#else
            ::kill(::getpid(), SIGTERM);
#endif
            co_return;
        }
        if (_scenario == "child-failure") {
            requestStop(_child && context.processIndex() == 1 ? 3 : 0);
            co_return;
        }
        if (_scenario == "ipc") {
            const auto& args = context.options().args;
            if (args.size() != 6 || args[2] != "alpha beta" || args[3] != "quote\"inside" ||
                args[4] != "C:\\path with space\\" || !args[5].empty()) {
                throw std::runtime_error("argument replay/quoting mismatch");
            }
        }
        std::array<std::byte, 4> message{};
        if (_child) {
            if (!context.parentPipe() || !context.childPipes().empty() || context.parentProcessId() == 0) {
                throw std::runtime_error("invalid child context");
            }
            if (_scenario == "graceful" || _scenario == "stubborn") {
                co_await context.parentPipe()->writeAll(bytes("rdy!"));
                co_return;
            }
            if (!(co_await context.parentPipe()->readExact(message)) ||
                std::to_integer<char>(message[0]) != 'p') throw std::runtime_error("child did not receive ping");
            co_await context.parentPipe()->writeAll(bytes("pong"));
        } else {
            if (context.childPipes().size() != 2 || context.parentPipe()) throw std::runtime_error("invalid root context");
            for (const auto& pipe : context.childPipes()) {
                if (_scenario != "graceful" && _scenario != "stubborn") co_await pipe->writeAll(bytes("ping"));
                if (!(co_await pipe->readExact(message))) throw std::runtime_error("child EOF before reply");
                const char expected = _scenario == "graceful" || _scenario == "stubborn" ? 'r' : 'p';
                if (std::to_integer<char>(message[0]) != expected) throw std::runtime_error("invalid child reply");
            }
        }
        requestStop();
    }

    asyncpp::task<void> onRun(const daemonz::CDaemonContext& context) override {
        if (_scenario == "body-failure") throw std::runtime_error("expected application body failure");
        if (_scenario == "body") {
            co_await context.work([this] { postSignal(daemonz::SSignal(daemonz::ESIG_TERMINATE)); });
            co_await waitForStop();
            if (!_signalSeen) throw std::runtime_error("application body prevented signal delivery");
            co_return;
        }
        co_await daemonz::CDaemon::onRun(context);
    }

    asyncpp::task<void> onSignal(daemonz::SSignal signal) override {
        _signalSeen = true;
        if (_scenario == "signal-failure") throw std::runtime_error("expected signal hook failure");
        if (_scenario == "stubborn" && _child) co_return;
        co_await daemonz::CDaemon::onSignal(signal);
    }

    asyncpp::task<void> onStop() override {
        _cleaned = true;
        if ((_scenario == "signal" || (_scenario == "graceful" && _child)) && !_signalSeen) {
            throw std::runtime_error("shutdown bypassed the signal hook");
        }
        if (!_markerFile.empty()) {
            const auto path = std::filesystem::path(std::u8string(_markerFile.begin(), _markerFile.end()));
            std::ofstream marker(path);
            marker << "done " << _processId << '\n';
            if (!marker) throw std::runtime_error("cannot write detached completion marker");
        }
        co_return;
    }
};

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    for (int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc < 2) return 90;
    const std::string scenario(argv[1]);
    daemonz::SDaemonOptions options;
    for (int i = 0; i < argc; ++i) options.args.emplace_back(argv[i]);
    options.max_fork = scenario == "ipc" || scenario == "graceful" || scenario == "stubborn" || scenario == "child-failure" ? 2 : 0;
    options.max_reactors = 2;
    options.max_workers = 2;
    if (scenario == "no-workers") options.max_workers = 0;
    if (scenario == "invalid") options.max_threads = 3;
    options.detach = scenario == "detach";
    if (scenario == "stubborn") options.shutdown_timeout = std::chrono::milliseconds{100};
    TestDaemon daemon(scenario);
    const int result = daemonz::IDaemon::launch(daemon, options);
    if (scenario == "invalid") return result == 1 && !daemon.cleaned() ? 1 : 92;
    if (!daemon.cleaned() && scenario != "detach") return 91;
    return result;
}
