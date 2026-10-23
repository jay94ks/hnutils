#ifndef __INCLUDE_DAEMONIZER_SIGNAL_HPP__
#define __INCLUDE_DAEMONIZER_SIGNAL_HPP__

#include <daemonizer/common.hpp>

#include <cstdint>

namespace daemonz {

/** Portable signal categories; nativeCode() retains the platform event. */
enum ESignal {
    ESIG_UNKNOWN,    // --> Unmapped native signal.
    ESIG_INTERRUPT,  // --> Interactive interrupt.
    ESIG_TERMINATE,  // --> Cooperative termination request.
    ESIG_RELOAD,     // --> Configuration reload request.
    ESIG_CHILD,      // --> A managed child changed state.
    ESIG_CLOSE,      // --> Console or session closing.
};

/** Read-only value wrapping a kernel signal or an explicit injected event. */
struct DAEMONIZER_API SSignal {
private:
    ESignal _type;
    int _nativeCode;
    std::uint64_t _sender;

public:
    /** Construct an event with its portable category and native metadata. */
    explicit SSignal(ESignal type, int nativeCode = 0, std::uint64_t sender = 0) noexcept;

    /** Translate a POSIX signal number or Windows console-control code. */
    static SSignal fromNative(int nativeCode, std::uint64_t sender = 0) noexcept;

    /** Return the portable signal category. */
    inline ESignal type() const noexcept { return _type; }

    /** Return the original operating-system event number. */
    inline int nativeCode() const noexcept { return _nativeCode; }

    /** Return the sending process ID when the kernel supplied it, or zero. */
    inline std::uint64_t sender() const noexcept { return _sender; }
};

} // namespace daemonz

#endif
