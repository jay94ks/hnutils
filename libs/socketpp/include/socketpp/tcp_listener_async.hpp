#ifndef __INCLUDE_SOCKETPP_TCP_LISTENER_ASYNC_HPP__
#define __INCLUDE_SOCKETPP_TCP_LISTENER_ASYNC_HPP__

#include <socketpp/tcp_client_async.hpp>

#include <memory>
#include <utility>

namespace sockpp {

/**
 * Owns a reactor-backed TCP listening socket.
 *
 * Only IPv4 and IPv6 are supported. Instances are move-only.
 * The watcher must outlive this object and every pending operation.
 * Keep the object, buffers, and address output parameters alive and do not
 * move or close the object until all its tasks have completed.
 */
class SOCKETPP_API CTcpListenerAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Construct a closed socket. */
    CTcpListenerAsync() noexcept;

    /**
     * Transfer socket ownership.
     * @param other The object to move from.
     */
    CTcpListenerAsync(CTcpListenerAsync&& other) noexcept;

    /** Destroy the object and close its socket. */
    ~CTcpListenerAsync();

    /**
     * Transfer socket ownership, closing the previous socket.
     * @param other The object to move from.
     * @return This object.
     */
    CTcpListenerAsync& operator=(CTcpListenerAsync&& other) noexcept;

    /** Socket ownership cannot be copied. */
    CTcpListenerAsync(const CTcpListenerAsync&) = delete;

    /** Socket ownership cannot be copied. */
    CTcpListenerAsync& operator=(const CTcpListenerAsync&) = delete;

public:
    /**
     * Create an unbound TCP socket.
     * @param watcher The event loop that drives asynchronous I/O.
     * @param family EAF_INET or EAF_INET6.
     * @return The open socket.
     * @throw std::invalid_argument for a non-IP address family.
     * @throw std::system_error when socket creation fails.
     */
    static CTcpListenerAsync open(CSocketWatcher& watcher, EAddressFamily family = EAF_INET);

    /** Test whether this object owns an open socket. */
    bool isOpen() const noexcept;

    /**
     * Close the socket; closing an already closed socket succeeds.
     * @return The native close error, or an empty error code on success.
     */
    std::error_code close() noexcept;

    /**
     * Bind to a local endpoint before listening.
     * @param address The local IP address and port.
     */
    void bind(const CSocketAddress& address);

    /** Return the bound local endpoint. */
    CSocketAddress localAddress() const;

    /**
     * Set a native socket option.
     * @param level The native option level.
     * @param option The native option name.
     * @param value The option value bytes.
     * @param size The value size in bytes.
     */
    void setOption(int level, int option, const void* value, std::size_t size);

    /**
     * Apply a typed option without exposing the underlying socket.
     * @tparam TOption An option type providing apply(socket).
     * @param option The option forwarded to apply().
     */
    template<typename TOption>
    inline void setOption(TOption&& option) {
        std::forward<TOption>(option).apply(*this);
    }

    /**
     * Read a native socket option.
     * @param level The native option level.
     * @param option The native option name.
     * @param value The destination for option value bytes.
     * @param size The destination capacity on input, actual size on output.
     */
    void getOption(int level, int option, void* value, std::size_t& size) const;

public:
    /**
     * Start listening after bind().
     * @param backlog The maximum pending connection count.
     */
    void listen(int backlog = 128);

    /**
     * Accept an incoming TCP connection.
     * @param peer Optional output for the remote endpoint.
     * @return A connected CTcpClientAsync using the same watcher.
     * @note peer must remain valid until the task completes.
     */
    asyncpp::task<CTcpClientAsync> accept(CSocketAddress* peer = nullptr);

private:
    /** Adopt a protocol-checked socket without exposing it to callers. */
    explicit CTcpListenerAsync(CSocketAsync&& socket);

    /** Return the implementation or report a closed/moved-from object. */
    Impl& state();

    /** Return the implementation or report a closed/moved-from object. */
    const Impl& state() const;
};

} // namespace sockpp

#endif
