#include <daemonizer/pipe_server.hpp>

#include "runtime.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
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
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace daemonz {
namespace detail {

#if defined(_WIN32)
using NativeHandle = HANDLE;
const NativeHandle INVALID_ENDPOINT = INVALID_HANDLE_VALUE;
#else
using NativeHandle = int;
constexpr NativeHandle INVALID_ENDPOINT = -1;
#endif

/** Native storage never exposed by either public pipe interface. */
class NativeEndpoint {
private:
    NativeHandle _handle;
    std::atomic<bool> _closed{false};
    std::mutex _mutex;
    bool _readBusy = false;
    bool _writeBusy = false;

    void releaseIfIdle() noexcept {
        if (_closed.load() && !_readBusy && !_writeBusy && _handle != INVALID_ENDPOINT) {
#if defined(_WIN32)
            ::CloseHandle(_handle);
#else
            ::close(_handle);
#endif
            _handle = INVALID_ENDPOINT;
        }
    }

public:
    explicit NativeEndpoint(NativeHandle handle) : _handle(handle) {}
    ~NativeEndpoint() {
        if (_handle != INVALID_ENDPOINT) {
#if defined(_WIN32)
            ::CloseHandle(_handle);
#else
            ::close(_handle);
#endif
        }
    }
    NativeHandle handle() const noexcept { return _handle; }
    bool isOpen() const noexcept { return !_closed.load(); }
    bool begin(bool write) noexcept {
        std::lock_guard lock(_mutex);
        auto& busy = write ? _writeBusy : _readBusy;
        if (_closed.load() || busy) return false;
        busy = true;
        return true;
    }
    void end(bool write) noexcept {
        std::lock_guard lock(_mutex);
        (write ? _writeBusy : _readBusy) = false;
        releaseIfIdle();
    }
    void close() noexcept {
        std::lock_guard lock(_mutex);
        if (!_closed.exchange(true)) {
#if defined(_WIN32)
            ::CancelIoEx(_handle, nullptr);
#else
            ::shutdown(_handle, SHUT_RDWR);
#endif
        }
        releaseIfIdle();
    }
};

namespace {

/** Own the native operation record until cancellation has actually completed. */
class PipeOperation final : public Operation {
private:
    NativeEndpointPtr _endpoint;
    bool _write;
    std::byte* _buffer;
    std::size_t _size;
    asyncpp::promise<std::size_t> _result;
#if defined(_WIN32)
    OVERLAPPED _overlapped{};
    bool _submitted = false;
#endif

    bool finish(std::size_t bytes, std::error_code error = {}) noexcept {
        _endpoint->end(_write);
        if (error) _result.try_reject<std::system_error>(error, "asynchronous pipe I/O");
        else _result.try_fulfill(bytes);
        return true;
    }

public:
    PipeOperation(NativeEndpointPtr endpoint, bool write, std::byte* buffer,
                  std::size_t size, asyncpp::promise<std::size_t> result)
        : _endpoint(std::move(endpoint)), _write(write), _buffer(buffer), _size(size),
          _result(std::move(result)) {
#if defined(_WIN32)
        _overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (_overlapped.hEvent == nullptr) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateEventW");
        }
#endif
    }
    ~PipeOperation() override {
#if defined(_WIN32)
        ::CloseHandle(_overlapped.hEvent);
#endif
    }
    void cancel() noexcept override { _endpoint->close(); }
    bool step() noexcept override {
#if defined(_WIN32)
        DWORD bytes = 0;
        if (!_submitted) {
            if (!_endpoint->isOpen()) {
                return finish(0, std::make_error_code(std::errc::operation_canceled));
            }
            const auto size = static_cast<DWORD>((std::min)(
                _size, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            _submitted = true;
            const BOOL success = _write
                ? ::WriteFile(_endpoint->handle(), _buffer, size, &bytes, &_overlapped)
                : ::ReadFile(_endpoint->handle(), _buffer, size, &bytes, &_overlapped);
            if (success) return finish(bytes);
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING) {
                if (!_write && (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)) return finish(0);
                return finish(0, {static_cast<int>(error), std::system_category()});
            }
        }
        if (!_endpoint->isOpen()) ::CancelIoEx(_endpoint->handle(), &_overlapped);
        if (::GetOverlappedResult(_endpoint->handle(), &_overlapped, &bytes, FALSE)) return finish(bytes);
        const DWORD error = ::GetLastError();
        if (error == ERROR_IO_INCOMPLETE) return false;
        if (!_write && (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)) return finish(0);
        return finish(0, {static_cast<int>(error), std::system_category()});
#else
        if (!_endpoint->isOpen()) {
            return finish(0, std::make_error_code(std::errc::operation_canceled));
        }
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        if (_write) flags |= MSG_NOSIGNAL;
#endif
        const auto size = (std::min)(_size, static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
        const auto bytes = _write
            ? ::send(_endpoint->handle(), _buffer, size, flags)
            : ::recv(_endpoint->handle(), _buffer, size, flags);
        if (bytes >= 0) return finish(static_cast<std::size_t>(bytes));
        const int error = errno;
        if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) return false;
        return finish(0, {error, std::generic_category()});
#endif
    }
};

/* Copy endpoint ownership and buffer descriptors into the coroutine frame. */
asyncpp::task<std::size_t> transfer(std::shared_ptr<Runtime> runtime, NativeEndpointPtr endpoint,
                                  bool write, std::byte* buffer, std::size_t size) {
    if (!endpoint->isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled), "pipe is closed");
    }
    if (size == 0) co_return 0;
    if (!endpoint->begin(write)) {
        if (!endpoint->isOpen()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled), "pipe is closed");
        }
        throw std::logic_error("a pipe operation is already pending in this direction");
    }
    asyncpp::promise<std::size_t> result;
    try {
        runtime->submit(std::make_shared<PipeOperation>(endpoint, write, buffer, size, result));
    } catch (...) {
        endpoint->end(write);
        throw;
    }
    co_return co_await result;
}

