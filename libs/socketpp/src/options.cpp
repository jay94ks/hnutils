#include <socketpp/options.hpp>

#include "platform.hpp"

#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif

namespace sockpp {
namespace opts {

/* Return the native TCP option level. */
int SNoDelay::level() noexcept {
    return IPPROTO_TCP;
}

/* Return the native TCP_NODELAY option name. */
int SNoDelay::option() noexcept {
    return TCP_NODELAY;
}

/* Return the native socket option level. */
int SReuseAddress::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_REUSEADDR option name. */
int SReuseAddress::option() noexcept {
    return SO_REUSEADDR;
}

/* Return the native socket option level. */
int SKeepAlive::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_KEEPALIVE option name. */
int SKeepAlive::option() noexcept {
    return SO_KEEPALIVE;
}

/* Return the native socket option level. */
int SBroadcast::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_BROADCAST option name. */
int SBroadcast::option() noexcept {
    return SO_BROADCAST;
}

/* Return the native socket option level. */
int SSendBuffer::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_SNDBUF option name. */
int SSendBuffer::option() noexcept {
    return SO_SNDBUF;
}

/* Return the native socket option level. */
int SReceiveBuffer::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_RCVBUF option name. */
int SReceiveBuffer::option() noexcept {
    return SO_RCVBUF;
}

#if !defined(_WIN32)
/* Return the native socket option level. */
int SReusePort::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_REUSEPORT option name. */
int SReusePort::option() noexcept {
    return SO_REUSEPORT;
}
#endif

#if defined(_WIN32)
/* Return the native socket option level. */
int SExclusiveAddressUse::level() noexcept {
    return SOL_SOCKET;
}

/* Return the native SO_EXCLUSIVEADDRUSE option name. */
int SExclusiveAddressUse::option() noexcept {
    return SO_EXCLUSIVEADDRUSE;
}
#endif

} // namespace opts
} // namespace sockpp
