#ifndef __INCLUDE_HNQUIC_OPTIONS_HPP__
#define __INCLUDE_HNQUIC_OPTIONS_HPP__

#include <hnquic/common.hpp>
#include <chrono>

namespace hnquic {
/** Local resource limits; peer transport parameters cannot relax hard bounds. */
struct SQuicOptions {
    ERole role = EROLE_CLIENT;
    std::uint32_t version = 1;
    std::uint64_t maximumConnectionData = 16 * 1024 * 1024;
    std::uint64_t maximumStreamData = 256 * 1024;
    std::uint64_t maximumBidirectionalStreams = 100;
    std::uint64_t maximumUnidirectionalStreams = 16;
    std::size_t maximumBufferedBytes = 32 * 1024 * 1024;
    std::size_t maximumReassemblyRanges = 4096;
    std::size_t maximumAckRanges = 256;
    std::size_t maximumPendingDatagrams = 1024;
    std::size_t maximumOperationsPerTurn = 64;
    /** Maximum admitted server connections, including incomplete handshakes. */
    std::size_t maximumConnections = 128;
    std::chrono::milliseconds idleTimeout{30000};
};
} // namespace hnquic
#endif
