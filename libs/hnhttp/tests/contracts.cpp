#include <hnhttp.hpp>
#include <doctest/doctest.h>
#include <type_traits>
#include <utility>

namespace {
using namespace hnhttp;
static_assert(std::is_abstract_v<IHttpClientContext> && std::is_abstract_v<IHttpServerContext>);
static_assert(std::is_abstract_v<IHttp2Connection> && std::is_abstract_v<IHttp3Connection>);
static_assert(!std::is_base_of_v<IHttp1Connection, IHttp2Connection>);
static_assert(!std::is_base_of_v<IAsyncByteChannel, IHttpQuicTransport>);
static_assert(!std::is_base_of_v<IHttpHeaders, IHttpServerRequest>);
static_assert(!std::is_copy_constructible_v<SQuicStreamResult>);
static_assert(!std::is_copy_constructible_v<SStreamHandoff>);
static_assert(std::is_move_constructible_v<SStreamHandoff>);
static_assert(std::is_same_v<decltype(SStreamHandoff::stream), std::shared_ptr<IHttpStream>>);
static_assert(std::is_same_v<decltype(&IHttpContext::setStream),
    ERetCode (IHttpContext::*)(std::shared_ptr<IHttpStream>)>);
static_assert(std::is_same_v<decltype(&IHttpConnection::createStream),
    std::shared_ptr<IHttpConnection> (*)(std::shared_ptr<IHttpStream>, EHttpRole, SHttpConnectionOptions)>);
static_assert(std::is_same_v<decltype(std::declval<IAsyncByteChannel&>().executor()),
    std::shared_ptr<asyncpp::dispatcher>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpQuicTransport&>().executor()),
    std::shared_ptr<asyncpp::dispatcher>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpConnection&>().executor()),
    std::shared_ptr<asyncpp::dispatcher>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpConnection&>().waitClosed()), asyncpp::task<ERetCode>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpStream&>().waitClosed()), asyncpp::task<ERetCode>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpQuicTransport&>().waitClosed()), asyncpp::task<ERetCode>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpClientRequest&>().queries().set("page", 1)), IHttpQueries&>);
static_assert(std::is_same_v<decltype(std::declval<IHttpClientRequest&>().headers().set("X-Test", "ok")), IHttpHeaders&>);
static_assert(std::is_same_v<decltype(std::declval<IHttpServerResponse&>().status(404)), IHttpServerResponse&>);
static_assert(std::is_same_v<decltype(std::declval<IHttpServerResponse&>().status(404, "Not Found")), IHttpServerResponse&>);
static_assert(std::is_same_v<decltype(std::declval<const IHttpServerRequest&>().headers()), const IReadOnlyHttpHeaders&>);
static_assert(std::is_same_v<decltype(std::declval<IHttpClientRequest&>().exec()), asyncpp::task<ERetCode>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpClientRequest&>().waitSent()), asyncpp::task<ERetCode>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpServerRequest&>().parseOnce()), asyncpp::task<ERetCode>>);
static_assert(std::is_same_v<decltype(std::declval<IHttpContext&>().streamId()), std::optional<std::uint64_t>>);
static_assert(std::is_same_v<decltype(std::declval<STransportSecurity>().peerCertificates)::value_type,
    std::shared_ptr<const certpp::x509::CCert>>);

DOCTEST_TEST_CASE("interface defaults are bounded and do not silently discard bodies") {
    SHttpConnectionOptions options;
    DOCTEST_CHECK(options.version == HTTP_11);
    DOCTEST_CHECK(options.limits.ioChunkBytes <= options.limits.maximumReadAheadBytes);
    DOCTEST_CHECK(options.limits.maximumStreamBufferedBytes <= options.limits.maximumConnectionBufferedBytes);
    DOCTEST_CHECK(options.http2.initialStreamWindow == 65535);
    DOCTEST_CHECK(options.http2.maximumFrameSize == 16384);
    DOCTEST_CHECK_FALSE(SFinishOptions{}.discardUnreadBody);
    DOCTEST_CHECK_FALSE(SSendOptions{}.expectContinue);
    DOCTEST_CHECK(SReadResult{}.result.code == ERET_INVALID_STATE);
    DOCTEST_CHECK(SWriteResult{}.result.code == ERET_INVALID_STATE);
    DOCTEST_CHECK(SAcceptResult{}.result.code == ERET_INVALID_STATE);
    DOCTEST_CHECK(options.scheduling.maximumOperationsPerTurn > 0);
    DOCTEST_CHECK(options.scheduling.maximumBytesPerTurn > 0);
    DOCTEST_CHECK_FALSE(SAsyncCapabilities{}.cancellation);
    DOCTEST_CHECK_FALSE(SAsyncCapabilities{}.deadlines);
    DOCTEST_CHECK(ERET_WRONG_EXECUTOR != ERET_NOT_RUNNING);
}

DOCTEST_TEST_CASE("security and unknown length defaults make no implicit trust or framing claims") {
    STransportSecurity security;
    DOCTEST_CHECK_FALSE(security.tls);
    DOCTEST_CHECK(security.alpn.empty());
    DOCTEST_CHECK(security.peerCertificates.empty());
    DOCTEST_CHECK(security.peerAuthentication == EPEER_NOT_CHECKED);
    DOCTEST_CHECK_FALSE(SBodyMetadata{}.contentLength.has_value());
    DOCTEST_CHECK_FALSE(SBodyMetadata{}.replayable);
    DOCTEST_CHECK_FALSE(SOperationOptions{}.deadline.has_value());
}

DOCTEST_TEST_CASE("stream identifiers and error scopes distinguish all four protocol versions") {
    static_assert(HTTP_10 != HTTP_11 && HTTP_11 != HTTP_2 && HTTP_2 != HTTP_3);
    SHttpError error;
    DOCTEST_CHECK(error.code == ERET_OK);
    DOCTEST_CHECK_FALSE(error.streamId.has_value());
    error.streamId = 0; // Valid HTTP/3 client-initiated bidirectional stream, not 'no stream'.
    DOCTEST_CHECK(error.streamId.has_value());
    error.scope = EERROR_STREAM;
    error.protocolError = 0x10c;
    DOCTEST_CHECK(error.scope != EERROR_CONNECTION);
    DOCTEST_CHECK(error.retry == ERETRY_UNKNOWN);
}
} // namespace
