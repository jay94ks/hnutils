#ifndef __EXAMPLE_HNTLS_IDENTITY_HPP__
#define __EXAMPLE_HNTLS_IDENTITY_HPP__
#include <hntls.hpp>
#include <certpp/crypto/asym.hpp>
#include <certpp/x509/cert.hpp>
#include <certpp/x509/exts/san.hpp>
#include <stdexcept>
/** Ephemeral self-signed test identity; never a production trust policy. */
inline hntls::SIdentity exampleIdentity() {
    using namespace certpp;
    using namespace certpp::crypto;
    using namespace certpp::x509;
    SKeyPair pair;
    if (IAsymmetric::builtIn(EASYM_ED25519)->generateKeyPair(256, pair) != certpp::ERET_OK) {
        throw std::runtime_error("Ed25519 generation failed");
    }
    CCertBuilder b;
    if (!CDistinguishedName::tryParse(b.issuer, CString("CN=localhost"))) {
        throw std::runtime_error("invalid test name");
    }
    b.subject = b.issuer;
    std::uint8_t serial = 1;
    b.serialNumber = COctet(&serial, 1);
    b.notBefore = SDateTime(2020, 1, 1, 0, 0, 0, 0, true);
    b.notAfter = SDateTime(2040, 1, 1, 0, 0, 0, 0, true);
    b.subjectKey = pair.publicKey;
    b.issuerKeyPair = pair;
    CSanExtensionBuilder san;
    san.addName(CGeneralName(EGNAME_DNS, CString("localhost")));
    b.extensions.add(san.build());
    auto c = std::make_shared<CCert>();
    if (b.build(*c) != certpp::ERET_OK) {
        throw std::runtime_error("certificate build failed");
    }
    return {{c}, pair.privateKey};
}
/** Public DER certificate encoded for a test harness; no private key is printed. */
inline std::string exampleHex(const certpp::COctet &data) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (auto b : data.toSpan()) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}
#endif
