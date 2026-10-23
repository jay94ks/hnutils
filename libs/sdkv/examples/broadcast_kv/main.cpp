#include "scenario.hpp"

#include <asyncpp/sync_wait.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {

using namespace std::chrono_literals;
using namespace sdkv_example;

volatile std::sig_atomic_t stopped = 0;

void requestStop(int) { stopped = 1; }

void configureConsoleFailures() {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

distkv::SBytes bytes(std::string_view value) {
    return distkv::SBytes(value.begin(), value.end());
}

/* Escape untrusted application bytes instead of writing terminal control codes. */
std::string render(const distkv::SBytes& value) {
    std::ostringstream result;
    result << '"';
    for (const auto ch : value) {
        if (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') {
            result << static_cast<char>(ch);
        } else {
            result << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch);
        }
    }
    result << '"';
    return result.str();
}

class CConsoleListener final : public distkv::ITableListener {
private:
    std::string _id;
    bool _quiet;
    bool _failed = false;

    void print(std::string_view type, const distkv::CTableChange& change) {
        if (_quiet) { return; }
        std::cout << '[' << _id << "] " << type << " key=" << render(change.key())
                  << " prev=" << (change.prev() ? render(change.prev()->bytes) : "<absent>")
                  << " now=" << (change.now() ? render(change.now()->bytes) : "<absent>")
                  << " revision=" << change.revision() << '\n';
    }

public:
    CConsoleListener(std::string id, bool quiet) : _id(std::move(id)), _quiet(quiet) {}
    void onAdded(const distkv::CTableChange& change) override { print("ADD", change); }
    void onChanged(const distkv::CTableChange& change) override { print("CHANGE", change); }
    void onRemoved(const distkv::CTableChange& change) override { print("REMOVE", change); }
    void onError(const distkv::CTableChange&, std::exception_ptr) noexcept override { _failed = true; }
    bool failed() const noexcept { return _failed; }
};

struct SArguments {
    SNodeOptions node;
    std::string value;
    std::uint64_t durationMs = 0;
    std::uint64_t updateAfterMs = 2000;
    bool quiet = false;
    bool silent = false;
    bool pickPort = false;
    bool help = false;
    std::optional<std::size_t> expectedKeys;
    std::optional<std::size_t> expectedPeak;
    std::optional<std::string> expectedValue;
    std::optional<std::string> absentKey;
};

std::uint64_t number(std::string_view value, std::uint64_t maximum) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > maximum) {
        throw std::invalid_argument("invalid numeric option");
    }
    return result;
}

SArguments arguments(int argc, char** argv) {
    SArguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--quiet") { result.quiet = true; continue; }
        if (option == "--silent") { result.quiet = true; result.silent = true; continue; }
        if (option == "--pick-port") { result.pickPort = true; continue; }
        if (option == "--help") { result.help = true; continue; }
        if (i + 1 == argc) { throw std::invalid_argument("missing option value"); }
        const std::string_view value(argv[++i]);
        if (option == "--id") { result.node.id = value; }
        else if (option == "--port") { result.node.port = static_cast<std::uint16_t>(number(value, 65535)); }
        else if (option == "--broadcast") { result.node.broadcastIp = value; }
        else if (option == "--interface") { result.node.interfaceIp = value; }
        else if (option == "--value") { result.value = value; }
        else if (option == "--duration") { result.durationMs = number(value, 86400) * 1000; }
        else if (option == "--update-after") { result.updateAfterMs = number(value, 86400) * 1000; }
        else if (option == "--expect-keys") { result.expectedKeys = static_cast<std::size_t>(number(value, 100)); }
        else if (option == "--expect-peak") { result.expectedPeak = static_cast<std::size_t>(number(value, 100)); }
        else if (option == "--expect-value") { result.expectedValue = value; }
        else if (option == "--expect-absent") { result.absentKey = value; }
        else { throw std::invalid_argument("unknown option: " + std::string(option)); }
    }
    if (!result.help && !result.pickPort && (!ValidId(result.node.id) || result.node.port == 0)) {
        throw std::invalid_argument("--id is required and --port must be nonzero");
    }
    if (result.value.empty()) { result.value = "hello-from-" + result.node.id; }
    std::random_device random;
    result.node.incarnation = (static_cast<std::uint64_t>(random()) << 32) ^ random();
    if (result.node.incarnation == 0) { result.node.incarnation = 1; }
    return result;
}

template<typename T>
T run(sockpp::CSocketWatcher& watcher, asyncpp::task<T> task) {
    auto result = asyncpp::as_promise(std::move(task));
    while (result.wait_for(0ms) != std::future_status::ready) { watcher.runOne(20ms); }
    return result.get();
}

