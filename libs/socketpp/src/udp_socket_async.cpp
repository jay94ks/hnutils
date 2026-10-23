#include <socketpp/udp_socket_async.hpp>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace sockpp {

struct CUdpSocketAsync::Impl {
    explicit Impl(CSocketAsync&& value) : socket(std::move(value)) {}

    CSocketAsync socket;
};

/* Construct a closed socket. */
CUdpSocketAsync::CUdpSocketAsync() noexcept = default;

/* Adopt a protocol-checked socket. */
CUdpSocketAsync::CUdpSocketAsync(CSocketAsync&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CUdpSocketAsync::CUdpSocketAsync(CUdpSocketAsync&& other) noexcept = default;

/* Close the owned socket. */
CUdpSocketAsync::~CUdpSocketAsync() = default;

/* Transfer socket ownership. */
CUdpSocketAsync& CUdpSocketAsync::operator=(CUdpSocketAsync&& other) noexcept = default;

/* Create an unbound IP socket. */
CUdpSocketAsync CUdpSocketAsync::open(CSocketWatcher& watcher, EAddressFamily family) {
    if (family != EAF_INET && family != EAF_INET6) {
        throw std::invalid_argument("CUdpSocketAsync requires IPv4 or IPv6");
    }
    return CUdpSocketAsync(CSocketAsync::open(watcher, family, ESOCK_DATAGRAM, EPROTO_UDP));
}

/* Test whether the socket is open. */
bool CUdpSocketAsync::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CUdpSocketAsync::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local endpoint. */
void CUdpSocketAsync::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the local endpoint. */
CSocketAddress CUdpSocketAsync::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CUdpSocketAsync::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CUdpSocketAsync::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Select a datagram peer. */
asyncpp::task<void> CUdpSocketAsync::connect(CSocketAddress address) {
    return state().socket.connect(std::move(address));
}

/* Return the connected remote endpoint. */
CSocketAddress CUdpSocketAsync::peerAddress() const {
    return state().socket.peerAddress();
}

/* Disable socket I/O in the selected direction. */
void CUdpSocketAsync::shutdown(ESocketShutdown direction) {
    state().socket.shutdown(direction);
}

/* Receive a datagram. */
asyncpp::task<std::size_t> CUdpSocketAsync::receive(SByteSpan buffer, int flags) {
    return state().socket.receive(buffer, flags);
}

/* Send a datagram. */
asyncpp::task<std::size_t> CUdpSocketAsync::send(SReadOnlyByteSpan buffer, int flags) {
    return state().socket.send(buffer, flags);
}

/* Receive a datagram and its source endpoint. */
asyncpp::task<std::size_t> CUdpSocketAsync::receiveFrom(
    SByteSpan buffer, CSocketAddress& source, int flags) {
    return state().socket.receiveFrom(buffer, source, flags);
}

/* Send exactly one datagram. */
asyncpp::task<std::size_t> CUdpSocketAsync::sendTo(
    SReadOnlyByteSpan buffer, CSocketAddress destination, int flags) {
    return state().socket.sendTo(buffer, std::move(destination), flags);
}

/* Return the implementation, rejecting closed or moved-from objects. */
CUdpSocketAsync::Impl& CUdpSocketAsync::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpSocketAsync is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CUdpSocketAsync::Impl& CUdpSocketAsync::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpSocketAsync is closed");
    }
    return *_impl;
}

} // namespace sockpp
