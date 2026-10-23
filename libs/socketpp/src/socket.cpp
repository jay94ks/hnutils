#include <socketpp/socket.hpp>

#include "platform.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sockpp {

struct CSocket::Impl {
    NativeSocket socket = INVALID_NATIVE_SOCKET;
    int family = AF_UNSPEC;
    int type = 0;
    int protocol = 0;
};

/* Return the native socket error category. */
const std::error_category& SocketErrorCategory() noexcept {
    return std::system_category();
}

/* Return the most recent native socket error. */
std::error_code LastSocketError() noexcept {
    return {lastSocketErrorValue(), SocketErrorCategory()};
}

/* Test whether an error reports that a non-blocking socket would block. */
bool IsSocketWouldBlock(const std::error_code& error) noexcept {
#if defined(_WIN32)
    return error.value() == WSAEWOULDBLOCK;
#else
    return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
#endif
}

/* Test whether an error reports an in-progress non-blocking connection. */
bool IsSocketInProgress(const std::error_code& error) noexcept {
#if defined(_WIN32)
    return error.value() == WSAEINPROGRESS || error.value() == WSAEALREADY ||
           error.value() == WSAEWOULDBLOCK;
#else
    return error.value() == EINPROGRESS || error.value() == EALREADY ||
           IsSocketWouldBlock(error);
#endif
}

/* Construct a closed socket. */
CSocket::CSocket() : _impl(std::make_unique<Impl>()) {}

/* Move a socket. */
CSocket::CSocket(CSocket&& other) noexcept = default;

/* Destroy the socket and close its native handle. */
CSocket::~CSocket() {
    (void)close();
}

/* Move a socket. */
CSocket& CSocket::operator=(CSocket&& other) noexcept {
    if (this != &other) {
        (void)close();
        _impl = std::move(other._impl);
    }
    return *this;
}

/* Create a socket. */
CSocket CSocket::open(EAddressFamily family, ESocketType type, ESocketProtocol protocol) {
    ensureSocketRuntime();
    const int nativeFamilyValue = toNative(family);
    const int nativeTypeValue = toNative(type);
    const int nativeProtocolValue = toNative(protocol);

#if defined(_WIN32)
    const NativeSocket socket = ::WSASocketW(
        nativeFamilyValue,
        nativeTypeValue,
        nativeProtocolValue,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);
#else
    const NativeSocket socket = ::socket(nativeFamilyValue, nativeTypeValue, nativeProtocolValue);
#endif
    if (socket == INVALID_NATIVE_SOCKET) {
        throwLastSocketError("socket");
    }
    return adopt(toHandle(socket), nativeFamilyValue, nativeTypeValue, nativeProtocolValue);
}

/* Test whether the socket owns a native handle. */
bool CSocket::isOpen() const noexcept {
    return _impl != nullptr && _impl->socket != INVALID_NATIVE_SOCKET;
}

/* Close the socket. */
std::error_code CSocket::close() noexcept {
    if (!isOpen()) {
        return {};
    }

    const NativeSocket socket = std::exchange(_impl->socket, INVALID_NATIVE_SOCKET);
    const int error = closeNativeSocket(socket);
    return error == 0 ? std::error_code{} : std::error_code{error, SocketErrorCategory()};
}

/* Set blocking or non-blocking mode. */
void CSocket::setBlocking(bool blocking) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }

#if defined(_WIN32)
    u_long mode = blocking ? 0UL : 1UL;
    if (::ioctlsocket(_impl->socket, FIONBIO, &mode) != 0) {
        throwLastSocketError("ioctlsocket(FIONBIO)");
    }
#else
    const int current = ::fcntl(_impl->socket, F_GETFL, 0);
    if (current < 0) {
        throwLastSocketError("fcntl(F_GETFL)");
    }
    const int desired = blocking ? (current & ~O_NONBLOCK) : (current | O_NONBLOCK);
    if (::fcntl(_impl->socket, F_SETFL, desired) != 0) {
        throwLastSocketError("fcntl(F_SETFL)");
    }
#endif
}

/* Bind the socket to a local address. */
void CSocket::bind(const CSocketAddress& address) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    if (!address.isValid()) {
        throw std::invalid_argument("cannot bind an empty address");
    }

    const auto* nativeAddress = static_cast<const sockaddr*>(address.nativeData());
    if (::bind(_impl->socket, nativeAddress, static_cast<socklen_t>(address.nativeSize())) != 0) {
        throwLastSocketError("bind");
    }
}

/* Mark a stream socket as a listener. */
void CSocket::listen(int backlog) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    if (::listen(_impl->socket, backlog) != 0) {
        throwLastSocketError("listen");
    }
}

/* Accept one incoming connection. */
CSocket CSocket::accept(CSocketAddress* peer) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }

    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    const NativeSocket accepted = ::accept(
        _impl->socket,
        reinterpret_cast<sockaddr*>(&address),
        &size);
    if (accepted == INVALID_NATIVE_SOCKET) {
        throwLastSocketError("accept");
    }
    if (peer != nullptr) {
        peer->assign(&address, size);
    }
    return adopt(toHandle(accepted), address.ss_family, _impl->type, _impl->protocol);
}

/* Connect the socket to a remote address. */
void CSocket::connect(const CSocketAddress& address) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    if (!address.isValid()) {
        throw std::invalid_argument("cannot connect to an empty address");
    }

    const auto* nativeAddress = static_cast<const sockaddr*>(address.nativeData());
    if (::connect(_impl->socket, nativeAddress, static_cast<socklen_t>(address.nativeSize())) != 0) {
        throwLastSocketError("connect");
    }
}

