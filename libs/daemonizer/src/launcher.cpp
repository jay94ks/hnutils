#include <daemonizer/daemon.hpp>

#include "runtime.hpp"

#include <asyncpp/sync_wait.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace daemonz {
namespace detail {

namespace {

constexpr const char* CHILD_FLAG = "--daemonz-internal-child-v1";
constexpr const char* ROOT_FLAG = "--daemonz-internal-root-v1";
std::mutex launchMutex;

/* Construct a C++20 filesystem path from the UTF-8 public configuration. */
std::filesystem::path utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/* Keep process-wide working-directory changes scoped to this launch. */
class WorkingDirectory {
private:
    std::filesystem::path _previous;
    bool _changed = false;
public:
    explicit WorkingDirectory(const std::string& path) {
        if (!path.empty()) {
            _previous = std::filesystem::current_path();
            std::filesystem::current_path(utf8Path(path));
            _changed = true;
        }
    }
    ~WorkingDirectory() {
        if (_changed) {
            std::error_code ignored;
            std::filesystem::current_path(_previous, ignored);
        }
    }
};

#if defined(_WIN32)

std::atomic<unsigned long> consoleEvents{0};
std::atomic<bool> signalBridgeActive{false};
std::atomic<unsigned int> consoleCallbacks{0};

/* The OS callback only records bits; virtual hooks run on a normal reactor. */
BOOL WINAPI consoleHandler(DWORD event) {
    if (event > CTRL_SHUTDOWN_EVENT || !signalBridgeActive.load()) return FALSE;
    consoleCallbacks.fetch_add(1);
    consoleEvents.fetch_or(1UL << event);
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        const auto deadline = ::GetTickCount64() + 4000;
        while (signalBridgeActive.load() && ::GetTickCount64() < deadline) ::Sleep(10);
    }
    consoleCallbacks.fetch_sub(1);
    return TRUE;
}

/** Scoped console-control registration with no application code in the handler. */
class SignalBridge {
private:
    bool _registered = false;
public:
    SignalBridge() {
        consoleEvents.store(0);
        signalBridgeActive.store(true);
        _registered = ::SetConsoleCtrlHandler(consoleHandler, TRUE) != FALSE;
        if (!_registered && ::GetLastError() != ERROR_INVALID_HANDLE) {
            signalBridgeActive.store(false);
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "SetConsoleCtrlHandler");
        }
    }
    ~SignalBridge() {
        signalBridgeActive.store(false);
        if (_registered) ::SetConsoleCtrlHandler(consoleHandler, FALSE);
        while (consoleCallbacks.load() != 0) ::Sleep(1);
    }
    std::vector<SSignal> poll() {
        std::vector<SSignal> result;
        const auto events = consoleEvents.exchange(0);
        for (int i = 0; i <= CTRL_SHUTDOWN_EVENT; ++i) {
            if ((events & (1UL << i)) != 0) result.push_back(SSignal::fromNative(i));
        }
        return result;
    }
};

/* Interpret public argument strings as UTF-8 rather than the active code page. */
std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("argument is too long");
    }
    const auto size = static_cast<int>(text.size());
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (count == 0) throw std::invalid_argument("arguments must be valid UTF-8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count);
    return result;
}

/* Follow the Windows CRT quote/backslash rules for every original argument. */
std::wstring quote(std::wstring_view value) {
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') {
            result.append(slashes * 2 + 1, L'\\');
        } else {
            result.append(slashes, L'\\');
        }
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

/* Own only process/event handles created by this particular launch. */
struct Child {
    HANDLE process = nullptr;
    HANDLE stop = nullptr;
    std::uint64_t pid = 0;
    bool finished = false;
    int code = 0;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept
        : process(std::exchange(other.process, nullptr)), stop(std::exchange(other.stop, nullptr)),
          pid(other.pid), finished(other.finished), code(other.code) {}
    ~Child() {
        if (process) ::CloseHandle(process);
        if (stop) ::CloseHandle(stop);
    }
};

