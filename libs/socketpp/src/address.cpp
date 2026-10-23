#include <socketpp/address.hpp>

#include "platform.hpp"

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sockpp {

struct CSocketAddress::Impl {
    sockaddr_storage storage{};
    socklen_t size = 0;
};

/* Construct an empty address. */
CSocketAddress::CSocketAddress() : _impl(std::make_unique<Impl>()) {}

/* Copy an address. */
CSocketAddress::CSocketAddress(const CSocketAddress& other)
    : _impl(std::make_unique<Impl>()) {
    if (other._impl != nullptr) {
        *_impl = *other._impl;
    }
}

/* Move an address. */
CSocketAddress::CSocketAddress(CSocketAddress&& other) noexcept = default;

/* Destroy the address. */
CSocketAddress::~CSocketAddress() = default;

/* Copy an address. */
CSocketAddress& CSocketAddress::operator=(const CSocketAddress& other) {
    if (this != &other) {
        if (_impl == nullptr) {
            _impl = std::make_unique<Impl>();
        }
        *_impl = other._impl != nullptr ? *other._impl : Impl{};
    }
    return *this;
}

/* Move an address. */
CSocketAddress& CSocketAddress::operator=(CSocketAddress&& other) noexcept = default;

/* Create an IPv4 wildcard address. */
CSocketAddress CSocketAddress::anyIpv4(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    return CSocketAddress(&address, sizeof(address));
}

/* Create an IPv4 loopback address. */
CSocketAddress CSocketAddress::loopbackIpv4(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return CSocketAddress(&address, sizeof(address));
}

/* Create an IPv6 wildcard address. */
CSocketAddress CSocketAddress::anyIpv6(std::uint16_t port) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_any;
    return CSocketAddress(&address, sizeof(address));
}

/* Create an IPv6 loopback address. */
CSocketAddress CSocketAddress::loopbackIpv6(std::uint16_t port) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_loopback;
    return CSocketAddress(&address, sizeof(address));
}

/* Create a wildcard address for an internet address family. */
CSocketAddress CSocketAddress::any(std::uint16_t port, EAddressFamily family) {
    switch (family) {
    case EAF_INET:
        return anyIpv4(port);
    case EAF_INET6:
        return anyIpv6(port);
    default:
        throw std::invalid_argument("wildcard address requires an internet address family");
    }
}

/* Create a loopback address for an internet address family. */
CSocketAddress CSocketAddress::loopbackAddress(std::uint16_t port, EAddressFamily family) {
    switch (family) {
    case EAF_INET:
        return loopbackIpv4(port);
    case EAF_INET6:
        return loopbackIpv6(port);
    default:
        throw std::invalid_argument("loopback address requires an internet address family");
    }
}

/* Create a local Unix-domain socket address. */
CSocketAddress CSocketAddress::local(std::string_view path) {
    if (path.empty()) {
        throw std::invalid_argument("local socket path is empty");
    }

    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) {
        throw std::length_error("local socket path is too long");
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return CSocketAddress(&address, sizeof(address));
}

/* Parse a numeric IPv4 or IPv6 address. */
CSocketAddress CSocketAddress::parse(std::string_view host, std::uint16_t port) {
    ensureSocketRuntime();
    const std::string hostText(host);

    if (host.find(':') != std::string_view::npos) {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        if (::inet_pton(AF_INET6, hostText.c_str(), &address.sin6_addr) != 1) {
            throw std::invalid_argument("invalid numeric IPv6 address");
        }
        return CSocketAddress(&address, sizeof(address));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, hostText.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid numeric IPv4 address");
    }
    return CSocketAddress(&address, sizeof(address));
}

/* Resolve a host and service through the system resolver. */
std::vector<CSocketAddress> CSocketAddress::resolve(
    std::string_view host,
    std::string_view service,
    EAddressFamily family,
    ESocketType type) {
    ensureSocketRuntime();

    addrinfo hints{};
    hints.ai_family = toNative(family);
    hints.ai_socktype = toNative(type);

    const std::string hostText(host);
    const std::string serviceText(service);
    addrinfo* results = nullptr;
    const int result = ::getaddrinfo(
        hostText.empty() ? nullptr : hostText.c_str(),
        serviceText.empty() ? nullptr : serviceText.c_str(),
        &hints,
        &results);
    if (result != 0) {
        throw std::system_error(result, SocketErrorCategory(), "getaddrinfo");
    }

    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> resultOwner(
        results,
        &::freeaddrinfo);

    std::vector<CSocketAddress> addresses;
    for (const addrinfo* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_addr == nullptr || current->ai_addrlen <= 0) {
            continue;
        }
        addresses.push_back(
            CSocketAddress(current->ai_addr, static_cast<std::size_t>(current->ai_addrlen)));
    }
    return addresses;
}

