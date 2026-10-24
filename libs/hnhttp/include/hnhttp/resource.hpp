#ifndef __INCLUDE_HNHTTP_RESOURCE_HPP__
#define __INCLUDE_HNHTTP_RESOURCE_HPP__

#include <hnhttp/message.hpp>

namespace hnhttp {

/** Application-supplied selected representation; validators describe the encoded bytes. */
struct SResourceMetadata {
    bool exists = true;
    std::uint64_t size = 0;
    /** Empty omits ETag; otherwise supply a quoted strong or W/ weak entity tag. */
    std::string entityTag;
    std::optional<std::chrono::sys_seconds> lastModified;
    /** Assert only when Last-Modified is a strong validator under RFC 9110 section 8.8.2.2. */
    bool strongLastModified = false;
};

/** Owned decision; callers select exactly offset/length bytes for a 200/206 body. */
struct SResourceDecision {
    std::uint16_t status = 200;
    bool sendBody = true;
    std::uint64_t offset = 0, length = 0;
    std::vector<SHeaderField> headers;
};

/** Stateless origin-server preconditions and single-byte-range selection, not a cache store. */
class HNHTTP_API CHttpResource {
public:
    /**
     * Evaluate only after routing, authorization and ordinary checks would succeed.
     * Mutating handlers must atomically compare validators and perform their update.
     * Supports If-Match/None-Match, date preconditions, If-Range and one bytes range.
     * Malformed condition lists yield 400; invalid/unknown/multiple ranges are ignored.
     * HEAD ignores Range. Missing GET/HEAD resources yield 404 unless a precondition fails;
     * other methods may proceed to application creation after successful preconditions.
     * No I/O, cache storage, ETag generation or automatic application response is performed.
     */
    static SResourceDecision evaluate(EHttpMethod method, const IReadOnlyHttpHeaders& request,
        const SResourceMetadata& resource);
};

} // namespace hnhttp
#endif
