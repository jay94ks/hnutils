#ifndef __INCLUDE_HNCONF_ERROR_HPP__
#define __INCLUDE_HNCONF_ERROR_HPP__

#include <hnconf/common.hpp>
#include <memory>
#include <stdexcept>
#include <string>

namespace hnconf {

/** Syntax or resource-limit failure with a location in the original input. */
class HNCONF_API CParseError : public std::runtime_error {
private:
    struct Impl;
    std::shared_ptr<const Impl> _impl;

public:
    /** Construct a diagnostic; line/column are one-based and offset is zero-based. */
    CParseError(std::string message, std::string source, std::size_t line,
        std::size_t column, std::size_t offset);

    /** Copy immutable diagnostic details. */
    CParseError(const CParseError&) noexcept;

    /** Copy immutable diagnostic details. */
    CParseError& operator=(const CParseError&) noexcept;

    /** Release the diagnostic. */
    ~CParseError() override;

    /** Return the source label or loaded file path. */
    std::string source() const;

    /** Return the one-based line number; LF advances the line. */
    std::size_t line() const noexcept;

    /** Return the one-based byte column, not a Unicode character column. */
    std::size_t column() const noexcept;

    /** Return the zero-based byte offset. */
    std::size_t offset() const noexcept;
};

} // namespace hnconf

#endif
