#include <hntls/context.hpp>
#include <hntls/session.hpp>
#include "protocol.hpp"
#include <certpp/crypto/asym.hpp>
#include <certpp/x509/validator.hpp>
#include <certpp/x509/exts/san.hpp>
#include <certpp/x509/exts/eku.hpp>
#include <socketpp/address.hpp>
#include <algorithm>
#include <atomic>
#include <set>

namespace hntls {
namespace {
using namespace detail;
using namespace certpp::crypto;
constexpr std::uint8_t HRR[32] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
                                  0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
                                  0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};
STlsError success() { return {ERET_OK, {}, {}}; }
ERetCode check(const std::shared_ptr<asyncpp::dispatcher> &e, const SOperationOptions &o) {
    if (asyncpp::dispatcher::current() != e.get()) {
        return ERET_WRONG_EXECUTOR;
    }
    if (o.cancellation.stop_requested()) {
        return ERET_CANCELLED;
    }
    if (o.deadline && *o.deadline <= std::chrono::steady_clock::now()) {
        return ERET_TIMEOUT;
    }
    return ERET_OK;
}
std::string lower(std::string s) {
    for (auto &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c += 32;
        }
    }
    return s;
}
bool validDns(const std::string &s) {
    if (s.empty() || s.size() > 253 || s.front() == '.' || s.back() == '.') {
        return false;
    }
    std::size_t label = 0;
    for (auto c : s) {
        if (c == '.') {
            if (!label || label > 63) {
                return false;
            }
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
                return false;
            }
            ++label;
        }
    }
    return label && label <= 63;
}
bool matchesName(const certpp::x509::CCert &cert, const std::string &name) {
    auto san = cert.extension<certpp::x509::CSanExtension>();
    if (!san || name.empty()) {
        return false;
    }
    std::optional<sockpp::CSocketAddress> ip;
    try {
        ip = sockpp::CSocketAddress::parse(name, 0);
    } catch (const std::exception &) {
    }
    for (const auto &n : san->names()) {
        if (ip && n.type() == certpp::x509::EGNAME_IP_ADDRESS) {
            auto raw = n.raw().toSpan();
            std::string text;
            if (raw.size == 4) {
                for (unsigned i = 0; i < 4; ++i) {
                    if (i) {
                        text += '.';
                    }
                    text += std::to_string(raw.data[i]);
                }
            } else if (raw.size == 16) {
                static const char hex[] = "0123456789abcdef";
                for (unsigned i = 0; i < 16; i += 2) {
                    if (i) {
                        text += ':';
                    }
                    for (unsigned j = 0; j < 2; ++j) {
                        text += hex[raw.data[i + j] >> 4];
                        text += hex[raw.data[i + j] & 15];
                    }
                }
            } else {
                continue;
            }
            if (sockpp::CSocketAddress::parse(text, 0).host() == ip->host()) {
                return true;
            }
        }
        if (!ip && n.type() == certpp::x509::EGNAME_DNS) {
            auto host = lower(name);
            std::string pattern = lower(std::string(n.text().toPtr(), n.text().size()));
            if (!validDns(host)) {
                return false;
            }
            if (validDns(pattern) && pattern == host) {
                return true;
            }
            if (pattern.starts_with("*.") && validDns(pattern.substr(2)) &&
                std::count(pattern.begin() + 2, pattern.end(), '.') >= 1) {
                auto dot = host.find('.');
                if (dot != std::string::npos && dot > 0 && !host.starts_with("xn--") &&
                    host.substr(dot) == pattern.substr(1)) {
                    return true;
                }
            }
        }
    }
    return false;
}
unsigned signatureScheme(const certpp::crypto::IPublicKeyPtr &key) {
    if (!key) {
        return 0;
    }
    switch (key->algorithm()) {
    case EASYM_ED25519:
        return 0x0807;
    case EASYM_P256:
        return 0x0403;
    case EASYM_RSA:
        return key->keySize() >= 2048 ? 0x0804 : 0;
    default:
        return 0;
    }
}
SBytes signatureInput(bool server, View transcript) {
    SBytes r(64, 32);
    std::string context = server ? "TLS 1.3, server CertificateVerify" : "TLS 1.3, client CertificateVerify";
    append(r, {reinterpret_cast<const std::uint8_t *>(context.data()), context.size()});
    r.push_back(0);
    append(r, transcript);
    return r;
}
class Session final : public ITlsSession {
  private:
    enum Step {
        CLIENT_HELLO,
        SERVER_HELLO,
        ENCRYPTED_EXTENSIONS,
        SERVER_CERTIFICATE,
        SERVER_VERIFY,
        SERVER_FINISHED,
        CLIENT_CERTIFICATE,
        CLIENT_VERIFY,
        CLIENT_FINISHED,
        COMPLETE
    };
    std::shared_ptr<asyncpp::dispatcher> _executor;
    STlsOptions _options;
    SIdentity _identity;
    STrustOptions _trust;
    EMode _mode;
    std::atomic<bool> _closed{false};
    ESessionState _state = ESTATE_NEW;
    Step _step;
    SPeerInfo _peer;
    STlsError _error = success();
    SBytes _parameters, _sessionId, _wire, _messages;
    EEncryptionLevel _fragmentLevel = ELEVEL_INITIAL;
    std::size_t _handshakeBytes = 0;
    IHasherPtr _transcript;
    IAsymmetricPtr _dh;
    SKeyPair _keyPair;
    Secret _master, _readSecret, _writeSecret, _readApplication, _writeApplication;
    RecordKey _readKey, _writeKey;
    EEncryptionLevel _readLevel = ELEVEL_INITIAL, _writeLevel = ELEVEL_INITIAL;
    std::uint64_t _readGeneration = 0, _writeGeneration = 0;
    bool _started = false, _clientCertificateRequested = false, _certificateReceived = false,
         _writeFinished = false, _peerFinished = false;
    std::set<unsigned> _peerSignatures;

