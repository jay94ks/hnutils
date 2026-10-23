#ifndef __INCLUDE_HNCONF_CONFIG_HPP__
#define __INCLUDE_HNCONF_CONFIG_HPP__

#include <hnconf/error.hpp>
#include <hnconf/node.hpp>
#include <filesystem>

namespace hnconf {

/**
 * Ordered document with private copy-on-write storage.
 * No directive semantics, include expansion, or variable substitution are performed.
 */
class HNCONF_API CConfig {
private:
    struct Impl;
    std::shared_ptr<const Impl> _impl;

public:
    /** Construct a document from ordered nodes, or an empty document. */
    explicit CConfig(std::vector<CConfigNode> nodes = {});

    /** Copy a document; later edits are independent. */
    CConfig(const CConfig&) noexcept;

    /** Assign a document; later edits are independent. */
    CConfig& operator=(const CConfig&) noexcept;

    /** Release the document. */
    ~CConfig();

    /**
     * Parse complete text, discarding comments/spacing and retaining duplicates.
     * @param text Borrowed only until this call returns.
     * @param options Resource limits; maxDepth must not exceed MAX_DEPTH.
     * @param source Diagnostic label, not a file to open.
     * @return A complete document; malformed input throws CParseError.
     */
    static CConfig parse(std::string_view text, SParseOptions options = {}, std::string source = "<memory>");

    /** Read a binary file and parse it; I/O failures throw std::system_error. */
    static CConfig load(const std::filesystem::path& path, SParseOptions options = {});

    /** Return isolated top-level node values. */
    std::vector<CConfigNode> nodes() const;

    /** Replace all top-level nodes. */
    void setNodes(std::vector<CConfigNode> nodes);

    /** Return top-level indexes with an exact matching name. */
    std::vector<std::size_t> find(std::string_view name) const;

    /** Append a top-level node. */
    void addNode(CConfigNode node);

    /** Replace a node; invalid indexes throw std::out_of_range. */
    void replaceNode(std::size_t index, CConfigNode node);

    /** Remove a node; invalid indexes throw std::out_of_range. */
    void removeNode(std::size_t index);

    /**
     * Write canonical LF text with safe quoting; limits throw std::length_error.
     * @return Text that parses to the same ordered names, arguments and children.
     */
    std::string serialize(SWriteOptions options = {}) const;

    /**
     * Serialize, then create/truncate a binary file; check write and close errors.
     * @note Not atomic or crash-durable; I/O failure can leave a partial file.
     * Parent directories are not created; existing permissions are not changed.
     */
    void save(const std::filesystem::path& path, SWriteOptions options = {}) const;
};

} // namespace hnconf

#endif
