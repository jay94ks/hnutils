#include <daemonizer/signal.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace daemonz {

/* Construct a portable signal value. */
SSignal::SSignal(ESignal type, int nativeCode, std::uint64_t sender) noexcept
    : _type(type), _nativeCode(nativeCode), _sender(sender) {}

/* Preserve native metadata while translating the event category. */
SSignal SSignal::fromNative(int nativeCode, std::uint64_t sender) noexcept {
    ESignal type = ESIG_UNKNOWN;
#if defined(_WIN32)
    switch (nativeCode) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT: type = ESIG_INTERRUPT; break;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: type = ESIG_CLOSE; break;
    }
#else
    switch (nativeCode) {
    case SIGINT: type = ESIG_INTERRUPT; break;
    case SIGTERM: type = ESIG_TERMINATE; break;
    case SIGHUP: type = ESIG_RELOAD; break;
    case SIGCHLD: type = ESIG_CHILD; break;
    }
#endif
    return SSignal(type, nativeCode, sender);
}

} // namespace daemonz
