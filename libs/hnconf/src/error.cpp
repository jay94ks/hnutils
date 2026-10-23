#include <hnconf/error.hpp>

#include <utility>

namespace hnconf {

struct CParseError::Impl {
    std::string source;
    std::size_t line;
    std::size_t column;
    std::size_t offset;
};

/* Capture a byte location and format a human-readable diagnostic. */
CParseError::CParseError(std::string message, std::string source, std::size_t line,
    std::size_t column, std::size_t offset)
    : std::runtime_error(source + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + message),
      _impl(std::make_shared<Impl>(Impl{std::move(source), line, column, offset})) {}

/* Copy immutable error details. */
CParseError::CParseError(const CParseError&) noexcept = default;

/* Assign immutable error details. */
CParseError& CParseError::operator=(const CParseError&) noexcept = default;

/* Release the error. */
CParseError::~CParseError() = default;

/* Return the source label. */
std::string CParseError::source() const { return _impl->source; }

/* Return the line. */
std::size_t CParseError::line() const noexcept { return _impl->line; }

/* Return the byte column. */
std::size_t CParseError::column() const noexcept { return _impl->column; }

/* Return the byte offset. */
std::size_t CParseError::offset() const noexcept { return _impl->offset; }

} // namespace hnconf
