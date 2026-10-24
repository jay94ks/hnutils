#ifndef __SRC_HNHTTP_HUFFMAN_HPP__
#define __SRC_HNHTTP_HUFFMAN_HPP__
#include <hnhttp/common.hpp>
#include <span>
#include <string_view>
#include <utility>
namespace hnhttp::detail {
SBytes huffmanEncode(std::string_view value);
std::pair<ERetCode, std::string> huffmanDecode(std::span<const std::uint8_t> bytes, std::size_t maximum);
}
#endif
