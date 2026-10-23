#ifndef __INCLUDE_DAEMONIZER_COMMON_HPP__
#define __INCLUDE_DAEMONIZER_COMMON_HPP__

#if defined(_WIN32) && defined(DAEMONIZER_SHARED)
#if defined(DAEMONIZER_BUILDING_LIBRARY)
#define DAEMONIZER_API __declspec(dllexport)
#else
#define DAEMONIZER_API __declspec(dllimport)
#endif
#else
#define DAEMONIZER_API
#endif

#include <cstddef>
#include <span>

namespace daemonz {

/** Mutable byte range whose storage must survive an asynchronous read. */
using SByteSpan = std::span<std::byte>;

/** Read-only byte range whose storage must survive an asynchronous write. */
using SReadOnlyByteSpan = std::span<const std::byte>;

} // namespace daemonz

#endif
