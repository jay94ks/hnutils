#include <socketpp/async_socket.hpp>

#include "platform.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <mswsock.h>
#endif

namespace sockpp {

struct CSocketAsync::Impl {
    CSocketWatcher* watcher = nullptr;
    CSocket socket;
};

#if defined(_WIN32)
namespace {

struct OverlappedState {
    OVERLAPPED overlapped{};
};

struct BufferOperation : OverlappedState {
    WSABUF buffer{};
};

struct AddressOperation : BufferOperation {
    sockaddr_storage address{};
    int addressSize = sizeof(address);
};

struct ConnectOperation : OverlappedState {
    explicit ConnectOperation(const CSocketAddress& destinationValue)
        : destination(destinationValue) {}

    CSocketAddress destination;
};

struct AcceptOperation : OverlappedState {
    std::shared_ptr<CSocket> accepted;
    std::array<std::byte, (sizeof(sockaddr_storage) + 16) * 2> addresses{};
};

template<typename TFunction>
TFunction loadExtensionFunction(NativeSocket socket, GUID identifier, const char* name) {
    TFunction function = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(
            socket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &identifier,
            sizeof(identifier),
            &function,
            sizeof(function),
            &bytes,
            nullptr,
            nullptr) == SOCKET_ERROR) {
        throwLastSocketError(name);
    }
    return function;
}

void rejectPromise(
    asyncpp::promise<void>& promise,
    const std::error_code& error,
    const char* operation) {
    promise.try_reject(std::make_exception_ptr(std::system_error(error, operation)));
}

template<typename TValue>
void rejectPromise(
    asyncpp::promise<TValue>& promise,
    const std::error_code& error,
    const char* operation) {
    promise.try_reject(std::make_exception_ptr(std::system_error(error, operation)));
}

} // namespace
#endif

/* Transfer a socket into asynchronous ownership. */
CSocketAsync::CSocketAsync(CSocketWatcher& watcher, CSocket&& socket)
    : _impl(std::make_unique<Impl>()) {
    if (!socket.isOpen()) {
        throw std::invalid_argument("cannot attach a closed socket");
    }
    socket.setBlocking(false);
    _impl->watcher = &watcher;
    _impl->socket = std::move(socket);
#if defined(_WIN32)
    watcher.associate(_impl->socket);
#endif
}

/* Move an asynchronous socket. */
CSocketAsync::CSocketAsync(CSocketAsync&& other) noexcept = default;

/* Destroy the asynchronous socket and close its native socket. */
CSocketAsync::~CSocketAsync() = default;

/* Move an asynchronous socket. */
CSocketAsync& CSocketAsync::operator=(CSocketAsync&& other) noexcept = default;

/* Create an asynchronous socket. */
CSocketAsync CSocketAsync::open(
    CSocketWatcher& watcher,
    EAddressFamily family,
    ESocketType type,
    ESocketProtocol protocol) {
    return CSocketAsync(watcher, CSocket::open(family, type, protocol));
}

/* Test whether the asynchronous socket is open. */
bool CSocketAsync::isOpen() const noexcept {
    return _impl != nullptr && _impl->socket.isOpen();
}

/* Close the socket. */
std::error_code CSocketAsync::close() noexcept {
    return _impl != nullptr ? _impl->socket.close() : std::error_code{};
}

/* Bind the socket to a local address. */
void CSocketAsync::bind(const CSocketAddress& address) {
    _impl->socket.bind(address);
}

/* Mark the socket as a listener. */
void CSocketAsync::listen(int backlog) {
    _impl->socket.listen(backlog);
}

/* Disable sends, receives, or both directions. */
void CSocketAsync::shutdown(ESocketShutdown direction) {
    _impl->socket.shutdown(direction);
}

/* Return the local socket address. */
CSocketAddress CSocketAsync::localAddress() const {
    return _impl->socket.localAddress();
}

/* Return the connected peer address. */
CSocketAddress CSocketAsync::peerAddress() const {
    return _impl->socket.peerAddress();
}

/* Set a native socket option without exposing the native handle. */
void CSocketAsync::setOption(int level, int option, const void* value, std::size_t size) {
    _impl->socket.setOption(level, option, value, size);
}

/* Read a native socket option without exposing the native handle. */
void CSocketAsync::getOption(int level, int option, void* value, std::size_t& size) const {
    _impl->socket.getOption(level, option, value, size);
}