/* Spawn argv[0] with explicit internal flags and a strict inherited-handle list. */
Child spawn(const SDaemonOptions& options, const std::vector<std::string>& internal,
            std::vector<HANDLE> handles, bool detached) {
    if (options.args.empty() || options.args.front().empty()) {
        throw std::invalid_argument("Windows process emulation requires args[0] to name this executable");
    }
    auto executable = std::filesystem::absolute(utf8Path(options.args.front())).wstring();
    std::wstring command;
    for (const auto& arg : options.args) {
        if (!command.empty()) command.push_back(L' ');
        command += quote(widen(arg));
    }
    for (const auto& arg : internal) {
        command.push_back(L' ');
        command += quote(widen(arg));
    }
    if (command.size() >= 32767) throw std::length_error("Windows command line exceeds 32767 characters");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    std::vector<std::byte> attributes;
    if (!handles.empty()) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "InitializeProcThreadAttributeList");
        }
        if (!::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         handles.data(), handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const auto error = ::GetLastError();
            ::DeleteProcThreadAttributeList(startup.lpAttributeList);
            throw std::system_error(static_cast<int>(error), std::system_category(), "UpdateProcThreadAttribute");
        }
    }
    for (HANDLE handle : handles) {
        if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            const auto error = ::GetLastError();
            for (HANDLE reset : handles) ::SetHandleInformation(reset, HANDLE_FLAG_INHERIT, 0);
            if (startup.lpAttributeList) ::DeleteProcThreadAttributeList(startup.lpAttributeList);
            throw std::system_error(static_cast<int>(error), std::system_category(), "SetHandleInformation");
        }
    }
    PROCESS_INFORMATION process{};
    const DWORD flags = (detached ? DETACHED_PROCESS : CREATE_NO_WINDOW) | CREATE_NEW_PROCESS_GROUP |
        (handles.empty() ? 0 : EXTENDED_STARTUPINFO_PRESENT);
    if (handles.empty()) startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    const BOOL success = ::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        !handles.empty(), flags, nullptr, nullptr, &startup.StartupInfo, &process);
    const auto error = ::GetLastError();
    for (HANDLE handle : handles) ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    if (startup.lpAttributeList) ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!success) throw std::system_error(static_cast<int>(error), std::system_category(), "CreateProcessW(argv[0])");
    ::CloseHandle(process.hThread);
    Child child;
    child.process = process.hProcess;
    child.pid = process.dwProcessId;
    return child;
}

/* Reject malformed internal numeric fields instead of guessing their values. */
std::uint64_t parseUnsigned(const std::string& text) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("malformed internal daemon process flag");
    }
    return result;
}

#else

/** Block managed signals before fork/thread creation, then synchronously drain them. */
class SignalBridge {
private:
    sigset_t _signals{};
    sigset_t _previous{};
public:
    SignalBridge() {
        ::sigemptyset(&_signals);
        for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGCHLD}) ::sigaddset(&_signals, signal);
        const int error = ::pthread_sigmask(SIG_BLOCK, &_signals, &_previous);
        if (error != 0) throw std::system_error(error, std::generic_category(), "pthread_sigmask");
    }
    ~SignalBridge() { ::pthread_sigmask(SIG_SETMASK, &_previous, nullptr); }
    std::vector<SSignal> poll() {
        std::vector<SSignal> result;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        timespec timeout{};
        for (;;) {
            siginfo_t info{};
            const int signal = ::sigtimedwait(&_signals, &info, &timeout);
            if (signal >= 0) result.push_back(SSignal::fromNative(signal, static_cast<std::uint64_t>(info.si_pid)));
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN) break;
            else throw std::system_error(errno, std::generic_category(), "sigtimedwait");
        }
#else
        for (;;) {
            sigset_t pending{};
            if (::sigpending(&pending) != 0) throw std::system_error(errno, std::generic_category(), "sigpending");
            bool available = false;
            for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGCHLD}) {
                available = available || ::sigismember(&pending, signal) == 1;
            }
            if (!available) break;
            int signal = 0;
            const int error = ::sigwait(&_signals, &signal);
            if (error != 0) throw std::system_error(error, std::generic_category(), "sigwait");
            result.push_back(SSignal::fromNative(signal));
        }
#endif
        return result;
    }
};

/** One owned child process, reaped only by its creating parent. */
struct Child {
    pid_t pid = -1;
    bool finished = false;
    int code = 0;
};