/* Disable sends, receives, or both directions. */
void CSocket::shutdown(ESocketShutdown direction) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }

    int nativeDirection = 0;
    switch (direction) {
    case ESHUT_READ:
        nativeDirection =
#if defined(_WIN32)
            SD_RECEIVE;
#else
            SHUT_RD;
#endif
        break;
    case ESHUT_WRITE:
        nativeDirection =
#if defined(_WIN32)
            SD_SEND;
#else
            SHUT_WR;
#endif
        break;
    case ESHUT_BOTH:
        nativeDirection =
#if defined(_WIN32)
            SD_BOTH;
#else
            SHUT_RDWR;
#endif
        break;
    default:
        throw std::invalid_argument("invalid socket shutdown direction");
    }

    if (::shutdown(_impl->socket, nativeDirection) != 0) {
        throwLastSocketError("shutdown");
    }
}

/* Receive bytes from a connected socket. */
std::size_t CSocket::receive(const SByteSpan& buffer, int flags) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    const int result = ::recv(
        _impl->socket,
        reinterpret_cast<char*>(buffer.data()),
        nativeBufferSize(buffer.size()),
        flags);
    if (result < 0) {
        throwLastSocketError("recv");
    }
    return static_cast<std::size_t>(result);
}

/* Send bytes on a connected socket. */
std::size_t CSocket::send(const SReadOnlyByteSpan& buffer, int flags) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    const int result = ::send(
        _impl->socket,
        reinterpret_cast<const char*>(buffer.data()),
        nativeBufferSize(buffer.size()),
        flags);
    if (result < 0) {
        throwLastSocketError("send");
    }
    return static_cast<std::size_t>(result);
}

/* Receive one datagram and its source address. */
std::size_t CSocket::receiveFrom(const SByteSpan& buffer, CSocketAddress& source, int flags) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }

    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    const int result = ::recvfrom(
        _impl->socket,
        reinterpret_cast<char*>(buffer.data()),
        nativeBufferSize(buffer.size()),
        flags,
        reinterpret_cast<sockaddr*>(&address),
        &size);
    if (result < 0) {
        throwLastSocketError("recvfrom");
    }
    source.assign(&address, size);
    return static_cast<std::size_t>(result);
}

/* Send one datagram to a destination address. */
std::size_t CSocket::sendTo(
    const SReadOnlyByteSpan& buffer,
    const CSocketAddress& destination,
    int flags) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    if (!destination.isValid()) {
        throw std::invalid_argument("cannot send to an empty address");
    }

    const int result = ::sendto(
        _impl->socket,
        reinterpret_cast<const char*>(buffer.data()),
        nativeBufferSize(buffer.size()),
        flags,
        static_cast<const sockaddr*>(destination.nativeData()),
        static_cast<socklen_t>(destination.nativeSize()));
    if (result < 0) {
        throwLastSocketError("sendto");
    }
    return static_cast<std::size_t>(result);
}

/* Return the local socket address. */
CSocketAddress CSocket::localAddress() const {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }

    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getsockname(_impl->socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        throwLastSocketError("getsockname");
    }
    return CSocketAddress(&address, size);
}

/* Return the connected peer address. */
CSocketAddress CSocket::peerAddress() const {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }

    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getpeername(_impl->socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        throwLastSocketError("getpeername");
    }
    return CSocketAddress(&address, size);
}

/* Set a native socket option. */
void CSocket::setOption(int level, int option, const void* value, std::size_t size) {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("socket option value is too large");
    }
    if (::setsockopt(
            _impl->socket,
            level,
            option,
            static_cast<const char*>(value),
            static_cast<int>(size)) != 0) {
        throwLastSocketError("setsockopt");
    }
}

/* Read a native socket option. */
void CSocket::getOption(int level, int option, void* value, std::size_t& size) const {
    if (!isOpen()) {
        throw std::logic_error("socket is closed");
    }
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("socket option buffer is too large");
    }

    socklen_t nativeSize = static_cast<socklen_t>(size);
    if (::getsockopt(
            _impl->socket,
            level,
            option,
            static_cast<char*>(value),
            &nativeSize) != 0) {
        throwLastSocketError("getsockopt");
    }
    size = static_cast<std::size_t>(nativeSize);
}

/* Return the native handle without transferring ownership. */
std::uintptr_t CSocket::nativeHandle() const noexcept {
    return isOpen() ? toHandle(_impl->socket) : toHandle(INVALID_NATIVE_SOCKET);
}

/* Return the native address family value. */
int CSocket::nativeFamily() const noexcept {
    return _impl != nullptr ? _impl->family : AF_UNSPEC;
}

/* Return the native socket type value. */
int CSocket::nativeType() const noexcept {
    return _impl != nullptr ? _impl->type : 0;
}

/* Return the native protocol value. */
int CSocket::nativeProtocol() const noexcept {
    return _impl != nullptr ? _impl->protocol : 0;
}

/* Adopt an existing native socket handle. */
CSocket CSocket::adopt(std::uintptr_t handle, int family, int type, int protocol) {
    CSocket result;
    result._impl->socket = fromHandle(handle);
    result._impl->family = family;
    result._impl->type = type;
    result._impl->protocol = protocol;
    return result;
}

} // namespace sockpp
