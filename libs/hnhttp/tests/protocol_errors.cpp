#include "test_transport.hpp"
#include <hntls.hpp>
#include <asyncpp/defer.h>
#include <asyncpp/launch.h>

namespace {
using namespace hnhttp;
SBytes h2frame(std::uint8_t type, std::uint8_t flags, std::uint32_t id, SBytes payload) {
    SBytes bytes{static_cast<std::uint8_t>(payload.size()>>16),static_cast<std::uint8_t>(payload.size()>>8),static_cast<std::uint8_t>(payload.size()),type,flags};
    for (int shift = 24; shift >= 0; shift -= 8) { bytes.push_back(static_cast<std::uint8_t>(id>>shift)); } bytes.insert(bytes.end(),payload.begin(),payload.end()); return bytes;
}
asyncpp::task<void> writeAll(std::shared_ptr<IHttpStream> stream, SBytes bytes) {
    std::size_t at = 0; while (at < bytes.size()) {
        SBytes part(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.end()); auto result = co_await stream->writeSome(std::move(part));
        DOCTEST_REQUIRE(result.result.code == ERET_OK); at += result.acceptedBytes;
    }
}
asyncpp::task<bool> h2error(std::shared_ptr<sockpp::CSocketWatcher> watcher, std::vector<SBytes> frames, std::uint64_t expected) {
    co_await asyncpp::defer(*watcher); auto pair = testhttp::bytePair(watcher); SHttpConnectionOptions options; options.version = HTTP_2;
    auto connection = IHttpConnection::createStream(pair.first,HTTP_SERVER,options); auto engine = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    asyncpp::async_launch_scope scope; scope.launch(engine->run());
    const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"; SBytes bytes(preface.begin(),preface.end());
    for (const auto& frame : frames) { bytes.insert(bytes.end(),frame.begin(),frame.end()); }
    co_await writeAll(pair.second,std::move(bytes));
    for (unsigned i = 0; i < 2048 && connection->lastError().code == ERET_OK; ++i) { co_await asyncpp::defer(*watcher); }
    const auto error = connection->lastError(); DOCTEST_CHECK(error.scope == EERROR_CONNECTION); DOCTEST_CHECK(error.protocolError == expected);
    connection->close(); pair.second->close(); co_await scope.join(); DOCTEST_CHECK(co_await connection->waitClosed() == ERET_OK); co_return error.protocolError == expected;
}
DOCTEST_TEST_CASE("HTTP2 independent malformed frames enforce connection and compression errors") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(watcher,h2error(watcher,{h2frame(6,0,0,SBytes(8))},1)));
    DOCTEST_CHECK(testhttp::drive(watcher,h2error(watcher,{h2frame(4,0,0,{}),h2frame(6,0,0,SBytes(7))},6)));
    DOCTEST_CHECK(testhttp::drive(watcher,h2error(watcher,{h2frame(4,0,0,{}),h2frame(1,0,1,{0x82}),h2frame(6,0,0,SBytes(8))},1)));
    DOCTEST_CHECK(testhttp::drive(watcher,h2error(watcher,{h2frame(4,0,0,{}),h2frame(1,5,1,{0x80})},9)));
    DOCTEST_CHECK(testhttp::drive(watcher,h2error(watcher,{h2frame(4,0,0,{}),h2frame(8,0,0,SBytes(4))},1)));
}
asyncpp::task<bool> isolated(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); auto pair = testhttp::bytePair(watcher); SHttpConnectionOptions options; options.version = HTTP_2; options.http2.initialStreamWindow = 1024;
    auto connection = IHttpConnection::createStream(pair.first,HTTP_SERVER,options); auto engine = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    asyncpp::async_launch_scope scope; scope.launch(engine->run());
    const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"; SBytes wire(preface.begin(),preface.end());
    const SBytes fields{0x82,0x86,0x84,0x01,0x01,'x'};
    for (const auto& bytes : {h2frame(4,0,0,{}),h2frame(1,4,1,fields),h2frame(0,0,1,SBytes(1025,'x')),h2frame(1,5,3,fields)}) { wire.insert(wire.end(),bytes.begin(),bytes.end()); }
    co_await writeAll(pair.second,std::move(wire)); auto accepted = co_await engine->accept();
    DOCTEST_REQUIRE(accepted.result.code == ERET_OK); DOCTEST_CHECK(accepted.context->streamId() == 3);
    auto body = co_await accepted.context->getRequest()->getBody()->collect(0); DOCTEST_CHECK(body.result.code == ERET_OK);
    accepted.context->getResponse()->status(204); DOCTEST_CHECK(co_await accepted.context->getResponse()->exec() == ERET_OK);
    DOCTEST_CHECK(co_await accepted.context->finish() == ERET_OK); DOCTEST_CHECK(connection->lastError().code == ERET_OK);
    connection->close(); pair.second->close(); co_await scope.join(); DOCTEST_CHECK(co_await connection->waitClosed() == ERET_OK); co_return true;
}
DOCTEST_TEST_CASE("HTTP2 a stream flow error does not corrupt another independent request") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,isolated(watcher)));
}
asyncpp::task<void> nativeWrite(std::shared_ptr<hnquic::IQuicStream> stream, SBytes bytes) {
    std::size_t at = 0; while (at < bytes.size()) {
        SBytes part(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.end()); auto result = co_await stream->writeSome(std::move(part));
        DOCTEST_REQUIRE(result.result.code == hnquic::ERET_OK); at += result.acceptedBytes;
    }
}
asyncpp::task<bool> h3error(std::shared_ptr<sockpp::CSocketWatcher> watcher, SBytes bytes, bool finish, std::uint64_t expected) {
    co_await asyncpp::defer(*watcher); auto pair = testhttp::nativePair(watcher); SHttpConnectionOptions options; options.version = HTTP_3;
    auto connection = IHttpConnection::createQuic(IHttpQuicTransport::fromQuic(pair.second),HTTP_SERVER,options); auto engine = std::dynamic_pointer_cast<IHttpMultiplexedConnection>(connection);
    asyncpp::async_launch_scope scope; scope.launch(engine->run());
    auto opened = co_await pair.first->openStream(hnquic::ESTREAM_UNIDIRECTIONAL); DOCTEST_REQUIRE(opened.result.code == hnquic::ERET_OK);
    co_await nativeWrite(opened.stream,std::move(bytes)); if (finish) { DOCTEST_CHECK(co_await opened.stream->finishWrite() == hnquic::ERET_OK); }
    for (unsigned i = 0; i < 2048 && connection->lastError().code == ERET_OK; ++i) { co_await asyncpp::defer(*watcher); }
    const auto failure = connection->lastError(); DOCTEST_CHECK(failure.scope == EERROR_CONNECTION); DOCTEST_CHECK(failure.protocolError == expected);
    connection->close(); co_await scope.join(); DOCTEST_CHECK(co_await connection->waitClosed() == ERET_OK); co_return failure.protocolError == expected;
}
DOCTEST_TEST_CASE("HTTP3 independent control bytes reject duplicate settings forbidden frames and critical FIN") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>();
    DOCTEST_CHECK(testhttp::drive(watcher,h3error(watcher,{0,4,0,4,0},false,0x105)));
    DOCTEST_CHECK(testhttp::drive(watcher,h3error(watcher,{0,4,0,0,0},false,0x105)));
    DOCTEST_CHECK(testhttp::drive(watcher,h3error(watcher,{0,4,2,2,0},false,0x109)));
    DOCTEST_CHECK(testhttp::drive(watcher,h3error(watcher,{0,4,0},true,0x104)));
}
asyncpp::task<bool> nativeDummies(std::shared_ptr<sockpp::CSocketWatcher> watcher) {
    co_await asyncpp::defer(*watcher); auto pair = testhttp::bytePair(watcher);
    auto recordTransport = IHttpStream::asTlsTransport(pair.first);
    DOCTEST_CHECK_THROWS_AS(IHttpConnection::createStream(pair.first,HTTP_CLIENT),std::logic_error);
    hntls::STlsOptions policy; policy.alpn={"h3"}; auto context = hntls::ITlsContext::create(policy); auto tls = hntls::ITlsStream::create(recordTransport,context);
    DOCTEST_CHECK(tls->isImplemented()); DOCTEST_CHECK(tls->state()==hntls::ESTATE_NEW);
    DOCTEST_CHECK((co_await tls->readSome(1)).result.code == hntls::ERET_INVALID_STATE);
    DOCTEST_CHECK_THROWS_AS(IHttpStream::fromTls(tls),std::logic_error);
    auto quic = hnquic::IQuicConnection::create(watcher,context);
    DOCTEST_CHECK_THROWS_AS(IHttpQuicTransport::fromQuic(quic),std::logic_error);
    tls->close(); pair.first->close(); pair.second->close(); co_return true;
}
DOCTEST_TEST_CASE("hnhttp native adapters reject unestablished TLS QUIC and duplicate transport leases") {
    auto watcher = std::make_shared<sockpp::CSocketWatcher>(); DOCTEST_CHECK(testhttp::drive(watcher,nativeDummies(watcher)));
}
}
