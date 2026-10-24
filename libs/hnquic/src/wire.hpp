#ifndef __SRC_HNQUIC_WIRE_HPP__
#define __SRC_HNQUIC_WIRE_HPP__
#include <hnquic/connection.hpp>
#include <certpp/crypto/aeads/aesgcm.hpp>
#include <certpp/crypto/hkdf.hpp>
#include <certpp/crypto/syms/aes.hpp>
#include <certpp/crypto/rng.hpp>
#include <certpp/io/buffer.hpp>
#include <span>
#include <map>
#include <set>
#include <deque>
#include <array>
#include <cstring>

namespace hnquic {
namespace detail {
using View = std::span<const std::uint8_t>;
using Secret = certpp::CSecureBuffer;
using Time = std::chrono::steady_clock::time_point;
inline certpp::SReadOnlyByteSpan span(View v) { return {v.data(), v.size()}; }
inline View view(const Secret &v) { return {v.toPtr(), v.size()}; }
inline void need(bool b, const char *why, std::uint64_t transport = 7) {
    if (!b) {
        throw SQuicError{ERET_INVALID_ARGUMENT, transport, {}, {}, why};
    }
}
inline void crypto(bool b) { need(b, "libcertpp operation failed", 1); }
inline void append(SBytes &out, View in) { out.insert(out.end(), in.begin(), in.end()); }
inline void number(SBytes &out, std::uint64_t n, unsigned width) {
    for (unsigned i = width; i; --i) {
        out.push_back(static_cast<std::uint8_t>(n >> ((i - 1) * 8)));
    }
}
inline void varint(SBytes &out, std::uint64_t n) {
    need(n < (UINT64_C(1) << 62), "QUIC integer overflow");
    unsigned w = n < 64 ? 1 : n < 16384 ? 2 : n < (UINT64_C(1) << 30) ? 4 : 8;
    auto offset = out.size();
    number(out, n, w);
    out[offset] |= w == 1 ? 0 : w == 2 ? 64 : w == 4 ? 128 : 192;
}
class Reader {
  private:
    View _bytes;

  public:
    explicit Reader(View b) : _bytes(b) {}
    std::size_t size() const { return _bytes.size(); }
    View take(std::size_t n) {
        need(n <= size(), "truncated QUIC data");
        auto v = _bytes.first(n);
        _bytes = _bytes.subspan(n);
        return v;
    }
    std::uint64_t get(unsigned n) {
        std::uint64_t r = 0;
        for (auto b : take(n)) {
            r = (r << 8) | b;
        }
        return r;
    }
    std::uint64_t integer() {
        auto first = get(1);
        unsigned width = 1u << (first >> 6);
        std::uint64_t r = first & 63;
        for (unsigned i = 1; i < width; ++i) {
            r = (r << 8) | get(1);
        }
        return r;
    }
};
inline Secret label(View key, const char *text, std::size_t size) {
    std::string name = std::string("tls13 ") + text;
    SBytes info;
    number(info, size, 2);
    info.push_back(static_cast<std::uint8_t>(name.size()));
    append(info, {reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});
    info.push_back(0);
    Secret out(size);
    crypto(out.size() == size);
    crypto(certpp::crypto::CHkdf::expand(certpp::crypto::EHASH_SHA256, span(key), span(info), out.toSpan()) ==
           certpp::ERET_OK);
    return out;
}
inline Secret initialSecret(View cid, bool client) {
    const std::uint8_t salt[] = {0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
                                 0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};
    Secret initial(32);
    crypto(initial.size() == 32);
    crypto(certpp::crypto::CHkdf::extract(certpp::crypto::EHASH_SHA256, {salt, 20}, span(cid),
                                          initial.toSpan()) == certpp::ERET_OK);
    return label(view(initial), client ? "client in" : "server in", 32);
}
inline SBytes random(std::size_t n) {
    SBytes b(n);
    crypto(certpp::crypto::CRng::fill({b.data(), b.size()}) == certpp::ERET_OK);
    return b;
}
class PacketKey {
  private:
    Secret _iv, _hp;
    certpp::crypto::CAesGcm _aead;
    std::uint64_t _uses = 0;
    SBytes mask(View sample) const {
        // --> First CBC block with an all-zero IV equals AES-ECB(sample). No AES clone here.
        certpp::crypto::AES aes;
        certpp::crypto::ISymmetricKeyPtr key;
        crypto(aes.createKey(key, span(view(_hp))) == certpp::ERET_OK);
        auto context = aes.createContext(key);
        crypto(bool(context));
        std::uint8_t zero[16]{};
        context->key(key, certpp::CBuffer(zero, 16));
        context->padding(certpp::crypto::ESYMPAD_NONE);
        certpp::crypto::ISymmetricTransformerPtr cipher;
        crypto(context->createEncrypter(cipher) == certpp::ERET_OK);
        SBytes out(16);
        certpp::SByteSpan result(out.data(), 16);
        crypto(cipher->transform(span(sample), result) == certpp::ERET_OK);
        crypto(result.size == 16);
        certpp::SByteSpan final;
        crypto(cipher->transformFinal(final) == certpp::ERET_OK);
        return out;
    }
    std::array<std::uint8_t, 12> nonce(std::uint64_t pn) const {
        std::array<std::uint8_t, 12> n{};
        std::memcpy(n.data(), _iv.toPtr(), 12);
        for (unsigned i = 0; i < 8; ++i) {
            n[11 - i] ^= static_cast<std::uint8_t>(pn >> (8 * i));
        }
        return n;
    }