/* Decode the status without hiding abnormal child termination. */
int childStatus(int status) noexcept {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* Detach with a bootstrap double fork; managed child slots remain separate. */
bool detachUnix(bool redirectStdio) {
    int statusPipe[2];
    if (::pipe(statusPipe) != 0) throw std::system_error(errno, std::generic_category(), "pipe(detach)");
    const pid_t child = ::fork();
    if (child < 0) {
        const int error = errno;
        ::close(statusPipe[0]); ::close(statusPipe[1]);
        throw std::system_error(error, std::generic_category(), "fork(detach)");
    }
    if (child > 0) {
        ::close(statusPipe[1]);
        unsigned char status = 1;
        ssize_t count;
        do { count = ::read(statusPipe[0], &status, 1); } while (count < 0 && errno == EINTR);
        ::close(statusPipe[0]);
        int ignored = 0;
        while (::waitpid(child, &ignored, 0) < 0 && errno == EINTR) {}
        if (count != 1 || status != 0) throw std::runtime_error("detached daemon bootstrap failed");
        return true;
    }
    ::close(statusPipe[0]);
    if (::setsid() < 0) { ::close(statusPipe[1]); ::_exit(1); }
    const pid_t grandchild = ::fork();
    if (grandchild < 0) { ::close(statusPipe[1]); ::_exit(1); }
    if (grandchild > 0) { ::close(statusPipe[1]); ::_exit(0); }
    ::umask(027);
    if (redirectStdio) {
        const int nullDevice = ::open("/dev/null", O_RDWR);
        if (nullDevice < 0) { ::close(statusPipe[1]); ::_exit(1); }
        for (int target = 0; target < 3; ++target) {
            if (::dup2(nullDevice, target) < 0) { ::close(statusPipe[1]); ::_exit(1); }
        }
        if (nullDevice > 2) ::close(nullDevice);
    }
    const unsigned char status = 0;
    const auto ignored = ::write(statusPipe[1], &status, 1);
    (void)ignored;
    ::close(statusPipe[1]);
    return false;
}

#endif

/* Start the application's task on a reactor rather than the launch thread. */
asyncpp::task<int> runOnReactor(IDaemon& daemon, CDaemonContext context,
                              std::shared_ptr<Runtime> runtime) {
    co_await runtime->schedule();
    co_return co_await daemon.run(context);
}

} // namespace

/** Process creation, supervision, and context construction stay private. */
class Launcher {
private:
    IDaemon& _daemon;
    SDaemonOptions _options;
    std::vector<Child> _children;
    std::size_t _index = 0;
    std::uint64_t _parentId = 0;
    NativeEndpointPtr _parentPipe;
    std::vector<NativeEndpointPtr> _childPipes;
#if defined(_WIN32)
    HANDLE _stopEvent = nullptr;
#endif

    bool collect(Child& child) {
        if (child.finished) return true;
#if defined(_WIN32)
        const DWORD result = ::WaitForSingleObject(child.process, 0);
        if (result == WAIT_TIMEOUT) return false;
        if (result != WAIT_OBJECT_0) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "WaitForSingleObject(child)");
        DWORD code = 0;
        if (!::GetExitCodeProcess(child.process, &code)) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "GetExitCodeProcess");
        child.code = static_cast<int>(code);