/** Public endpoint implementation; only the private runtime sees native state. */
class Pipe final : public IPipe {
private:
    std::shared_ptr<Runtime> _runtime;
    NativeEndpointPtr _endpoint;

public:
    Pipe(std::shared_ptr<Runtime> runtime, NativeEndpointPtr endpoint)
        : _runtime(std::move(runtime)), _endpoint(std::move(endpoint)) {}
    ~Pipe() override { close(); }
    bool isOpen() const noexcept override { return _endpoint->isOpen(); }
    void close() noexcept override { _endpoint->close(); }
    asyncpp::task<std::size_t> read(SByteSpan buffer) override {
        return transfer(_runtime, _endpoint, false, buffer.data(), buffer.size());
    }
    asyncpp::task<std::size_t> write(SReadOnlyByteSpan buffer) override {
        return transfer(_runtime, _endpoint, true, const_cast<std::byte*>(buffer.data()), buffer.size());
    }
};

/** Common ownership manager; concrete server types choose the native backend. */
class PipeServer : public IPipeServer {
private:
    std::shared_ptr<Runtime> _runtime;
    bool _ownsRuntime;
    bool _closed = false;
    mutable std::mutex _mutex;
    std::vector<IPipePtr> _pipes;

public:
    PipeServer(std::shared_ptr<Runtime> runtime, bool ownsRuntime)
        : _runtime(std::move(runtime)), _ownsRuntime(ownsRuntime) {}
    ~PipeServer() override {
        close();
        if (_ownsRuntime) _runtime->stop();
    }
    IPipePtr adopt(NativeEndpointPtr endpoint) {
        std::lock_guard lock(_mutex);
        if (_closed) throw std::logic_error("pipe server is closed");
        auto pipe = std::make_shared<Pipe>(_runtime, std::move(endpoint));
        _pipes.push_back(pipe);
        return pipe;
    }
    SPipePair createPair() override {
        auto pair = createNativePair();
        return {adopt(std::move(pair.parent)), adopt(std::move(pair.child))};
    }
    std::size_t pipeCount() const noexcept override {
        std::lock_guard lock(_mutex);
        return static_cast<std::size_t>(std::count_if(_pipes.begin(), _pipes.end(),
            [](const auto& pipe) { return pipe->isOpen(); }));
    }
    void close() noexcept override {
        std::lock_guard lock(_mutex);
        _closed = true;
        for (const auto& pipe : _pipes) pipe->close();
    }
};

