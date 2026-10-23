#ifndef __SRC_HNCONF_INTERNAL_HPP__
#define __SRC_HNCONF_INTERNAL_HPP__

#include <hnconf/config.hpp>

namespace hnconf {
namespace detail {

/** Validate limits before parsing or opening a source file. */
void validateOptions(const SParseOptions& options);

/** Parse a bounded complete source into ordered nodes. */
std::vector<CConfigNode> parseNodes(std::string_view text, const SParseOptions& options, const std::string& source);

/** Serialize a bounded canonical tree. */
std::string writeNodes(const std::vector<CConfigNode>& nodes, const SWriteOptions& options);

} // namespace detail
} // namespace hnconf

#endif
