#ifndef __TEST_HNTLS_IDENTITY_HPP__
#define __TEST_HNTLS_IDENTITY_HPP__
#include <hntls.hpp>
#include <certpp/crypto/asym.hpp>
#include <certpp/x509/cert.hpp>
#include <certpp/x509/exts/san.hpp>
#include <doctest/doctest.h>
inline hntls::SIdentity testIdentity(const char *name = "localhost",
                                     certpp::crypto::EAsymmetrics algorithm = certpp::crypto::EASYM_ED25519) {
    using namespace certpp;
    using namespace certpp::crypto;
    using namespace certpp::x509;
    SKeyPair pair;
    DOCTEST_REQUIRE(IAsymmetric::builtIn(algorithm)->generateKeyPair(algorithm == EASYM_RSA ? 2048 : 256,
                                                                     pair) == certpp::ERET_OK);
    CCertBuilder b;
    DOCTEST_REQUIRE(CDistinguishedName::tryParse(b.issuer, CString("CN=hntls test")));
    b.subject = b.issuer;
    std::uint8_t serial = 1;
    b.serialNumber = COctet(&serial, 1);
    b.notBefore = SDateTime(2020, 1, 1, 0, 0, 0, 0, true);
    b.notAfter = SDateTime(2040, 1, 1, 0, 0, 0, 0, true);
    b.subjectKey = pair.publicKey;
    b.issuerKeyPair = pair;
    CSanExtensionBuilder san;
    san.addName(CGeneralName(EGNAME_DNS, CString(name)));
    b.extensions.add(san.build());
    auto c = std::make_shared<CCert>();
    DOCTEST_REQUIRE(b.build(*c) == certpp::ERET_OK);
    return {{std::move(c)}, pair.privateKey};
}
#endif
