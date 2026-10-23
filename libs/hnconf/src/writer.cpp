#include <hnconf/config.hpp>

#include "internal.hpp"

#include <utility>

namespace hnconf {
namespace detail {
namespace {

class Writer {
private:
    const SWriteOptions& _options;
    std::string _output;

    void append(std::string_view text) {
        if (text.size() > _options.maxOutputBytes - _output.size()) {
            throw std::length_error("hnconf output byte limit exceeded");
        }
        _output.append(text);
    }

    void token(const std::string& value) {
        bool quote = value.empty();
        for (unsigned char ch : value) {
            if (ch <= 32 || ch == 127 || ch == ';' || ch == '{' || ch == '}' || ch == '#' ||
                ch == '\\' || ch == '\'' || ch == '"') { quote = true; break; }
        }
        if (!quote) { append(value); return; }
        append("\"");
        for (const auto ch : value) {
            switch (ch) {
            case '\\': append("\\\\"); break;
            case '"': append("\\\""); break;
            case '\n': append("\\n"); break;
            case '\r': append("\\r"); break;
            case '\t': append("\\t"); break;
            default: append(std::string_view(&ch, 1));
            }
        }
        append("\"");
    }

    void list(const std::vector<CConfigNode>& nodes, std::size_t depth) {
        for (const auto& node : nodes) {
            append(std::string(depth * _options.indentWidth, ' '));
            token(node.name());
            for (const auto& argument : node.arguments()) { append(" "); token(argument); }
            if (node.type() == ENODE_DIRECTIVE) { append(";\n"); }
            else {
                append(" {\n");
                list(node.children(), depth + 1);
                append(std::string(depth * _options.indentWidth, ' '));
                append("}\n");
            }
        }
    }

public:
    explicit Writer(const SWriteOptions& options) : _options(options) {}

    std::string write(const std::vector<CConfigNode>& nodes) {
        if (_options.indentWidth > 16) { throw std::invalid_argument("indentWidth must not exceed sixteen"); }
        list(nodes, 0);
        return std::move(_output);
    }
};

} // namespace

/* Quote tokens without treating variables, regexes or include paths as executable input. */
std::string writeNodes(const std::vector<CConfigNode>& nodes, const SWriteOptions& options) {
    return Writer(options).write(nodes);
}

} // namespace detail
} // namespace hnconf
