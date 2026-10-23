#include <hnconf/config.hpp>

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <system_error>
#include <utility>

namespace hnconf {
namespace {

std::string label(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

void validatePath(const std::filesystem::path& path) {
    if (path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
        throw std::invalid_argument("hnconf file paths cannot contain NUL");
    }
}

[[noreturn]] void ioFailure(const std::string& operation, const std::filesystem::path& path) {
    const auto code = errno == 0 ? std::make_error_code(std::errc::io_error)
        : std::error_code(errno, std::generic_category());
    throw std::system_error(code, operation + ": " + label(path));
}

} // namespace

struct CConfig::Impl {
    std::vector<CConfigNode> nodes;
};

/* Construct owned top-level storage. */
CConfig::CConfig(std::vector<CConfigNode> nodes)
    : _impl(std::make_shared<Impl>(Impl{std::move(nodes)})) {}

/* Share immutable storage; edits always replace the storage. */
CConfig::CConfig(const CConfig&) noexcept = default;

/* Assign immutable storage. */
CConfig& CConfig::operator=(const CConfig&) noexcept = default;

/* Release the document. */
CConfig::~CConfig() = default;

/* Return a complete document or a located error, never partial state. */
CConfig CConfig::parse(std::string_view text, SParseOptions options, std::string source) {
    return CConfig(detail::parseNodes(text, options, source));
}

/* Read incrementally to bound memory even if the source grows during a read. */
CConfig CConfig::load(const std::filesystem::path& path, SParseOptions options) {
    detail::validateOptions(options);
    validatePath(path);
    errno = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) { ioFailure("open for reading", path); }
    std::string text;
    std::array<char, 8192> buffer{};
    for (;;) {
        errno = 0;
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (input.bad() || (input.fail() && !input.eof())) { ioFailure("read", path); }
        if (count > options.maxInputBytes - text.size()) {
            throw CParseError("input byte limit exceeded", label(path), 1, 1, 0);
        }
        text.append(buffer.data(), count);
        if (input.eof()) { break; }
    }
    return parse(text, options, label(path));
}

/* Return isolated node values. */
std::vector<CConfigNode> CConfig::nodes() const { return _impl->nodes; }

/* Allocate before publishing a replacement document. */
void CConfig::setNodes(std::vector<CConfigNode> nodes) {
    _impl = std::make_shared<Impl>(Impl{std::move(nodes)});
}

/* Preserve duplicate indexes and original ordering. */
std::vector<std::size_t> CConfig::find(std::string_view name) const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < _impl->nodes.size(); ++i) {
        if (_impl->nodes[i].name() == name) { result.push_back(i); }
    }
    return result;
}

/* Append without mutating other document snapshots. */
void CConfig::addNode(CConfigNode node) {
    auto next = nodes();
    next.push_back(std::move(node));
    setNodes(std::move(next));
}

/* Validate the index before publishing a new document. */
void CConfig::replaceNode(std::size_t index, CConfigNode node) {
    if (index >= _impl->nodes.size()) { throw std::out_of_range("hnconf node index"); }
    auto next = nodes();
    next[index] = std::move(node);
    setNodes(std::move(next));
}

/* Remove one ordered entry. */
void CConfig::removeNode(std::size_t index) {
    if (index >= _impl->nodes.size()) { throw std::out_of_range("hnconf node index"); }
    auto next = nodes();
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    setNodes(std::move(next));
}

/* Emit bounded canonical text. */
std::string CConfig::serialize(SWriteOptions options) const {
    return detail::writeNodes(_impl->nodes, options);
}

/* Serialize before truncating; explicitly check both write and close. */
void CConfig::save(const std::filesystem::path& path, SWriteOptions options) const {
    validatePath(path);
    const auto text = serialize(options);
    errno = 0;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) { ioFailure("open for writing", path); }
    // --> Chunking avoids narrowing a user-configured large size to streamsize.
    for (std::size_t pos = 0; pos < text.size();) {
        const auto count = std::min<std::size_t>(8192, text.size() - pos);
        errno = 0;
        output.write(text.data() + pos, static_cast<std::streamsize>(count));
        if (!output) { ioFailure("write", path); }
        pos += count;
    }
    errno = 0;
    output.close();
    if (!output) { ioFailure("close after writing", path); }
}

} // namespace hnconf