#else
        int status = 0;
        const auto result = ::waitpid(child.pid, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return false;
        if (result < 0) throw std::system_error(errno, std::generic_category(), "waitpid");
        child.code = childStatus(status);
#endif
        child.finished = true;
        return true;
    }

    void stopChildren() noexcept {
        for (auto& child : _children) {
            if (child.finished) continue;
#if defined(_WIN32)
            ::SetEvent(child.stop);
#else
            ::kill(child.pid, SIGTERM);
#endif
        }
    }

    int joinChildren() {
        stopChildren();
        const auto deadline = std::chrono::steady_clock::now() + _options.shutdown_timeout;
        int exitCode = 0;
        for (auto& child : _children) {
            while (!collect(child)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::cerr << "daemonizer: child " << child.pid << " exceeded shutdown_timeout\n";
#if defined(_WIN32)
                    ::TerminateProcess(child.process, 1);
                    ::WaitForSingleObject(child.process, INFINITE);
#else
                    ::kill(child.pid, SIGKILL);
                    int status = 0;
                    while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
#endif
                    child.finished = true;
                    child.code = 1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            if (exitCode == 0 && child.code != 0) exitCode = child.code;
        }
        return exitCode;
    }

    int runInstance(SignalBridge& signals) {
        WorkingDirectory directory(_options.working_directory);
        auto runtime = std::make_shared<Runtime>(static_cast<std::size_t>(_options.max_reactors),
                                                static_cast<std::size_t>(_options.max_workers));
        auto server = makePipeServer(runtime, false);
        auto impl = std::make_shared<CDaemonContext::Impl>();
        impl->options = _options;
        impl->index = _index;
        impl->parentId = _parentId;
#if defined(_WIN32)
        impl->processId = ::GetCurrentProcessId();
#else
        impl->processId = static_cast<std::uint64_t>(::getpid());
#endif
        impl->server = server;
        impl->runtime = runtime;
        if (_parentPipe) impl->parent = adoptPipe(server, std::move(_parentPipe));
        for (auto& pipe : _childPipes) impl->children.push_back(adoptPipe(server, std::move(pipe)));
        runtime->start(true);
        struct RuntimeGuard {
            IPipeServerPtr server;
            std::shared_ptr<Runtime> runtime;
            ~RuntimeGuard() { server->close(); runtime->stop(); }
        } guard{server, runtime};
        int result = 1;
        try {
            auto completed = asyncpp::as_promise(runOnReactor(_daemon, CDaemonContext(impl), runtime));
            while (completed.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
                runtime->poll();
                if (completed.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) break;
                for (const auto& signal : signals.poll()) {
                    _daemon.postSignal(signal);
                    if (signal.type() == ESIG_TERMINATE || signal.type() == ESIG_INTERRUPT || signal.type() == ESIG_CLOSE) stopChildren();
                }
#if defined(_WIN32)
                if (_stopEvent && ::WaitForSingleObject(_stopEvent, 0) == WAIT_OBJECT_0) {
                    ::ResetEvent(_stopEvent);
                    _daemon.postSignal(SSignal(ESIG_TERMINATE, 0, _parentId));
                }
#endif
                bool allFinished = !_children.empty();
                for (auto& child : _children) {
#if defined(_WIN32)
                    const bool wasFinished = child.finished;
#endif
                    allFinished = collect(child) && allFinished;
#if defined(_WIN32)
                    if (child.finished && !wasFinished) _daemon.postSignal(SSignal(ESIG_CHILD, 0, child.pid));
#endif
                }
                if (allFinished && !_daemon.stopRequested()) _daemon.postSignal(SSignal(ESIG_TERMINATE));
                completed.wait_for(std::chrono::milliseconds{5});
            }
            result = completed.get();
        } catch (...) {
            server->close();
            runtime->stop();
            throw;
        }
        const int childResult = joinChildren();
        server->close();
        runtime->stop();
        return result != 0 ? result : childResult;
    }

public:
    Launcher(IDaemon& daemon, SDaemonOptions options) : _daemon(daemon), _options(std::move(options)) {}
    ~Launcher() {
        try { joinChildren(); } catch (...) {}
#if defined(_WIN32)
        if (_stopEvent) ::CloseHandle(_stopEvent);
#endif
    }

    int execute() {
        SignalBridge signals;
        bool backgroundRoot = false;
#if defined(_WIN32)
        auto& args = _options.args;
        auto marker = std::find(args.begin(), args.end(), CHILD_FLAG);
        if (marker != args.end()) {
            const auto offset = static_cast<std::size_t>(marker - args.begin());
            if (offset == 0 || args.size() - offset != 5) throw std::invalid_argument("malformed internal child arguments");
            _index = static_cast<std::size_t>(parseUnsigned(args[offset + 1]));
            const auto pipe = parseUnsigned(args[offset + 2]);
            const auto stop = parseUnsigned(args[offset + 3]);
            _parentId = parseUnsigned(args[offset + 4]);
            if (_index == 0 || _index > static_cast<std::size_t>(_options.max_fork) ||
                pipe > (std::numeric_limits<std::uintptr_t>::max)() || stop > (std::numeric_limits<std::uintptr_t>::max)()) {
                throw std::invalid_argument("internal child slot/handles are invalid");
            }
            _parentPipe = adoptEndpoint(static_cast<std::uintptr_t>(pipe));
            _stopEvent = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(stop));
            if (::WaitForSingleObject(_stopEvent, 0) == WAIT_FAILED) {
                _stopEvent = nullptr;
                throw std::invalid_argument("internal child stop event is invalid");
            }
            ::SetHandleInformation(_stopEvent, HANDLE_FLAG_INHERIT, 0);
            args.resize(offset);
            _options.detach = false;
            return runInstance(signals);
        }
        marker = std::find(args.begin(), args.end(), ROOT_FLAG);
        if (marker != args.end()) {
            if (marker + 1 != args.end() || marker == args.begin()) throw std::invalid_argument("malformed internal root arguments");
            args.pop_back();
            _options.detach = false;
            backgroundRoot = true;
        }
        for (const auto& arg : args) {
            if (arg.starts_with("--daemonz-internal-")) throw std::invalid_argument("reserved internal daemon argument");
        }
        if (_options.detach) {
            auto root = spawn(_options, {ROOT_FLAG}, {}, true);
            return 0;
        }
#else
        for (const auto& arg : _options.args) {
            if (arg.starts_with("--daemonz-internal-")) throw std::invalid_argument("reserved internal daemon argument");
        }
#if defined(__linux__)
        if (_options.detach || _options.max_fork > 0) {
            std::size_t threads = 0;
            for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) { (void)entry; ++threads; }
            if (threads != 1) throw std::logic_error("Unix process creation must precede application thread creation");
        }