/* Connect to a remote address without blocking the calling thread. */
asyncpp::task<void> CSocketAsync::connect(CSocketAddress address) {
    if (!address.isValid()) {
        throw std::invalid_argument("cannot connect to an empty address");
    }

#if defined(_WIN32)
    // --> UDP selects a peer locally; ConnectEx only supports connection-oriented sockets.
    if (_impl->socket.nativeType() == SOCK_DGRAM) {
        _impl->socket.connect(address);
        co_return;
    }
    NativeSocket socket = fromHandle(_impl->socket.nativeHandle());
    GUID identifier = WSAID_CONNECTEX;
    const auto connectEx = loadExtensionFunction<LPFN_CONNECTEX>(
        socket, identifier, "WSAIoctl(ConnectEx)");

    bool needsBind = false;
    try {
        needsBind = _impl->socket.localAddress().port() == 0;
    } catch (const std::system_error& error) {
        if (error.code().value() != WSAEINVAL) {
            throw;
        }
        needsBind = true;
    }
    if (needsBind) {
        _impl->socket.bind(address.family() == EAF_INET6
                               ? CSocketAddress::anyIpv6(0)
                               : CSocketAddress::anyIpv4(0));
    }

    asyncpp::promise<void> completion;
    auto operation = std::make_shared<ConnectOperation>(address);
    _impl->watcher->submit(
        _impl->socket,
        &operation->overlapped,
        [completion, operation, socket](std::size_t, std::error_code error) mutable {
            if (error) {
                rejectPromise(completion, error, "ConnectEx");
                return;
            }
            if (::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0) {
                rejectPromise(completion, LastSocketError(), "setsockopt(SO_UPDATE_CONNECT_CONTEXT)");
                return;
            }
            (void)completion.try_fulfill();
        });

    const BOOL started = connectEx(
        socket,
        static_cast<const sockaddr*>(operation->destination.nativeData()),
        static_cast<int>(operation->destination.nativeSize()),
        nullptr,
        0,
        nullptr,
        &operation->overlapped);
    if (!started) {
        const auto error = LastSocketError();
        if (error.value() != ERROR_IO_PENDING && error.value() != WSA_IO_PENDING) {
            _impl->watcher->cancel(&operation->overlapped);
            throw std::system_error(error, "ConnectEx");
        }
    }
    co_await completion;
#else
    try {
        _impl->socket.connect(address);
        co_return;
    } catch (const std::system_error& error) {
        if (!IsSocketInProgress(error.code())) {
            throw;
        }
    }

    co_await _impl->watcher->wait(_impl->socket, true);
    int socketError = 0;
    std::size_t size = sizeof(socketError);
    _impl->socket.getOption(SOL_SOCKET, SO_ERROR, &socketError, size);
    if (socketError != 0) {
        throw std::system_error(socketError, SocketErrorCategory(), "connect");
    }
#endif
}

/* Accept one incoming connection without blocking the calling thread. */
asyncpp::task<CSocketAsync> CSocketAsync::accept(CSocketAddress* peer) {
#if defined(_WIN32)
    const NativeSocket listener = fromHandle(_impl->socket.nativeHandle());
    GUID identifier = WSAID_ACCEPTEX;
    const auto acceptEx = loadExtensionFunction<LPFN_ACCEPTEX>(
        listener, identifier, "WSAIoctl(AcceptEx)");

    auto accepted = CSocket::open(
        fromNativeFamily(_impl->socket.nativeFamily()),
        _impl->socket.nativeType() == SOCK_DGRAM ? ESOCK_DATAGRAM : ESOCK_STREAM,
        _impl->socket.nativeProtocol() == IPPROTO_UDP
            ? EPROTO_UDP
            : (_impl->socket.nativeProtocol() == IPPROTO_TCP ? EPROTO_TCP : EPROTO_DEFAULT));
    auto operation = std::make_shared<AcceptOperation>();
    operation->accepted = std::make_shared<CSocket>(std::move(accepted));

    asyncpp::promise<CSocket> completion;
    _impl->watcher->submit(
        _impl->socket,
        &operation->overlapped,
        [completion, operation, listener, peer](std::size_t, std::error_code error) mutable {
            if (error) {
                rejectPromise(completion, error, "AcceptEx");
                return;
            }
            const NativeSocket acceptedSocket = fromHandle(operation->accepted->nativeHandle());
            if (::setsockopt(
                    acceptedSocket,
                    SOL_SOCKET,
                    SO_UPDATE_ACCEPT_CONTEXT,
                    reinterpret_cast<const char*>(&listener),
                    sizeof(listener)) != 0) {
                rejectPromise(completion, LastSocketError(), "setsockopt(SO_UPDATE_ACCEPT_CONTEXT)");
                return;
            }
            if (peer != nullptr) {
                try {
                    *peer = operation->accepted->peerAddress();
                } catch (...) {
                    completion.try_reject(std::current_exception());
                    return;
                }
            }
            (void)completion.try_fulfill(std::move(*operation->accepted));
        });

    DWORD bytes = 0;
    const DWORD addressSize = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
    const BOOL started = acceptEx(
        listener,
        fromHandle(operation->accepted->nativeHandle()),
        operation->addresses.data(),
        0,
        addressSize,
        addressSize,
        &bytes,
        &operation->overlapped);
    if (!started) {
        const auto error = LastSocketError();
        if (error.value() != ERROR_IO_PENDING && error.value() != WSA_IO_PENDING) {
            _impl->watcher->cancel(&operation->overlapped);
            throw std::system_error(error, "AcceptEx");
        }
    }

    CSocket result = std::move(co_await completion);
    co_return CSocketAsync(*_impl->watcher, std::move(result));
#else
    while (true) {
        try {
            CSocket accepted = _impl->socket.accept(peer);
            co_return CSocketAsync(*_impl->watcher, std::move(accepted));
        } catch (const std::system_error& error) {
            if (error.code().value() == EINTR) {
                continue;
            }
            if (!IsSocketWouldBlock(error.code())) {
                throw;
            }
        }
        co_await _impl->watcher->wait(_impl->socket, false);
    }
#endif
}

