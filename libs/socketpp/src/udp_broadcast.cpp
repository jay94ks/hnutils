#include <socketpp/udp_broadcast.hpp>

#include <socketpp/options.hpp>

#include <system_error>
#include <utility>

namespace sockpp {

struct CUdpBroadcast::Impl {
    explicit Impl(CUdpSocket&& value) : socket(std::move(value)) {}

    CUdpSocket socket;
};

/* Construct a closed broadcast socket. */
CUdpBroadcast::CUdpBroadcast() noexcept = default;

/* Adopt a broadcast-enabled socket. */
CUdpBroadcast::CUdpBroadcast(CUdpSocket&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CUdpBroadcast::CUdpBroadcast(CUdpBroadcast&& other) noexcept = default;

/* Close the owned socket. */
CUdpBroadcast::~CUdpBroadcast() = default;

/* Transfer socket ownership. */
CUdpBroadcast& CUdpBroadcast::operator=(CUdpBroadcast&& other) noexcept = default;

/* Create an IPv4 socket and enable broadcasting. */
CUdpBroadcast CUdpBroadcast::open() {
    auto socket = CUdpSocket::open(EAF_INET);
    socket.setOption(opts::SBroadcast{true});
    return CUdpBroadcast(std::move(socket));
}

/* Test whether the socket is open. */
bool CUdpBroadcast::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CUdpBroadcast::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local IPv4 endpoint. */
void CUdpBroadcast::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the bound local endpoint. */
CSocketAddress CUdpBroadcast::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CUdpBroadcast::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CUdpBroadcast::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Send a single limited-broadcast datagram. */
std::size_t CUdpBroadcast::send(const SReadOnlyByteSpan& buffer, std::uint16_t port, int flags) {
    return state().socket.sendTo(buffer, CSocketAddress::parse("255.255.255.255", port), flags);
}

/* Send a single directed-broadcast or unicast datagram. */
std::size_t CUdpBroadcast::sendTo(
    const SReadOnlyByteSpan& buffer, const CSocketAddress& destination, int flags) {
    return state().socket.sendTo(buffer, destination, flags);
}

/* Receive a datagram and its sender endpoint. */
std::size_t CUdpBroadcast::receiveFrom(
    const SByteSpan& buffer, CSocketAddress& source, int flags) {
    return state().socket.receiveFrom(buffer, source, flags);
}

/* Return the implementation, rejecting closed or moved-from objects. */
CUdpBroadcast::Impl& CUdpBroadcast::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpBroadcast is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CUdpBroadcast::Impl& CUdpBroadcast::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpBroadcast is closed");
    }
    return *_impl;
}

} // namespace sockpp
