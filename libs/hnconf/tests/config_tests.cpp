#include <hnconf.hpp>

#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>
#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <optional>
#include <random>
#include <system_error>
#include <thread>

namespace {

using namespace hnconf;
using namespace std::chrono_literals;

class TestDirectory {
private:
    std::filesystem::path _base = std::filesystem::canonical(std::filesystem::current_path());
    std::filesystem::path _path;

public:
    TestDirectory() {
        std::random_device random;
        for (unsigned i = 0; i < 64; ++i) {
            auto candidate = _base / (".hnconf-test-" + std::to_string(random()) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) { _path = std::move(candidate); return; }
        }
        throw std::runtime_error("could not create test directory");
    }

    ~TestDirectory() {
        if (!_path.empty() && _path.parent_path() == _base &&
            _path.filename().string().starts_with(".hnconf-test-")) {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
    }

    std::filesystem::path file(const std::filesystem::path& name = "test.conf") const { return _path / name; }
};

class ManualDispatcher final : public asyncpp::dispatcher {
private:
    std::deque<std::function<void()>> _queue;

public:
    void push(std::function<void()> fn) override { _queue.push_back(std::move(fn)); }
    std::size_t pending() const { return _queue.size(); }
    void run() {
        while (!_queue.empty()) {
            auto fn = std::move(_queue.front());
            _queue.pop_front();
            fn();
        }
    }
};

class RejectDispatcher final : public asyncpp::dispatcher {
public:
    void push(std::function<void()>) override { throw std::runtime_error("executor rejected task"); }
};

void sameNodes(const std::vector<CConfigNode>& left, const std::vector<CConfigNode>& right) {
    DOCTEST_REQUIRE(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        DOCTEST_CHECK(left[i].type() == right[i].type());
        DOCTEST_CHECK(left[i].name() == right[i].name());
        DOCTEST_CHECK(left[i].arguments() == right[i].arguments());
        sameNodes(left[i].children(), right[i].children());
    }
}

asyncpp::task<std::thread::id> resumedThread(CConfigAsync& adapter) {
    auto parsed = co_await adapter.parse("worker_processes auto;");
    if (parsed.nodes().size() != 1) { throw std::runtime_error("unexpected parsed document"); }
    co_return std::this_thread::get_id();
}

DOCTEST_TEST_CASE("ordered directives blocks duplicate keys and opaque includes parse") {
    auto config = CConfig::parse(R"(
        # Context names and directive names have no built-in semantics.
        worker_processes auto;
        include /not/opened/*.conf;
        http {
            server {
                listen 8080;
                listen 8443 ssl;
                location / { proxy_pass http://${upstream}/api; }
            }
            server {}
        }
        flag;
    )");
    DOCTEST_CHECK(config.nodes().size() == 4);
    DOCTEST_CHECK(config.nodes()[1].arguments()[0] == "/not/opened/*.conf");
    auto http = config.nodes()[2];
    DOCTEST_CHECK(http.type() == ENODE_BLOCK);
    DOCTEST_CHECK(http.find("server") == std::vector<std::size_t>{0, 1});
    auto server = http.children()[0];
    DOCTEST_CHECK(server.find("listen") == std::vector<std::size_t>{0, 1});
    DOCTEST_CHECK(server.children()[2].children()[0].arguments()[0] == "http://${upstream}/api");
    DOCTEST_CHECK(http.children()[1].children().empty());
    DOCTEST_CHECK(config.nodes()[3].arguments().empty());
}

DOCTEST_TEST_CASE("quotes comments nginx escapes variables and regex bytes are retained") {
    auto config = CConfig::parse(R"CONF(
        value '' "" "space ; { } # hash" 'single "quoted"' foo#bar; # comment
        escape "a\tb\nc\rd\\e\"f\'g\d\$x" a\ b x\;y;
        regex ~ ^/user/\d+$;
        variable ${host} $uri;
    )CONF");
    const auto values = config.nodes()[0].arguments();
    DOCTEST_CHECK(values == std::vector<std::string>{"", "", "space ; { } # hash", "single \"quoted\"", "foo#bar"});
    const auto escaped = config.nodes()[1].arguments();
    DOCTEST_CHECK(escaped[0] == "a\tb\nc\rd\\e\"f'g\\d\\$x");
    DOCTEST_CHECK(escaped[1] == "a\\ b");
    DOCTEST_CHECK(escaped[2] == "x\\;y");
    DOCTEST_CHECK(config.nodes()[2].arguments()[1] == "^/user/\\d+$");
    DOCTEST_CHECK(config.nodes()[3].arguments()[0] == "${host}");
    DOCTEST_CHECK_NOTHROW(CConfig::parse("if ($host = \"example.test\") { allow all; }"));
}

DOCTEST_TEST_CASE("empty CRLF BOM and multiline quoted configuration is accepted") {
    DOCTEST_CHECK(CConfig::parse("").nodes().empty());
    DOCTEST_CHECK(CConfig::parse("  # comment\r\n\t").nodes().empty());
    auto config = CConfig::parse("\xEF\xBB\xBF" "http {\r\n value \"a\nb\";\r\n}\r\n");
    DOCTEST_CHECK(config.nodes()[0].children()[0].arguments()[0] == "a\nb");
    sameNodes(config.nodes(), CConfig::parse(config.serialize()).nodes());
}

DOCTEST_TEST_CASE("malformed configuration never returns a partial tree") {
    const std::vector<std::string> bad{
        ";", "{", "}", "name", "name value", "name {", "name { inner;", "name };",
        "name \"unterminated", "name 'unterminated", "name tail\\", "\"\" value;",
        "name \"a\"suffix;", "name { };", "name { inner value }", "name \"value\"#bad;"
    };
    for (const auto& text : bad) {
        DOCTEST_INFO(text);
        DOCTEST_CHECK_THROWS_AS(CConfig::parse(text), CParseError);
    }
    DOCTEST_CHECK_THROWS_AS(CConfig::parse(std::string("#x\0y", 4)), CParseError);
    DOCTEST_CHECK_THROWS_AS(CConfig::parse(std::string("x \"a\0b\";", 8)), CParseError);
}

DOCTEST_TEST_CASE("parse errors expose copied source line byte column and offset") {
    try {
        (void)CConfig::parse("http {\n  x \"unterminated", {}, "demo.conf");
        DOCTEST_FAIL("parse should fail");
    } catch (const CParseError& error) {
        auto copy = error;
        DOCTEST_CHECK(copy.source() == "demo.conf");
        DOCTEST_CHECK(copy.line() == 2);
        DOCTEST_CHECK(copy.column() == 5);
        DOCTEST_CHECK(copy.offset() == 11);
        DOCTEST_CHECK(std::string(copy.what()).find("demo.conf:2:5") != std::string::npos);
    }
    try {
        (void)CConfig::parse("x;\r\n}");
        DOCTEST_FAIL("parse should fail");
    } catch (const CParseError& error) {
        DOCTEST_CHECK(error.line() == 2);
        DOCTEST_CHECK(error.column() == 1);
        DOCTEST_CHECK(error.offset() == 4);
    }
}

DOCTEST_TEST_CASE("parser resource limits enforce exact boundaries") {
    SParseOptions options;
    options.maxInputBytes = 2;
    DOCTEST_CHECK_NOTHROW(CConfig::parse("x;", options));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("x; ", options), CParseError);
    options = {};
    options.maxTokenBytes = 1;
    DOCTEST_CHECK_NOTHROW(CConfig::parse("x a;", options));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("xx;", options), CParseError);
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("x \"aa\";", options), CParseError);
    options = {};
    options.maxArguments = 0;
    DOCTEST_CHECK_NOTHROW(CConfig::parse("x;", options));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("x a;", options), CParseError);
    options = {};
    options.maxNodes = 1;
    DOCTEST_CHECK_NOTHROW(CConfig::parse("x {}", options));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("x { y; }", options), CParseError);
    options = {};
    options.maxDepth = 0;
    DOCTEST_CHECK_NOTHROW(CConfig::parse("x;", options));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("x {}", options), CParseError);
    options.maxDepth = MAX_DEPTH + 1;
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("", options), std::invalid_argument);
    options = {};
    options.maxNodes = 0;
    DOCTEST_CHECK_NOTHROW(CConfig::parse("# blank", options));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("x;", options), CParseError);
}

DOCTEST_TEST_CASE("bounded nesting protects parsed and programmatically built trees") {
    auto node = CConfigNode::directive("leaf");
    std::string text;
    for (std::size_t i = 0; i < MAX_DEPTH; ++i) {
        node = CConfigNode::block("block", {}, {node});
        text += "block { ";
    }
    text += "leaf;";
    text.append(MAX_DEPTH, '}');
    // --> A brace inside an unquoted token is not a closing-block token.
    // --> Here each closing brace is a separate token after a semicolon/brace.
    DOCTEST_CHECK(node.depth() == MAX_DEPTH);
    DOCTEST_CHECK_NOTHROW(CConfig::parse(text));
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("block {" + text + "}"), CParseError);
    DOCTEST_CHECK_THROWS_AS(CConfigNode::block("tooDeep", {}, {node}), std::length_error);
    auto parent = CConfigNode::block("parent");
    DOCTEST_CHECK_THROWS_AS(parent.setChildren({node}), std::length_error);
    DOCTEST_CHECK(parent.children().empty());
}

DOCTEST_TEST_CASE("canonical serialization round trips arbitrary non NUL bytes and empty values") {
    std::vector<std::string> arguments{"", "simple", "two words", "${host}", "a}b", "a#b", "\\d+", "\\ ", "\"'"};
    std::string allBytes;
    for (unsigned i = 1; i < 256; ++i) { allBytes.push_back(static_cast<char>(i)); }
    arguments.push_back(allBytes);
    CConfig config({CConfigNode::block("quoted #name", {"a;{}"}, {
        CConfigNode::directive("data", arguments), CConfigNode::block("empty")})});
    auto text = config.serialize();
    sameNodes(config.nodes(), CConfig::parse(text).nodes());
    DOCTEST_CHECK(CConfig::parse(text).serialize() == text);
    DOCTEST_CHECK(text.find("    data") != std::string::npos);
    SWriteOptions options;
    options.indentWidth = 2;
    DOCTEST_CHECK(config.serialize(options).find("  data") != std::string::npos);
    options.indentWidth = 17;
    DOCTEST_CHECK_THROWS_AS(config.serialize(options), std::invalid_argument);
    options = {};
    options.maxOutputBytes = text.size();
    DOCTEST_CHECK(config.serialize(options) == text);
    --options.maxOutputBytes;
    DOCTEST_CHECK_THROWS_AS(config.serialize(options), std::length_error);
}

DOCTEST_TEST_CASE("node edits and document snapshots cannot expose mutable internal state") {
    auto config = CConfig::parse("http { listen 80; } duplicate 1; duplicate 2;");
    auto snapshot = config;
    auto http = config.nodes()[0];
    auto children = http.children();
    children[0].setArguments({"8080"});
    DOCTEST_CHECK(http.children()[0].arguments()[0] == "80");
    http.setChildren(children);
    DOCTEST_CHECK(config.nodes()[0].children()[0].arguments()[0] == "80");
    config.replaceNode(0, http);
    DOCTEST_CHECK(config.nodes()[0].children()[0].arguments()[0] == "8080");
    DOCTEST_CHECK(snapshot.nodes()[0].children()[0].arguments()[0] == "80");
    DOCTEST_CHECK(config.find("duplicate") == std::vector<std::size_t>{1, 2});
    config.removeNode(1);
    config.addNode(CConfigNode::directive("last"));
    DOCTEST_CHECK(config.nodes()[2].name() == "last");
    DOCTEST_CHECK_THROWS_AS(config.removeNode(99), std::out_of_range);
    DOCTEST_CHECK_THROWS_AS(config.replaceNode(99, http), std::out_of_range);
    DOCTEST_CHECK_THROWS_AS(http.setName(""), std::invalid_argument);
    DOCTEST_CHECK(http.name() == "http");
    DOCTEST_CHECK_THROWS_AS(children[0].setChildren({http}), std::logic_error);
    DOCTEST_CHECK_THROWS_AS(children[0].setArguments({std::string("a\0b", 3)}), std::invalid_argument);
    DOCTEST_CHECK(children[0].arguments()[0] == "8080");
    DOCTEST_CHECK_THROWS_AS(CConfigNode::directive(""), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(CConfigNode::block(std::string("x\0", 2)), std::invalid_argument);
    config.setNodes({});
    DOCTEST_CHECK(config.nodes().empty());
    DOCTEST_CHECK(snapshot.nodes().size() == 3);
}

DOCTEST_TEST_CASE("binary file IO supports native Unicode paths truncation and empty documents") {
    TestDirectory directory;
    auto path = directory.file(std::filesystem::path(u8"\uD55C\uAE00.conf"));
    auto config = CConfig::parse("server { listen 80; name \"hello world\"; }");
    config.save(path);
    sameNodes(config.nodes(), CConfig::load(path).nodes());
    CConfig{}.save(path);
    DOCTEST_CHECK(std::filesystem::file_size(path) == 0);
    DOCTEST_CHECK(CConfig::load(path).nodes().empty());
    SParseOptions options;
    options.maxInputBytes = 2;
    config.save(path);
    DOCTEST_CHECK_THROWS_AS(CConfig::load(path, options), CParseError);
}

DOCTEST_TEST_CASE("file failures and serialization limits do not masquerade as empty configurations") {
    TestDirectory directory;
    DOCTEST_CHECK_THROWS_AS(CConfig::load(directory.file("missing.conf")), std::system_error);
    DOCTEST_CHECK_THROWS_AS(CConfig{}.save(directory.file("missing/child.conf")), std::system_error);
    auto path = directory.file();
    CConfig::parse("original value;").save(path);
    SWriteOptions options;
    options.maxOutputBytes = 1;
    DOCTEST_CHECK_THROWS_AS(CConfig::parse("replacement value;").save(path, options), std::length_error);
    DOCTEST_CHECK(CConfig::load(path).nodes()[0].name() == "original");
    auto invalidPath = std::filesystem::path(std::string("bad\0.conf", 9));
    DOCTEST_CHECK_THROWS_AS(CConfig::load(invalidPath), std::invalid_argument);
    DOCTEST_CHECK_THROWS_AS(CConfig{}.save(invalidPath), std::invalid_argument);
    { std::ofstream stream(path, std::ios::binary); stream << "bad {"; }
    try {
        (void)CConfig::load(path);
        DOCTEST_FAIL("file syntax should fail");
    } catch (const CParseError& error) {
        DOCTEST_CHECK(error.source().find("test.conf") != std::string::npos);
    }
}

DOCTEST_TEST_CASE("async operations queue own arguments and survive adapter destruction") {
    ManualDispatcher worker;
    auto adapter = std::make_unique<CConfigAsync>(worker);
    std::string text = "name before;";
    auto parsing = adapter->parse(text);
    text.assign("corrupted");
    auto config = CConfig::parse("value before;");
    auto serializing = adapter->serialize(config);
    config.replaceNode(0, CConfigNode::directive("changed"));
    adapter.reset();
    auto parsed = asyncpp::as_promise(std::move(parsing));
    auto serialized = asyncpp::as_promise(std::move(serializing));
    DOCTEST_CHECK(worker.pending() == 2);
    DOCTEST_CHECK(parsed.wait_for(0ms) == std::future_status::timeout);
    worker.run();
    DOCTEST_CHECK(parsed.get().nodes()[0].arguments()[0] == "before");
    DOCTEST_CHECK(serialized.get() == "value before;\n");
}

DOCTEST_TEST_CASE("async file save captures document snapshot before lazy task execution") {
    TestDirectory directory;
    ManualDispatcher worker;
    auto config = CConfig::parse("value before;");
    auto adapter = std::make_unique<CConfigAsync>(worker);
    auto path = directory.file();
    auto saving = adapter->save(config, path);
    path = directory.file("other.conf");
    config.setNodes({});
    adapter.reset();
    auto result = asyncpp::as_promise(std::move(saving));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(directory.file()));
    worker.run();
    result.get();
    DOCTEST_CHECK(CConfig::load(directory.file()).nodes()[0].arguments()[0] == "before");
    DOCTEST_CHECK_FALSE(std::filesystem::exists(path));
}

DOCTEST_TEST_CASE("async worker performs file IO and resumes callers off the initiating thread") {
    TestDirectory directory;
    asyncpp::thread_pool worker(2);
    CConfigAsync adapter(worker);
    DOCTEST_CHECK(asyncpp::as_promise(resumedThread(adapter)).get() != std::this_thread::get_id());
    auto config = asyncpp::as_promise(adapter.parse("server { listen 8080; }")).get();
    asyncpp::as_promise(adapter.save(config, directory.file())).get();
    auto loaded = asyncpp::as_promise(adapter.load(directory.file())).get();
    sameNodes(config.nodes(), loaded.nodes());
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(adapter.parse("bad {")).get(), CParseError);
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(adapter.load(directory.file("absent"))).get(), std::system_error);
}

DOCTEST_TEST_CASE("async scheduling and output limit failures propagate through tasks") {
    RejectDispatcher rejected;
    CConfigAsync adapter(rejected);
    DOCTEST_CHECK_THROWS_AS(asyncpp::as_promise(adapter.parse("x;")).get(), std::runtime_error);
    ManualDispatcher worker;
    CConfigAsync queued(worker);
    SWriteOptions options;
    options.maxOutputBytes = 1;
    auto result = asyncpp::as_promise(queued.serialize(CConfig::parse("x;"), options));
    worker.run();
    DOCTEST_CHECK_THROWS_AS(result.get(), std::length_error);
}

DOCTEST_TEST_CASE("deterministic malformed byte corpus either fails cleanly or round trips") {
    std::minstd_rand random(123456);
    const std::string alphabet = "abc {};'\"\\#\t\n$012";
    SParseOptions options;
    options.maxDepth = 8;
    options.maxNodes = 50;
    options.maxTokenBytes = 256;
    for (unsigned trial = 0; trial < 2000; ++trial) {
        std::string text;
        const auto length = random() % 128;
        for (unsigned i = 0; i < length; ++i) { text.push_back(alphabet[random() % alphabet.size()]); }
        std::optional<CConfig> config;
        try {
            config = CConfig::parse(text, options);
        } catch (const CParseError&) {
            // --> A located syntax or limit failure is an expected corpus outcome.
        }
        if (config) { sameNodes(config->nodes(), CConfig::parse(config->serialize()).nodes()); }
    }
}

} // namespace
