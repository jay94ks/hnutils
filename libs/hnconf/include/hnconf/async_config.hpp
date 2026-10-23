#ifndef __INCLUDE_HNCONF_ASYNC_CONFIG_HPP__
#define __INCLUDE_HNCONF_ASYNC_CONFIG_HPP__

#include <hnconf/config.hpp>
#include <asyncpp/dispatcher.h>
#include <asyncpp/task.h>

namespace hnconf {

/**
 * Coroutine operations queued on a caller-owned asyncpp dispatcher.
 * Use a worker (for example thread_pool), not a network reactor, for blocking I/O.
 * The dispatcher must outlive tasks and run accepted continuations exactly once.
 * Awaiting callers resume on that dispatcher, without an implicit switch back.
 */
class HNCONF_API CConfigAsync {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

public:
    /** Associate a worker with this adapter; worker ownership is not transferred. */
    explicit CConfigAsync(asyncpp::dispatcher& worker);

    /** Destroy the adapter; created tasks stay valid while worker lives. */
    ~CConfigAsync();

    /** Adapters have stable identity and cannot be copied. */
    CConfigAsync(const CConfigAsync&) = delete;

    /** Adapters have stable identity and cannot be copied. */
    CConfigAsync& operator=(const CConfigAsync&) = delete;

    /** Parse owned text after scheduling; parse/scheduling errors propagate. */
    asyncpp::task<CConfig> parse(std::string text, SParseOptions options = {},
        std::string source = "<memory>") const;

    /** Load an owned path on the worker, not the initiating thread. */
    asyncpp::task<CConfig> load(std::filesystem::path path, SParseOptions options = {}) const;

    /** Serialize a snapshot captured at this method call. */
    asyncpp::task<std::string> serialize(CConfig config, SWriteOptions options = {}) const;

    /** Save an owned snapshot and path; same non-atomic semantics as save(). */
    asyncpp::task<void> save(CConfig config, std::filesystem::path path, SWriteOptions options = {}) const;
};

} // namespace hnconf

#endif
