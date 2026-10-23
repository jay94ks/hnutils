#ifndef __INCLUDE_DAEMONIZER_PIPE_SERVER_HPP__
#define __INCLUDE_DAEMONIZER_PIPE_SERVER_HPP__

#include <daemonizer/pipe.hpp>

namespace daemonz {

class IPipeServer;

/** Shared ownership of a pipe server and its managed endpoints. */
using IPipeServerPtr = std::shared_ptr<IPipeServer>;

/** Factory and lifetime manager for platform-specific duplex pipes. */
class DAEMONIZER_API IPipeServer {
public:
    /** Close managed endpoints and shut down a privately owned reactor. */
    virtual ~IPipeServer();

    /**
     * Create a native pipe server with its own asynchronous I/O runtime.
     * @param reactors Number of reactor threads, at least one.
     * @return A Windows named-pipe or Unix socketpair server implementation.
     */
    static IPipeServerPtr create(std::size_t reactors = 1);

    /** Create and retain both ends of a connected duplex byte-stream pipe. */
    virtual SPipePair createPair() = 0;

    /** Return the number of managed endpoints still open. */
    virtual std::size_t pipeCount() const noexcept = 0;

    /** Cancel I/O and close every managed endpoint. */
    virtual void close() noexcept = 0;
};

} // namespace daemonz

#endif
