#ifndef __SRC_HNTLS_PROTOCOL_HPP__
#define __SRC_HNTLS_PROTOCOL_HPP__
#include <hntls/session.hpp>
#include <certpp/crypto/aeads/aesgcm.hpp>
#include <certpp/crypto/hkdf.hpp>
#include <certpp/crypto/hmac.hpp>
#include <certpp/crypto/rng.hpp>
#include <certpp/io/buffer.hpp>
#include <certpp/utils/secure.hpp>
#include <span>
#include <map>
#include <stdexcept>
#include <cstring>
#include <source_location>

namespace hntls {
namespace detail {
using View = std::span<const std::uint8_t>;
using Secret = certpp::CSecureBuffer;
inline certpp::SReadOnlyByteSpan bytes(View v) { return {v.data(), v.size()}; }
inline View view(const Secret &v) { return {v.toPtr(), v.size()}; }
inline void require(bool condition, const char *why, ERetCode code = ERET_INVALID_ARGUMENT,
                    unsigned alert = 50) {
    if (!condition) {
        throw STlsError{code, static_cast<std::uint8_t>(alert), why};
    }
}
inline void crypto(bool ok, std::source_location where = std::source_location::current()) {
    if (!ok) {
        throw STlsError{ERET_IO, 80,
                        std::string("libcertpp operation failed in ") + where.function_name() + ":" +
                            std::to_string(where.line())};
    }
}
inline void append(SBytes &out, View v) { out.insert(out.end(), v.begin(), v.end()); }
inline void number(SBytes &out, std::uint64_t n, unsigned width) {
    for (unsigned i = width; i; --i) {
        out.push_back(static_cast<std::uint8_t>(n >> ((i - 1) * 8)));
    }
}
inline SBytes vector(View v, unsigned width) {
    SBytes r;
    number(r, v.size(), width);
    append(r, v);
    return r;
}
class Reader {
  private:
    View _v;

  public:
    explicit Reader(View v) : _v(v) {}
    std::size_t size() const { return _v.size(); }
    View take(std::size_t n) {
        require(n <= _v.size(), "truncated TLS message");
        auto r = _v.first(n);
        _v = _v.subspan(n);
        return r;
    }
    std::uint64_t get(unsigned n) {
        std::uint64_t r = 0;
        for (auto b : take(n)) {
            r = (r << 8) | b;
        }
        return r;
    }
    View vec(unsigned n) { return take(get(n)); }
    void end() { require(_v.empty(), "trailing TLS bytes"); }
};
inline std::map<unsigned, SBytes> extensions(Reader &r) {
    Reader list(r.vec(2));
    r.end();
    std::map<unsigned, SBytes> result;
    while (list.size()) {
        auto type = list.get(2);
        auto v = list.vec(2);
        require(!result.contains(type), "duplicate TLS extension", ERET_INVALID_ARGUMENT, 47);
        result[type] = {v.begin(), v.end()};
    }
    return result;
}
inline void extension(SBytes &out, unsigned type, View data) {
    number(out, type, 2);
    append(out, vector(data, 2));
}
inline SBytes hash(View input) {
    certpp::crypto::IHasherPtr h;
    crypto(certpp::crypto::IHasher::create(certpp::crypto::EHASH_SHA256, h) == certpp::ERET_OK);
    crypto(h->push(bytes(input)) == input.size());
    SBytes out(32);
    crypto(h->finish({out.data(), out.size()}));
    return out;
}
inline Secret extract(View salt, View input) {
    Secret out(32);
    crypto(out.size() == 32);
    crypto(certpp::crypto::CHkdf::extract(certpp::crypto::EHASH_SHA256, bytes(salt), bytes(input),
                                          out.toSpan()) == certpp::ERET_OK);
    return out;
}
inline Secret label(View secret, const std::string &text, View context, std::size_t length = 32) {
    require(text.size() <= 249 && context.size() <= 255 && length <= 65535, "invalid HKDF label");
    SBytes info;
    number(info, length, 2);
    std::string name = "tls13 " + text;
    info.push_back(static_cast<std::uint8_t>(name.size()));
    append(info, {reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});
    append(info, vector(context, 1));
    Secret out(length);
    crypto(out.size() == length);
    crypto(certpp::crypto::CHkdf::expand(certpp::crypto::EHASH_SHA256, bytes(secret), bytes(info),
                                         out.toSpan()) == certpp::ERET_OK);
    return out;
}
inline SBytes random(std::size_t n) {
    SBytes out(n);
    crypto(certpp::crypto::CRng::fill({out.data(), out.size()}) == certpp::ERET_OK);
    return out;
}
class RecordKey {
  private:
    certpp::crypto::CAesGcm _aead;
    Secret _iv;
    std::uint64_t _sequence = 0;

  public:
    bool ready() const { return _aead.keyed(); }
    void clear() {
        _aead.reset({});
        _iv.clear();
        _sequence = 0;
    }
    void install(View secret) {
        auto key = label(secret, "key", {}, 16);
        _iv = label(secret, "iv", {}, 12);
        crypto(_aead.reset(bytes(view(key))));
        _sequence = 0;
    }
    SBytes transform(View input, View header, bool encrypt) {
        // --> Stay below RFC 8446's AES-GCM confidentiality bound; no nonce wrap.
        require(_sequence < (UINT64_C(1) << 24), "TLS traffic key exhausted", ERET_LIMIT, 80);
        std::uint8_t nonce[12];
        std::memcpy(nonce, _iv.toPtr(), 12);
        for (unsigned i = 0; i < 8; ++i) {
            nonce[11 - i] ^= static_cast<std::uint8_t>(_sequence >> (i * 8));
        }
        require(encrypt || input.size() >= 16, "short encrypted TLS record", ERET_AUTHENTICATION, 20);
        SBytes out(encrypt ? input.size() + 16 : input.size() - 16);
        bool ok = encrypt ? _aead.seal({nonce, 12}, bytes(header), bytes(input), {out.data(), input.size()},
                                       {out.data() + input.size(), 16})
                          : _aead.open({nonce, 12}, bytes(header), bytes(input.first(input.size() - 16)),
                                       bytes(input.last(16)), {out.data(), out.size()});
        require(ok, "TLS record authentication failed", ERET_AUTHENTICATION, 20);
        ++_sequence;
        return out;
    }
};
} // namespace detail
} // namespace hntls
#endif
