#ifndef __SRC_HNHTTP_INTERNAL_HPP__
#define __SRC_HNHTTP_INTERNAL_HPP__
#include <hnhttp.hpp>
#include <asyncpp/defer.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hnhttp {
namespace detail {

inline SHttpError error(ERetCode code, std::string message = {}, std::error_code cause = {}) {
    SHttpError value;
    value.code = code; value.message = std::move(message); value.cause = cause;
    return value;
}
inline ERetCode policy(const SOperationOptions& options) {
    if (options.cancellation.stop_requested()) { return ERET_CANCELLED; }
    if (options.deadline || options.cancellation.stop_possible()) { return ERET_UNSUPPORTED; }
    return ERET_OK;
}
inline bool current(const std::shared_ptr<asyncpp::dispatcher>& executor) {
    return executor && executor.get() == asyncpp::dispatcher::current();
}
inline void require(const std::shared_ptr<asyncpp::dispatcher>& executor) {
    if (!current(executor)) { throw std::logic_error("HTTP operation on foreign executor"); }
}
inline void unsupported(const char* message) {
    throw std::system_error(std::make_error_code(std::errc::not_supported), message);
}
inline std::string lower(std::string value) {
    for (auto& c : value) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a'-'A')); } }
    return value;
}
inline bool token(std::string_view value) {
    if (value.empty()) { return false; }
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) { return false; }
    }
    return true;
}
inline bool fieldValue(std::string_view value) {
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return (c < 32 && c != '\t') || c == 127;
    });
}
inline std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string{} : std::string(value.substr(first,
        value.find_last_not_of(" \t") - first + 1));
}
inline bool managed(const std::string& name) {
    const auto n = lower(name);
    return n == "host" || n == "content-length" || n == "transfer-encoding" || n == "connection"
        || n == "trailer" || n == "expect" || n == "upgrade" || n == "content-type";
}
inline std::uint64_t number(std::string_view value, int radix = 10) {
    std::uint64_t result = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, radix);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("invalid HTTP numeric field");
    }
    return result;
}
inline void validateOptions(const SHttpConnectionOptions& o) {
    if (o.version != HTTP_10 && o.version != HTTP_11) { unsupported("HTTP/1 convenience factory: use connection factories for HTTP/2 or HTTP/3"); }
    if (!o.scheduling.maximumOperationsPerTurn || !o.scheduling.maximumBytesPerTurn
        || !o.limits.ioChunkBytes || o.limits.ioChunkBytes > o.limits.maximumReadAheadBytes
        || !o.limits.maximumHeaderBytes || !o.limits.maximumHeaderFields || !o.limits.maximumStartLineBytes) {
        throw std::invalid_argument("invalid HTTP connection bounds");
    }
}

