#include <daemonizer/options.hpp>

#include <limits>
#include <stdexcept>

namespace daemonz {

/* Validate and normalize a launch configuration. */
SDaemonOptions SDaemonOptions::validated() const {
    if (max_fork < 0 || max_reactors < 1 || max_workers < 0 || max_threads < -1) {
        throw std::invalid_argument("invalid process/thread limits: require max_fork >= 0, "
                                    "max_reactors >= 1, max_workers >= 0, max_threads >= -1");
    }
    if (max_workers > (std::numeric_limits<int>::max)() - max_reactors) {
        throw std::invalid_argument("reactor and worker thread counts overflow max_threads");
    }
    const int required = max_reactors + max_workers;
    auto result = *this;
    if (result.max_threads == -1) {
        result.max_threads = required;
    }
    if (result.max_threads < required) {
        throw std::invalid_argument("max_threads is smaller than max_reactors + max_workers");
    }
    if (shutdown_timeout.count() < 0) {
        throw std::invalid_argument("shutdown_timeout must not be negative");
    }
    for (const auto& arg : args) {
        if (arg.find('\0') != std::string::npos) {
            throw std::invalid_argument("arguments must not contain embedded NUL bytes");
        }
    }
    if (working_directory.find('\0') != std::string::npos) {
        throw std::invalid_argument("working_directory contains an embedded NUL byte");
    }
    return result;
}

} // namespace daemonz