  public:
    bool ready() const { return _aead.keyed(); }
    void clear() {
        _aead.reset({});
        _iv.clear();
        _hp.clear();
        _uses = 0;
    }
    void install(View secret) {
        auto key = label(secret, "quic key", 16);
        _iv = label(secret, "quic iv", 12);
        _hp = label(secret, "quic hp", 16);
        crypto(_aead.reset(span(view(key))));
        _uses = 0;
    }
    SBytes protect(SBytes header, View payload, std::size_t pnOffset, std::uint64_t pn) {
        need(_uses < (UINT64_C(1) << 23), "QUIC key confidentiality limit", 15);
        need(pn < (UINT64_C(1) << 62), "packet number exhaustion", 15);
        auto iv = nonce(pn);
        auto n = header.size();
        header.resize(n + payload.size() + 16);
        crypto(_aead.seal({iv.data(), 12}, span(View(header).first(n)), span(payload),
                          {header.data() + n, payload.size()}, {header.data() + n + payload.size(), 16}));
        need(header.size() >= pnOffset + 20, "header protection sample too short");
        auto m = mask(View(header).subspan(pnOffset + 4, 16));
        auto width = (header[0] & 3) + 1;
        header[0] ^= m[0] & ((header[0] & 0x80) ? 15 : 31);
        for (unsigned i = 0; i < width; ++i) {
            header[pnOffset + i] ^= m[i + 1];
        }
        ++_uses;
        return header;
    }
    bool open(View packet, std::size_t pnOffset, std::uint64_t expected, SBytes &plain, std::uint64_t &pn) {
        if (packet.size() < pnOffset + 20) {
            return false;
        }
        auto m = mask(packet.subspan(pnOffset + 4, 16));
        SBytes header(packet.begin(), packet.begin() + pnOffset);
        header[0] ^= m[0] & ((header[0] & 0x80) ? 15 : 31);
        auto width = (header[0] & 3) + 1;
        if (packet.size() < pnOffset + width + 16) {
            return false;
        }
        std::uint64_t truncated = 0;
        for (unsigned i = 0; i < width; ++i) {
            auto b = packet[pnOffset + i] ^ m[i + 1];
            header.push_back(b);
            truncated = (truncated << 8) | b;
        }
        auto window = UINT64_C(1) << (width * 8), half = window / 2;
        pn = (expected & ~(window - 1)) | truncated;
        if (pn + half <= expected && pn < (UINT64_C(1) << 62) - window) {
            pn += window;
        } else if (pn > expected + half && pn >= window) {
            pn -= window;
        }
        if (pn >= (UINT64_C(1) << 62)) {
            return false;
        }
        auto iv = nonce(pn);
        auto cipher = packet.subspan(header.size(), packet.size() - header.size() - 16);
        plain.resize(cipher.size());
        if (!_aead.open({iv.data(), 12}, span(header), span(cipher), span(packet.last(16)),
                        {plain.data(), plain.size()})) {
            plain.clear();
            return false;
        }
        need(!(header[0] & ((header[0] & 0x80) ? 12 : 24)), "nonzero QUIC reserved bits", 10);
        need((header[0] & 0x80) || !(header[0] & 4), "QUIC key update not yet supported", 14);
        return true;
    }
};
class Reassembly {
  private:
    std::map<std::uint64_t, SBytes> _ranges;
    std::uint64_t _offset = 0;
    std::size_t _bytes = 0;

