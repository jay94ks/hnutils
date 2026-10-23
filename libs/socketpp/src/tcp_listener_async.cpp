#include <socketpp/tcp_listener_async.hpp>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace sockpp {

struct CTcpListenerAsync::Impl {
    explicit Impl(CSocketAsync&& value) : socket(std::move(value)) {}

    CSocketAsync socket;
};

/* Construct a closed socket. */
CTcpListenerAsync::CTcpListenerAsync() noexcept = default;

/* Adopt a protocol-checked socket. */
CTcpListenerAsync::CTcpListenerAsync(CSocketAsync&& socket)
    : _impl(std::make_unique<Impl>(std::move(socket))) {}

/* Transfer socket ownership. */
CTcpListenerAsync::CTcpListenerAsync(CTcpListenerAsync&& other) noexcept = default;

/* Close the owned socket. */
CTcpListenerAsync::~CTcpListenerAsync() = default;

/* Transfer socket ownership. */
CTcpListenerAsync& CTcpListenerAsync::operator=(CTcpListenerAsync&& other) noexcept = default;

/* Create an unbound IP socket. */
CTcpListenerAsync CTcpListenerAsync::open(CSocketWatcher& watcher, EAddressFamily family) {
    if (family != EAF_INET && family != EAF_INET6) {
        throw std::invalid_argument("CTcpListenerAsync requires IPv4 or IPv6");
    }
    return CTcpListenerAsync(CSocketAsync::open(watcher, family, ESOCK_STREAM, EPROTO_TCP));
}

/* Test whether the socket is open. */
bool CTcpListenerAsync::isOpen() const noexcept {
    return _impl && _impl->socket.isOpen();
}

/* Close the owned socket. */
std::error_code CTcpListenerAsync::close() noexcept {
    return _impl ? _impl->socket.close() : std::error_code{};
}

/* Bind a local endpoint. */
void CTcpListenerAsync::bind(const CSocketAddress& address) {
    state().socket.bind(address);
}

/* Return the local endpoint. */
CSocketAddress CTcpListenerAsync::localAddress() const {
    return state().socket.localAddress();
}

/* Set a native socket option. */
void CTcpListenerAsync::setOption(int level, int option, const void* value, std::size_t size) {
    state().socket.setOption(level, option, value, size);
}

/* Read a native socket option. */
void CTcpListenerAsync::getOption(int level, int option, void* value, std::size_t& size) const {
    state().socket.getOption(level, option, value, size);
}

/* Start listening after bind. */
void CTcpListenerAsync::listen(int backlog) {
    state().socket.listen(backlog);
}

/* Accept a connection into a protocol-specific client. */
asyncpp::task<CTcpClientAsync> CTcpListenerAsync::accept(CSocketAddress* peer) {
    co_return CTcpClientAsync(co_await state().socket.accept(peer));
}

/* Return the implementation, rejecting closed or moved-from objects. */
CTcpListenerAsync::Impl& CTcpListenerAsync::state() {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpListenerAsync is closed");
    }
    return *_impl;
}

/* Return the implementation, rejecting closed or moved-from objects. */
const CTcpListenerAsync::Impl& CTcpListenerAsync::state() const {
    if (!isOpen()) {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor), "CTcpListenerAsync is closed");
    }
    return *_impl;
}

} // namespace sockpp
