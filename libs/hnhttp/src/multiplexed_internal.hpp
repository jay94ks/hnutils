#ifndef __SRC_HNHTTP_MULTIPLEXED_INTERNAL_HPP__
#define __SRC_HNHTTP_MULTIPLEXED_INTERNAL_HPP__
#include "internal.hpp"
#include <asyncpp/event.h>
#include <asyncpp/mutex.h>
#include <deque>
#include <map>
#include <atomic>

namespace hnhttp::detail::mux {
struct SConnection;
struct SExchange;
/** Wire engines retain no public facade and keep transport-specific state private. */
class IWire {
public:
    virtual ~IWire() = default;
    virtual asyncpp::task<ERetCode> run(std::shared_ptr<SConnection> state) = 0;
    virtual asyncpp::task<ERetCode> open(std::shared_ptr<SExchange> exchange) = 0;
    virtual asyncpp::task<ERetCode> headers(std::shared_ptr<SExchange> exchange, std::vector<SHeaderField> fields, bool end) = 0;
    virtual asyncpp::task<ERetCode> data(std::shared_ptr<SExchange> exchange, SBytes bytes, bool end) = 0;
    virtual void consumed(const std::shared_ptr<SExchange>& exchange, std::size_t bytes) = 0;
    virtual void reset(const std::shared_ptr<SExchange>& exchange, std::uint64_t code) = 0;
    virtual void stop(std::uint64_t code) noexcept = 0;
    virtual asyncpp::task<ERetCode> join(SOperationOptions options) = 0;
    virtual asyncpp::task<ERetCode> goaway(std::shared_ptr<SConnection> state) = 0;
    virtual STransportSecurity security() const = 0;
    virtual std::vector<SProtocolSetting> settings() const = 0;
    virtual asyncpp::task<ERetCode> update(std::vector<SProtocolSetting>, SOperationOptions) = 0;
    virtual asyncpp::task<ERetCode> ping(std::array<std::uint8_t,8>, SOperationOptions) = 0;
    virtual SHttp2FlowState flow(std::optional<std::uint32_t>) const = 0;
};
struct SConnection {
    SHttpConnectionOptions options;
    EHttpRole role = HTTP_CLIENT;
    std::shared_ptr<asyncpp::dispatcher> executor;
    std::shared_ptr<IWire> wire;
    std::weak_ptr<IHttpConnection> facade;
    std::map<std::uint64_t,std::weak_ptr<SExchange>> streams;
    std::deque<std::shared_ptr<SExchange>> incoming;
    std::optional<SGoawayInfo> goaway;
    SHttpError failure;
    std::atomic<bool> closed{false};
    bool running = false, ready = false, draining = false, accepting = false;
    std::size_t buffered = 0, reservedReads = 0, uploads = 0, operations = 0, bytes = 0;
    std::uint64_t highestPeer = 0;
    bool hasPeerStream = false;
    asyncpp::multi_consumer_event changed, stopped{true}, uploadDone{true};
};
struct SExchange {
    std::shared_ptr<SConnection> c;
    std::shared_ptr<IHttpConnection> connection;
    std::optional<std::uint64_t> id;
    EContextState state = ECONTEXT_DRAFT;
    Headers requestHeaders, responseHeaders, trailers;
    Queries queries;
    SRequestTarget target;
    std::string method = "GET", rawPath = "/";
    std::uint16_t status = 200;
    SSendOptions sendOptions;
    std::shared_ptr<IHttpBodySource> source;
    std::shared_ptr<IHttpInformationalObserver> observer;
    SHttpError failure;
    std::deque<SBytes> body;
    SHttpError sendFailure;
    std::unique_ptr<IHttpQuicStream> quic;
    asyncpp::mutex sendMutex;
    std::size_t offset = 0, buffered = 0, informational = 0;
    std::uint64_t received = 0, frames = 0;
    std::optional<std::uint64_t> expected;
    bool head = false, receivedEnd = false, sentEnd = false, sealed = false, reading = false, executing = false, uploading = false, noBody = false, receivedTrailers = false;
    asyncpp::multi_consumer_event changed, sent;
};
struct SLease {
    std::shared_ptr<SExchange> e;
    ~SLease();
};
ERetCode entry(const std::shared_ptr<SConnection>& c, const SOperationOptions& options);
void fail(const std::shared_ptr<SConnection>& c, ERetCode code, std::uint64_t protocol, std::string message = {});
void streamFail(const std::shared_ptr<SExchange>& e, ERetCode code, std::uint64_t protocol);
asyncpp::task<void> budget(std::shared_ptr<SConnection> c, std::size_t bytes);
std::shared_ptr<SExchange> incoming(const std::shared_ptr<SConnection>& c, std::uint64_t id);
ERetCode receiveHeaders(const std::shared_ptr<SExchange>& e, std::vector<SHeaderField> fields, bool end);
ERetCode receiveData(const std::shared_ptr<SExchange>& e, SBytes bytes, bool end);
std::shared_ptr<IHttpServerContext> serverContext(const std::shared_ptr<SExchange>& e);
std::shared_ptr<IHttpClientContext> clientContext(std::shared_ptr<IHttpConnection> connection);
std::shared_ptr<IHttpConnection> h2(std::shared_ptr<IHttpStream> stream, EHttpRole role, SHttpConnectionOptions options);
std::shared_ptr<IHttpConnection> h3(std::unique_ptr<IHttpQuicTransport> transport, EHttpRole role, SHttpConnectionOptions options);
std::shared_ptr<IHttpConnection> facade(const std::shared_ptr<SConnection>& state);
void validate(const SHttpConnectionOptions& options);
}
#endif