/* Resolve a host and numeric port through the system resolver. */
std::vector<CSocketAddress> CSocketAddress::resolve(
    std::string_view host,
    std::uint16_t port,
    EAddressFamily family,
    ESocketType type) {
    return resolve(host, std::to_string(port), family, type);
}

/* Resolve the first matching host address. */
CSocketAddress CSocketAddress::resolveOne(
    std::string_view host,
    std::uint16_t port,
    EAddressFamily family,
    ESocketType type) {
    auto addresses = resolve(host, port, family, type);
    if (addresses.empty()) {
        throw std::system_error(
            std::make_error_code(std::errc::address_not_available),
            "getaddrinfo returned no usable addresses");
    }
    return std::move(addresses.front());
}

/* Test whether this object contains a socket address. */
bool CSocketAddress::isValid() const noexcept {
    return _impl != nullptr && _impl->size != 0;
}

/* Return the address family. */
EAddressFamily CSocketAddress::family() const noexcept {
    if (!isValid()) {
        return EAF_UNSPEC;
    }
    return fromNativeFamily(reinterpret_cast<const sockaddr*>(&_impl->storage)->sa_family);
}

/* Return the host-order port number. */
std::int32_t CSocketAddress::port() const noexcept {
    if (!isValid()) {
        return -1;
    }

    const auto* address = reinterpret_cast<const sockaddr*>(&_impl->storage);
    if (address->sa_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(address)->sin_port);
    }
    if (address->sa_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(address)->sin6_port);
    }
    return -1;
}

/* Set the host-order port number. */
void CSocketAddress::port(std::uint16_t value) {
    if (!isValid()) {
        throw std::logic_error("cannot set a port on an empty address");
    }

    auto* address = reinterpret_cast<sockaddr*>(&_impl->storage);
    if (address->sa_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(address)->sin_port = htons(value);
        return;
    }
    if (address->sa_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6*>(address)->sin6_port = htons(value);
        return;
    }
    throw std::logic_error("local socket addresses do not have a port");
}

/* Test whether this is an IPv4 or IPv6 loopback address. */
bool CSocketAddress::isLoopback() const noexcept {
    if (!isValid()) {
        return false;
    }

    const auto* address = reinterpret_cast<const sockaddr*>(&_impl->storage);
    if (address->sa_family == AF_INET) {
        return reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr ==
               htonl(INADDR_LOOPBACK);
    }
    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        return IN6_ARE_ADDR_EQUAL(&ipv6->sin6_addr, &in6addr_loopback) != 0;
    }
    return false;
}

/* Format the numeric host without a port. */
std::string CSocketAddress::host() const {
    if (!isValid()) {
        return {};
    }

    std::array<char, INET6_ADDRSTRLEN> text{};
    const auto* address = reinterpret_cast<const sockaddr*>(&_impl->storage);
    const void* source = nullptr;
    if (address->sa_family == AF_INET) {
        source = &reinterpret_cast<const sockaddr_in*>(address)->sin_addr;
    } else if (address->sa_family == AF_INET6) {
        source = &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
    } else if (address->sa_family == AF_UNIX) {
        return reinterpret_cast<const sockaddr_un*>(address)->sun_path;
    } else {
        throw std::logic_error("unsupported socket address family");
    }

    if (::inet_ntop(address->sa_family, source, text.data(), text.size()) == nullptr) {
        throwLastSocketError("inet_ntop");
    }
    return text.data();
}

/* Format the numeric endpoint. */
std::string CSocketAddress::toString() const {
    return host();
}

/* Format an internet address with its port. */
std::string CSocketAddress::endpointString() const {
    if (family() == EAF_IPV6) {
        return "[" + host() + "]:" + std::to_string(port());
    }
    if (family() == EAF_IPV4) {
        return host() + ":" + std::to_string(port());
    }
    return host();
}

/* Construct an address from native sockaddr storage. */
CSocketAddress::CSocketAddress(const void* address, std::size_t size)
    : CSocketAddress() {
    assign(address, size);
}

/* Replace this address with native sockaddr storage. */
void CSocketAddress::assign(const void* address, std::size_t size) {
    if (address == nullptr || size == 0 || size > sizeof(_impl->storage)) {
        throw std::invalid_argument("invalid native socket address");
    }
    std::memset(&_impl->storage, 0, sizeof(_impl->storage));
    std::memcpy(&_impl->storage, address, size);
    _impl->size = static_cast<socklen_t>(size);
}

/* Return the native sockaddr storage. */
const void* CSocketAddress::nativeData() const noexcept {
    return isValid() ? &_impl->storage : nullptr;
}

/* Return the native sockaddr size. */
std::size_t CSocketAddress::nativeSize() const noexcept {
    return isValid() ? static_cast<std::size_t>(_impl->size) : 0;
}

} // namespace sockpp