/* One process models one host; use the same port on four LAN hosts. */
int execute(int argc, char** argv) {
    const auto options = arguments(argc, argv);
    if (options.help) {
        std::cout << "sdkv_example_broadcast_kv --id host1|host2|host3|host4 [options]\n"
                  << "  --port N             shared UDP port (45454)\n"
                  << "  --broadcast IPv4     limited/directed broadcast (255.255.255.255)\n"
                  << "  --interface IPv4     optional local sender interface\n"
                  << "  --value TEXT         this host's demo/<id> application value\n"
                  << "  --duration SECONDS   0 runs until Ctrl+C (default 0)\n"
                  << "  --update-after SEC   host1 changes cluster/message (default 2)\n"
                  << "  --quiet              print final summary only\n"
                  << "  --silent             suppress stdout (test orchestration)\n"
                  << "  Beacons: 1 second. Remote-record TTL: 5 seconds. No authentication.\n";
        return 0;
    }
    if (options.pickPort) {
        auto probe = sockpp::CSocket::open(sockpp::EAF_INET, sockpp::ESOCK_DATAGRAM, sockpp::EPROTO_UDP);
        probe.bind(sockpp::CSocketAddress::anyIpv4(0));
        std::cout << probe.localAddress().port() << '\n';
        return 0;
    }
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    sockpp::CSocketWatcher watcher;
    CBroadcastNode node(watcher, options.node);
    auto table = node.replica().table();
    auto listener = std::make_shared<CConsoleListener>(options.node.id, options.quiet);
    auto registration = table->subscribe(listener, false);
    run(watcher, node.replica().setOwned("demo/" + options.node.id, bytes(options.value)));
    if (options.node.id == "host1") {
        run(watcher, node.replica().setOwned("cluster/message", bytes("shared-start")));
    }
    const auto start = MonotonicMillis();
    bool updated = false;
    std::map<std::string, std::string> discovered;
    for (;;) {
        const auto now = MonotonicMillis();
        const auto elapsed = now - start;
        if (stopped || (options.durationMs != 0 && elapsed >= options.durationMs)) { break; }
        if (!updated && options.node.id == "host1" && elapsed >= options.updateAfterMs) {
            run(watcher, node.replica().setOwned("cluster/message", bytes("shared-updated")));
            updated = true;
        }
        run(watcher, node.tick(now));
        for (const auto& peer : node.replica().peers()) {
            if (!options.quiet && (!discovered.contains(peer.id) || discovered[peer.id] != peer.ip)) {
                std::cout << '[' << options.node.id << "] discovered " << peer.id << " ip=" << peer.ip << '\n';
            }
            discovered[peer.id] = peer.ip;
        }
        // --> Bound idle CPU use and TTL latency without pending receive cancellation.
        watcher.runOne(20ms);
    }
    const auto entries = table->snapshot();
    if (!options.silent) {
        std::cout << '[' << options.node.id << "] SUMMARY keys=" << entries.size()
                  << " peak=" << node.replica().peakKeys() << " sent=" << node.sentBeacons()
                  << " discovered=" << discovered.size() << '\n';
    }
    for (const auto& entry : entries) {
        if (!options.quiet) { std::cout << "  " << render(entry.key) << '=' << render(entry.value.bytes) << '\n'; }
    }
    if (listener->failed()) { throw std::runtime_error("sdkv listener failed"); }
    if (options.expectedKeys && entries.size() != *options.expectedKeys) {
        throw std::runtime_error("unexpected final key count");
    }
    if (options.expectedPeak && node.replica().peakKeys() < *options.expectedPeak) {
        throw std::runtime_error("four-host collection was not fully synchronized");
    }
    if (options.expectedValue) {
        distkv::SBytes value;
        if (table->get(bytes("cluster/message"), value) != distkv::ERET_OK || value != bytes(*options.expectedValue)) {
            throw std::runtime_error("common cluster/message was not replicated");
        }
    }
    if (options.absentKey) {
        distkv::SBytes value;
        if (table->get(bytes(*options.absentKey), value) != distkv::ERET_NOT_FOUND) {
            throw std::runtime_error("absent owner's record did not expire");
        }
    }
    if ((options.expectedKeys || options.expectedPeak) && discovered.size() != 3) {
        throw std::runtime_error("not all three other hosts were discovered by IP");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    configureConsoleFailures();
    try {
        return execute(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "broadcast_kv: " << error.what() << '\n';
        return 1;
    }
}
