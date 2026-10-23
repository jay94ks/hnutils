#ifndef __INCLUDE_DAEMONIZER_OPTIONS_HPP__
#define __INCLUDE_DAEMONIZER_OPTIONS_HPP__

#include <daemonizer/common.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace daemonz {

/** Caller-owned launch configuration, copied and validated before use. */
struct DAEMONIZER_API SDaemonOptions {
    /** Original argv, including the executable's own filename at index zero. */
    std::vector<std::string> args;

    /** Maximum managed child count; zero runs only the current instance. */
    int max_fork = 0;

    /** Runtime thread budget per process; -1 uses reactors plus workers. */
    int max_threads = -1;

    /** Number of dedicated asynchronous I/O reactor threads per process. */
    int max_reactors = 1;

    /** Number of dedicated worker threads per process; zero disables work(). */
    int max_workers = 1;

    /** Detach the root process from its terminal before launching children. */
    bool detach = false;

    /** Redirect standard streams to the null device when detaching on Unix. */
    bool redirect_stdio = true;

    /** Optional working directory applied to every daemon instance. */
    std::string working_directory;

    /** Grace period before forcibly terminating this launch's own children. */
    std::chrono::milliseconds shutdown_timeout{5000};

    /**
     * Validate counts and return a normalized configuration snapshot.
     * @return A copy with max_threads resolved to a non-negative budget.
     * @throw std::invalid_argument for inconsistent or invalid options.
     */
    SDaemonOptions validated() const;
};

} // namespace daemonz

#endif
