#include <socketpp/udp_broadcast_async.hpp>

#include <socketpp/options.hpp>

#include <system_error>
#include <utility>

namespace sockpp {

struct CUdpBroadcastAsync::Impl {
    explicit Impl(CUdpSocketAsync&& value) : socket(std::move(value)) {}

    CUdpSocketAsync socket;
};

/* Construct a closed broadcast socket. */
CUdpBroadcastAsync::CUdpBroadcastAsync() noexcept = default;

/* Adopt a broadcast-enabled socket. */
CUdpBroadcastAsync::CUdpBroadcastAsync(CUdpSocketAsync&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CUdpBroadcastAsync::CUdpBroadcastAsync(CUdpBroadcastAsync&& other) noexcept = default;

/* Close the owned socket. */
CUdpBroadcastAsync::~CUdpBroadcastAsync() = default;

/* Transfer socket ownership. */
CUdpBroadcastAsync& CUdpBroadcastAsync::operator=(CUdpBroadcastAsync&& other) noexcept = default;

/* Create an IPv4 socket and enable broadcasting. */
CUdpBroadcastAsync CUdpBroadcastAsync::open(CSocketWatcher& watcher) {
    auto socket = CUdpSocketAsync::open(watcher, EAF_INET);
    socket.setOption(opts::SBroadcast{true});
    return CUdpBroadcastAsync(std::move(socket));
}

/* Test whether the socket is open. */
bool CUdpBroadcastAsync::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CUdpBroadcastAsync::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local IPv4 endpoint. */
void CUdpBroadcastAsync::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the bound local endpoint. */
CSocketAddress CUdpBroadcastAsync::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CUdpBroadcastAsync::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CUdpBroadcastAsync::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Send a single limited-broadcast datagram. */
asyncpp::task<std::size_t> CUdpBroadcastAsync::send(SReadOnlyByteSpan buffer, std::uint16_t port, int flags) {
    return state().socket.sendTo(buffer, CSocketAddress::parse("255.255.255.255", port), flags);
}

/* Send a single directed-broadcast or unicast datagram. */
asyncpp::task<std::size_t> CUdpBroadcastAsync::sendTo(
    SReadOnlyByteSpan buffer, CSocketAddress destination, int flags) {
    return state().socket.sendTo(buffer, std::move(destination), flags);
}

/* Receive a datagram and its sender endpoint. */
asyncpp::task<std::size_t> CUdpBroadcastAsync::receiveFrom(
    SByteSpan buffer, CSocketAddress& source, int flags) {
    return state().socket.receiveFrom(buffer, source, flags);
}

/* Return the implementation, rejecting closed or moved-from objects. */
CUdpBroadcastAsync::Impl& CUdpBroadcastAsync::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpBroadcastAsync is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CUdpBroadcastAsync::Impl& CUdpBroadcastAsync::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CUdpBroadcastAsync is closed");
    }
    return *_impl;
}

} // namespace sockpp
