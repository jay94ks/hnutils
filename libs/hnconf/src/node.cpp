#include <hnconf/node.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hnconf {
namespace {

void validateToken(const std::string& token, bool name = false) {
    if ((name && token.empty()) || token.find('\0') != std::string::npos) {
        throw std::invalid_argument("hnconf tokens cannot contain NUL; names cannot be empty");
    }
}

void validateArguments(const std::vector<std::string>& arguments) {
    for (const auto& argument : arguments) { validateToken(argument); }
}

std::size_t blockDepth(const std::vector<CConfigNode>& children) {
    std::size_t depth = 1;
    for (const auto& child : children) { depth = std::max(depth, child.depth() + 1); }
    if (depth > MAX_DEPTH) { throw std::length_error("hnconf maximum tree depth exceeded"); }
    return depth;
}

} // namespace

struct CConfigNode::Impl {
    ENodeType type;
    std::string name;
    std::vector<std::string> arguments;
    std::vector<CConfigNode> children;
    std::size_t depth;
};

/* Adopt immutable storage. */
CConfigNode::CConfigNode(std::shared_ptr<const Impl> impl) : _impl(std::move(impl)) {}

/* Validate before constructing a leaf. */
CConfigNode CConfigNode::directive(std::string name, std::vector<std::string> arguments) {
    validateToken(name, true);
    validateArguments(arguments);
    return CConfigNode(std::make_shared<Impl>(Impl{ENODE_DIRECTIVE, std::move(name), std::move(arguments), {}, 0}));
}

/* Validate tokens and depth before constructing a block. */
CConfigNode CConfigNode::block(std::string name, std::vector<std::string> arguments,
    std::vector<CConfigNode> children) {
    validateToken(name, true);
    validateArguments(arguments);
    const auto depth = blockDepth(children);
    return CConfigNode(std::make_shared<Impl>(Impl{
        ENODE_BLOCK, std::move(name), std::move(arguments), std::move(children), depth}));
}

/* Copy only immutable storage ownership. */
CConfigNode::CConfigNode(const CConfigNode&) noexcept = default;

/* Assign immutable storage ownership. */
CConfigNode& CConfigNode::operator=(const CConfigNode&) noexcept = default;

/* Release the value. */
CConfigNode::~CConfigNode() = default;

/* Return the kind. */
ENodeType CConfigNode::type() const noexcept { return _impl->type; }

/* Return an owned name. */
std::string CConfigNode::name() const { return _impl->name; }

/* Return owned argument values. */
std::vector<std::string> CConfigNode::arguments() const { return _impl->arguments; }

/* Return isolated child values. */
std::vector<CConfigNode> CConfigNode::children() const { return _impl->children; }

/* Find every matching child, not just the first duplicate. */
std::vector<std::size_t> CConfigNode::find(std::string_view name) const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < _impl->children.size(); ++i) {
        if (_impl->children[i].name() == name) { result.push_back(i); }
    }
    return result;
}

/* Return the cached bounded depth. */
std::size_t CConfigNode::depth() const noexcept { return _impl->depth; }

/* Commit only after validation and allocation succeed. */
void CConfigNode::setName(std::string name) {
    validateToken(name, true);
    auto next = std::make_shared<Impl>(*_impl);
    next->name = std::move(name);
    _impl = std::move(next);
}

/* Replace owned arguments. */
void CConfigNode::setArguments(std::vector<std::string> arguments) {
    validateArguments(arguments);
    auto next = std::make_shared<Impl>(*_impl);
    next->arguments = std::move(arguments);
    _impl = std::move(next);
}

/* Snapshot children cannot create self-referential mutable trees. */
void CConfigNode::setChildren(std::vector<CConfigNode> children) {
    if (_impl->type != ENODE_BLOCK) { throw std::logic_error("a directive cannot contain children"); }
    const auto depth = blockDepth(children);
    auto next = std::make_shared<Impl>(*_impl);
    next->children = std::move(children);
    next->depth = depth;
    _impl = std::move(next);
}

} // namespace hnconf
