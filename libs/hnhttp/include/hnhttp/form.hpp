#ifndef __INCLUDE_HNHTTP_FORM_HPP__
#define __INCLUDE_HNHTTP_FORM_HPP__

#include <hnhttp/body.hpp>

namespace hnhttp {

/** Form encoding is independent of HTTP version and of binary entity bodies. */
enum EFormEncoding { EFORM_MULTIPART, EFORM_URLENCODED };

/** Resource policy for outgoing preflight and incoming incremental form parsing. */
struct SFormOptions {
    std::size_t maximumParts = 1024;
    std::size_t maximumNameBytes = 4096;
    std::size_t maximumFieldBytes = 1024*1024;
    std::uint64_t maximumPartBytes = UINT64_C(16)*1024*1024*1024;
    std::size_t maximumPartHeaderBytes = 16384;
    std::size_t maximumPartHeaderFields = 32;
    std::size_t maximumPreambleBytes = 16384;
    std::size_t maximumEpilogueBytes = 16384;
    /** Empty chooses a boundary during bounded-candidate preflight; never ignore collisions. */
    std::string boundary;
    std::size_t maximumBoundaryAttempts = 32;
};

/** Ordered form container and outgoing source, preserving repeated field/file names. */
class HNHTTP_API IFormData : public virtual IHttpBodySource {
public:
    /** Release owned parts without implicitly sending them. */
    virtual ~IFormData() = default;

    /** Create a bounded multipart or URL-encoded source, without opening host files. */
    static std::shared_ptr<IFormData> create(EFormEncoding encoding = EFORM_MULTIPART,
        SFormOptions options = {});

    /** Inspect the chosen form representation. */
    virtual EFormEncoding encoding() const noexcept = 0;

    /** Append owned text/octet values; encoding operates on supplied bytes, not locale text. */
    virtual IFormData& addField(std::string name, std::string value) = 0;

    /** Append a locale-independent decimal integer field. */
    virtual IFormData& addField(std::string name, std::int64_t value) = 0;

    /**
     * Append a multipart file without opening a host path. filename is wire metadata.
     * A replayable part source is required for boundary preflight; URL-encoded forms reject files.
     */
    virtual IFormData& addFile(std::string name, std::string filename,
        std::shared_ptr<IHttpBodySource> content) = 0;
};

/** One incoming form part. Filenames are untrusted metadata, never host filesystem paths. */
class IHttpFormPart {
public:
    /** Release the part; next() still requires explicit consumption or discard. */
    virtual ~IHttpFormPart() = default;

    /** Copy the decoded field name. */
    virtual std::string name() const = 0;

    /** Distinguish an absent filename from an empty supplied filename. */
    virtual std::optional<std::string> filename() const = 0;

    /** View bounded part headers, not HTTP connection/message headers. */
    virtual const IReadOnlyHttpHeaders& headers() const = 0;

    /** Stream only this part's contents; it cannot consume a following part. */
    virtual std::shared_ptr<IHttpBodyReader> body() const = 0;
};

/** EOF means the form terminator and enclosing HTTP entity boundary have both been validated. */
struct SFormPartResult {
    SHttpError result{.code = ERET_INVALID_STATE};
    std::shared_ptr<IHttpFormPart> part;
};

/** Incremental multipart/URL-encoded reader; binary bodies bypass this interface entirely. */
class HNHTTP_API IHttpFormReader {
public:
    /** Release parser state without implicitly draining the HTTP body. */
    virtual ~IHttpFormReader() = default;

    /** Parse multipart or URL-encoded entities incrementally; unsupported content types throw. */
    static std::shared_ptr<IHttpFormReader> create(std::shared_ptr<IHttpBodyReader> body,
        std::string contentType, SFormOptions options = {});

    /** Return BODY_PENDING if the previous part remains unread; never silently skip it. */
    virtual asyncpp::task<SFormPartResult> next(SOperationOptions options = {}) = 0;

    /** Copy the latest payload-parser diagnostic; malformed form data is not necessarily a wire error. */
    virtual SHttpError lastError() const = 0;
};

} // namespace hnhttp
#endif
