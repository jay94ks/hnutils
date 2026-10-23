#ifndef __SRC_PLATFORM_HPP__
#define __SRC_PLATFORM_HPP__

#include <socketpp/common.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sockpp {

#if defined(_WIN32)
using NativeSocket = SOCKET;
inline constexpr NativeSocket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket INVALID_NATIVE_SOCKET = -1;
#endif

/** Initialize process-wide socket support where the platform requires it. */
inline void ensureSocketRuntime() {
#if defined(_WIN32)
    struct SocketRuntime {
        SocketRuntime() {
            WSADATA data{};
            const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
            if (result != 0) {
                throw std::system_error(result, std::system_category(), "WSAStartup");
            }
        }

        ~SocketRuntime() {
            ::WSACleanup();
        }
    };
    static SocketRuntime runtime;
    (void)runtime;
#endif
}

/** Convert the public address family to its native value. */
inline int toNative(EAddressFamily family) {
    switch (family) {
    case EAF_UNSPEC:
        return AF_UNSPEC;
    case EAF_IPV4:
        return AF_INET;
    case EAF_IPV6:
        return AF_INET6;
    case EAF_UNIX:
        return AF_UNIX;
    case EAF_MAX:
        break;
    }
    throw std::invalid_argument("invalid socket address family");
}

/** Convert a native address family to its public value. */
inline EAddressFamily fromNativeFamily(int family) noexcept {
    switch (family) {
    case AF_INET:
        return EAF_IPV4;
    case AF_INET6:
        return EAF_IPV6;
    case AF_UNIX:
        return EAF_UNIX;
    default:
        return EAF_UNSPEC;
    }
}

/** Convert the public socket type to its native value. */
inline int toNative(ESocketType type) {
    switch (type) {
    case ESOCK_STREAM:
        return SOCK_STREAM;
    case ESOCK_DATAGRAM:
        return SOCK_DGRAM;
    }
    throw std::invalid_argument("invalid socket type");
}

/** Convert the public protocol to its native value. */
inline int toNative(ESocketProtocol protocol) {
    switch (protocol) {
    case EPROTO_DEFAULT:
        return 0;
    case EPROTO_TCP:
        return IPPROTO_TCP;
    case EPROTO_UDP:
        return IPPROTO_UDP;
    }
    throw std::invalid_argument("invalid socket protocol");
}

/** Return the current native socket error value. */
inline int lastSocketErrorValue() noexcept {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

/** Throw the current native socket error with operation context. */
[[noreturn]] inline void throwLastSocketError(const char* operation) {
    throw std::system_error(lastSocketErrorValue(), SocketErrorCategory(), operation);
}

/** Close a native socket and return its native error value. */
inline int closeNativeSocket(NativeSocket socket) noexcept {
#if defined(_WIN32)
    if (::closesocket(socket) == 0) {
        return 0;
    }
#else
    if (::close(socket) == 0) {
        return 0;
    }
#endif
    return lastSocketErrorValue();
}

/** Clamp a buffer length to the maximum accepted by one native socket call. */
inline int nativeBufferSize(std::size_t size) noexcept {
    const auto maximum = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    return static_cast<int>((std::min)(size, maximum));
}

/** Convert an opaque public handle value back to the platform socket type. */
inline NativeSocket fromHandle(std::uintptr_t handle) noexcept {
    return static_cast<NativeSocket>(handle);
}

/** Convert a platform socket type to an opaque private handle value. */
inline std::uintptr_t toHandle(NativeSocket socket) noexcept {
    return static_cast<std::uintptr_t>(socket);
}

} // namespace sockpp

#endif
