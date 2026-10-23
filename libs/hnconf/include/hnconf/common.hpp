#ifndef __INCLUDE_HNCONF_COMMON_HPP__
#define __INCLUDE_HNCONF_COMMON_HPP__

#include <cstddef>

#if defined(_WIN32) && defined(HNCONF_SHARED)
#if defined(HNCONF_BUILDING_LIBRARY)
#define HNCONF_API __declspec(dllexport)
#else
#define HNCONF_API __declspec(dllimport)
#endif
#else
#define HNCONF_API
#endif

namespace hnconf {

/** Hard nesting ceiling for parsed and programmatically constructed trees. */
inline constexpr std::size_t MAX_DEPTH = 128;

/** Resource limits for parsing untrusted configuration text. */
struct SParseOptions {

    /** Maximum source size in bytes, including comments and an optional BOM. */
    std::size_t maxInputBytes = 16 * 1024 * 1024;

    /** Maximum decoded byte length of a name or argument. */
    std::size_t maxTokenBytes = 1024 * 1024;

    /** Maximum total number of directives and blocks. */
    std::size_t maxNodes = 100000;

    /** Maximum nested block count, between zero and MAX_DEPTH inclusive. */
    std::size_t maxDepth = MAX_DEPTH;

    /** Maximum arguments per directive or block, excluding its name. */
    std::size_t maxArguments = 4096;
};

/** Canonical output settings; comments and original spacing are not retained. */
struct SWriteOptions {

    /** Spaces per nesting level, between zero and sixteen inclusive. */
    std::size_t indentWidth = 4;

    /** Maximum serialized size; failure never opens the destination file. */
    std::size_t maxOutputBytes = 16 * 1024 * 1024;
};

} // namespace hnconf

#endif
