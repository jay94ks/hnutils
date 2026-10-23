#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <stdexcept>

namespace sdkv_example {
namespace {

constexpr std::array<std::uint8_t, 8> MAGIC{'S', 'D', 'K', 'V', 'D', 'E', 'M', '1'};

void appendNumber(distkv::SBytes& bytes, std::uint64_t value, unsigned size) {
    for (unsigned i = size; i != 0; --i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
}

void appendText(distkv::SBytes& bytes, std::string_view text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
}

class Reader {
private:
    std::span<const std::byte> _bytes;

public:
    explicit Reader(std::span<const std::byte> bytes) : _bytes(bytes) {}

    std::uint64_t number(unsigned size) {
        if (_bytes.size() < size) { throw std::invalid_argument("truncated number"); }
        std::uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i) {
            result = (result << 8) | std::to_integer<std::uint8_t>(_bytes[i]);
        }
        _bytes = _bytes.subspan(size);
        return result;
    }

    distkv::SBytes bytes(std::size_t count) {
        if (_bytes.size() < count) { throw std::invalid_argument("truncated bytes"); }
        distkv::SBytes result(count);
        if (count != 0) { std::memcpy(result.data(), _bytes.data(), count); }
        _bytes = _bytes.subspan(count);
        return result;
    }

    std::string text(std::size_t count) {
        if (_bytes.size() < count) { throw std::invalid_argument("truncated text"); }
        std::string result(reinterpret_cast<const char*>(_bytes.data()), count);
        _bytes = _bytes.subspan(count);
        return result;
    }

    bool empty() const noexcept { return _bytes.empty(); }
};

} // namespace

/* Node identities are application-level labels, not claimed IP addresses. */
bool ValidId(std::string_view id) noexcept {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

/* Keep conflict policy explicitly inside this example. */
bool Owns(std::string_view owner, std::string_view key) {
    return key == "demo/" + std::string(owner) || (owner == "host1" && key == "cluster/message");
}

/* Encode integers in network byte order, never by copying a native C++ struct. */
distkv::SBytes Encode(const SBeacon& beacon) {
    if (!ValidId(beacon.owner) || beacon.incarnation == 0 || beacon.sequence == 0 || beacon.records.size() > 2) {
        throw std::invalid_argument("invalid beacon header");
    }
    distkv::SBytes result(MAGIC.begin(), MAGIC.end());
    appendNumber(result, beacon.owner.size(), 1);
    appendText(result, beacon.owner);
    appendNumber(result, beacon.incarnation, 8);
    appendNumber(result, beacon.sequence, 8);
    appendNumber(result, beacon.records.size(), 1);
    std::set<std::string> keys;
    for (const auto& record : beacon.records) {
        if (!Owns(beacon.owner, record.key) || record.value.size() > MAX_DEMO_VALUE ||
            record.revision == 0 || !keys.insert(record.key).second) {
            throw std::invalid_argument("invalid owned record");
        }
        appendNumber(result, record.key.size(), 1);
        appendNumber(result, record.value.size(), 2);
        appendNumber(result, record.revision, 8);
        appendText(result, record.key);
        result.insert(result.end(), record.value.begin(), record.value.end());
    }
    if (result.size() > MAX_DATAGRAM) { throw std::invalid_argument("beacon exceeds datagram limit"); }
    return result;
}

/* Parse the entire packet before any discovery or sdkv state can change. */
std::optional<SBeacon> Decode(std::span<const std::byte> packet) {
    if (packet.size() > MAX_DATAGRAM || packet.size() < MAGIC.size()) { return std::nullopt; }
    if (std::memcmp(packet.data(), MAGIC.data(), MAGIC.size()) != 0) { return std::nullopt; }
    try {
        Reader reader(packet.subspan(MAGIC.size()));
        SBeacon result;
        const auto idLength = reader.number(1);
        if (idLength == 0 || idLength > 32) { return std::nullopt; }
        result.owner = reader.text(static_cast<std::size_t>(idLength));
        result.incarnation = reader.number(8);
        result.sequence = reader.number(8);
        const auto count = reader.number(1);
        if (!ValidId(result.owner) || result.incarnation == 0 || result.sequence == 0 || count > 2) {
            return std::nullopt;
        }
        std::set<std::string> keys;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto keyLength = reader.number(1);
            const auto valueLength = reader.number(2);
            const auto revision = reader.number(8);
            if (keyLength == 0 || keyLength > 64 || valueLength > MAX_DEMO_VALUE || revision == 0) {
                return std::nullopt;
            }
            auto key = reader.text(static_cast<std::size_t>(keyLength));
            auto value = reader.bytes(static_cast<std::size_t>(valueLength));
            if (!Owns(result.owner, key) || !keys.insert(key).second) { return std::nullopt; }
            result.records.push_back({std::move(key), std::move(value), revision});
        }
        if (!reader.empty()) { return std::nullopt; }
        return result;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

} // namespace sdkv_example
