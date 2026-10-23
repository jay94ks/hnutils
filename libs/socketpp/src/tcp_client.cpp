#include <socketpp/tcp_client.hpp>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace sockpp {

struct CTcpClient::Impl {
    explicit Impl(CSocket&& value) : socket(std::move(value)) {}

    CSocket socket;
};

/* Construct a closed socket. */
CTcpClient::CTcpClient() noexcept = default;

/* Adopt a protocol-checked socket. */
CTcpClient::CTcpClient(CSocket&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CTcpClient::CTcpClient(CTcpClient&& other) noexcept = default;

/* Close the owned socket. */
CTcpClient::~CTcpClient() = default;

/* Transfer socket ownership. */
CTcpClient& CTcpClient::operator=(CTcpClient&& other) noexcept = default;

/* Create an unbound IP socket. */
CTcpClient CTcpClient::open(EAddressFamily family) {
    if (family != EAF_INET && family != EAF_INET6) {
        throw std::invalid_argument("CTcpClient requires IPv4 or IPv6");
    }
    return CTcpClient(CSocket::open(family, ESOCK_STREAM, EPROTO_TCP));
}

/* Test whether the socket is open. */
bool CTcpClient::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CTcpClient::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local endpoint. */
void CTcpClient::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the local endpoint. */
CSocketAddress CTcpClient::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CTcpClient::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CTcpClient::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Connect to a remote endpoint. */
void CTcpClient::connect(const CSocketAddress& address) {
    state().socket.connect(address);
}

/* Return the connected remote endpoint. */
CSocketAddress CTcpClient::peerAddress() const {
    return state().socket.peerAddress();
}

/* Disable socket I/O in the selected direction. */
void CTcpClient::shutdown(ESocketShutdown direction) {
    state().socket.shutdown(direction);
}

/* Receive stream bytes. */
std::size_t CTcpClient::receive(const SByteSpan& buffer, int flags) {
    return state().socket.receive(buffer, flags);
}

/* Send stream bytes. */
std::size_t CTcpClient::send(const SReadOnlyByteSpan& buffer, int flags) {
    return state().socket.send(buffer, flags);
}

/* Send the entire buffer, retrying partial writes. */
void CTcpClient::sendAll(const SReadOnlyByteSpan& buffer, int flags) {
    auto& socket = state().socket;
    auto remaining = buffer;
    while (!remaining.empty()) {
        const auto sent = socket.send(remaining, flags);
        if (sent == 0) {
            throw std::system_error(std::make_error_code(std::errc::broken_pipe), "sendAll made no progress");
        }
        remaining = remaining.subspan(sent);
    }
}

/* Return the implementation, rejecting closed or moved-from objects. */
CTcpClient::Impl& CTcpClient::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpClient is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CTcpClient::Impl& CTcpClient::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpClient is closed");
    }
    return *_impl;
}

} // namespace sockpp
