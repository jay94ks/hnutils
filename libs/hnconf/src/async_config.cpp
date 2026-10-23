#include <hnconf/async_config.hpp>

#include <coroutine>
#include <utility>

namespace hnconf {
namespace {

struct DispatchOn {
    asyncpp::dispatcher* worker;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> continuation) const {
        // --> Always queue: header-only dispatcher TLS can differ across DLL boundaries.
        worker->push([continuation] { continuation.resume(); });
    }

    void await_resume() const noexcept {}
};

asyncpp::task<CConfig> parseOn(asyncpp::dispatcher* worker, std::string text,
    SParseOptions options, std::string source) {
    co_await DispatchOn{worker};
    co_return CConfig::parse(text, options, std::move(source));
}

asyncpp::task<CConfig> loadOn(asyncpp::dispatcher* worker, std::filesystem::path path, SParseOptions options) {
    co_await DispatchOn{worker};
    co_return CConfig::load(path, options);
}

asyncpp::task<std::string> serializeOn(asyncpp::dispatcher* worker, CConfig config, SWriteOptions options) {
    co_await DispatchOn{worker};
    co_return config.serialize(options);
}

asyncpp::task<void> saveOn(asyncpp::dispatcher* worker, CConfig config,
    std::filesystem::path path, SWriteOptions options) {
    co_await DispatchOn{worker};
    config.save(path, options);
}

} // namespace

struct CConfigAsync::Impl {
    asyncpp::dispatcher* worker;
};

/* Store a non-owning executor reference behind the PIMPL. */
CConfigAsync::CConfigAsync(asyncpp::dispatcher& worker) : _impl(std::make_unique<Impl>(Impl{&worker})) {}

/* Created tasks own their arguments and never reference this adapter. */
CConfigAsync::~CConfigAsync() = default;

/* Capture all inputs in the coroutine frame at call time. */
asyncpp::task<CConfig> CConfigAsync::parse(std::string text, SParseOptions options, std::string source) const {
    return parseOn(_impl->worker, std::move(text), options, std::move(source));
}

/* Capture the path before a lazy task can suspend. */
asyncpp::task<CConfig> CConfigAsync::load(std::filesystem::path path, SParseOptions options) const {
    return loadOn(_impl->worker, std::move(path), options);
}

/* Capture an immutable document snapshot. */
asyncpp::task<std::string> CConfigAsync::serialize(CConfig config, SWriteOptions options) const {
    return serializeOn(_impl->worker, std::move(config), options);
}

/* Do not borrow the source document, path or adapter. */
asyncpp::task<void> CConfigAsync::save(CConfig config, std::filesystem::path path, SWriteOptions options) const {
    return saveOn(_impl->worker, std::move(config), std::move(path), options);
}

} // namespace hnconf
