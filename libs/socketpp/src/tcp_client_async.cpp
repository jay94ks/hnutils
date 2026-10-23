#include <socketpp/tcp_client_async.hpp>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace sockpp {

struct CTcpClientAsync::Impl {
    explicit Impl(CSocketAsync&& value) : socket(std::move(value)) {}

    CSocketAsync socket;
};

/* Construct a closed socket. */
CTcpClientAsync::CTcpClientAsync() noexcept = default;

/* Adopt a protocol-checked socket. */
CTcpClientAsync::CTcpClientAsync(CSocketAsync&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CTcpClientAsync::CTcpClientAsync(CTcpClientAsync&& other) noexcept = default;

/* Close the owned socket. */
CTcpClientAsync::~CTcpClientAsync() = default;

/* Transfer socket ownership. */
CTcpClientAsync& CTcpClientAsync::operator=(CTcpClientAsync&& other) noexcept = default;

/* Create an unbound IP socket. */
CTcpClientAsync CTcpClientAsync::open(CSocketWatcher& watcher, EAddressFamily family) {
    if (family != EAF_INET && family != EAF_INET6) {
        throw std::invalid_argument("CTcpClientAsync requires IPv4 or IPv6");
    }
    return CTcpClientAsync(CSocketAsync::open(watcher, family, ESOCK_STREAM, EPROTO_TCP));
}

/* Test whether the socket is open. */
bool CTcpClientAsync::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CTcpClientAsync::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local endpoint. */
void CTcpClientAsync::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the local endpoint. */
CSocketAddress CTcpClientAsync::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CTcpClientAsync::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CTcpClientAsync::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Connect to a remote endpoint. */
asyncpp::task<void> CTcpClientAsync::connect(CSocketAddress address) {
    return state().socket.connect(std::move(address));
}

/* Return the connected remote endpoint. */
CSocketAddress CTcpClientAsync::peerAddress() const {
    return state().socket.peerAddress();
}

/* Disable socket I/O in the selected direction. */
void CTcpClientAsync::shutdown(ESocketShutdown direction) {
    state().socket.shutdown(direction);
}

/* Receive stream bytes. */
asyncpp::task<std::size_t> CTcpClientAsync::receive(SByteSpan buffer, int flags) {
    return state().socket.receive(buffer, flags);
}

/* Send stream bytes. */
asyncpp::task<std::size_t> CTcpClientAsync::send(SReadOnlyByteSpan buffer, int flags) {
    return state().socket.send(buffer, flags);
}

/* Send the entire buffer, retrying partial writes. */
asyncpp::task<void> CTcpClientAsync::sendAll(SReadOnlyByteSpan buffer, int flags) {
    return state().socket.sendAll(buffer, flags);
}

/* Return the implementation, rejecting closed or moved-from objects. */
CTcpClientAsync::Impl& CTcpClientAsync::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpClientAsync is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CTcpClientAsync::Impl& CTcpClientAsync::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpClientAsync is closed");
    }
    return *_impl;
}

} // namespace sockpp
