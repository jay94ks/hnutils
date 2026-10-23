#ifndef __INCLUDE_SOCKETPP_SOCKET_HPP__
#define __INCLUDE_SOCKETPP_SOCKET_HPP__

#include <socketpp/address.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace sockpp {

class CSocketAsync;
class CSocketWatcher;

/** Mutable byte range used by receive operations. */
using SByteSpan = std::span<std::byte>;

/** Read-only byte range used by send operations. */
using SReadOnlyByteSpan = std::span<const std::byte>;

/**
 * Owns a native socket and exposes synchronous BSD-style operations.
 */
class SOCKETPP_API CSocket {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /**
     * Construct a closed socket.
     */
    CSocket();

    /**
     * Move a socket.
     *
     * @param other The socket to move from.
     */
    CSocket(CSocket&& other) noexcept;

    /**
     * Destroy the socket and close its native handle.
     */
    ~CSocket();

    /**
     * Move a socket.
     *
     * @param other The socket to move from.
     * @return This socket.
     */
    CSocket& operator=(CSocket&& other) noexcept;

    CSocket(const CSocket&) = delete;
    CSocket& operator=(const CSocket&) = delete;

public:
    /**
     * Create a socket.
     *
     * @param family The address family.
     * @param type The transport type.
     * @param protocol The protocol, or EPROTO_DEFAULT for the system default.
     * @return The created socket.
     * @throw std::system_error when socket creation fails.
     */
    static CSocket open(
        EAddressFamily family,
        ESocketType type,
        ESocketProtocol protocol = EPROTO_DEFAULT);

public:
    /**
     * Test whether the socket owns a native handle.
     *
     * @return true when the socket is open.
     */
    bool isOpen() const noexcept;

    /**
     * Close the socket.
     *
     * @return An empty error code on success, otherwise the close error.
     */
    std::error_code close() noexcept;

    /**
     * Set blocking or non-blocking mode.
     *
     * @param blocking true for blocking mode and false for non-blocking mode.
     */
    void setBlocking(bool blocking);

    /**
     * Bind the socket to a local address.
     *
     * @param address The local address.
     */
    void bind(const CSocketAddress& address);

    /**
     * Mark a stream socket as a listener.
     *
     * @param backlog The maximum pending connection count.
     */
    void listen(int backlog = 128);

    /**
     * Accept one incoming connection.
     *
     * @param peer Optional destination for the peer address.
     * @return The connected socket.
     */
    CSocket accept(CSocketAddress* peer = nullptr);

    /**
     * Connect the socket to a remote address.
     *
     * @param address The remote address.
     */
    void connect(const CSocketAddress& address);

    /**
     * Disable sends, receives, or both directions.
     *
     * @param direction The direction to disable.
     */
    void shutdown(ESocketShutdown direction);

    /**
     * Receive bytes from a connected socket.
     *
     * @param buffer The destination buffer.
     * @param flags Native receive flags.
     * @return The number of bytes received; zero reports an orderly EOF.
     */
    std::size_t receive(const SByteSpan& buffer, int flags = 0);

    /**
     * Send bytes on a connected socket.
     *
     * @param buffer The source buffer.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     */
    std::size_t send(const SReadOnlyByteSpan& buffer, int flags = 0);

    /**
     * Receive one datagram and its source address.
     *
     * @param buffer The destination buffer.
     * @param source Receives the datagram source address.
     * @param flags Native receive flags.
     * @return The number of bytes received.
     */
    std::size_t receiveFrom(
        const SByteSpan& buffer,
        CSocketAddress& source,
        int flags = 0);

    /**
     * Send one datagram to a destination address.
     *
     * @param buffer The source buffer.
     * @param destination The datagram destination.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     */
    std::size_t sendTo(
        const SReadOnlyByteSpan& buffer,
        const CSocketAddress& destination,
        int flags = 0);

    /**
     * Return the local socket address.
     *
     * @return The bound local address.
     */
    CSocketAddress localAddress() const;

    /**
     * Return the connected peer address.
     *
     * @return The remote peer address.
     */
    CSocketAddress peerAddress() const;

    /**
     * Set a native socket option.
     *
     * @param level The native option level.
     * @param option The native option name.
     * @param value The option value bytes.
     * @param size The value size in bytes.
     */
    void setOption(int level, int option, const void* value, std::size_t size);

    /**
     * Apply a typed socket option to this socket.
     *
     * @tparam TOption An option type providing apply(socket).
     * @param option The option to forward to its apply() method.
     */
    template<typename TOption>
    inline void setOption(TOption&& option) {
        std::forward<TOption>(option).apply(*this);
    }

    /**
     * Read a native socket option.
     *
     * @param level The native option level.
     * @param option The native option name.
     * @param value The destination for option value bytes.
     * @param size On input the destination capacity; on output the value size.
     */
    void getOption(int level, int option, void* value, std::size_t& size) const;

private:
    /** Return the native handle without transferring ownership. */
    std::uintptr_t nativeHandle() const noexcept;

    /** Return the native address family value. */
    int nativeFamily() const noexcept;

    /** Return the native socket type value. */
    int nativeType() const noexcept;

    /** Return the native protocol value. */
    int nativeProtocol() const noexcept;

    /** Adopt an existing native socket handle. */
    static CSocket adopt(std::uintptr_t handle, int family, int type, int protocol);

    friend class CSocketAsync;
    friend class CSocketWatcher;
};

} // namespace sockpp

#endif