/* Receive bytes without blocking the calling thread. */
asyncpp::task<std::size_t> CSocketAsync::receive(SByteSpan buffer, int flags) {
#if defined(_WIN32)
    asyncpp::promise<std::size_t> completion;
    auto operation = std::make_shared<BufferOperation>();
    operation->buffer.buf = reinterpret_cast<char*>(buffer.data());
    operation->buffer.len = static_cast<ULONG>(nativeBufferSize(buffer.size()));
    _impl->watcher->submit(
        _impl->socket,
        &operation->overlapped,
        [completion, operation](std::size_t bytes, std::error_code error) mutable {
            if (error) {
                rejectPromise(completion, error, "WSARecv");
            } else {
                (void)completion.try_fulfill(bytes);
            }
        });

    DWORD nativeFlags = static_cast<DWORD>(flags);
    DWORD bytes = 0;
    const int result = ::WSARecv(
        fromHandle(_impl->socket.nativeHandle()),
        &operation->buffer,
        1,
        &bytes,
        &nativeFlags,
        &operation->overlapped,
        nullptr);
    if (result == SOCKET_ERROR) {
        const auto error = LastSocketError();
        if (error.value() != WSA_IO_PENDING) {
            _impl->watcher->cancel(&operation->overlapped);
            throw std::system_error(error, "WSARecv");
        }
    }
    co_return co_await completion;
#else
    while (true) {
        try {
            co_return _impl->socket.receive(buffer, flags);
        } catch (const std::system_error& error) {
            if (error.code().value() == EINTR) {
                continue;
            }
            if (!IsSocketWouldBlock(error.code())) {
                throw;
            }
        }
        co_await _impl->watcher->wait(_impl->socket, false);
    }
#endif
}

/* Send bytes without blocking the calling thread. */
asyncpp::task<std::size_t> CSocketAsync::send(SReadOnlyByteSpan buffer, int flags) {
#if defined(_WIN32)
    asyncpp::promise<std::size_t> completion;
    auto operation = std::make_shared<BufferOperation>();
    operation->buffer.buf = reinterpret_cast<char*>(const_cast<std::byte*>(buffer.data()));
    operation->buffer.len = static_cast<ULONG>(nativeBufferSize(buffer.size()));
    _impl->watcher->submit(
        _impl->socket,
        &operation->overlapped,
        [completion, operation](std::size_t bytes, std::error_code error) mutable {
            if (error) {
                rejectPromise(completion, error, "WSASend");
            } else {
                (void)completion.try_fulfill(bytes);
            }
        });

    DWORD bytes = 0;
    const int result = ::WSASend(
        fromHandle(_impl->socket.nativeHandle()),
        &operation->buffer,
        1,
        &bytes,
        static_cast<DWORD>(flags),
        &operation->overlapped,
        nullptr);
    if (result == SOCKET_ERROR) {
        const auto error = LastSocketError();
        if (error.value() != WSA_IO_PENDING) {
            _impl->watcher->cancel(&operation->overlapped);
            throw std::system_error(error, "WSASend");
        }
    }
    co_return co_await completion;
#else
    while (true) {
        try {
            co_return _impl->socket.send(buffer, flags);
        } catch (const std::system_error& error) {
            if (error.code().value() == EINTR) {
                continue;
            }
            if (!IsSocketWouldBlock(error.code())) {
                throw;
            }
        }
        co_await _impl->watcher->wait(_impl->socket, true);
    }
#endif
}

