#ifndef __INCLUDE_HNTLS_OPTIONS_HPP__
#define __INCLUDE_HNTLS_OPTIONS_HPP__

#include <hntls/common.hpp>
#include <cstddef>

namespace hntls {
/** Bounded TLS 1.3 policy; AES-128-GCM/SHA-256 and X25519, without version fallback. */
struct STlsOptions {
    ERole role = EROLE_CLIENT;
    std::string serverName;
    std::vector<std::string> alpn;
    std::size_t maximumHandshakeBytes = 1024 * 1024;
    std::size_t maximumCertificateBytes = 4 * 1024 * 1024;
    std::size_t maximumBufferedBytes = 8 * 1024 * 1024;
    std::size_t maximumRecordBytes = 16384;
    bool requirePeerAuthentication = true;
    bool enableEarlyData = false;
};
} // namespace hntls
#endif
