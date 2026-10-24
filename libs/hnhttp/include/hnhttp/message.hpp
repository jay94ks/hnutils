#ifndef __INCLUDE_HNHTTP_MESSAGE_HPP__
#define __INCLUDE_HNHTTP_MESSAGE_HPP__

#include <hnhttp/body.hpp>

namespace hnhttp {

/** Read-only request semantics shared by HTTP/1, HTTP/2 and HTTP/3. */
class IHttpRequest {
public:
    /** Release the facade; retained tasks/bodies keep the underlying exchange alive. */
    virtual ~IHttpRequest() = default;

    /** Inspect this request's actual protocol version. */
    virtual EHttpVersion version() const noexcept = 0;

    /** Inspect a known method, HTTP_EXTENSION, or HTTP_INVALID before an incoming head is ready. */
    virtual EHttpMethod method() const noexcept = 0;

    /** Copy the exact case-sensitive method token, including extension methods. */
    virtual std::string methodName() const = 0;

    /** Copy target semantics mapped to/from HTTP/2/3 pseudo-fields when applicable. */
    virtual SRequestTarget target() const = 0;

    /** Copy the escaped path without decoding separators, dot segments or percent triplets. */
    virtual std::string path() const = 0;

    /** Copy the original HTTP/1 target, or HTTP/2/3 :path; unavailable before preparation. */
    virtual std::string rawTarget() const = 0;

    /** View ordinary fields; pseudo-fields and framing remain serializer-controlled. */
    virtual const IReadOnlyHttpHeaders& headers() const = 0;

    /** View decoded query octets, preserving order and duplicate names. */
    virtual const IReadOnlyHttpQueries& queries() const = 0;

    /** Copy the latest exchange error. */
    virtual SHttpError lastError() const = 0;
};

/** Incoming request: the body remains streamed after parseOnce() succeeds. */
class IHttpServerRequest : public virtual IHttpRequest {
public:
    /** Release without implicit body draining. */
    virtual ~IHttpServerRequest() = default;

    /**
     * Parse one HTTP/1 header section, not the whole connection or whole body.
     * Multiplexed requests come from accept(); this call is idempotent once their head is ready.
     */
    virtual asyncpp::task<ERetCode> parseOnce(SOperationOptions options = {}) = 0;

    /** Retain the incoming entity reader; no unbounded eager body buffering is performed. */
    virtual std::shared_ptr<IHttpBodyReader> getBody() const = 0;
};

/** Bounded informational response snapshot; HTTP/2/3 have no wire reason phrase. */
struct SResponseHead {
    EHttpVersion version = HTTP_11;
    std::uint16_t status = 0;
    std::string reason;
    std::vector<SHeaderField> headers;
};

/** Optional provisional-response callback, executed on the protocol's serialized executor. */
class IHttpInformationalObserver {
public:
    /** Release callback ownership. */
    virtual ~IHttpInformationalObserver() = default;

    /**
     * Inspect 100/102/103 without blocking or reentering the exchange. The borrowed snapshot
     * expires on return; copy bounded data before posting work to another executor.
     */
    virtual void onInformational(const SResponseHead& head) = 0;
};

/** Outgoing request: mutable only until exec() atomically acquires/freeze-prepares its exchange. */
class IHttpClientRequest : public virtual IHttpRequest {
public:
    /** Release draft or sealed message state. */
    virtual ~IHttpClientRequest() = default;

    /** Keep read-only overloads visible alongside the draft setters. */
    using IHttpRequest::method;
    using IHttpRequest::path;
    using IHttpRequest::headers;
    using IHttpRequest::queries;
    using IHttpRequest::target;

    /** Select a standard method. */
    virtual IHttpClientRequest& method(EHttpMethod method) = 0;

    /** Set a valid case-sensitive extension token. */
    virtual IHttpClientRequest& method(std::string method) = 0;

    /** Set an escaped origin-form path; query data is added only through queries(). */
    virtual IHttpClientRequest& path(std::string encodedPath) = 0;

    /** Set required authority/Host semantics; this does not resolve or connect to a host. */
    virtual IHttpClientRequest& authority(std::string authority) = 0;

    /** Select an explicit target form; invalid method/version/form combinations fail before I/O. */
    virtual IHttpClientRequest& target(SRequestTarget target) = 0;

    /** Access a private-state mutation facade, not a container reference. */
    virtual IHttpHeaders& headers() = 0;

