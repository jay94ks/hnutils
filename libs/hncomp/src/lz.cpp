#include "internal.hpp"

#include <map>
#include <unordered_map>

namespace hncomp {
namespace detail {
namespace {

void varint(SBytes& output, std::uint32_t value) {
    do { auto byte = value & 127; value >>= 7; output.push_back(static_cast<std::uint8_t>(byte | (value ? 128 : 0))); } while (value);
}

std::uint32_t varint(Reader& reader) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        const auto byte = reader.byte();
        if (shift == 28 && byte > 15) { invalid("varint overflow"); }
        value |= std::uint32_t(byte & 127) << shift;
        if (!(byte & 128)) { return value; }
    }
    invalid("invalid varint");
}

struct Match { std::size_t length = 0; std::size_t distance = 0; };

class Matcher {
private:
    SReadOnlyByteSpan _input;
    std::vector<std::size_t> _heads = std::vector<std::size_t>(65536, SIZE_MAX);
    std::vector<std::size_t> _previous;
    std::size_t _window;

public:
    explicit Matcher(SReadOnlyByteSpan input, std::size_t window) : _input(input), _previous(65536, SIZE_MAX), _window(std::min<std::size_t>(65535, window)) {}
    Match match(std::size_t pos, std::size_t maximum, unsigned effort = 32) {
        if (pos + 4 > _input.size()) { return {}; }
        const auto hash = ((_input[pos] * 251u + _input[pos + 1]) * 251u + _input[pos + 2]) & 65535u;
        auto candidate = _heads[hash];
        _previous[pos % _previous.size()] = candidate; _heads[hash] = pos;
        Match result;
        while (candidate != SIZE_MAX && pos - candidate <= _window && effort--) {
            std::size_t length = 0;
            while (length < maximum && pos + length < _input.size() && _input[candidate + length] == _input[pos + length]) { ++length; }
            if (length > result.length) { result = {length, pos - candidate}; }
            const auto next = _previous[candidate % _previous.size()];
            if (next >= candidate) { break; }
            candidate = next;
        }
        return result;
    }
};

void literalSnappy(SBytes& output, SReadOnlyByteSpan input) {
    if (input.empty()) { return; }
    auto length = input.size() - 1;
    if (length < 60) { output.push_back(static_cast<std::uint8_t>(length << 2)); }
    else {
        unsigned bytes = 1;
        while (bytes < 4 && (length >> (8 * bytes))) { ++bytes; }
        output.push_back(static_cast<std::uint8_t>((59 + bytes) << 2)); putLe(output, length, bytes);
    }
    output.insert(output.end(), input.begin(), input.end());
}

std::uint32_t maskedCrc(SReadOnlyByteSpan input) { return std::rotr(crc32(input, 0x82f63b78u), 15) + 0xa282ead8u; }

SBytes snappyBlock(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    if (input.size() > UINT32_MAX) { limit("Snappy raw size exceeds uint32"); }
    SBytes output; varint(output, static_cast<std::uint32_t>(input.size()));
    Matcher matcher(input, options.maxWindowBytes);
    std::size_t pos = 0, literal = 0;
    while (pos + 4 <= input.size()) {
        const auto match = matcher.match(pos, input.size() - pos, options.level * 8);
        if (match.length < 4) { ++pos; continue; }
        literalSnappy(output, input.subspan(literal, pos - literal));
        auto remaining = match.length;
        while (remaining) {
            const auto length = std::min<std::size_t>(remaining, 64);
            output.push_back(static_cast<std::uint8_t>(((length - 1) << 2) | 2)); putLe(output, match.distance, 2);
            remaining -= length;
        }
        pos += match.length; literal = pos;
    }
    literalSnappy(output, input.subspan(literal));
    return output;
}

SBytes unsnappyBlock(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    Reader reader(input);
    const auto size = varint(reader);
    if (size > options.maxOutputBytes) { limit("Snappy declared output exceeds limit"); }
    SBytes output;
    while (output.size() < size) {
        const auto tag = reader.byte(); const unsigned kind = tag & 3;
        if (kind == 0) {
            std::size_t length = tag >> 2;
            if (length >= 60) { length = static_cast<std::size_t>(reader.le(static_cast<unsigned>(length - 59))); }
            ++length; room(output, length, size);
            auto data = reader.span(length); output.insert(output.end(), data.begin(), data.end());
        } else {
            const auto length = kind == 1 ? ((tag >> 2) & 7u) + 4u : (tag >> 2) + 1u;
            const auto distance = kind == 1 ? ((tag & 224u) << 3) | reader.byte() : reader.le(kind == 2 ? 2 : 4);
            if (distance > options.maxWindowBytes) { limit("Snappy backward distance exceeds window policy"); }
            copyMatch(output, static_cast<std::size_t>(distance), length, size);
        }
    }
    if (reader.remaining()) { invalid("trailing Snappy raw data"); }
    return output;
}

void extended(SBytes& output, std::size_t length) {
    while (length >= 255) { output.push_back(255); length -= 255; }
    output.push_back(static_cast<std::uint8_t>(length));
}

SBytes lz4Block(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    SBytes output;
    Matcher matcher(input, options.maxWindowBytes);
    std::size_t pos = 0, literal = 0;
    while (pos + 12 <= input.size()) {
        auto match = matcher.match(pos, input.size() - pos - 5, options.level * 8);
        if (match.length < 4) { ++pos; continue; }
        const auto literals = pos - literal;
        output.push_back(static_cast<std::uint8_t>((std::min<std::size_t>(literals, 15) << 4) | std::min<std::size_t>(match.length - 4, 15)));
        if (literals >= 15) { extended(output, literals - 15); }
        output.insert(output.end(), input.begin() + literal, input.begin() + pos);
        putLe(output, match.distance, 2);
        if (match.length >= 19) { extended(output, match.length - 19); }
        pos += match.length; literal = pos;
    }
    const auto literals = input.size() - literal;
    output.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(literals, 15) << 4));
    if (literals >= 15) { extended(output, literals - 15); }
    output.insert(output.end(), input.begin() + literal, input.end());
    return output;
}

