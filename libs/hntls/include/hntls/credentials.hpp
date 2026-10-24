#ifndef __INCLUDE_HNTLS_CREDENTIALS_HPP__
#define __INCLUDE_HNTLS_CREDENTIALS_HPP__

#include <memory>
#include <vector>

namespace certpp {
namespace crypto {
class IPrivateKey;
}
namespace x509 {
class CCert;
}
} // namespace certpp

namespace hntls {
/** Immutable-by-contract credentials; callers must not mutate aliases after context creation. */
struct SIdentity {
    std::vector<std::shared_ptr<const certpp::x509::CCert>> certificateChain;
    std::shared_ptr<const certpp::crypto::IPrivateKey> privateKey;
};
/** Explicit trust anchors; system trust is never silently enabled by a protocol library. */
struct STrustOptions {
    std::vector<std::shared_ptr<const certpp::x509::CCert>> anchors;
    bool useSystemTrust = false;
};
} // namespace hntls
#endif
