#include <daemonizer/pipe.hpp>

#include <stdexcept>
#include <system_error>

namespace daemonz {

/* Destroy the abstract endpoint. */
IPipe::~IPipe() = default;

/* Read an entire buffer without treating truncated input as clean EOF. */
asyncpp::task<bool> IPipe::readExact(SByteSpan buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const auto count = co_await read(buffer.subspan(offset));
        if (count == 0) {
            if (offset == 0) co_return false;
            throw std::system_error(std::make_error_code(std::errc::broken_pipe),
                                    "pipe EOF in the middle of a readExact buffer");
        }
        offset += count;
    }
    co_return true;
}

/* Retry partial writes until the caller's entire buffer is transferred. */
asyncpp::task<void> IPipe::writeAll(SReadOnlyByteSpan buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const auto count = co_await write(buffer.subspan(offset));
        if (count == 0) {
            throw std::system_error(std::make_error_code(std::errc::broken_pipe), "pipe write made no progress");
        }
        offset += count;
    }
}

} // namespace daemonz