SBytes unlz4Block(SReadOnlyByteSpan input, const SCompressionOptions& options, SBytes output = {}) {
    Reader reader(input);
    auto length = [&](std::size_t initial) {
        if (initial != 15) { return initial; }
        for (;;) {
            const auto value = reader.byte();
            if (initial > options.maxOutputBytes || value > options.maxOutputBytes - initial) { limit("LZ4 length exceeds limit"); }
            initial += value;
            if (value != 255) { return initial; }
        }
    };
    while (reader.remaining()) {
        const auto token = reader.byte();
        const auto literals = length(token >> 4); room(output, literals, options.maxOutputBytes);
        auto data = reader.span(literals); output.insert(output.end(), data.begin(), data.end());
        if (!reader.remaining()) { return output; }
        const auto distance = reader.le(2);
        if (distance > options.maxWindowBytes) { limit("LZ4 backward distance exceeds window policy"); }
        const auto count = length(token & 15);
        if (count > options.maxOutputBytes - std::min<std::size_t>(4, options.maxOutputBytes)) { limit("LZ4 length overflow"); }
        copyMatch(output, static_cast<std::size_t>(distance), count + 4, options.maxOutputBytes);
    }
    invalid("LZ4 block has no final literal sequence");
}

} // namespace

/* Private LZ77 format: HNL7 + size, then (offset, length, optional next byte) triples. */
SBytes encodeLz77(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    SBytes output{'H', 'N', 'L', '7'}; putLe(output, input.size(), 8);
    Matcher matcher(input, options.maxWindowBytes);
    for (std::size_t pos = 0; pos < input.size();) {
        const auto match = matcher.match(pos, 258, options.level * 8);
        const auto length = match.length >= 3 ? match.length : 0;
        putLe(output, length ? match.distance : 0, 2); putLe(output, length, 2); pos += length;
        const bool next = pos < input.size(); output.push_back(next ? 1 : 0);
        if (next) { output.push_back(input[pos++]); }
    }
    return output;
}

/* Validate every length before emitting or referencing output. */
SBytes decodeLz77(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    Reader reader(input);
    if (reader.le(4) != 0x374c4e48u) { invalid("invalid HNL7 magic"); }
    const auto size = reader.le(8);
    if (size > options.maxOutputBytes) { limit("LZ77 declared size exceeds limit"); }
    SBytes output;
    while (output.size() < size) {
        const auto distance = reader.le(2), length = reader.le(2); const auto next = reader.byte();
        if (next > 1 || length > 258 || (length == 0) != (distance == 0)) { invalid("invalid LZ77 triple"); }
        if (distance > options.maxWindowBytes) { limit("LZ77 distance exceeds window policy"); }
        if (length) { copyMatch(output, distance, length, static_cast<std::size_t>(size)); }
        if (next) { room(output, 1, static_cast<std::size_t>(size)); output.push_back(reader.byte()); }
        else if (length == 0 || output.size() != size) { invalid("nonterminal LZ77 tail"); }
    }
    if (reader.remaining()) { invalid("trailing HNL7 bytes"); }
    return output;
}

