#ifndef __INCLUDE_SOCKETPP_COMMON_HPP__
#define __INCLUDE_SOCKETPP_COMMON_HPP__

#include <cstdint>
#include <system_error>

#if defined(_WIN32) && defined(SOCKETPP_SHARED)
#if defined(SOCKETPP_BUILDING_LIBRARY)
#define SOCKETPP_API __declspec(dllexport)
#else
#define SOCKETPP_API __declspec(dllimport)
#endif
#else
#define SOCKETPP_API
#endif

namespace sockpp {

/**
 * Address families supported by socketpp.
 */
enum EAddressFamily {
    EAF_UNSPEC = 0,
    EAF_INET,
    EAF_INET6,
    EAF_UNIX,

    // --
    EAF_MAX,
    EAF_LOCAL = EAF_UNIX,
    EAF_IPV4 = EAF_INET,
    EAF_IPV6 = EAF_INET6,
};

/**
 * Socket transport types supported by socketpp.
 */
enum ESocketType {
    ESOCK_STREAM = 1,
    ESOCK_DATAGRAM,
};

/**
 * Well-known socket protocols supported by socketpp.
 */
enum ESocketProtocol {
    EPROTO_DEFAULT = 0,
    EPROTO_TCP,
    EPROTO_UDP,
};

/**
 * Directions accepted by CSocket::shutdown().
 */
enum ESocketShutdown {
    ESHUT_READ = 0,
    ESHUT_WRITE,
    ESHUT_BOTH,
};

/**
 * Native event backend selected by CSocketWatcher.
 */
enum ESocketWatcherBackend {
    EWATCH_IOCP = 0,
    EWATCH_EPOLL,
    EWATCH_KQUEUE,
};

/**
 * Return the native socket error category.
 *
 * @return The error category used by socketpp socket operations.
 */
SOCKETPP_API const std::error_category& SocketErrorCategory() noexcept;

/**
 * Return the most recent native socket error.
 *
 * @return The current thread's native socket error code.
 */
SOCKETPP_API std::error_code LastSocketError() noexcept;

/**
 * Test whether an error reports that a non-blocking socket would block.
 *
 * @param error The socket error to inspect.
 * @return true when the operation should wait for readiness and retry.
 */
SOCKETPP_API bool IsSocketWouldBlock(const std::error_code& error) noexcept;

/**
 * Test whether an error reports an in-progress non-blocking connection.
 *
 * @param error The socket error to inspect.
 * @return true when connection completion should be awaited.
 */
SOCKETPP_API bool IsSocketInProgress(const std::error_code& error) noexcept;

} // namespace sockpp

#endif