#if defined(_WIN32)
/** Windows named-pipe server implementation. */
class WindowsPipeServer final : public PipeServer {
public:
    using PipeServer::PipeServer;
};
#else
/** Unix-domain socketpair server implementation. */
class UnixPipeServer final : public PipeServer {
public:
    using PipeServer::PipeServer;
};
#endif

} // namespace

/* Create connected native endpoints without any background thread. */
NativePair createNativePair() {
#if defined(_WIN32)
    static std::atomic<unsigned long long> sequence{0};
    const auto name = L"\\\\.\\pipe\\daemonz-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
    auto server = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateNamedPipeW");
    }
    auto parent = std::make_shared<NativeEndpoint>(server);
    auto client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateFileW(pipe)");
    }
    auto child = std::make_shared<NativeEndpoint>(client);
    OVERLAPPED connected{};
    if (!::ConnectNamedPipe(server, &connected) && ::GetLastError() != ERROR_PIPE_CONNECTED) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "ConnectNamedPipe");
    }
    ULONG clientProcess = 0;
    if (!::GetNamedPipeClientProcessId(server, &clientProcess) || clientProcess != ::GetCurrentProcessId()) {
        throw std::runtime_error("unexpected process connected to the private daemon pipe");
    }
    return {std::move(parent), std::move(child)};
#else
    int handles[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, handles) != 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    auto parent = std::make_shared<NativeEndpoint>(handles[0]);
    auto child = std::make_shared<NativeEndpoint>(handles[1]);
    for (const int handle : handles) {
        const int status = ::fcntl(handle, F_GETFL, 0);
        if (status < 0 || ::fcntl(handle, F_SETFL, status | O_NONBLOCK) < 0 ||
            ::fcntl(handle, F_SETFD, FD_CLOEXEC) < 0) {
            throw std::system_error(errno, std::generic_category(), "fcntl(pipe)");
        }
#if defined(SO_NOSIGPIPE)
        const int enabled = 1;
        if (::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, std::generic_category(), "SO_NOSIGPIPE");
        }
#endif
    }
    return {std::move(parent), std::move(child)};
#endif
}

/* Construct the current platform's private server implementation. */
IPipeServerPtr makePipeServer(std::shared_ptr<Runtime> runtime, bool ownsRuntime) {
#if defined(_WIN32)
    return std::make_shared<WindowsPipeServer>(std::move(runtime), ownsRuntime);
#else
    return std::make_shared<UnixPipeServer>(std::move(runtime), ownsRuntime);
#endif
}

/* Adopt a process-local endpoint into its owning server. */
IPipePtr adoptPipe(const IPipeServerPtr& server, NativeEndpointPtr endpoint) {
    return std::static_pointer_cast<PipeServer>(server)->adopt(std::move(endpoint));
}

#if defined(_WIN32)
/* Read a native handle exclusively for the launcher's inheritance allowlist. */
std::uintptr_t endpointHandle(const NativeEndpointPtr& endpoint) {
    return reinterpret_cast<std::uintptr_t>(endpoint->handle());
}

/* Validate and adopt the explicitly inherited named-pipe client handle. */
NativeEndpointPtr adoptEndpoint(std::uintptr_t handle) {
    auto native = reinterpret_cast<HANDLE>(handle);
    DWORD flags = 0;
    if (::GetFileType(native) != FILE_TYPE_PIPE || !::GetNamedPipeInfo(native, &flags, nullptr, nullptr, nullptr)) {
        throw std::invalid_argument("internal child pipe handle is invalid");
    }
    ::SetHandleInformation(native, HANDLE_FLAG_INHERIT, 0);
    return std::make_shared<NativeEndpoint>(native);
}
#endif

} // namespace detail

/* Destroy the abstract pipe server. */
IPipeServer::~IPipeServer() = default;

/* Create and start a standalone server with a bounded reactor runtime. */
IPipeServerPtr IPipeServer::create(std::size_t reactors) {
    auto runtime = std::make_shared<detail::Runtime>(reactors, 0);
    auto server = detail::makePipeServer(runtime, true);
    runtime->start();
    return server;
}

} // namespace daemonz
