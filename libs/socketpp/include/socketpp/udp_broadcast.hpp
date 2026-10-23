#ifndef __INCLUDE_SOCKETPP_UDP_BROADCAST_HPP__
#define __INCLUDE_SOCKETPP_UDP_BROADCAST_HPP__

#include <socketpp/udp_socket.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace sockpp {

/**
 * Owns an IPv4 UDP socket with broadcasting enabled on creation.
 *
 * send() targets 255.255.255.255; sendTo() accepts a subnet broadcast or
 * unicast endpoint. Routing, interface selection, and broadcast delivery
 * remain subject to the operating system and network configuration.
 */
class SOCKETPP_API CUdpBroadcast {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Construct a closed broadcast socket. */
    CUdpBroadcast() noexcept;

    /**
     * Transfer socket ownership.
     * @param other The object to move from.
     */
    CUdpBroadcast(CUdpBroadcast&& other) noexcept;

    /** Close the owned broadcast socket. */
    ~CUdpBroadcast();

    /**
     * Transfer ownership, closing the previous socket.
     * @param other The object to move from.
     * @return This object.
     */
    CUdpBroadcast& operator=(CUdpBroadcast&& other) noexcept;

    /** Socket ownership cannot be copied. */
    CUdpBroadcast(const CUdpBroadcast&) = delete;

    /** Socket ownership cannot be copied. */
    CUdpBroadcast& operator=(const CUdpBroadcast&) = delete;

public:
    /**
     * Create an unbound IPv4 socket and enable SO_BROADCAST.
     * @return The broadcast-enabled socket.
     * @throw std::system_error when creation or enabling broadcasts fails.
     */
    static CUdpBroadcast open();

    /** Test whether this object owns an open socket. */
    bool isOpen() const noexcept;

    /**
     * Close the socket; closing an already closed socket succeeds.
     * @return The close error, or an empty error code on success.
     */
    std::error_code close() noexcept;

    /**
     * Bind an IPv4 endpoint for sending and receiving.
     * @param address A local endpoint; anyIpv4(port) receives on all interfaces.
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
     * @param value The destination for option bytes.
     * @param size The destination capacity on input, actual size on output.
     */
    void getOption(int level, int option, void* value, std::size_t& size) const;

public:
    /**
     * Send one datagram to the limited broadcast address 255.255.255.255.
     * @param buffer The source buffer.
     * @param port The destination UDP port.
     * @param flags Native send flags.
     * @return The byte count; datagrams are never split into multiple sends.
     */
    std::size_t send(const SReadOnlyByteSpan& buffer, std::uint16_t port, int flags = 0);

    /**
     * Send one datagram to a directed broadcast or unicast endpoint.
     * @param buffer The source buffer.
     * @param destination The destination IPv4 address and port.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     */
    std::size_t sendTo(
        const SReadOnlyByteSpan& buffer,
        const CSocketAddress& destination,
        int flags = 0);

    /**
     * Receive one datagram, including unicast replies.
     * @param buffer The destination buffer.
     * @param source Receives the sender endpoint.
     * @param flags Native receive flags.
     * @return The byte count; zero is a valid empty datagram.
     */
    std::size_t receiveFrom(
        const SByteSpan& buffer, CSocketAddress& source, int flags = 0);

private:
    /** Adopt a broadcast-enabled IPv4 UDP socket. */
    explicit CUdpBroadcast(CUdpSocket&& socket);

    /** Return the implementation or report a closed/moved-from object. */
    Impl& state();

    /** Return the implementation or report a closed/moved-from object. */
    const Impl& state() const;
};

} // namespace sockpp

#endif
