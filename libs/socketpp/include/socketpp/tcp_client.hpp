#ifndef __INCLUDE_SOCKETPP_TCP_CLIENT_HPP__
#define __INCLUDE_SOCKETPP_TCP_CLIENT_HPP__

#include <socketpp/socket.hpp>

#include <memory>
#include <utility>

namespace sockpp {

class CTcpListener;

/**
 * Owns a synchronous TCP byte-stream socket.
 *
 * Only IPv4 and IPv6 are supported. Instances are move-only.
 */
class SOCKETPP_API CTcpClient {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Construct a closed socket. */
    CTcpClient() noexcept;

    /**
     * Transfer socket ownership.
     * @param other The object to move from.
     */
    CTcpClient(CTcpClient&& other) noexcept;

    /** Destroy the object and close its socket. */
    ~CTcpClient();

    /**
     * Transfer socket ownership, closing the previous socket.
     * @param other The object to move from.
     * @return This object.
     */
    CTcpClient& operator=(CTcpClient&& other) noexcept;

    /** Socket ownership cannot be copied. */
    CTcpClient(const CTcpClient&) = delete;

    /** Socket ownership cannot be copied. */
    CTcpClient& operator=(const CTcpClient&) = delete;

public:
    /**
     * Create an unbound TCP socket.
     * @param family EAF_INET or EAF_INET6.
     * @return The open socket.
     * @throw std::invalid_argument for a non-IP address family.
     * @throw std::system_error when socket creation fails.
     */
    static CTcpClient open(EAddressFamily family = EAF_INET);

    /** Test whether this object owns an open socket. */
    bool isOpen() const noexcept;

    /**
     * Close the socket; closing an already closed socket succeeds.
     * @return The native close error, or an empty error code on success.
     */
    std::error_code close() noexcept;

    /**
     * Bind to a local endpoint before connecting.
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
     * Connect to a remote TCP endpoint.
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
     * Receive bytes from the TCP stream.
     * @param buffer The destination buffer.
     * @param flags Native receive flags.
     * @return The byte count; zero indicates orderly EOF.
     */
    std::size_t receive(const SByteSpan& buffer, int flags = 0);

    /**
     * Send bytes to the TCP stream; a partial write is allowed.
     * @param buffer The source buffer.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     */
    std::size_t send(const SReadOnlyByteSpan& buffer, int flags = 0);

    /**
     * Send the entire buffer, retrying partial writes.
     * @param buffer The source buffer.
     * @param flags Native send flags.
     */
    void sendAll(const SReadOnlyByteSpan& buffer, int flags = 0);

private:
    /** Adopt a protocol-checked socket without exposing it to callers. */
    explicit CTcpClient(CSocket&& socket);

    /** Return the implementation or report a closed/moved-from object. */
    Impl& state();

    /** Return the implementation or report a closed/moved-from object. */
    const Impl& state() const;

    friend class CTcpListener;
};

} // namespace sockpp

#endif