/* Private LZ78 format HNL8, size, then varint parent + tagged byte; bounded dictionary. */
SBytes encodeLz78(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    SBytes output{'H', 'N', 'L', '8'}; putLe(output, input.size(), 8);
    putLe(output, options.maxDictionaryEntries, 4);
    std::map<std::pair<std::uint32_t, std::uint8_t>, std::uint32_t> dictionary;
    std::uint32_t parent = 0;
    for (auto ch : input) {
        const auto key = std::pair{parent, ch};
        if (auto found = dictionary.find(key); found != dictionary.end()) { parent = found->second; }
        else {
            varint(output, parent); output.push_back(1); output.push_back(ch);
            if (dictionary.size() < options.maxDictionaryEntries) { dictionary.emplace(key, static_cast<std::uint32_t>(dictionary.size() + 1)); }
            parent = 0;
        }
    }
    if (parent) { varint(output, parent); output.push_back(0); }
    return output;
}

/* Parent/byte nodes prevent quadratic dictionary-string storage. */
SBytes decodeLz78(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    Reader reader(input);
    if (reader.le(4) != 0x384c4e48u) { invalid("invalid HNL8 magic"); }
    const auto size = reader.le(8);
    if (size > options.maxOutputBytes) { limit("LZ78 declared output exceeds limit"); }
    const auto entries = reader.le(4);
    if (entries > options.maxDictionaryEntries) { limit("LZ78 dictionary entry limit exceeded"); }
    std::vector<std::pair<std::uint32_t, std::uint8_t>> dictionary{{0, 0}};
    SBytes output, phrase;
    while (output.size() < size) {
        auto parent = varint(reader); const auto tag = reader.byte();
        if (parent >= dictionary.size() || tag > 1 || (tag == 0 && parent == 0)) { invalid("invalid LZ78 dictionary record"); }
        const auto original = parent;
        phrase.clear();
        while (parent) { phrase.push_back(dictionary[parent].second); parent = dictionary[parent].first; }
        room(output, phrase.size(), static_cast<std::size_t>(size));
        output.insert(output.end(), phrase.rbegin(), phrase.rend());
        if (tag == 1) {
            room(output, 1, static_cast<std::size_t>(size));
            const auto ch = reader.byte(); output.push_back(ch);
            if (dictionary.size() - 1 < entries) { dictionary.emplace_back(original, ch); }
        } else if (output.size() != size) { invalid("nonterminal LZ78 tail"); }
    }
    if (reader.remaining()) { invalid("trailing HNL8 data"); }
    return output;
}

/* Standard raw Snappy or standard checksummed framed chunks. */
SBytes encodeSnappy(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    if (options.format == EFORMAT_RAW) { return snappyBlock(input, options); }
    SBytes output{0xff, 6, 0, 0, 's', 'N', 'a', 'P', 'p', 'Y'};
    for (std::size_t pos = 0; pos < input.size();) {
        const auto chunk = input.subspan(pos, std::min(options.blockBytes, input.size() - pos));
        auto compressed = snappyBlock(chunk, options);
        const bool stored = compressed.size() >= chunk.size();
        output.push_back(stored ? 1 : 0); putLe(output, 4 + (stored ? chunk.size() : compressed.size()), 3);
        putLe(output, maskedCrc(chunk), 4);
        if (stored) { output.insert(output.end(), chunk.begin(), chunk.end()); }
        else { output.insert(output.end(), compressed.begin(), compressed.end()); }
        pos += chunk.size();
    }
    return output;
}

/* Decode copy tags of all three widths; verify masked CRC32C for framed chunks. */
SBytes decodeSnappy(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    if (options.format == EFORMAT_RAW) { return unsnappyBlock(input, options); }
    Reader reader(input);
    bool identified = false;
    SBytes output;
    while (reader.remaining()) {
        const auto tag = reader.byte(); const auto size = reader.le(3); auto chunk = reader.span(size);
        if (tag == 0xff) {
            const SBytes magic{'s', 'N', 'a', 'P', 'p', 'Y'};
            if (chunk.size() != 6 || !std::equal(chunk.begin(), chunk.end(), magic.begin())) { invalid("invalid Snappy identifier"); }
            identified = true; continue;
        }
        if (!identified) { invalid("missing Snappy identifier"); }
        if (tag >= 0x80) { continue; }
        if (tag > 1) { unsupported("unrecognized mandatory Snappy chunk"); }
        if (chunk.size() < 4) { invalid("missing Snappy checksum"); }
        Reader block(chunk); const auto checksum = block.le(4);
        auto data = block.span(block.remaining());
        auto blockOptions = options; blockOptions.maxOutputBytes = std::min<std::size_t>(65536, options.maxOutputBytes - output.size());
        auto decoded = tag == 0 ? unsnappyBlock(data, blockOptions) : SBytes(data.begin(), data.end());
        if (decoded.size() > 65536 || checksum != maskedCrc(decoded)) { invalid("Snappy chunk checksum/size mismatch"); }
        room(output, decoded.size(), options.maxOutputBytes); output.insert(output.end(), decoded.begin(), decoded.end());
    }
    if (!identified) { invalid("missing Snappy stream identifier"); }
    return output;
}

