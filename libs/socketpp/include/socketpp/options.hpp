#ifndef __INCLUDE_SOCKETPP_OPTIONS_HPP__
#define __INCLUDE_SOCKETPP_OPTIONS_HPP__

#include <socketpp/common.hpp>

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace sockpp {
namespace opts {

namespace detail {

#if defined(_WIN32)
/** Emit an instantiation-time warning for the unsupported SO_REUSEPORT option. */
template<typename TSocket>
[[deprecated("sockpp::opts::SReusePort is not supported on Windows and is a no-op")]]
inline void reusePortUnavailable(TSocket&) noexcept {}
#else
/** Emit an instantiation-time warning for the unsupported SO_EXCLUSIVEADDRUSE option. */
template<typename TSocket>
[[deprecated("sockpp::opts::SExclusiveAddressUse is only supported on Windows and is a no-op")]]
inline void exclusiveAddressUseUnavailable(TSocket&) noexcept {}
#endif

} // namespace detail

/**
 * Set or inspect the TCP no-delay option.
 */
struct SOCKETPP_API SNoDelay {
    /** Whether TCP coalescing is disabled. */
    bool how = true;

    /** Apply the option to a synchronous or asynchronous socket. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
        const int value = how ? 1 : 0;
        socket.setOption(level(), option(), &value, sizeof(value));
    }

    /** Read the option from a synchronous or asynchronous socket. */
    template<typename TSocket>
    static inline SNoDelay read(const TSocket& socket) {
        int value = 0;
        std::size_t size = sizeof(value);
        socket.getOption(level(), option(), &value, size);
        return {value != 0};
    }

private:
    /** Return the native TCP option level. */
    static int level() noexcept;

    /** Return the native TCP_NODELAY option name. */
    static int option() noexcept;
};

/**
 * Set or inspect address reuse for local binds.
 */
struct SOCKETPP_API SReuseAddress {
    /** Whether a local address may be reused. */
    bool how = true;

    /** Apply the option to a synchronous or asynchronous socket. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
        const int value = how ? 1 : 0;
        socket.setOption(level(), option(), &value, sizeof(value));
    }

    /** Read the option from a synchronous or asynchronous socket. */
    template<typename TSocket>
    static inline SReuseAddress read(const TSocket& socket) {
        int value = 0;
        std::size_t size = sizeof(value);
        socket.getOption(level(), option(), &value, size);
        return {value != 0};
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_REUSEADDR option name. */
    static int option() noexcept;
};

/**
 * Set or inspect transport keep-alive behavior.
 */
struct SOCKETPP_API SKeepAlive {
    /** Whether transport keep-alive probes are enabled. */
    bool how = true;

    /** Apply the option to a synchronous or asynchronous socket. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
        const int value = how ? 1 : 0;
        socket.setOption(level(), option(), &value, sizeof(value));
    }

    /** Read the option from a synchronous or asynchronous socket. */
    template<typename TSocket>
    static inline SKeepAlive read(const TSocket& socket) {
        int value = 0;
        std::size_t size = sizeof(value);
        socket.getOption(level(), option(), &value, size);
        return {value != 0};
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_KEEPALIVE option name. */
    static int option() noexcept;
};

/**
 * Set or inspect datagram broadcast permission.
 */
struct SOCKETPP_API SBroadcast {
    /** Whether datagram broadcast transmission is allowed. */
    bool how = true;

    /** Apply the option to a synchronous or asynchronous socket. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
        const int value = how ? 1 : 0;
        socket.setOption(level(), option(), &value, sizeof(value));
    }

    /** Read the option from a synchronous or asynchronous socket. */
    template<typename TSocket>
    static inline SBroadcast read(const TSocket& socket) {
        int value = 0;
        std::size_t size = sizeof(value);
        socket.getOption(level(), option(), &value, size);
        return {value != 0};
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_BROADCAST option name. */
    static int option() noexcept;
};

/**
 * Set or inspect the kernel send-buffer size.
 */
struct SOCKETPP_API SSendBuffer {
    /** Requested kernel send-buffer size in bytes. */
    std::size_t size = 0;

    /** Apply the option to a synchronous or asynchronous socket. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
        if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            throw std::length_error("send buffer size exceeds the native integer range");
        }
        const int value = static_cast<int>(size);
        socket.setOption(level(), option(), &value, sizeof(value));
    }

    /** Read the option from a synchronous or asynchronous socket. */
    template<typename TSocket>
    static inline SSendBuffer read(const TSocket& socket) {
        int value = 0;
        std::size_t nativeSize = sizeof(value);
        socket.getOption(level(), option(), &value, nativeSize);
        return {value > 0 ? static_cast<std::size_t>(value) : 0};
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_SNDBUF option name. */
    static int option() noexcept;
};

/**
 * Set or inspect the kernel receive-buffer size.
 */
struct SOCKETPP_API SReceiveBuffer {
    /** Requested kernel receive-buffer size in bytes. */
    std::size_t size = 0;

    /** Apply the option to a synchronous or asynchronous socket. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
        if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            throw std::length_error("receive buffer size exceeds the native integer range");
        }
        const int value = static_cast<int>(size);
        socket.setOption(level(), option(), &value, sizeof(value));
    }

    /** Read the option from a synchronous or asynchronous socket. */
    template<typename TSocket>
    static inline SReceiveBuffer read(const TSocket& socket) {
        int value = 0;
        std::size_t nativeSize = sizeof(value);
        socket.getOption(level(), option(), &value, nativeSize);
        return {value > 0 ? static_cast<std::size_t>(value) : 0};
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_RCVBUF option name. */
    static int option() noexcept;
};

/**
 * Set or inspect port reuse on platforms that provide SO_REUSEPORT.
 *
 * On Windows, apply() is a no-op and emits a compiler warning when the
 * function template is instantiated.
 */
struct SOCKETPP_API SReusePort {
    /** Whether a local port may be reused. */
    bool how = true;

    /** Apply the option where SO_REUSEPORT is available. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
#if defined(_WIN32)
        detail::reusePortUnavailable(socket);
#else
        const int value = how ? 1 : 0;
        socket.setOption(level(), option(), &value, sizeof(value));
#endif
    }

    /** Read the option where SO_REUSEPORT is available. */
    template<typename TSocket>
    static inline SReusePort read(const TSocket& socket) {
#if defined(_WIN32)
        detail::reusePortUnavailable(socket);
        return {};
#else
        int value = 0;
        std::size_t size = sizeof(value);
        socket.getOption(level(), option(), &value, size);
        return {value != 0};
#endif
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_REUSEPORT option name. */
    static int option() noexcept;
};

/**
 * Set or inspect exclusive local-address use on Windows.
 *
 * On non-Windows platforms, apply() is a no-op and emits a compiler warning
 * when the function template is instantiated.
 */
struct SOCKETPP_API SExclusiveAddressUse {
    /** Whether the local address is reserved exclusively for this socket. */
    bool how = true;

    /** Apply the option where SO_EXCLUSIVEADDRUSE is available. */
    template<typename TSocket>
    inline void apply(TSocket& socket) const {
#if defined(_WIN32)
        const int value = how ? 1 : 0;
        socket.setOption(level(), option(), &value, sizeof(value));
#else
        detail::exclusiveAddressUseUnavailable(socket);
#endif
    }

    /** Read the option where SO_EXCLUSIVEADDRUSE is available. */
    template<typename TSocket>
    static inline SExclusiveAddressUse read(const TSocket& socket) {
#if defined(_WIN32)
        int value = 0;
        std::size_t size = sizeof(value);
        socket.getOption(level(), option(), &value, size);
        return {value != 0};
#else
        detail::exclusiveAddressUseUnavailable(socket);
        return {};
#endif
    }

private:
    /** Return the native socket option level. */
    static int level() noexcept;

    /** Return the native SO_EXCLUSIVEADDRUSE option name. */
    static int option() noexcept;
};

} // namespace opts
} // namespace sockpp

#endif
