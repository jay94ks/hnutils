#include <socketpp/tcp_listener.hpp>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace sockpp {

struct CTcpListener::Impl {
    explicit Impl(CSocket&& value) : socket(std::move(value)) {}

    CSocket socket;
};

/* Construct a closed socket. */
CTcpListener::CTcpListener() noexcept = default;

/* Adopt a protocol-checked socket. */
CTcpListener::CTcpListener(CSocket&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CTcpListener::CTcpListener(CTcpListener&& other) noexcept = default;

/* Close the owned socket. */
CTcpListener::~CTcpListener() = default;

/* Transfer socket ownership. */
CTcpListener& CTcpListener::operator=(CTcpListener&& other) noexcept = default;

/* Create an unbound IP socket. */
CTcpListener CTcpListener::open(EAddressFamily family) {
    if (family != EAF_INET && family != EAF_INET6) {
        throw std::invalid_argument("CTcpListener requires IPv4 or IPv6");
    }
    return CTcpListener(CSocket::open(family, ESOCK_STREAM, EPROTO_TCP));
}

/* Test whether the socket is open. */
bool CTcpListener::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CTcpListener::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local endpoint. */
void CTcpListener::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the local endpoint. */
CSocketAddress CTcpListener::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CTcpListener::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CTcpListener::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Start listening after bind. */
void CTcpListener::listen(int backlog) {
    state().socket.listen(backlog);
}

/* Accept a connection into a protocol-specific client. */
CTcpClient CTcpListener::accept(CSocketAddress* peer) {
    return CTcpClient(state().socket.accept(peer));
}

/* Return the implementation, rejecting closed or moved-from objects. */
CTcpListener::Impl& CTcpListener::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpListener is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CTcpListener::Impl& CTcpListener::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpListener is closed");
    }
    return *_impl;
}

} // namespace sockpp