/* Send an entire buffer, retrying partial writes. */
asyncpp::task<void> CSocketAsync::sendAll(SReadOnlyByteSpan buffer, int flags) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const std::size_t written = co_await send(buffer.subspan(offset), flags);
        if (written == 0) {
            throw std::system_error(std::make_error_code(std::errc::broken_pipe), "send returned zero");
        }
        offset += written;
    }
}

/* Receive one datagram without blocking the calling thread. */
asyncpp::task<std::size_t> CSocketAsync::receiveFrom(
    SByteSpan buffer,
    CSocketAddress& source,
    int flags) {
#if defined(_WIN32)
    asyncpp::promise<std::size_t> completion;
    auto operation = std::make_shared<AddressOperation>();
    operation->buffer.buf = reinterpret_cast<char*>(buffer.data());
    operation->buffer.len = static_cast<ULONG>(nativeBufferSize(buffer.size()));
    _impl->watcher->submit(
        _impl->socket,
        &operation->overlapped,
        [completion, operation, &source](std::size_t bytes, std::error_code error) mutable {
            if (error) {
                rejectPromise(completion, error, "WSARecvFrom");
            } else {
                try {
                    source.assign(&operation->address, static_cast<std::size_t>(operation->addressSize));
                    (void)completion.try_fulfill(bytes);
                } catch (...) {
                    completion.try_reject(std::current_exception());
                }
            }
        });

    DWORD nativeFlags = static_cast<DWORD>(flags);
    DWORD bytes = 0;
    const int result = ::WSARecvFrom(
        fromHandle(_impl->socket.nativeHandle()),
        &operation->buffer,
        1,
        &bytes,
        &nativeFlags,
        reinterpret_cast<sockaddr*>(&operation->address),
        &operation->addressSize,
        &operation->overlapped,
        nullptr);
    if (result == SOCKET_ERROR) {
        const auto error = LastSocketError();
        if (error.value() != WSA_IO_PENDING) {
            _impl->watcher->cancel(&operation->overlapped);
            throw std::system_error(error, "WSARecvFrom");
        }
    }
    co_return co_await completion;
#else
    while (true) {
        try {
            co_return _impl->socket.receiveFrom(buffer, source, flags);
        } catch (const std::system_error& error) {
            if (error.code().value() == EINTR) {
                continue;
            }
            if (!IsSocketWouldBlock(error.code())) {
                throw;
            }
        }
        co_await _impl->watcher->wait(_impl->socket, false);
    }
#endif
}

/* Send one datagram without blocking the calling thread. */
asyncpp::task<std::size_t> CSocketAsync::sendTo(
    SReadOnlyByteSpan buffer,
    CSocketAddress destination,
    int flags) {
#if defined(_WIN32)
    if (!destination.isValid()) {
        throw std::invalid_argument("cannot send to an empty address");
    }
    asyncpp::promise<std::size_t> completion;
    auto operation = std::make_shared<AddressOperation>();
    operation->buffer.buf = reinterpret_cast<char*>(const_cast<std::byte*>(buffer.data()));
    operation->buffer.len = static_cast<ULONG>(nativeBufferSize(buffer.size()));
    std::memcpy(&operation->address, destination.nativeData(), destination.nativeSize());
    operation->addressSize = static_cast<int>(destination.nativeSize());
    _impl->watcher->submit(
        _impl->socket,
        &operation->overlapped,
        [completion, operation](std::size_t bytes, std::error_code error) mutable {
            if (error) {
                rejectPromise(completion, error, "WSASendTo");
            } else {
                (void)completion.try_fulfill(bytes);
            }
        });

    DWORD bytes = 0;
    const int result = ::WSASendTo(
        fromHandle(_impl->socket.nativeHandle()),
        &operation->buffer,
        1,
        &bytes,
        static_cast<DWORD>(flags),
        reinterpret_cast<const sockaddr*>(&operation->address),
        operation->addressSize,
        &operation->overlapped,
        nullptr);
    if (result == SOCKET_ERROR) {
        const auto error = LastSocketError();
        if (error.value() != WSA_IO_PENDING) {
            _impl->watcher->cancel(&operation->overlapped);
            throw std::system_error(error, "WSASendTo");
        }
    }
    co_return co_await completion;
#else
    while (true) {
        try {
            co_return _impl->socket.sendTo(buffer, destination, flags);
        } catch (const std::system_error& error) {
            if (error.code().value() == EINTR) {
                continue;
            }
            if (!IsSocketWouldBlock(error.code())) {
                throw;
            }
        }
        co_await _impl->watcher->wait(_impl->socket, true);
    }
#endif
}

} // namespace sockpp
