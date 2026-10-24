#ifndef __INCLUDE_HNHTTP_FIELDS_HPP__
#define __INCLUDE_HNHTTP_FIELDS_HPP__

#include <hnhttp/common.hpp>

namespace hnhttp {

/** Compression hints; secrets are never indexed regardless of the default hint. */
enum EHeaderIndexing { EINDEX_DEFAULT, EINDEX_NEVER };

/** Copied field, preserving duplicates and order; HTTP/2/3 pseudo-fields are not generic headers. */
struct SHeaderField {
    std::string name;
    std::string value;
    EHeaderIndexing indexing = EINDEX_DEFAULT;
};

/** Case-insensitive HTTP field lookup; no underlying mutable container escapes. */
class IReadOnlyHttpHeaders {
public:
    /** Release this view; references returned by a message live with that message. */
    virtual ~IReadOnlyHttpHeaders() = default;

    /** Count field lines, not distinct names. */
    virtual std::size_t size() const noexcept = 0;

    /** Copy the first matching value; missing and an empty value remain distinct. */
    virtual std::optional<std::string> get(const std::string& name) const = 0;

    /** Copy a field-line page, count <= 4096, without comma-merging Set-Cookie or other fields. */
    virtual std::vector<SHeaderField> entries(std::uint64_t offset, std::size_t count = 128) const = 0;

    /** Copy matching values in original order, count <= 4096. */
    virtual std::vector<std::string> values(const std::string& name, std::uint64_t offset,
        std::size_t count = 128) const = 0;
};

/** Draft-only mutation facade; invalid local syntax throws before changing stored fields. */
class IHttpHeaders : public virtual IReadOnlyHttpHeaders {
public:
    /** Release the facade without publishing a message. */
    virtual ~IHttpHeaders() = default;

    /** Replace all matches. Reject CR/LF/NUL, pseudo-fields and serializer-managed fields. */
    virtual IHttpHeaders& set(std::string name, std::string value, EHeaderIndexing indexing = EINDEX_DEFAULT) = 0;

    /** Add a distinct field line rather than implicitly combining it. */
    virtual IHttpHeaders& add(std::string name, std::string value, EHeaderIndexing indexing = EINDEX_DEFAULT) = 0;

    /** Remove all matching lines. A frozen or managed-field mutation throws a state/value error. */
    virtual IHttpHeaders& remove(const std::string& name) = 0;
};

/** Query octets are decoded once; hasEquals distinguishes ?flag from ?flag=. */
struct SQueryField {
    std::string name;
    std::string value;
    bool hasEquals = true;
};

/** Ordered, case-sensitive query view; '+' is literal here, unlike URL-encoded form bodies. */
class IReadOnlyHttpQueries {
public:
    /** Release the view. */
    virtual ~IReadOnlyHttpQueries() = default;

    /** Count query fields, including duplicates. */
    virtual std::size_t size() const noexcept = 0;

    /** Copy the first matching decoded value. */
    virtual std::optional<std::string> get(const std::string& name) const = 0;

    /** Copy a bounded ordered page; count <= 4096. */
    virtual std::vector<SQueryField> entries(std::uint64_t offset, std::size_t count = 128) const = 0;
};

/** Outgoing query builder; URI-component escaping is separate from form-body encoding. */
class IHttpQueries : public virtual IReadOnlyHttpQueries {
public:
    /** Release the facade. */
    virtual ~IHttpQueries() = default;

    /** Replace all matching decoded names; serialize a name=value pair. */
    virtual IHttpQueries& set(std::string name, std::string value) = 0;

    /** Replace with a locale-independent decimal signed integer. */
    virtual IHttpQueries& set(std::string name, std::int64_t value) = 0;

    /** Append without deleting duplicate names. */
    virtual IHttpQueries& add(std::string name, std::string value) = 0;

    /** Append a name with no '=' delimiter. */
    virtual IHttpQueries& addBare(std::string name) = 0;

    /** Remove every exact name match. */
    virtual IHttpQueries& remove(const std::string& name) = 0;
};

} // namespace hnhttp
#endif
