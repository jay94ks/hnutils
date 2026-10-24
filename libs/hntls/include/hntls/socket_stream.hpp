#ifndef __INCLUDE_HNTLS_SOCKET_STREAM_HPP__
#define __INCLUDE_HNTLS_SOCKET_STREAM_HPP__
#include <hntls/stream.hpp>
#include <socketpp/async_socket.hpp>
namespace hntls {
/** Owned socketpp byte transport, also usable with an accepted server TCP socket. */
class HNTLS_API CSocketByteStream final : public IByteStream {
  private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
    /** Retain private socket state until pending callbacks complete. */
    explicit CSocketByteStream(std::shared_ptr<Impl> impl);

  public:
    /** Adopt a connected stream socket using the same reactor. */
    static std::shared_ptr<CSocketByteStream> create(std::shared_ptr<sockpp::CSocketWatcher> watcher,
                                                     sockpp::CSocketAsync socket);
    /** Abort owned I/O without stopping the shared reactor. */
    ~CSocketByteStream() override;
    /** Stable owning executor. */
    std::shared_ptr<asyncpp::dispatcher> executor() const noexcept override;
    /** Read owned bytes; bounded to 16 MiB per operation. */
    asyncpp::task<SBytesResult> readSome(std::size_t maximumBytes, SOperationOptions options = {}) override;
    /** Write an owned buffer, reporting partial progress. */
    asyncpp::task<SWriteResult> writeSome(SBytes bytes, SOperationOptions options = {}) override;
    /** Half-close after prior writes. */
    asyncpp::task<ERetCode> finishWrite(SOperationOptions options = {}) override;
    /** Thread-safe abort, waking outstanding native I/O. */
    void close() noexcept override;
    /** Join all native buffer accesses after close. */
    asyncpp::task<ERetCode> waitClosed(SOperationOptions options = {}) override;
};
} // namespace hntls
#endif
