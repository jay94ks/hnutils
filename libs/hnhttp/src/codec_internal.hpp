#ifndef __SRC_HNHTTP_CODEC_INTERNAL_HPP__
#define __SRC_HNHTTP_CODEC_INTERNAL_HPP__
#include "internal.hpp"
#include "huffman.hpp"
#include <span>
namespace hnhttp::detail {
struct CodecFailure { ERetCode code; };
[[noreturn]] inline void bad(ERetCode code = ERET_MALFORMED) { throw CodecFailure{code}; }
inline std::uint64_t integer(std::span<const std::uint8_t> bytes, std::size_t& pos, unsigned bits) {
    if (pos >= bytes.size()) { bad(ERET_TRUNCATED); }
    const auto mask = (1u << bits)-1;
    std::uint64_t value = bytes[pos++] & mask;
    if (value != mask) { return value; }
    unsigned shift = 0;
    while (true) {
        if (pos >= bytes.size()) { bad(ERET_TRUNCATED); }
        const auto digit = bytes[pos++];
        if (shift >= 63 || static_cast<std::uint64_t>(digit & 127) > ((UINT64_MAX-value) >> shift)) { bad(); }
        value += static_cast<std::uint64_t>(digit & 127) << shift;
        if (!(digit & 128)) { return value; } shift += 7;
    }
}
inline void integer(SBytes& bytes, std::uint64_t value, unsigned bits, unsigned flags) {
    const auto mask = (1u << bits)-1;
    if (value < mask) { bytes.push_back(static_cast<std::uint8_t>(flags | value)); return; }
    bytes.push_back(static_cast<std::uint8_t>(flags | mask)); value -= mask;
    while (value >= 128) { bytes.push_back(static_cast<std::uint8_t>((value & 127) | 128)); value >>= 7; }
    bytes.push_back(static_cast<std::uint8_t>(value));
}
inline std::string literal(std::span<const std::uint8_t> bytes, std::size_t& pos, unsigned bits = 7,
    unsigned huffman = 128, std::size_t maximum = 65536) {
    if (pos >= bytes.size()) { bad(ERET_TRUNCATED); }
    const bool encoded = (bytes[pos] & huffman) != 0;
    const auto count = integer(bytes, pos, bits);
    if (count > bytes.size()-pos) { bad(ERET_TRUNCATED); }
    auto wire = bytes.subspan(pos, static_cast<std::size_t>(count)); pos += static_cast<std::size_t>(count);
    if (encoded) { auto decoded = huffmanDecode(wire, maximum); if (decoded.first != ERET_OK) { bad(decoded.first); } return std::move(decoded.second); }
    if (count > maximum) { bad(ERET_LIMIT); }
    return {reinterpret_cast<const char*>(wire.data()), wire.size()};
}
inline void literal(SBytes& bytes, const std::string& value, unsigned bits = 7, unsigned flags = 0) {
    auto encoded = huffmanEncode(value);
    if (encoded.size() < value.size()) {
        integer(bytes, encoded.size(), bits, flags | (1u<<bits)); bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    } else { integer(bytes, value.size(), bits, flags); bytes.insert(bytes.end(), value.begin(), value.end()); }
}
inline bool secret(const SHeaderField& f) {
    return f.indexing == EINDEX_NEVER || f.name == "authorization" || f.name == "proxy-authorization"
        || f.name == "cookie" || f.name == "set-cookie" || f.name == "password";
}
inline bool validField(const SHeaderField& f) {
    std::string_view name(f.name);
    if (name.starts_with(':')) { name.remove_prefix(1); }
    return token(name) && lower(f.name) == f.name && fieldValue(f.value);
}
}
#endif