  public:
    std::uint64_t offset() const { return _offset; }
    std::size_t buffered() const { return _bytes; }
    void insert(std::uint64_t offset, View input, std::size_t byteLimit, std::size_t rangeLimit) {
        need(offset < (UINT64_C(1) << 62) && input.size() < (UINT64_C(1) << 62) - offset,
             "stream offset overflow", 3);
        if (offset + input.size() <= _offset) {
            return;
        }
        if (offset < _offset) {
            input = input.subspan(_offset - offset);
            offset = _offset;
        }
        std::uint64_t end = offset + input.size();
        auto begin = _ranges.lower_bound(offset);
        if (begin != _ranges.begin()) {
            auto prev = std::prev(begin);
            if (prev->first + prev->second.size() >= offset) {
                begin = prev;
            }
        }
        auto last = begin;
        std::size_t replaced = 0;
        auto first = offset;
        while (last != _ranges.end() && last->first <= end) {
            first = std::min(first, last->first);
            end = std::max(end, last->first + last->second.size());
            replaced += last->second.size();
            ++last;
        }
        auto length = end - first;
        need(length <= byteLimit && _bytes - replaced <= byteLimit - length, "reassembly byte limit", 13);
        auto remaining = _ranges.size() - std::distance(begin, last);
        need(remaining < rangeLimit, "reassembly range limit", 13);
        SBytes merged(length);
        for (auto it = begin; it != last; ++it) {
            std::memcpy(merged.data() + it->first - first, it->second.data(), it->second.size());
            auto a = std::max(offset, it->first),
                 b = std::min(offset + input.size(), it->first + it->second.size());
            if (a < b) {
                need(std::equal(input.begin() + (a - offset), input.begin() + (b - offset),
                                it->second.begin() + (a - it->first)),
                     "conflicting retransmitted bytes", 10);
            }
        }
        if (!input.empty()) {
            std::memcpy(merged.data() + offset - first, input.data(), input.size());
        }
        _ranges.erase(begin, last);
        _ranges.emplace(first, std::move(merged));
        _bytes = _bytes - replaced + length;
    }
    SBytes take(std::size_t maximum) {
        auto it = _ranges.find(_offset);
        if (it == _ranges.end()) {
            return {};
        }
        auto node = _ranges.extract(it);
        auto n = std::min(maximum, node.mapped().size());
        SBytes out(node.mapped().begin(), node.mapped().begin() + n);
        _offset += n;
        _bytes -= n;
        if (n < node.mapped().size()) {
            node.mapped().erase(node.mapped().begin(), node.mapped().begin() + n);
            node.key() = _offset;
            _ranges.insert(std::move(node));
        }
        return out;
    }
};
struct Stream {
    std::uint64_t id = 0, sendOffset = 0, receiveHighest = 0, receiveLimit = 0, sendLimit = 0, consumed = 0;
    Reassembly receive;
    std::optional<std::uint64_t> finalSize;
    bool sendFin = false, readReset = false, writeReset = false, readBusy = false, writeBusy = false;
    std::uint64_t applicationError = 0;
};
struct Sent {
    Time time;
    SBytes frames;
    std::size_t bytes;
};
struct Space {
    PacketKey read, write;
    std::uint64_t next = 0, expected = 0;
    std::map<std::uint64_t, Sent> sent;
    std::set<std::uint64_t> received;
    bool ack = false;
    std::deque<SBytes> pending;
    Reassembly crypto;
    std::uint64_t cryptoOffset = 0;
};
struct PacketHeader {
    bool longHeader = false;
    unsigned level = 2;
    SBytes destination, source;
    std::size_t pnOffset = 0, length = 0;
};
inline PacketHeader parseHeader(View packet, std::size_t shortCidLength = 8) {
    Reader r(packet);
    auto flags = r.get(1);
    need(flags & 64, "missing QUIC fixed bit");
    PacketHeader h;
    h.longHeader = flags & 128;
    if (h.longHeader) {
        auto version = r.get(4);
        need(version == 1, "unsupported QUIC version");
        auto id = r.take(r.get(1));
        need(id.size() <= 20, "oversized destination CID");
        h.destination = {id.begin(), id.end()};
        id = r.take(r.get(1));
        need(id.size() <= 20, "oversized source CID");
        h.source = {id.begin(), id.end()};
        auto type = (flags >> 4) & 3;
        need(type == 0 || type == 2, "Retry and 0-RTT unavailable");
        h.level = type == 0 ? 0 : 1;
        if (type == 0) {
            r.take(r.integer());
        }
        auto length = r.integer();
        h.pnOffset = packet.size() - r.size();
        need(length <= r.size(), "truncated QUIC packet");
        h.length = h.pnOffset + length;
    } else {
        auto id = r.take(shortCidLength);
        h.destination = {id.begin(), id.end()};
        h.pnOffset = packet.size() - r.size();
        h.length = packet.size();
    }
    return h;
}
} // namespace detail
} // namespace hnquic
#endif
