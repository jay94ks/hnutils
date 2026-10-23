#include <hnconf/config.hpp>

#include "internal.hpp"

#include <utility>

namespace hnconf {
namespace detail {
namespace {

bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

enum Kind { Word, Semicolon, Open, Close, End };

struct Token {
    Kind kind;
    std::string text;
    std::size_t line;
    std::size_t column;
    std::size_t offset;
};

class Parser {
private:
    std::string_view _text;
    const SParseOptions& _options;
    const std::string& _source;
    std::size_t _pos = 0;
    std::size_t _line = 1;
    std::size_t _column = 1;
    std::size_t _nodes = 0;

    [[noreturn]] void fail(const std::string& message) const {
        throw CParseError(message, _source, _line, _column, _pos);
    }

    [[noreturn]] void fail(const Token& token, const std::string& message) const {
        throw CParseError(message, _source, token.line, token.column, token.offset);
    }

    char take() {
        const char ch = _text[_pos];
        if (ch == '\0') { fail("NUL byte in configuration"); }
        ++_pos;
        if (ch == '\n') { ++_line; _column = 1; }
        else { ++_column; }
        return ch;
    }

    void append(Token& token, char ch) {
        if (token.text.size() >= _options.maxTokenBytes) { fail(token, "token byte limit exceeded"); }
        token.text.push_back(ch);
    }

    void escape(Token& token) {
        take();
        if (_pos == _text.size()) { fail("unfinished escape"); }
        const char ch = take();
        switch (ch) {
        case 'n': append(token, '\n'); break;
        case 'r': append(token, '\r'); break;
        case 't': append(token, '\t'); break;
        case '\\': case '\'': case '"': append(token, ch); break;
        default:
            // --> Unknown escapes (including regexes and escaped spaces) retain the backslash.
            append(token, '\\');
            append(token, ch);
        }
    }

    Token next() {
        while (_pos < _text.size()) {
            if (space(_text[_pos])) { take(); continue; }
            if (_text[_pos] != '#') { break; }
            while (_pos < _text.size() && _text[_pos] != '\n') { take(); }
        }
        Token result{End, {}, _line, _column, _pos};
        if (_pos == _text.size()) { return result; }
        const auto first = _text[_pos];
        if (first == ';' || first == '{' || first == '}') {
            take();
            result.kind = first == ';' ? Semicolon : first == '{' ? Open : Close;
            return result;
        }
        result.kind = Word;
        if (first == '\'' || first == '"') {
            const auto quote = take();
            while (_pos < _text.size() && _text[_pos] != quote) {
                if (_text[_pos] == '\\') { escape(result); }
                else { append(result, take()); }
            }
            if (_pos == _text.size()) { fail(result, "unterminated quoted token"); }
            take();
            if (_pos < _text.size() && !space(_text[_pos]) && _text[_pos] != ';' &&
                _text[_pos] != '{' && _text[_pos] != ')') {
                fail("expected whitespace, ';' or '{' after quoted token");
            }
            return result;
        }
        bool dollar = false;
        while (_pos < _text.size()) {
            const char ch = _text[_pos];
            if (space(ch) || ch == ';' || (ch == '{' && !dollar)) { break; }
            if (ch == '\\') { escape(result); dollar = false; }
            else { append(result, take()); dollar = ch == '$'; }
        }
        return result;
    }

    std::vector<CConfigNode> list(bool inBlock, std::size_t depth) {
        std::vector<CConfigNode> result;
        for (;;) {
            auto name = next();
            if (name.kind == End) {
                if (inBlock) { fail(name, "expected '}' before end of input"); }
                return result;
            }
            if (name.kind == Close && inBlock) { return result; }
            if (name.kind != Word || name.text.empty()) { fail(name, "expected directive name"); }
            if (_nodes >= _options.maxNodes) { fail(name, "node count limit exceeded"); }
            ++_nodes;
            std::vector<std::string> arguments;
            for (;;) {
                auto token = next();
                if (token.kind == Word) {
                    if (arguments.size() >= _options.maxArguments) { fail(token, "argument count limit exceeded"); }
                    arguments.push_back(std::move(token.text));
                } else if (token.kind == Semicolon) {
                    result.push_back(CConfigNode::directive(std::move(name.text), std::move(arguments)));
                    break;
                } else if (token.kind == Open) {
                    if (depth >= _options.maxDepth) { fail(token, "block depth limit exceeded"); }
                    auto children = list(true, depth + 1);
                    result.push_back(CConfigNode::block(std::move(name.text), std::move(arguments), std::move(children)));
                    break;
                } else {
                    fail(token, "expected ';' or '{' after directive");
                }
            }
        }
    }

public:
    Parser(std::string_view text, const SParseOptions& options, const std::string& source)
        : _text(text), _options(options), _source(source) {}

    std::vector<CConfigNode> parse() {
        if (_text.size() > _options.maxInputBytes) { fail("input byte limit exceeded"); }
        // --> Accept a UTF-8 BOM only at the start; offsets still refer to original bytes.
        if (_text.starts_with("\xEF\xBB\xBF")) { take(); take(); take(); }
        return list(false, 0);
    }
};

} // namespace

/* Reject unsafe recursive depth settings before consuming input. */
void validateOptions(const SParseOptions& options) {
    if (options.maxDepth > MAX_DEPTH) { throw std::invalid_argument("maxDepth exceeds hnconf hard depth limit"); }
}

/* Parse without retaining borrowed input. */
std::vector<CConfigNode> parseNodes(std::string_view text, const SParseOptions& options, const std::string& source) {
    validateOptions(options);
    return Parser(text, options, source).parse();
}

} // namespace detail
} // namespace hnconf
