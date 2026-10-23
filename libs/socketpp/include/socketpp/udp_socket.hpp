#ifndef __INCLUDE_SOCKETPP_UDP_SOCKET_HPP__
#define __INCLUDE_SOCKETPP_UDP_SOCKET_HPP__

#include <socketpp/socket.hpp>

#include <memory>
#include <utility>

namespace sockpp {

/**
 * Owns a synchronous UDP datagram socket.
 *
 * Only IPv4 and IPv6 are supported. Instances are move-only.
 */
class SOCKETPP_API CUdpSocket {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Construct a closed socket. */
    CUdpSocket() noexcept;

    /**
     * Transfer socket ownership.
     * @param other The object to move from.
     */
    CUdpSocket(CUdpSocket&& other) noexcept;

    /** Destroy the object and close its socket. */
    ~CUdpSocket();

    /**
     * Transfer socket ownership, closing the previous socket.
     * @param other The object to move from.
     * @return This object.
     */
    CUdpSocket& operator=(CUdpSocket&& other) noexcept;

    /** Socket ownership cannot be copied. */
    CUdpSocket(const CUdpSocket&) = delete;

    /** Socket ownership cannot be copied. */
    CUdpSocket& operator=(const CUdpSocket&) = delete;

public:
    /**
     * Create an unbound UDP socket.
     * @param family EAF_INET or EAF_INET6.
     * @return The open socket.
     * @throw std::invalid_argument for a non-IP address family.
     * @throw std::system_error when socket creation fails.
     */
    static CUdpSocket open(EAddressFamily family = EAF_INET);

    /** Test whether this object owns an open socket. */
    bool isOpen() const noexcept;

    /**
     * Close the socket; closing an already closed socket succeeds.
     * @return The native close error, or an empty error code on success.
     */
    std::error_code close() noexcept;

    /**
     * Bind to a local endpoint before receiving datagrams.
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
     * Select a default datagram destination and filter incoming peers.
     * @param address The remote IP address and port.
     */
    void connect(const CSocketAddress& address);

    /** Return the connected remote endpoint. */
    CSocketAddress peerAddress() const;

    /**
     * Disable socket I/O in the selected direction.
     * @param direction Sends, receives, or both.
     */
    void shutdown(ESocketShutdown direction);

    /**
     * Receive one datagram from the connected peer.
     * @param buffer The destination buffer.
     * @param flags Native receive flags.
     * @return The datagram byte count; zero is a valid empty datagram.
     */
    std::size_t receive(const SByteSpan& buffer, int flags = 0);

    /**
     * Send one datagram to the connected peer.
     * @param buffer The source buffer.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     */
    std::size_t send(const SReadOnlyByteSpan& buffer, int flags = 0);

    /**
     * Receive one datagram and its sender endpoint.
     * @param buffer The destination buffer.
     * @param source Receives the sender address.
     * @param flags Native receive flags.
     * @return The byte count; zero is a valid empty datagram.
     */
    std::size_t receiveFrom(
        const SByteSpan& buffer, CSocketAddress& source, int flags = 0);

    /**
     * Send exactly one datagram to an endpoint.
     * @param buffer The source buffer.
     * @param destination The destination IP address and port.
     * @param flags Native send flags.
     * @return The number of bytes sent; this operation never splits datagrams.
     */
    std::size_t sendTo(
        const SReadOnlyByteSpan& buffer,
        const CSocketAddress& destination,
        int flags = 0);

private:
    /** Adopt a protocol-checked socket without exposing it to callers. */
    explicit CUdpSocket(CSocket&& socket);

    /** Return the implementation or report a closed/moved-from object. */
    Impl& state();

    /** Return the implementation or report a closed/moved-from object. */
    const Impl& state() const;
};

} // namespace sockpp

#endif