class Headers final : public IHttpHeaders {
private:
    std::vector<SHeaderField> _fields;
    bool _frozen = false;
    std::shared_ptr<asyncpp::dispatcher> _executor;
    void check() const { if (_executor) { require(_executor); } }
    void edit(const std::string& name) const {
        check();
        if (_frozen) { throw std::logic_error("frozen HTTP fields"); }
        if (!token(name) || managed(name)) { throw std::invalid_argument("invalid or managed HTTP field"); }
    }
    void bounds(const std::string& name, const std::string& value, bool replace) const {
        std::size_t bytes = 0, count = 0; const auto n = lower(name);
        for (const auto& f : _fields) {
            if (replace && lower(f.name) == n) { continue; }
            bytes += f.name.size()+f.value.size(); ++count;
        }
        if (count >= 128 || name.size() > 65536 || value.size() > 65536 || name.size()+value.size() > 65536-bytes) { throw std::length_error("HTTP fields limit"); }
    }
public:
    void executor(std::shared_ptr<asyncpp::dispatcher> value) { _executor = std::move(value); }
    void freeze() { _frozen = true; }
    void appendParsed(SHeaderField field) { _fields.push_back(std::move(field)); }
    std::size_t size() const noexcept override { return _fields.size(); }
    std::optional<std::string> get(const std::string& name) const override {
        check(); auto n = lower(name);
        for (const auto& f : _fields) { if (lower(f.name) == n) { return f.value; } }
        return {};
    }
    std::vector<SHeaderField> entries(std::uint64_t offset, std::size_t count) const override {
        check(); if (count > 4096) { throw std::length_error("HTTP field page too large"); }
        if (offset >= _fields.size()) { return {}; }
        const auto begin = _fields.begin() + static_cast<std::ptrdiff_t>(offset);
        return {begin, begin + static_cast<std::ptrdiff_t>((std::min)(count, _fields.size()-offset))};
    }
    std::vector<std::string> values(const std::string& name, std::uint64_t offset, std::size_t count) const override {
        check(); if (count > 4096) { throw std::length_error("HTTP field page too large"); }
        std::vector<std::string> result; auto n = lower(name);
        for (const auto& f : _fields) { if (lower(f.name) == n) {
            if (offset) { --offset; } else if (result.size() < count) { result.push_back(f.value); }
        } }
        return result;
    }
    IHttpHeaders& set(std::string name, std::string value, EHeaderIndexing hint) override {
        edit(name); if (!fieldValue(value)) { throw std::invalid_argument("invalid HTTP value"); }
        bounds(name, value, true);
        remove(name); return add(std::move(name), std::move(value), hint);
    }
    IHttpHeaders& add(std::string name, std::string value, EHeaderIndexing hint) override {
        edit(name); if (!fieldValue(value)) { throw std::invalid_argument("invalid HTTP value"); }
        bounds(name, value, false);
        _fields.push_back({std::move(name), std::move(value), hint}); return *this;
    }
    IHttpHeaders& remove(const std::string& name) override {
        edit(name); const auto n = lower(name);
        std::erase_if(_fields, [&](const auto& f) { return lower(f.name) == n; }); return *this;
    }
};

std::string encodeComponent(std::string_view value, bool form = false);
std::string decodeComponent(std::string_view value, bool form = false);

class Queries final : public IHttpQueries {
private:
    std::vector<SQueryField> _fields;
    bool _frozen = false;
    std::shared_ptr<asyncpp::dispatcher> _executor;
    void check(bool write = false) const {
        if (_executor) { require(_executor); }
        if (write && _frozen) { throw std::logic_error("frozen HTTP queries"); }
    }
    void bounds(const std::string& name, const std::string& value, bool replace) const {
        std::size_t bytes = 0, count = 0;
        for (const auto& f : _fields) {
            if (replace && f.name == name) { continue; }
            bytes += f.name.size()+f.value.size(); ++count;
        }
        if (count >= 1024 || name.size() > 65536 || value.size() > 65536 || name.size()+value.size() > 65536-bytes) { throw std::length_error("query limit"); }
    }
public:
    void executor(std::shared_ptr<asyncpp::dispatcher> value) { _executor = std::move(value); }
    void freeze() { _frozen = true; }
    void parse(std::string_view value, const SHttpLimits& limits);
    std::string wire() const;
    std::size_t size() const noexcept override { return _fields.size(); }
    std::optional<std::string> get(const std::string& name) const override {
        check(); for (const auto& f : _fields) { if (f.name == name) { return f.value; } } return {};
    }
    std::vector<SQueryField> entries(std::uint64_t offset, std::size_t count) const override {
        check(); if (count > 4096) { throw std::length_error("query page too large"); }
        if (offset >= _fields.size()) { return {}; }
        const auto begin = _fields.begin() + static_cast<std::ptrdiff_t>(offset);
        return {begin, begin + static_cast<std::ptrdiff_t>((std::min)(count, _fields.size()-offset))};
    }
    IHttpQueries& set(std::string name, std::string value) override {
        check(true); bounds(name, value, true);
        remove(name); return add(std::move(name), std::move(value));
    }
    IHttpQueries& set(std::string name, std::int64_t value) override { return set(std::move(name), std::to_string(value)); }
    IHttpQueries& add(std::string name, std::string value) override {
        check(true); bounds(name, value, false);
        _fields.push_back({std::move(name), std::move(value), true}); return *this;
    }
    IHttpQueries& addBare(std::string name) override { add(std::move(name), {}); _fields.back().hasEquals = false; return *this; }
    IHttpQueries& remove(const std::string& name) override {
        check(true); std::erase_if(_fields, [&](const auto& f) { return f.name == name; }); return *this;
    }
};

} // namespace detail
} // namespace hnhttp
#endif