    SBytes transcript() {
        SBytes r(32);
        crypto(_transcript->finish({r.data(), r.size()}));
        return r;
    }
    void absorb(View message) {
        require(message.size() <= _options.maximumHandshakeBytes - _handshakeBytes, "TLS handshake limit",
                ERET_LIMIT, 80);
        _handshakeBytes += message.size();
        crypto(_transcript->push(bytes(message)) == message.size());
    }
    void secret(SHandshakeResult &result, EDirection direction, EEncryptionLevel level, View value,
                std::uint64_t generation = 0) {
        auto copy = std::make_shared<certpp::CSecureOctet>(bytes(value));
        crypto(copy->size() == value.size());
        result.secrets.push_back({level, direction, generation, std::move(copy), 0x1301});
    }
    void install(SHandshakeResult &r, EDirection d, EEncryptionLevel level, View value) {
        if (d == EDIRECTION_READ) {
            _readSecret = Secret(value.data(), value.size());
            crypto(_readSecret.size() == value.size());
            _readLevel = level;
            if (_mode == EMODE_RECORDS) {
                _readKey.install(value);
            }
        } else {
            _writeSecret = Secret(value.data(), value.size());
            crypto(_writeSecret.size() == value.size());
            _writeLevel = level;
            if (_mode == EMODE_RECORDS) {
                _writeKey.install(value);
            }
        }
        secret(r, d, level, value);
    }
    SBytes record(unsigned type, View input) {
        SBytes header{static_cast<std::uint8_t>(type), 3, 3};
        if (_writeKey.ready()) {
            SBytes inner(input.begin(), input.end());
            inner.push_back(static_cast<std::uint8_t>(type));
            header[0] = 23;
            number(header, inner.size() + 16, 2);
            auto protectedBytes = _writeKey.transform(inner, header, true);
            append(header, protectedBytes);
        } else {
            number(header, input.size(), 2);
            append(header, input);
        }
        return header;
    }
    void emit(SHandshakeResult &result, unsigned type, View body, bool addTranscript = true) {
        SBytes message{static_cast<std::uint8_t>(type)};
        number(message, body.size(), 3);
        append(message, body);
        if (addTranscript) {
            absorb(message);
        }
        if (_mode == EMODE_QUIC) {
            result.outgoing.push_back({_writeLevel, std::move(message)});
            return;
        }
        for (std::size_t offset = 0; offset < message.size();) {
            auto length = std::min(_options.maximumRecordBytes, message.size() - offset);
            result.outgoing.push_back({_writeLevel, record(22, View(message).subspan(offset, length))});
            offset += length;
        }
    }
    SBytes keyShare() {
        certpp::COctet raw;
        crypto(_keyPair.publicKey->serialize(raw) == certpp::ERET_OK);
        return {raw.toPtr(), raw.toPtr() + raw.size()};
    }
    void generateKey() {
        _dh = IAsymmetric::builtIn(EASYM_X25519);
        crypto(bool(_dh));
        crypto(_dh->generateKeyPair(256, _keyPair) == certpp::ERET_OK);
    }
    void deriveKeys(SHandshakeResult &r, View peerKey) {
        require(peerKey.size() == 32, "invalid X25519 key share", ERET_INVALID_ARGUMENT, 47);
        IPublicKeyPtr publicKey;
        crypto(_dh->createPublicKey(publicKey, bytes(peerKey)) == certpp::ERET_OK);
        auto context = _dh->createContext();
        crypto(bool(context));
        context->keyPair(_keyPair);
        Secret shared(32);
        crypto(shared.size() == 32);
        auto out = shared.toSpan();
        require(context->deriveSharedSecret(publicKey, out) == certpp::ERET_OK, "invalid peer shared secret",
                ERET_AUTHENTICATION, 47);
        std::uint8_t zero[32]{};
        require(!certpp::CSecure::equals(bytes(view(shared)), {zero, 32}), "zero shared secret",
                ERET_AUTHENTICATION, 47);
        auto early = extract({zero, 32}, {zero, 32});
        auto empty = hash({});
        auto derived = label(view(early), "derived", empty);
        auto handshake = extract(view(derived), view(shared));
        auto th = transcript();
        auto client = label(view(handshake), "c hs traffic", th);
        auto server = label(view(handshake), "s hs traffic", th);
        install(r, EDIRECTION_READ, ELEVEL_HANDSHAKE, view(_options.role == EROLE_CLIENT ? server : client));
        install(r, EDIRECTION_WRITE, ELEVEL_HANDSHAKE, view(_options.role == EROLE_CLIENT ? client : server));
        derived = label(view(handshake), "derived", empty);
        _master = extract(view(derived), {zero, 32});
        _keyPair = {};
        _peer.cipherSuite = 0x1301;
    }
    void applicationKeys() {
        auto th = transcript();
        auto c = label(view(_master), "c ap traffic", th);
        auto s = label(view(_master), "s ap traffic", th);
        _readApplication = std::move(_options.role == EROLE_CLIENT ? s : c);
        _writeApplication = std::move(_options.role == EROLE_CLIENT ? c : s);
        _master.clear();
    }
    SBytes finished(View traffic) {
        auto key = label(traffic, "finished", {});
        auto th = transcript();
        SBytes mac(32);
        crypto(CHmac::compute(EHASH_SHA256, bytes(view(key)), bytes(th), {mac.data(), mac.size()}) ==
               certpp::ERET_OK);
        return mac;
    }
    void sendCertificate(SHandshakeResult &r) {
        SBytes list;
        for (auto &cert : _identity.certificateChain) {
            auto raw = cert->rawData().toSpan();
            append(list, vector({raw.data, raw.size}, 3));
            number(list, 0, 2);
        }
        SBytes body{0};
        append(body, vector(list, 3));
        emit(r, 11, body);
        if (list.empty()) {
            return;
        }
        auto key = _identity.privateKey;
        unsigned scheme = signatureScheme(key->publicKey());
        require(scheme && _peerSignatures.contains(scheme), "no common signature algorithm", ERET_UNSUPPORTED,
                40);
        auto context = IAsymmetric::builtIn(key->algorithm())->createContext();
        crypto(bool(context));
        context->keyPair(key->publicKey(), std::const_pointer_cast<IPrivateKey>(key));
        auto input = signatureInput(_options.role == EROLE_SERVER, transcript());
        auto digest = scheme == 0x0807 ? input : hash(input);
        certpp::COctet signature;
        crypto((scheme == 0x0804 ? context->signPss(bytes(digest), EHASH_SHA256, 32, signature)
                                 : context->sign(bytes(digest), signature)) == certpp::ERET_OK);
        SBytes verify;
        number(verify, scheme, 2);
        append(verify, vector({signature.toPtr(), signature.size()}, 2));
        emit(r, 15, verify);
    }
    void authenticate(Reader &r) {
        require(r.vec(1).empty(), "unexpected certificate request context", ERET_INVALID_ARGUMENT, 47);
        Reader list(r.vec(3));
        r.end();
        require(list.size() <= _options.maximumCertificateBytes, "TLS certificate limit", ERET_LIMIT, 80);
        while (list.size()) {
            auto der = list.vec(3);
            require(!der.empty(), "empty certificate", ERET_AUTHENTICATION, 42);
            auto cert = std::make_shared<certpp::x509::CCert>();
            require(cert->importFrom(bytes(der), certpp::x509::ECERT_DER) == certpp::ERET_OK,
                    "invalid certificate", ERET_AUTHENTICATION, 42);
            Reader ext(list.vec(2));
            std::set<unsigned> seen;
            while (ext.size()) {
                auto id = ext.get(2);
                require(seen.insert(id).second, "duplicate certificate extension");
                ext.vec(2);
            }
            _peer.certificateChain.push_back(std::move(cert));
            require(_peer.certificateChain.size() <= 16, "certificate chain too deep", ERET_LIMIT, 42);
        }
        _certificateReceived = !_peer.certificateChain.empty();
        if (!_options.requirePeerAuthentication) {
            return;
        }
        require(_certificateReceived, "peer certificate required", ERET_AUTHENTICATION, 116);
        for (auto &cert : _peer.certificateChain) {
            auto digest = cert->createHasher();
            const auto &algorithm = cert->signAlgo();
            require((digest && digest->byteWidth() >= 32) || algorithm == certpp::CString("Ed25519") ||
                        algorithm == certpp::CString("Ed448"),
                    "weak or unsupported certificate signature", ERET_AUTHENTICATION, 43);
            auto key = cert->publicKey();
            require(key && ((key->algorithm() == EASYM_RSA && key->keySize() >= 2048) ||
                            key->algorithm() == EASYM_P256 || key->algorithm() == EASYM_P384 ||
                            key->algorithm() == EASYM_P521 || key->algorithm() == EASYM_ED25519 ||
                            key->algorithm() == EASYM_ED448),
                    "weak or unsupported certificate public key", ERET_AUTHENTICATION, 43);
        }
        certpp::x509::CTrustStore store;
        for (auto &cert : _trust.anchors) {
            crypto(store.add(*cert) == certpp::ERET_OK);
        }
        certpp::x509::CCertCollection intermediates;
        for (std::size_t i = 1; i < _peer.certificateChain.size(); ++i) {
            std::size_t index;
            crypto(intermediates.add(*_peer.certificateChain[i], index) == certpp::ERET_OK);
        }
        certpp::x509::SPathValidationPolicy policy;
        policy.requiredLeafKeyUsages = certpp::x509::EKUSE_DIGITAL_SIGNATURE;
        policy.requiredExtendedKeyPurpose =
            certpp::COid(_options.role == EROLE_CLIENT ? certpp::COid::PURPOSE_SERVER_AUTH
                                                       : certpp::COid::PURPOSE_CLIENT_AUTH);
        require(certpp::x509::CPathValidator::validate(*_peer.certificateChain.front(), intermediates, store,
                                                       policy)
                    .ok(),
                "certificate path validation failed", ERET_AUTHENTICATION, 48);
        if (_options.role == EROLE_CLIENT) {
            require(matchesName(*_peer.certificateChain.front(), _options.serverName),
                    "certificate server identity mismatch", ERET_AUTHENTICATION, 42);
        }
        // --> Trust is not published until CertificateVerify and Finished also pass.
    }
    void verifyCertificate(Reader &r) {
        auto scheme = r.get(2);
        auto signature = r.vec(2);
        r.end();
        require(_certificateReceived, "missing certificate", ERET_AUTHENTICATION, 42);
        auto key = _peer.certificateChain.front()->publicKey();
        require(scheme == signatureScheme(key), "unoffered certificate signature algorithm",
                ERET_AUTHENTICATION, 47);
        auto context = IAsymmetric::builtIn(key->algorithm())->createContext();
        crypto(bool(context));
        context->keyPair(key, {});
        auto input = signatureInput(_options.role == EROLE_CLIENT, transcript());
        auto digest = scheme == 0x0807 ? input : hash(input);
        require((scheme == 0x0804 ? context->verifyPss(bytes(digest), EHASH_SHA256, 32, bytes(signature))
                                  : context->verify(bytes(digest), bytes(signature))) == certpp::ERET_OK,
                "CertificateVerify invalid", ERET_AUTHENTICATION, 51);
    }
    SBytes alpnExtension() {
        SBytes list;
        for (auto &p : _options.alpn) {
            append(list, vector({reinterpret_cast<const std::uint8_t *>(p.data()), p.size()}, 1));
        }
        return vector(list, 2);
    }
    void clientHello(SHandshakeResult &r) {
        generateKey();
        SBytes hello{3, 3};
        append(hello, random(32));
        _sessionId = _mode == EMODE_RECORDS ? random(32) : SBytes{};
        append(hello, vector(_sessionId, 1));
        append(hello, SBytes{0, 2, 0x13, 1, 1, 0});
        SBytes ext;
        if (!_options.serverName.empty()) {
            bool ip = false;
            try {
                sockpp::CSocketAddress::parse(_options.serverName, 0);
                ip = true;
            } catch (const std::exception &) {
            }
            if (!ip) {
                SBytes sni{0};
                append(sni, vector({reinterpret_cast<const std::uint8_t *>(_options.serverName.data()),
                                    _options.serverName.size()},
                                   2));
                extension(ext, 0, vector(sni, 2));
            }
        }
        extension(ext, 43, SBytes{2, 3, 4});
        extension(ext, 10, SBytes{0, 2, 0, 29});
        extension(ext, 13, SBytes{0, 6, 8, 7, 4, 3, 8, 4});
        SBytes share{0, 29};
        append(share, vector(keyShare(), 2));
        extension(ext, 51, vector(share, 2));
        if (!_options.alpn.empty()) {
            extension(ext, 16, alpnExtension());
        }
        if (_mode == EMODE_QUIC) {
            extension(ext, 57, _parameters);
        }
        append(hello, vector(ext, 2));
        emit(r, 1, hello);
        _step = SERVER_HELLO;
    }
    void serverHello(SHandshakeResult &result, Reader &r, View message) {
        require(r.get(2) == 0x0303, "invalid ClientHello version", ERET_UNSUPPORTED, 70);
        r.take(32);
        auto id = r.vec(1);
        require(id.size() <= 32, "invalid session ID");
        _sessionId = {id.begin(), id.end()};
        Reader suites(r.vec(2));
        bool supported = false;
        require(suites.size() && !(suites.size() % 2), "invalid cipher suite list");
        while (suites.size()) {
            supported |= suites.get(2) == 0x1301;
        }
        require(supported, "no supported cipher suite", ERET_UNSUPPORTED, 40);
        auto compression = r.vec(1);
        require(compression.size() == 1 && compression[0] == 0, "invalid TLS compression",
                ERET_INVALID_ARGUMENT, 47);
        auto ext = extensions(r);
        require(ext.contains(43) && ext.contains(51) && ext.contains(13) && ext.contains(10),
                "missing ClientHello extensions", ERET_INVALID_ARGUMENT, 109);
        Reader versions(ext[43]);
        Reader vers(versions.vec(1));
        versions.end();
        bool tls13 = false;
        while (vers.size()) {
            tls13 |= vers.get(2) == 0x0304;
        }
        require(tls13, "TLS 1.3 required", ERET_UNSUPPORTED, 70);
        Reader groups(ext[10]);
        Reader gs(groups.vec(2));
        groups.end();
        bool x = false;
        while (gs.size()) {
            x |= gs.get(2) == 29;
        }
        require(x, "X25519 required", ERET_UNSUPPORTED, 40);
        Reader sig(ext[13]);
        Reader sigs(sig.vec(2));
        sig.end();
        while (sigs.size()) {
            _peerSignatures.insert(sigs.get(2));
        }
        Reader shares(ext[51]);
        Reader keys(shares.vec(2));
        shares.end();
        SBytes peerKey;
        std::set<unsigned> offered;
        while (keys.size()) {
            auto group = keys.get(2);
            auto v = keys.vec(2);
            require(offered.insert(group).second, "duplicate key share", ERET_INVALID_ARGUMENT, 47);
            if (group == 29) {
                peerKey = {v.begin(), v.end()};
            }
        }
        require(peerKey.size() == 32, "X25519 key share required; HRR unavailable", ERET_UNSUPPORTED, 40);
        require(!ext.contains(42), "early data unsupported", ERET_UNSUPPORTED, 47);
        if (ext.contains(16)) {
            Reader a(ext[16]);
            Reader names(a.vec(2));
            a.end();
            std::vector<std::string> offeredAlpn;
            while (names.size()) {
                auto v = names.vec(1);
                require(!v.empty(), "empty ALPN");
                offeredAlpn.emplace_back(reinterpret_cast<const char *>(v.data()), v.size());
            }
            for (auto &preferred : _options.alpn) {
                if (std::find(offeredAlpn.begin(), offeredAlpn.end(), preferred) != offeredAlpn.end()) {
                    _peer.alpn = preferred;
                    break;
                }
            }
        }
        require(_options.alpn.empty() || !_peer.alpn.empty(), "no common ALPN", ERET_UNSUPPORTED, 120);
        if (_mode == EMODE_QUIC) {
            require(ext.contains(57) && !_peer.alpn.empty(), "QUIC requires transport parameters and ALPN",
                    ERET_INVALID_ARGUMENT, 109);
            _peer.quicTransportParameters = ext[57];
        } else {
            require(!ext.contains(57), "QUIC parameters on record TLS", ERET_INVALID_ARGUMENT, 110);
        }
        require(_identity.privateKey && !_identity.certificateChain.empty(), "server identity missing",
                ERET_INVALID_STATE, 80);
        absorb(message);
        generateKey();
        SBytes body{3, 3};
        append(body, random(32));
        append(body, vector(_sessionId, 1));
        append(body, SBytes{0x13, 1, 0});
        SBytes response;
        extension(response, 43, SBytes{3, 4});
        SBytes share{0, 29};
        append(share, vector(keyShare(), 2));
        extension(response, 51, share);
        append(body, vector(response, 2));
        emit(result, 2, body);
        deriveKeys(result, peerKey);
        SBytes ee;
        if (!_peer.alpn.empty()) {
            SBytes protocol =
                vector({reinterpret_cast<const std::uint8_t *>(_peer.alpn.data()), _peer.alpn.size()}, 1);
            extension(ee, 16, vector(protocol, 2));
        }
        if (_mode == EMODE_QUIC) {
            extension(ee, 57, _parameters);
        }
        emit(result, 8, vector(ee, 2));
        if (_options.requirePeerAuthentication) {
            SBytes sigext;
            extension(sigext, 13, SBytes{0, 6, 8, 7, 4, 3, 8, 4});
            SBytes request{0};
            append(request, vector(sigext, 2));
            emit(result, 13, request);
            _clientCertificateRequested = true;
        }
        sendCertificate(result);
        auto mac = finished(view(_writeSecret));
        emit(result, 20, mac);
        applicationKeys();
        install(result, EDIRECTION_WRITE, ELEVEL_APPLICATION, view(_writeApplication));
        _writeApplication.clear();
        _step = _clientCertificateRequested ? CLIENT_CERTIFICATE : CLIENT_FINISHED;
    }
    void handle(SHandshakeResult &result, View message, EEncryptionLevel level) {
        Reader header(message);
        auto type = header.get(1);
        Reader r(header.vec(3));
        header.end();
        if (_mode == EMODE_QUIC) {
            auto expected = (_step == CLIENT_HELLO || _step == SERVER_HELLO)
                                ? ELEVEL_INITIAL
                                : (_step == COMPLETE ? ELEVEL_APPLICATION : ELEVEL_HANDSHAKE);
            require(level == expected, "TLS message at wrong QUIC encryption level", ERET_INVALID_ARGUMENT,
                    10);
        }
        if (_step == CLIENT_HELLO) {
            require(type == 1, "expected ClientHello", ERET_INVALID_ARGUMENT, 10);
            serverHello(result, r, message);
            return;
        }
        if (_step == SERVER_HELLO) {
            require(type == 2, "expected ServerHello", ERET_INVALID_ARGUMENT, 10);
            require(r.get(2) == 0x0303, "invalid ServerHello version", ERET_INVALID_ARGUMENT, 47);
            auto randomBytes = r.take(32);
            require(!std::equal(randomBytes.begin(), randomBytes.end(), HRR), "HelloRetryRequest unavailable",
                    ERET_UNSUPPORTED, 40);
            auto id = r.vec(1);
            require(std::equal(id.begin(), id.end(), _sessionId.begin(), _sessionId.end()),
                    "session ID echo mismatch", ERET_INVALID_ARGUMENT, 47);
            require(r.get(2) == 0x1301 && r.get(1) == 0, "unoffered cipher/compression",
                    ERET_INVALID_ARGUMENT, 47);
            auto ext = extensions(r);
            require(ext.size() == 2 && ext.contains(43) && ext.contains(51), "invalid ServerHello extensions",
                    ERET_INVALID_ARGUMENT, 110);
            Reader v(ext[43]);
            require(v.get(2) == 0x0304, "TLS version mismatch", ERET_UNSUPPORTED, 70);
            v.end();
            Reader share(ext[51]);
            require(share.get(2) == 29, "unoffered key share", ERET_INVALID_ARGUMENT, 47);
            auto key = share.vec(2);
            share.end();
            absorb(message);
            deriveKeys(result, key);
            _step = ENCRYPTED_EXTENSIONS;
            return;
        }
        if (_step == ENCRYPTED_EXTENSIONS) {
            require(type == 8, "expected EncryptedExtensions", ERET_INVALID_ARGUMENT, 10);
            auto ext = extensions(r);
            for (auto &[id, v] : ext) {
                require(id == 0 || id == 10 || id == 16 || (_mode == EMODE_QUIC && id == 57),
                        "unsolicited TLS extension", ERET_INVALID_ARGUMENT, 110);
            }
            if (ext.contains(16)) {
                Reader a(ext[16]);
                Reader names(a.vec(2));
                a.end();
                auto value = names.vec(1);
                names.end();
                _peer.alpn = {reinterpret_cast<const char *>(value.data()), value.size()};
                require(std::find(_options.alpn.begin(), _options.alpn.end(), _peer.alpn) !=
                            _options.alpn.end(),
                        "unoffered ALPN", ERET_INVALID_ARGUMENT, 47);
            }
            require(_options.alpn.empty() || !_peer.alpn.empty(), "missing negotiated ALPN", ERET_UNSUPPORTED,
                    120);
            if (_mode == EMODE_QUIC) {
                require(ext.contains(57) && !_peer.alpn.empty(), "missing QUIC extensions",
                        ERET_INVALID_ARGUMENT, 109);
                _peer.quicTransportParameters = ext[57];
            }
            absorb(message);
            _step = SERVER_CERTIFICATE;
            return;
        }
        if (_step == SERVER_CERTIFICATE && type == 13) {
            require(!_clientCertificateRequested && r.vec(1).empty(), "invalid CertificateRequest",
                    ERET_INVALID_ARGUMENT, 47);
            auto ext = extensions(r);
            require(ext.contains(13), "missing signature_algorithms", ERET_INVALID_ARGUMENT, 109);
            Reader sig(ext[13]);
            Reader sigs(sig.vec(2));
            sig.end();
            while (sigs.size()) {
                _peerSignatures.insert(sigs.get(2));
            }
            _clientCertificateRequested = true;
            absorb(message);
            return;
        }
        if (_step == SERVER_CERTIFICATE || _step == CLIENT_CERTIFICATE) {
            require(type == 11, "expected Certificate", ERET_INVALID_ARGUMENT, 10);
            authenticate(r);
            absorb(message);
            _step = _options.role == EROLE_CLIENT ? SERVER_VERIFY
                                                  : (_certificateReceived ? CLIENT_VERIFY : CLIENT_FINISHED);
            return;
        }
        if (_step == SERVER_VERIFY || _step == CLIENT_VERIFY) {
            require(type == 15, "expected CertificateVerify", ERET_INVALID_ARGUMENT, 10);
            verifyCertificate(r);
            absorb(message);
            _step = _options.role == EROLE_CLIENT ? SERVER_FINISHED : CLIENT_FINISHED;
            return;
        }
        if (_step == SERVER_FINISHED || _step == CLIENT_FINISHED) {
            require(type == 20, "expected Finished", ERET_INVALID_ARGUMENT, 10);
            auto mac = finished(view(_readSecret));
            auto incoming = r.take(32);
            r.end();
            require(certpp::CSecure::equals(bytes(mac), bytes(incoming)), "Finished verification failed",
                    ERET_AUTHENTICATION, 51);
            absorb(message);
            if (_options.role == EROLE_CLIENT) {
                applicationKeys();
                install(result, EDIRECTION_READ, ELEVEL_APPLICATION, view(_readApplication));
                _readApplication.clear();
                if (_clientCertificateRequested) {
                    sendCertificate(result);
                }
                auto own = finished(view(_writeSecret));
                emit(result, 20, own);
                install(result, EDIRECTION_WRITE, ELEVEL_APPLICATION, view(_writeApplication));
                _writeApplication.clear();
            } else {
                install(result, EDIRECTION_READ, ELEVEL_APPLICATION, view(_readApplication));
                _readApplication.clear();
            }
            _peer.authentication = _options.requirePeerAuthentication ? EPEER_ACCEPTED : EPEER_NOT_CHECKED;
            _step = COMPLETE;
            _state = ESTATE_ESTABLISHED;
            result.handshakeComplete = true;
            return;
        }
        require(_step == COMPLETE, "invalid handshake state", ERET_INVALID_STATE, 10);
        if (type == 4 &&
            _options.role ==
                EROLE_CLIENT) { // --> Validate then discard tickets; resumption is deliberately disabled.
            r.get(4);
            r.get(4);
            r.vec(1);
            require(!r.vec(2).empty(), "empty session ticket");
            extensions(r);
            return;
        }
        if (type == 24) {
            require(_mode == EMODE_RECORDS, "TLS KeyUpdate forbidden in QUIC", ERET_INVALID_ARGUMENT, 10);
            auto request = r.get(1);
            r.end();
            require(request <= 1, "invalid KeyUpdate request", ERET_INVALID_ARGUMENT, 47);
            _readSecret = label(view(_readSecret), "traffic upd", {});
            _readKey.install(view(_readSecret));
            secret(result, EDIRECTION_READ, ELEVEL_APPLICATION, view(_readSecret), ++_readGeneration);
            if (request) {
                emit(result, 24, SBytes{0}, false);
                _writeSecret = label(view(_writeSecret), "traffic upd", {});
                _writeKey.install(view(_writeSecret));
                secret(result, EDIRECTION_WRITE, ELEVEL_APPLICATION, view(_writeSecret), ++_writeGeneration);
            }
            return;
        }
        require(false, "unexpected post-handshake message", ERET_UNSUPPORTED, 10);
    }
    void handshakeBytes(SHandshakeResult &result, View input, EEncryptionLevel level) {
        require(_messages.empty() || level == _fragmentLevel, "fragment crosses encryption levels",
                ERET_INVALID_ARGUMENT, 10);
        _fragmentLevel = level;
        require(input.size() <= _options.maximumBufferedBytes - _messages.size(), "handshake buffering limit",
                ERET_LIMIT, 80);
        append(_messages, input);
        while (_messages.size() >= 4) {
            std::size_t n =
                (std::size_t(_messages[1]) << 16) | (std::size_t(_messages[2]) << 8) | _messages[3];
            require(n <= _options.maximumHandshakeBytes &&
                        n <= _options.maximumBufferedBytes -
                                 std::min<std::size_t>(4, _options.maximumBufferedBytes),
                    "TLS message too large", ERET_LIMIT, 80);
            if (_messages.size() < n + 4) {
                break;
            }
            handle(result, View(_messages).first(n + 4), level);
            _messages.erase(_messages.begin(), _messages.begin() + n + 4);
        }
    }
    void records(SHandshakeResult &result, View input) {
        require(input.size() <= _options.maximumBufferedBytes - _wire.size(), "record buffering limit",
                ERET_LIMIT, 80);
        append(_wire, input);
        while (_wire.size() >= 5) {
            unsigned type = _wire[0];
            std::size_t n = (unsigned(_wire[3]) << 8) | _wire[4];
            require(_wire[1] == 3 && (_wire[2] == 3 || (!_readKey.ready() && _wire[2] == 1)),
                    "invalid TLS record version", ERET_INVALID_ARGUMENT, 70);
            require(n <= 16640, "TLS record overflow", ERET_LIMIT, 22);
            if (_wire.size() < n + 5) {
                break;
            }
            View payload = View(_wire).subspan(5, n);
            SBytes plain;
            if (type == 20) {
                require(_state == ESTATE_HANDSHAKING && payload.size() == 1 && payload[0] == 1,
                        "invalid change_cipher_spec", ERET_INVALID_ARGUMENT, 10);
            } else {
                if (_readKey.ready()) {
                    require(type == 23 && _wire[2] == 3, "unencrypted TLS record after ServerHello",
                            ERET_INVALID_ARGUMENT, 10);
                    plain = _readKey.transform(payload, View(_wire).first(5), false);
                    while (!plain.empty() && plain.back() == 0) {
                        plain.pop_back();
                    }
                    require(!plain.empty(), "empty TLSInnerPlaintext", ERET_INVALID_ARGUMENT, 10);
                    type = plain.back();
                    plain.pop_back();
                    payload = plain;
                } else {
                    require(type == 22 || type == 21, "unexpected plaintext content type",
                            ERET_INVALID_ARGUMENT, 10);
                }
                require(payload.size() <= 16384, "TLS plaintext overflow", ERET_LIMIT, 22);
                if (type == 22) {
                    handshakeBytes(result, payload, _readLevel);
                } else if (type == 23) {
                    require(_state == ESTATE_ESTABLISHED && !_peerFinished && _messages.empty(),
                            "unexpected application record", ERET_INVALID_ARGUMENT, 10);
                    if (!payload.empty()) {
                        result.plaintext.emplace_back(payload.begin(), payload.end());
                    }
                } else if (type == 21) {
                    require(payload.size() == 2, "invalid alert");
                    if (payload[1] == 0) {
                        require(_state == ESTATE_ESTABLISHED, "unauthenticated close_notify",
                                ERET_AUTHENTICATION, 10);
                        _peerFinished = true;
                        result.peerClosedWrite = true;
                    } else {
                        throw STlsError{ERET_AUTHENTICATION, payload[1], "peer sent TLS alert"};
                    }
                } else {
                    require(false, "invalid inner content type", ERET_INVALID_ARGUMENT, 10);
                }
            }
            _wire.erase(_wire.begin(), _wire.begin() + n + 5);
        }
    }
    void wipe() {
        _master.clear();
        _readSecret.clear();
        _writeSecret.clear();
        _readApplication.clear();
        _writeApplication.clear();
        _readKey.clear();
        _writeKey.clear();
        _keyPair = {};
    }
    void failed(SHandshakeResult &r, const STlsError &e) {
        _error = e;
        _state = ESTATE_FAILED;
        _peer.authentication = EPEER_REJECTED;
        r = {};
        r.result = e;
        wipe();
    }

