#ifndef __INCLUDE_HNCONF_NODE_HPP__
#define __INCLUDE_HNCONF_NODE_HPP__

#include <hnconf/common.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hnconf {

/** Whether a node ends with a semicolon or contains children in braces. */
enum ENodeType {
    ENODE_DIRECTIVE, // --> A simple directive, including one without arguments.
    ENODE_BLOCK,     // --> A block, including an empty block.
};

/**
 * Encapsulated value with immutable shared storage and copy-on-write edits.
 * Getters return owned copies. Serialize concurrent edits to the same instance.
 */
class HNCONF_API CConfigNode {
private:
    struct Impl;
    std::shared_ptr<const Impl> _impl;

    /** Adopt validated immutable storage. */
    explicit CConfigNode(std::shared_ptr<const Impl> impl);

public:
    /** Create a directive; reject empty names and embedded NUL bytes. */
    static CConfigNode directive(std::string name, std::vector<std::string> arguments = {});

    /** Create a block; reject invalid tokens or nesting deeper than MAX_DEPTH. */
    static CConfigNode block(std::string name, std::vector<std::string> arguments = {},
        std::vector<CConfigNode> children = {});

    /** Copy immutable storage ownership; later edits are isolated. */
    CConfigNode(const CConfigNode&) noexcept;

    /** Assign a node value; later edits are isolated. */
    CConfigNode& operator=(const CConfigNode&) noexcept;

    /** Release node storage. */
    ~CConfigNode();

    /** Return the node kind. */
    ENodeType type() const noexcept;

    /** Return an owned name. */
    std::string name() const;

    /** Return ordered arguments, retaining empty strings and duplicates. */
    std::vector<std::string> arguments() const;

    /** Return ordered child values, empty for a directive. */
    std::vector<CConfigNode> children() const;

    /** Return child indexes with an exact matching name, preserving order. */
    std::vector<std::size_t> find(std::string_view name) const;

    /** Return the maximum nested block count, zero for a directive. */
    std::size_t depth() const noexcept;

    /** Replace the name after validation; failure preserves the original. */
    void setName(std::string name);

    /** Replace arguments after validation; failure preserves the original. */
    void setArguments(std::vector<std::string> arguments);

    /** Replace block children; reject directives or excessive nesting. */
    void setChildren(std::vector<CConfigNode> children);
};

} // namespace hnconf

#endif
