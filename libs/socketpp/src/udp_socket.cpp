#include <socketpp/udp_socket.hpp>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace sockpp {

struct CUdpSocket::Impl {
    explicit Impl(CSocket&& value) : socket(std::move(value)) {}

    CSocket socket;
};

/* Construct a closed socket. */
CUdpSocket::CUdpSocket() noexcept = default;

/* Adopt a protocol-checked socket. */
CUdpSocket::CUdpSocket(CSocket&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CUdpSocket::CUdpSocket(CUdpSocket&& other) noexcept = default;

/* Close the owned socket. */
CUdpSocket::~CUdpSocket() = default;

/* Transfer socket ownership. */
CUdpSocket& CUdpSocket::operator=(CUdpSocket&& other) noexcept = default;

/* Create an unbound IP socket. */
CUdpSocket CUdpSocket::open(EAddressFamily family) {
    if (family != EAF_INET && family != EAF_INET6) {
        throw std::invalid_argument("CUdpSocket requires IPv4 or IPv6");
    }
    return CUdpSocket(CSocket::open(family, ESOCK_DATAGRAM, EPROTO_UDP));
}

/* Test whether the socket is open. */
bool CUdpSocket::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CUdpSocket::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local endpoint. */
void CUdpSocket::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the local endpoint. */
CSocketAddress CUdpSocket::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CUdpSocket::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CUdpSocket::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Select a datagram peer. */
void CUdpSocket::connect(const CSocketAddress& address) {
    state().socket.connect(address);
}

/* Return the connected remote endpoint. */
CSocketAddress CUdpSocket::peerAddress() const {
    return state().socket.peerAddress();
}

/* Disable socket I/O in the selected direction. */
void CUdpSocket::shutdown(ESocketShutdown direction) {
    state().socket.shutdown(direction);
}

/* Receive a datagram. */
std::size_t CUdpSocket::receive(const SByteSpan& buffer, int flags) {
    return state().socket.receive(buffer, flags);
}

/* Send a datagram. */
std::size_t CUdpSocket::send(const SReadOnlyByteSpan& buffer, int flags) {
    return state().socket.send(buffer, flags);
}

/* Receive a datagram and its source endpoint. */
std::size_t CUdpSocket::receiveFrom(
    const SByteSpan& buffer, CSocketAddress& source, int flags) {
    return state().socket.receiveFrom(buffer, source, flags);
}

/* Send exactly one datagram. */
std::size_t CUdpSocket::sendTo(
    const SReadOnlyByteSpan& buffer, const CSocketAddress& destination, int flags) {
    return state().socket.sendTo(buffer, destination, flags);
}

/* Return the implementation, rejecting closed or moved-from objects. */
CUdpSocket::Impl& CUdpSocket::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpSocket is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CUdpSocket::Impl& CUdpSocket::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpSocket is closed");
    }
    return *_impl;
}

} // namespace sockpp