  public:
    Session(std::shared_ptr<asyncpp::dispatcher> e, EMode m, STlsOptions o, SIdentity i, STrustOptions t,
            SBytes p)
        : _executor(std::move(e)), _options(std::move(o)), _identity(std::move(i)), _trust(std::move(t)),
          _mode(m), _step(_options.role == EROLE_CLIENT ? SERVER_HELLO : CLIENT_HELLO),
          _parameters(std::move(p)) {
        crypto(IHasher::create(EHASH_SHA256, _transcript) == certpp::ERET_OK);
    }
    bool isImplemented() const noexcept override { return true; }
    EMode mode() const noexcept override { return _mode; }
    ESessionState state() const noexcept override { return _closed ? ESTATE_CLOSED : _state; }
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override { return _executor; }
    asyncpp::task<SHandshakeResult> start(SOperationOptions o) override {
        SHandshakeResult r;
        r.result = success();
        auto c = check(_executor, o);
        if (c != ERET_OK) {
            r.result.code = c;
            co_return r;
        }
        if (_closed || _started) {
            r.result.code = _closed ? ERET_CLOSED : ERET_INVALID_STATE;
            co_return r;
        }
        _started = true;
        _state = ESTATE_HANDSHAKING;
        try {
            if (_options.role == EROLE_CLIENT) {
                clientHello(r);
            }
        } catch (const STlsError &e) {
            failed(r, e);
        }
        co_return r;
    }
    asyncpp::task<SHandshakeResult> receive(EEncryptionLevel level, SBytes data,
                                            SOperationOptions o) override {
        SHandshakeResult r;
        r.result = success();
        auto c = check(_executor, o);
        if (c != ERET_OK) {
            r.result.code = c;
            co_return r;
        }
        if (_closed || !_started || _state == ESTATE_FAILED) {
            r.result.code = _closed ? ERET_CLOSED : ERET_INVALID_STATE;
            co_return r;
        }
        try {
            if (_mode == EMODE_RECORDS) {
                records(r, data);
            } else {
                handshakeBytes(r, data, level);
            }
        } catch (const STlsError &e) {
            failed(r, e);
        }
        co_return r;
    }
    asyncpp::task<SBytesResult> encrypt(SBytes data, SOperationOptions o) override {
        SBytesResult r;
        r.result = success();
        auto c = check(_executor, o);
        if (c != ERET_OK) {
            r.result.code = c;
            co_return r;
        }
        if (_mode != EMODE_RECORDS || state() != ESTATE_ESTABLISHED || _writeFinished) {
            r.result.code = ERET_INVALID_STATE;
            co_return r;
        }
        if (data.size() > _options.maximumBufferedBytes) {
            r.result.code = ERET_LIMIT;
            co_return r;
        }
        try {
            for (std::size_t offset = 0; offset < data.size();) {
                auto n = std::min(_options.maximumRecordBytes, data.size() - offset);
                append(r.bytes, record(23, View(data).subspan(offset, n)));
                offset += n;
            }
        } catch (const STlsError &e) {
            _error = e;
            _state = ESTATE_FAILED;
            wipe();
            r = {e, {}};
        }
        co_return r;
    }
    asyncpp::task<SBytesResult> finishWrite(SOperationOptions o) override {
        SBytesResult r;
        r.result = success();
        auto c = check(_executor, o);
        if (c != ERET_OK) {
            r.result.code = c;
            co_return r;
        }
        if (_mode != EMODE_RECORDS || state() != ESTATE_ESTABLISHED) {
            r.result.code = ERET_INVALID_STATE;
            co_return r;
        }
        if (!_writeFinished) {
            try {
                r.bytes = record(21, SBytes{1, 0});
                _writeFinished = true;
            } catch (const STlsError &e) {
                _error = e;
                _state = ESTATE_FAILED;
                wipe();
                r = {e, {}};
            }
        }
        co_return r;
    }
    SPeerInfo peerInfo() const override { return _peer; }
    STlsError lastError() const override { return _error; }
    void close() noexcept override {
        _closed = true;
        if (asyncpp::dispatcher::current() == _executor.get()) {
            wipe();
        }
    }
    asyncpp::task<ERetCode> waitClosed(SOperationOptions o) override {
        auto c = check(_executor, o);
        co_return c != ERET_OK ? c : (_closed ? ERET_OK : ERET_INVALID_STATE);
    }
};
class Context final : public ITlsContext {
  private:
    STlsOptions _options;
    SIdentity _identity;
    STrustOptions _trust;

