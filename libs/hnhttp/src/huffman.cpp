#include "huffman.hpp"
#include "huffman_table.hpp"
#include <array>

namespace hnhttp::detail {
namespace {
struct SNode { std::array<int,2> child{-1,-1}; int symbol = -1; };
const std::vector<SNode>& trie() {
    static const auto nodes = [] {
        std::vector<SNode> result(1);
        for (unsigned symbol = 0; symbol < 257; ++symbol) {
            const auto code = huffmanCodes[symbol]; int node = 0;
            for (unsigned bit = code.bits; bit; --bit) {
                const auto direction = (code.code >> (bit-1)) & 1;
                if (result[node].child[direction] < 0) {
                    result[node].child[direction] = static_cast<int>(result.size()); result.emplace_back();
                }
                node = result[node].child[direction];
            }
            result[node].symbol = static_cast<int>(symbol);
        }
        return result;
    }();
    return nodes;
}
}
SBytes huffmanEncode(std::string_view value) {
    SBytes result; std::uint64_t pending = 0; unsigned count = 0;
    for (const unsigned char c : value) {
        const auto code = huffmanCodes[c]; pending = (pending << code.bits) | code.code; count += code.bits;
        while (count >= 8) { count -= 8; result.push_back(static_cast<std::uint8_t>(pending >> count)); }
        pending &= (UINT64_C(1)<<count)-1;
    }
    if (count) { result.push_back(static_cast<std::uint8_t>((pending << (8-count)) | ((1u<<(8-count))-1))); }
    return result;
}
std::pair<ERetCode, std::string> huffmanDecode(std::span<const std::uint8_t> bytes, std::size_t maximum) {
    const auto& nodes = trie(); int node = 0; unsigned trailing = 0; bool ones = true; std::string result;
    for (const auto byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            const auto direction = (byte >> bit) & 1; ++trailing; ones = ones && direction;
            node = nodes[node].child[direction];
            if (node < 0) { return {ERET_MALFORMED, {}}; }
            if (nodes[node].symbol >= 0) {
                if (nodes[node].symbol == 256) { return {ERET_MALFORMED, {}}; }
                if (result.size() == maximum) { return {ERET_LIMIT, {}}; }
                result.push_back(static_cast<char>(nodes[node].symbol)); node = 0; trailing = 0; ones = true;
            }
        }
    }
    if (node && (trailing > 7 || !ones)) { return {ERET_MALFORMED, {}}; }
    return {ERET_OK, std::move(result)};
}
}