    /** Access draft query mutation. */
    virtual IHttpQueries& queries() = 0;

    /** Retain an entity source; null selects no entity. A source cannot be concurrently consumed twice. */
    virtual IHttpClientRequest& body(std::shared_ptr<IHttpBodySource> body) = 0;

    /** Set local send policy; protocol-specific framing validation happens before network output. */
    virtual IHttpClientRequest& options(SSendOptions options) = 0;

    /** Retain an observer; do not retain callbacks beyond this exchange. */
    virtual IHttpClientRequest& observer(std::shared_ptr<IHttpInformationalObserver> observer) = 0;

    /**
     * Start a supervised send and obtain final response headers, leaving its entity streamed.
     * Upload may still be active: consume the response before waiting for send completion to
     * permit full-duplex progress. The exchange owns the sender; it is never an unowned task.
     * 4xx/5xx still return OK. No redirects, automatic retries or response-body collection.
     */
    virtual asyncpp::task<ERetCode> exec(SOperationOptions options = {}) = 0;

    /** True once upload completed or Expect's final rejection safely suppressed body transmission. */
    virtual bool isSendComplete() const noexcept = 0;

    /** Join the supervised upload and observe errors after final headers; avoid blocking response consumption. */
    virtual asyncpp::task<ERetCode> waitSent(SOperationOptions options = {}) = 0;
};

/** Read-only response semantics; reason() is empty on received HTTP/2/3 responses. */
class IHttpResponse {
public:
    /** Release this facade without changing other exchanges. */
    virtual ~IHttpResponse() = default;

    /** Inspect the actual response protocol. */
    virtual EHttpVersion version() const noexcept = 0;

    /** Retrieve the status code after headers are ready. */
    virtual std::uint16_t status() const = 0;

    /** Copy the HTTP/1 reason phrase or an empty string for received HTTP/2/3. */
    virtual std::string reason() const = 0;

    /** View ordinary response field lines without joining repeated Set-Cookie fields. */
    virtual const IReadOnlyHttpHeaders& headers() const = 0;

    /** Copy the latest response diagnostic. */
    virtual SHttpError lastError() const = 0;
};

/** Incoming client response; exec() already obtains its final header section. */
class IHttpClientResponse : public virtual IHttpResponse {
public:
    /** Release without silently draining. */
    virtual ~IHttpClientResponse() = default;

    /** Retain a reader restricted to this response's framed entity. */
    virtual std::shared_ptr<IHttpBodyReader> getBody() const = 0;
};

/** Outgoing server response; status/body rules depend on the paired request semantics. */
class IHttpServerResponse : public virtual IHttpResponse {
public:
    /** Release without implicitly sending a response. */
    virtual ~IHttpServerResponse() = default;

    /** Keep read-only status and field access visible. */
    using IHttpResponse::status;
    using IHttpResponse::headers;

    /** Set a final status and its canonical HTTP/1 phrase, or an empty phrase for unknown codes. */
    virtual IHttpServerResponse& status(std::uint16_t status) = 0;

    /** Set a final status and a validated custom phrase; HTTP/2/3 do not transmit the phrase. */
    virtual IHttpServerResponse& status(std::uint16_t status, std::string reason) = 0;

    /** Access draft field mutation; serializer-managed fields cannot be injected. */
    virtual IHttpHeaders& headers() = 0;

    /** Select an outgoing entity; HEAD/204/304 and successful CONNECT obey their no-body rules. */
    virtual IHttpServerResponse& body(std::shared_ptr<IHttpBodySource> body) = 0;

    /** Set send policy without changing the paired request or negotiated protocol. */
    virtual IHttpServerResponse& options(SSendOptions options) = 0;

    /** Emit a bounded 1xx response other than 101, without sealing the final response. */
    virtual asyncpp::task<ERetCode> sendInformational(std::uint16_t status,
        std::vector<SHeaderField> headers = {}, SOperationOptions options = {}) = 0;

    /** Select HTTP/1.1 protocol upgrade; unsupported for HTTP/2/3. */
    virtual IHttpServerResponse& acceptUpgrade(std::string protocol) = 0;

    /** Send and finish this response; outbound entity errors remain visible to the caller. */
    virtual asyncpp::task<ERetCode> exec(SOperationOptions options = {}) = 0;
};

} // namespace hnhttp
#endif