  public:
    Context(STlsOptions o, SIdentity i, STrustOptions t)
        : _options(std::move(o)), _identity(std::move(i)), _trust(std::move(t)) {}
    bool isImplemented() const noexcept override { return true; }
    STlsOptions options() const override { return _options; }
    std::shared_ptr<ITlsSession> createSession(std::shared_ptr<asyncpp::dispatcher> e, EMode m,
                                               SBytes p) override {
        if (!e || (m != EMODE_RECORDS && m != EMODE_QUIC) || (m == EMODE_RECORDS && !p.empty()) ||
            p.size() > 65535) {
            throw std::invalid_argument("invalid TLS executor/mode/transport parameters");
        }
        return std::make_shared<Session>(std::move(e), m, _options, _identity, _trust, std::move(p));
    }
};
} // namespace
std::shared_ptr<ITlsContext> ITlsContext::create(STlsOptions o, SIdentity i, STrustOptions t) {
    if ((o.role != EROLE_CLIENT && o.role != EROLE_SERVER) || o.maximumHandshakeBytes < 4 ||
        !o.maximumCertificateBytes || o.maximumBufferedBytes < 5 || !o.maximumRecordBytes ||
        o.maximumRecordBytes > 16384 || o.enableEarlyData || t.useSystemTrust || o.serverName.size() > 253 ||
        o.serverName.find('\0') != std::string::npos) {
        throw std::invalid_argument("unsupported or invalid TLS policy");
    }
    std::size_t count = 0;
    std::set<std::string> protocols;
    for (auto &p : o.alpn) {
        if (p.empty() || p.size() > 255 || count > 65533 - p.size() - 1 || !protocols.insert(p).second) {
            throw std::invalid_argument("invalid ALPN list");
        }
        count += p.size() + 1;
    }
    for (auto &c : i.certificateChain) {
        if (!c || c->empty()) {
            throw std::invalid_argument("invalid identity certificate");
        }
    }
    for (auto &c : t.anchors) {
        if (!c || c->empty()) {
            throw std::invalid_argument("invalid trust anchor");
        }
    }
    if (bool(i.privateKey) != !i.certificateChain.empty()) {
        throw std::invalid_argument("identity requires certificate and key");
    }
    if (i.privateKey &&
        (!signatureScheme(i.privateKey->publicKey()) || !i.certificateChain.front()->publicKey() ||
         i.privateKey->publicKey()->compare(i.certificateChain.front()->publicKey()) != 0)) {
        throw std::invalid_argument("unsupported or mismatched identity key");
    }
    return std::make_shared<Context>(std::move(o), std::move(i), std::move(t));
}
} // namespace hntls