/* Emit interoperable independent blocks with content-size and header checksum. */
SBytes encodeLz4(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    if (options.format == EFORMAT_RAW) { return lz4Block(input, options); }
    SBytes output{4, 0x22, 0x4d, 0x18};
    SBytes header{0x68, 0x40}; putLe(header, input.size(), 8); header.push_back(static_cast<std::uint8_t>(xxh32(header) >> 8));
    output.insert(output.end(), header.begin(), header.end());
    for (std::size_t pos = 0; pos < input.size();) {
        const auto chunk = input.subspan(pos, std::min(options.blockBytes, input.size() - pos));
        auto compressed = lz4Block(chunk, options);
        if (compressed.size() >= chunk.size()) {
            putLe(output, 0x80000000u | chunk.size(), 4); output.insert(output.end(), chunk.begin(), chunk.end());
        } else { putLe(output, compressed.size(), 4); output.insert(output.end(), compressed.begin(), compressed.end()); }
        pos += chunk.size();
    }
    putLe(output, 0, 4); return output;
}

/* Accept linked/independent standard frames, checksums, and optional content size. */
SBytes decodeLz4(SReadOnlyByteSpan input, const SCompressionOptions& options) {
    if (options.format == EFORMAT_RAW) { return unlz4Block(input, options); }
    Reader reader(input);
    if (reader.le(4) != 0x184d2204u) { invalid("invalid LZ4 frame magic"); }
    const auto headerStart = reader.pos(); const auto flags = reader.byte(), descriptor = reader.byte();
    if ((flags & 0xc0) != 0x40 || (flags & 2) || (descriptor & 0x8f)) { invalid("invalid LZ4 descriptor"); }
    const auto blockCode = (descriptor >> 4) & 7;
    if (blockCode < 4 || blockCode > 7) { invalid("invalid LZ4 block size"); }
    const std::size_t blockMaximum = std::size_t{1} << (8 + 2 * blockCode);
    if (blockMaximum > options.maxWindowBytes) { limit("LZ4 block window exceeds policy"); }
    const auto expected = flags & 8 ? reader.le(8) : UINT64_MAX;
    if (expected != UINT64_MAX && expected > options.maxOutputBytes) { limit("LZ4 declared size exceeds limit"); }
    if (flags & 1) { (void)reader.le(4); unsupported("LZ4 external dictionaries are not supplied"); }
    const auto checksum = static_cast<std::uint8_t>(xxh32(input.subspan(headerStart, reader.pos() - headerStart)) >> 8);
    if (reader.byte() != checksum) { invalid("LZ4 header checksum mismatch"); }
    SBytes output;
    for (;;) {
        const auto size = reader.le(4);
        if (size == 0) { break; }
        const auto length = size & 0x7fffffffu;
        if (length > blockMaximum) { invalid("LZ4 block exceeds descriptor"); }
        auto chunk = reader.span(length);
        if ((flags & 16) && reader.le(4) != xxh32(chunk)) { invalid("LZ4 block checksum mismatch"); }
        if (size & 0x80000000u) {
            room(output, chunk.size(), options.maxOutputBytes); output.insert(output.end(), chunk.begin(), chunk.end());
        } else {
            const auto previous = output.size();
            SBytes history;
            if (!(flags & 32)) { history.assign(output.end() - std::min<std::size_t>(65536, output.size()), output.end()); }
            const auto historySize = history.size();
            auto blockOptions = options;
            blockOptions.maxOutputBytes = historySize + std::min(blockMaximum, options.maxOutputBytes - previous);
            auto decoded = unlz4Block(chunk, blockOptions, std::move(history));
            output.insert(output.end(), decoded.begin() + historySize, decoded.end());
        }
    }
    if ((flags & 4) && reader.le(4) != xxh32(output)) { invalid("LZ4 content checksum mismatch"); }
    if (expected != UINT64_MAX && output.size() != expected) { invalid("LZ4 content size mismatch"); }
    if (reader.remaining()) { invalid("trailing LZ4 frame data"); }
    return output;
}

} // namespace detail
} // namespace hncomp