#endif
        if (_options.detach) {
            if (detachUnix(_options.redirect_stdio)) return 0;
            _options.detach = false;
            backgroundRoot = true;
        }
#endif
        _children.reserve(static_cast<std::size_t>(_options.max_fork));
        _childPipes.reserve(static_cast<std::size_t>(_options.max_fork));
        for (int i = 0; i < _options.max_fork; ++i) {
            auto pair = createNativePair();
#if defined(_WIN32)
            const HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stop) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateEventW(child stop)");
            try {
                auto spawned = spawn(_options, {CHILD_FLAG, std::to_string(i + 1),
                    std::to_string(endpointHandle(pair.child)),
                    std::to_string(reinterpret_cast<std::uintptr_t>(stop)),
                    std::to_string(::GetCurrentProcessId())},
                    {reinterpret_cast<HANDLE>(endpointHandle(pair.child)), stop}, false);
                spawned.stop = stop;
                _children.push_back(std::move(spawned));
            } catch (...) {
                ::CloseHandle(stop);
                throw;
            }
#else
            const pid_t parent = ::getpid();
            const pid_t child = ::fork();
            if (child < 0) throw std::system_error(errno, std::generic_category(), "fork(child)");
            if (child == 0) {
                _children.clear();
                _childPipes.clear();
                pair.parent.reset();
                _parentPipe = std::move(pair.child);
                _index = static_cast<std::size_t>(i + 1);
                _parentId = static_cast<std::uint64_t>(parent);
                int result = 1;
                try { result = runInstance(signals); }
                catch (const std::exception& error) { std::cerr << "daemonizer child: " << error.what() << '\n'; }
                ::_exit(result);
            }
            _children.push_back(Child{child});
#endif
            pair.child.reset();
            _childPipes.push_back(std::move(pair.parent));
        }
        const int result = runInstance(signals);
#if !defined(_WIN32)
        if (backgroundRoot) ::_exit(result);
#else
        (void)backgroundRoot;
#endif
        return result;
    }
};

} // namespace detail

/* Validate the complete configuration before creating processes or threads. */
int IDaemon::launch(IDaemon& daemon, const SDaemonOptions& options) {
    try {
        std::unique_lock lock(detail::launchMutex, std::try_to_lock);
        if (!lock.owns_lock()) throw std::logic_error("another daemon launch is active in this process");
        auto validated = options.validated();
        detail::Launcher launcher(daemon, std::move(validated));
        return launcher.execute();
    } catch (const std::exception& error) {
        std::cerr << "daemonizer: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "daemonizer: unknown launch failure\n";
        return 1;
    }
}

} // namespace daemonz
