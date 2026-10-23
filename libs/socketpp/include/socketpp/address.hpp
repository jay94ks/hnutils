#ifndef __INCLUDE_SOCKETPP_ADDRESS_HPP__
#define __INCLUDE_SOCKETPP_ADDRESS_HPP__

#include <socketpp/common.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sockpp {

class CSocket;
class CSocketAsync;
class CSocketWatcher;

/**
 * Owns an encapsulated IPv4, IPv6, or local socket address.
 */
class SOCKETPP_API CSocketAddress {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /**
     * Construct an empty address.
     */
    CSocketAddress();

    /**
     * Copy an address.
     *
     * @param other The address to copy.
     */
    CSocketAddress(const CSocketAddress& other);

    /**
     * Move an address.
     *
     * @param other The address to move from.
     */
    CSocketAddress(CSocketAddress&& other) noexcept;

    /**
     * Destroy the address.
     */
    ~CSocketAddress();

    /**
     * Copy an address.
     *
     * @param other The address to copy.
     * @return This address.
     */
    CSocketAddress& operator=(const CSocketAddress& other);

    /**
     * Move an address.
     *
     * @param other The address to move from.
     * @return This address.
     */
    CSocketAddress& operator=(CSocketAddress&& other) noexcept;

public:
    /**
     * Create an IPv4 wildcard address.
     *
     * @param port The host-order port number.
     * @return The wildcard address.
     */
    static CSocketAddress anyIpv4(std::uint16_t port);

    /**
     * Create an IPv4 loopback address.
     *
     * @param port The host-order port number.
     * @return The loopback address.
     */
    static CSocketAddress loopbackIpv4(std::uint16_t port);

    /**
     * Create an IPv6 wildcard address.
     *
     * @param port The host-order port number.
     * @return The wildcard address.
     */
    static CSocketAddress anyIpv6(std::uint16_t port);

    /**
     * Create an IPv6 loopback address.
     *
     * @param port The host-order port number.
     * @return The loopback address.
     */
    static CSocketAddress loopbackIpv6(std::uint16_t port);

    /**
     * Create a wildcard address for an internet address family.
     *
     * @param port The host-order port number.
     * @param family EAF_INET or EAF_INET6.
     * @return The wildcard address.
     */
    static CSocketAddress any(
        std::uint16_t port = 0,
        EAddressFamily family = EAF_INET);

    /**
     * Create a loopback address for an internet address family.
     *
     * @param port The host-order port number.
     * @param family EAF_INET or EAF_INET6.
     * @return The loopback address.
     */
    static CSocketAddress loopbackAddress(
        std::uint16_t port = 0,
        EAddressFamily family = EAF_INET);

    /**
     * Create a local Unix-domain socket address.
     *
     * @param path The filesystem or platform local-socket path.
     * @return The local socket address.
     */
    static CSocketAddress local(std::string_view path);

    /**
     * Parse a numeric IPv4 or IPv6 address.
     *
     * @param host The numeric host text.
     * @param port The host-order port number.
     * @return The parsed address.
     * @throw std::invalid_argument when the host is not a numeric address.
     */
    static CSocketAddress parse(std::string_view host, std::uint16_t port);

    /**
     * Resolve a host and service through the system resolver.
     *
     * @param host The host name or numeric address.
     * @param service The service name or decimal port.
     * @param family The requested address family.
     * @param type The requested socket type.
     * @return Every address returned by the system resolver.
     */
    static std::vector<CSocketAddress> resolve(
        std::string_view host,
        std::string_view service,
        EAddressFamily family = EAF_UNSPEC,
        ESocketType type = ESOCK_STREAM);

    /**
     * Resolve a host and numeric port through the system resolver.
     *
     * @param host The host name or numeric address.
     * @param port The host-order port number.
     * @param family The requested internet address family.
     * @param type The requested socket type.
     * @return Every address returned by the system resolver.
     */
    static std::vector<CSocketAddress> resolve(
        std::string_view host,
        std::uint16_t port = 0,
        EAddressFamily family = EAF_UNSPEC,
        ESocketType type = ESOCK_STREAM);

    /**
     * Resolve the first matching host address.
     *
     * @param host The host name or numeric address.
     * @param port The host-order port number.
     * @param family The requested internet address family.
     * @param type The requested socket type.
     * @return The first address returned by the system resolver.
     */
    static CSocketAddress resolveOne(
        std::string_view host,
        std::uint16_t port = 0,
        EAddressFamily family = EAF_INET,
        ESocketType type = ESOCK_STREAM);

public:
    /**
     * Test whether this object contains a socket address.
     *
     * @return true when the address is valid.
     */
    bool isValid() const noexcept;

    /**
     * Return the address family.
     *
     * @return The address family, or EAF_UNSPEC for an empty address.
     */
    EAddressFamily family() const noexcept;

    /**
     * Return the host-order port number.
     *
     * @return The port number, or zero for an empty address.
     */
    std::int32_t port() const noexcept;

    /**
     * Set the host-order port number.
     *
     * @param value The new port number.
     * @throw std::logic_error when the address has no internet port.
     */
    void port(std::uint16_t value);

    /**
     * Test whether this is an IPv4 or IPv6 loopback address.
     *
     * @return true for 127.0.0.1 or ::1.
     */
    bool isLoopback() const noexcept;

    /**
     * Format the numeric host without a port.
     *
     * @return The numeric IPv4 or IPv6 host.
     */
    std::string host() const;

    /**
     * Format the numeric endpoint.
     *
     * @return The numeric host or local-socket path.
     */
    std::string toString() const;

    /**
     * Format an internet address with its port.
     *
     * @return `host:port` for IPv4 or `[host]:port` for IPv6.
     */
    std::string endpointString() const;

private:
    /**
     * Construct an address from native sockaddr storage.
     *
     * @param address The native sockaddr bytes.
     * @param size The number of valid bytes.
     */
    CSocketAddress(const void* address, std::size_t size);

    /**
     * Replace this address with native sockaddr storage.
     *
     * @param address The native sockaddr bytes.
     * @param size The number of valid bytes.
     */
    void assign(const void* address, std::size_t size);

    /** Return the native sockaddr storage. */
    const void* nativeData() const noexcept;

    /** Return the native sockaddr size. */
    std::size_t nativeSize() const noexcept;

    friend class CSocket;
    friend class CSocketAsync;
    friend class CSocketWatcher;
};

} // namespace sockpp

#endif
