#ifndef __INCLUDE_SOCKETPP_ASYNC_SOCKET_HPP__
#define __INCLUDE_SOCKETPP_ASYNC_SOCKET_HPP__

#include <socketpp/socket.hpp>
#include <socketpp/watcher.hpp>

#include <asyncpp/task.h>

#include <cstddef>
#include <memory>
#include <utility>

namespace sockpp {

/**
 * Owns a non-blocking socket and exposes reactor-backed coroutine operations.
 */
class SOCKETPP_API CSocketAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /**
     * Transfer a socket into asynchronous ownership.
     *
     * @param watcher The watcher that will drive this socket.
     * @param socket The socket to take ownership of.
     */
    CSocketAsync(CSocketWatcher& watcher, CSocket&& socket);

    /**
     * Move an asynchronous socket.
     *
     * @param other The socket to move from.
     */
    CSocketAsync(CSocketAsync&& other) noexcept;

    /**
     * Destroy the asynchronous socket and close its native socket.
     */
    ~CSocketAsync();

    /**
     * Move an asynchronous socket.
     *
     * @param other The socket to move from.
     * @return This socket.
     */
    CSocketAsync& operator=(CSocketAsync&& other) noexcept;

    CSocketAsync(const CSocketAsync&) = delete;
    CSocketAsync& operator=(const CSocketAsync&) = delete;

public:
    /**
     * Create an asynchronous socket.
     *
     * @param watcher The watcher that will drive the socket.
     * @param family The address family.
     * @param type The transport type.
     * @param protocol The protocol, or EPROTO_DEFAULT for the system default.
     * @return The created asynchronous socket.
     */
    static CSocketAsync open(
        CSocketWatcher& watcher,
        EAddressFamily family,
        ESocketType type,
        ESocketProtocol protocol = EPROTO_DEFAULT);

public:
    /** Test whether the asynchronous socket is open. */
    bool isOpen() const noexcept;

    /** Test reactor identity without exposing mutable implementation state. */
    bool usesWatcher(const CSocketWatcher& watcher) const noexcept;

    /** Test whether the owned socket has ordered stream semantics rather than datagrams. */
    bool isByteStream() const noexcept;

    /**
     * Close the socket.
     * Pending POSIX readiness waits are rejected after native close. IOCP completions
     * must still be driven and all tasks joined before releasing buffers or the watcher.
     *
     * @return An empty error code on success, otherwise the close error.
     */
    std::error_code close() noexcept;

    /** Bind the socket to a local address. */
    void bind(const CSocketAddress& address);

    /** Mark the socket as a listener. */
    void listen(int backlog = 128);

    /** Disable sends, receives, or both directions. */
    void shutdown(ESocketShutdown direction);

    /** Return the local socket address. */
    CSocketAddress localAddress() const;

    /** Return the connected peer address. */
    CSocketAddress peerAddress() const;

    /** Set a native socket option without exposing the native handle. */
    void setOption(int level, int option, const void* value, std::size_t size);

    /**
     * Apply a typed socket option to this asynchronous socket.
     *
     * @tparam TOption An option type providing apply(socket).
     * @param option The option to forward to its apply() method.
     */
    template<typename TOption>
    inline void setOption(TOption&& option) {
        std::forward<TOption>(option).apply(*this);
    }

    /** Read a native socket option without exposing the native handle. */
    void getOption(int level, int option, void* value, std::size_t& size) const;

public:
    /**
     * Connect to a remote address without blocking the calling thread.
     *
     * @param address The remote address.
     * @note The address is copied into the coroutine frame.
     */
    asyncpp::task<void> connect(CSocketAddress address);

    /**
     * Accept one incoming connection without blocking the calling thread.
     *
     * @param peer Optional destination for the peer address.
     * @return An asynchronous socket attached to the same watcher.
     * @note When supplied, peer must remain valid until the task completes.
     */
    asyncpp::task<CSocketAsync> accept(CSocketAddress* peer = nullptr);

    /**
     * Receive bytes without blocking the calling thread.
     *
     * @param buffer The destination buffer.
     * @param flags Native receive flags.
     * @return The number of bytes received; zero reports an orderly EOF.
     * @note The buffer storage must remain valid until the task completes.
     */
    asyncpp::task<std::size_t> receive(SByteSpan buffer, int flags = 0);

    /**
     * Send bytes without blocking the calling thread.
     *
     * @param buffer The source buffer.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     * @note The buffer storage must remain valid until the task completes.
     */
    asyncpp::task<std::size_t> send(SReadOnlyByteSpan buffer, int flags = 0);

    /**
     * Send an entire buffer, retrying partial writes.
     *
     * @param buffer The source buffer.
     * @param flags Native send flags.
     * @note The buffer storage must remain valid until the task completes.
     */
    asyncpp::task<void> sendAll(SReadOnlyByteSpan buffer, int flags = 0);

    /**
     * Receive one datagram without blocking the calling thread.
     *
     * @param buffer The destination buffer.
     * @param source Receives the datagram source address.
     * @param flags Native receive flags.
     * @return The number of bytes received.
     * @note The buffer storage and source must remain valid until completion.
     */
    asyncpp::task<std::size_t> receiveFrom(
        SByteSpan buffer,
        CSocketAddress& source,
        int flags = 0);

    /**
     * Send one datagram without blocking the calling thread.
     *
     * @param buffer The source buffer.
     * @param destination The datagram destination.
     * @param flags Native send flags.
     * @return The number of bytes sent.
     * @note The buffer storage must remain valid until the task completes.
     */
    asyncpp::task<std::size_t> sendTo(
        SReadOnlyByteSpan buffer,
        CSocketAddress destination,
        int flags = 0);
};

} // namespace sockpp

#endif
