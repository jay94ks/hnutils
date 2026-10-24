#include <hnhttp.hpp>
#include <hntls/stream.hpp>
#include <hnquic/connection.hpp>

namespace hnhttp_examples {
/** Connect already-confirmed native QUIC; the caller supervises native and HTTP run tasks. */
std::shared_ptr<hnhttp::IHttpConnection> bindNativeQuic(std::shared_ptr<hnquic::IQuicConnection> native, hnhttp::EHttpRole role) {
    hnhttp::SHttpConnectionOptions options; options.version = hnhttp::HTTP_3;
    return hnhttp::IHttpConnection::createQuic(hnhttp::IHttpQuicTransport::fromQuic(std::move(native)),role,options);
}
/** Bind already-handshaken hntls application bytes; no TLS logic is implemented by hnhttp. */
std::shared_ptr<hnhttp::IHttpConnection> bindNativeTls(std::shared_ptr<hntls::ITlsStream> native, hnhttp::EHttpRole role, hnhttp::EHttpVersion version) {
    hnhttp::SHttpConnectionOptions options; options.version = version;
    return hnhttp::IHttpConnection::createStream(hnhttp::IHttpStream::fromTls(std::move(native)),role,options);
}
}
