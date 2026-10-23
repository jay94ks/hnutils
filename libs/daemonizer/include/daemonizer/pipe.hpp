#ifndef __INCLUDE_DAEMONIZER_PIPE_HPP__
#define __INCLUDE_DAEMONIZER_PIPE_HPP__

#include <daemonizer/common.hpp>

#include <asyncpp/task.h>

#include <memory>

namespace daemonz {

class IPipe;

/** Shared ownership of one endpoint of a duplex byte-stream pipe. */
using IPipePtr = std::shared_ptr<IPipe>;

/** Encapsulated bidirectional parent/child communication endpoint. */
class DAEMONIZER_API IPipe {
public:
    /** Destroy the endpoint and release its owned native resources. */
    virtual ~IPipe();

    /** Test whether close() has been requested for this endpoint. */
    virtual bool isOpen() const noexcept = 0;

    /** Cancel pending I/O and close the endpoint; safe to call repeatedly. */
    virtual void close() noexcept = 0;

    /**
     * Read available bytes asynchronously; zero reports peer EOF.
     * @param buffer Storage that must remain valid until the task completes.
     * @return Number of bytes read, possibly smaller than the buffer.
     * @note At most one read and one write may be pending per endpoint.
     */
    virtual asyncpp::task<std::size_t> read(SByteSpan buffer) = 0;

    /**
     * Write bytes asynchronously, permitting partial writes.
     * @param buffer Storage that must remain valid until the task completes.
     * @return Number of bytes written.
     */
    virtual asyncpp::task<std::size_t> write(SReadOnlyByteSpan buffer) = 0;

    /** Read an entire buffer; return false only for EOF before any bytes. */
    asyncpp::task<bool> readExact(SByteSpan buffer);

    /** Write an entire buffer, retrying partial writes. */
    asyncpp::task<void> writeAll(SReadOnlyByteSpan buffer);
};

/** The two connected endpoints of an operating-system duplex pipe. */
struct SPipePair {
    /** First endpoint, normally retained by the parent. */
    IPipePtr parent;

    /** Second endpoint, normally transferred to a child. */
    IPipePtr child;
};

} // namespace daemonz

#endif
