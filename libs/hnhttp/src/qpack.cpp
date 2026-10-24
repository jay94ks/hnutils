#include <hnhttp/qpack.hpp>
#include "codec_internal.hpp"
#include "qpack_table.hpp"
#include <deque>
#include <map>
#include <set>

namespace hnhttp::detail {
namespace {
constexpr auto maximumInteger = (UINT64_C(1)<<62)-1;
struct SEntry { std::uint64_t index; SHeaderField field; };
struct STable {
    std::deque<SEntry> entries;
    std::size_t capacity = 0, bytes = 0;
    std::uint64_t inserted = 0;
    const SHeaderField& at(std::uint64_t absolute) const {
        if (entries.empty() || absolute < entries.front().index || absolute > entries.back().index) { bad(); }
        return entries[static_cast<std::size_t>(absolute-entries.front().index)].field;
    }
};
struct SSection { std::uint64_t required; std::set<std::uint64_t> references; };
class CQpackCodec final : public IQpackCodec {
private:
    SQpackOptions _options;
    STable _encoder, _decoder;
    std::uint64_t _peerCapacity = 0, _peerBlocked = 0, _known = 0;
    bool _poisoned = false;
    SBytes _encoderInput, _decoderInput, _decoderOutput;
    std::map<std::uint64_t, std::deque<SSection>> _outstanding;
    std::map<std::uint64_t, std::uint64_t> _blocked;
    static std::size_t size(const SHeaderField& field) { return field.name.size()+field.value.size()+32; }
    bool referenced(std::uint64_t index) const {
        for (const auto& [id, sections] : _outstanding) { (void)id;
            for (const auto& section : sections) { if (section.references.contains(index)) { return true; } }
        }
        return false;
    }
    void evict(STable& table, std::size_t target, bool protect) {
        while (table.bytes > target) {
            if (table.entries.empty()) { bad(); }
            if (protect && referenced(table.entries.front().index)) { bad(ERET_BUSY); }
            table.bytes -= size(table.entries.front().field); table.entries.pop_front();
        }
    }
    void add(STable& table, SHeaderField field, bool protect) {
        if (size(field) > table.capacity || table.inserted == maximumInteger) { bad(); }
        auto copy = table; evict(copy, copy.capacity-size(field), protect);
        copy.bytes += size(field); copy.entries.push_back({copy.inserted++, std::move(field)}); table = std::move(copy);
    }
    void queue(SBytes bytes) {
        if (bytes.size() > _options.codec.maximumEncodedBytes-_decoderOutput.size()) { bad(ERET_LIMIT); }
        _decoderOutput.insert(_decoderOutput.end(), bytes.begin(), bytes.end());
    }
    void boundField(const SHeaderField& f, std::size_t& count, std::size_t& bytes) const {
        if (count == _options.codec.maximumFields || size(f) > _options.codec.maximumDecodedBytes-bytes) { bad(ERET_LIMIT); }
        ++count; bytes += size(f);
    }
    static SHeaderField entry(bool isStatic, std::uint64_t index, const STable& table) {
        if (isStatic) { if (index >= std::size(qpackStatic)) { bad(); } return qpackStatic[index]; }
        if (index >= table.inserted) { bad(); } return table.at(table.inserted-index-1);
    }
    SHttpError feed(std::span<const std::uint8_t> bytes, bool encoder) {
        if (_poisoned) { return error(ERET_INVALID_STATE); }
        auto& input = encoder ? _encoderInput : _decoderInput;
        if (bytes.size() > _options.codec.maximumEncodedBytes-input.size()) { _poisoned = true; return error(ERET_LIMIT); }
        input.insert(input.end(), bytes.begin(), bytes.end()); std::size_t consumed = 0;
        const auto previous = _decoder.inserted;
        try {
            while (consumed < input.size()) {
                auto position = consumed; const auto first = input[position];
                try {
                    if (encoder) {
                        if (first & 128) {
                            auto field = entry((first & 64) != 0, integer(input, position, 6), _decoder);
                            field.value = literal(input, position, 7, 128, _options.codec.maximumDecodedBytes); add(_decoder, std::move(field), false);
                        } else if (first & 64) {
                            SHeaderField field; field.name = literal(input, position, 5, 32, _options.codec.maximumDecodedBytes);
                            field.value = literal(input, position, 7, 128, _options.codec.maximumDecodedBytes); add(_decoder, std::move(field), false);
                        } else if (first & 32) {
                            const auto capacity = integer(input, position, 5);
                            if (capacity > _options.codec.maximumDynamicTableBytes) { bad(); }
                            _decoder.capacity = static_cast<std::size_t>(capacity); evict(_decoder, _decoder.capacity, false);
                        } else { auto field = entry(false, integer(input, position, 5), _decoder); add(_decoder, std::move(field), false); }
                    } else {
                        if (first & 128) {
                            const auto id = integer(input, position, 7); auto found = _outstanding.find(id);
                            if (found == _outstanding.end() || found->second.empty()) { bad(); }
                            _known = (std::max)(_known, found->second.front().required); found->second.pop_front();
                            if (found->second.empty()) { _outstanding.erase(found); }
                        } else if (first & 64) { _outstanding.erase(integer(input, position, 6)); }
                        else {
                            const auto delta = integer(input, position, 6);
                            if (!delta || delta > _encoder.inserted-_known) { bad(); } _known += delta;
                        }
                    }
                } catch (const CodecFailure& failure) { if (failure.code == ERET_TRUNCATED) { break; } throw; }
                consumed = position;
            }
            input.erase(input.begin(), input.begin()+static_cast<std::ptrdiff_t>(consumed));
            if (encoder && _decoder.inserted != previous) { SBytes output; integer(output, _decoder.inserted-previous, 6, 0); queue(std::move(output)); }
        } catch (const CodecFailure& failure) { _poisoned = true; return error(failure.code); }
        return error(ERET_OK);
    }
public:
    explicit CQpackCodec(SQpackOptions options) : _options(std::move(options)) {
        if (!_options.codec.maximumFields || !_options.codec.maximumEncodedBytes || !_options.codec.maximumDecodedBytes
            || !_options.maximumOutstandingSections || _options.codec.maximumDynamicTableBytes > maximumInteger) {
            throw std::invalid_argument("invalid QPACK limits");
        }
    }
    ERetCode configurePeer(std::uint64_t capacity, std::uint64_t blocked) override {
        if (_poisoned) { return ERET_INVALID_STATE; }
        if (!_outstanding.empty() || _encoder.inserted || capacity > maximumInteger || blocked > maximumInteger) { return ERET_INVALID_ARGUMENT; }
        _peerCapacity = capacity; _peerBlocked = blocked; return ERET_OK;
    }
    SHeaderEncodeResult setEncoderCapacity(std::size_t capacity) override {
        if (_poisoned) { return {error(ERET_INVALID_STATE), {}}; }
        try {
            if (capacity > _peerCapacity || capacity > _options.codec.maximumDynamicTableBytes) { bad(ERET_INVALID_ARGUMENT); }
            auto table = _encoder; evict(table, capacity, true); table.capacity = capacity;
            SBytes wire; integer(wire, capacity, 5, 32); _encoder = std::move(table); return {error(ERET_OK), std::move(wire)};
        } catch (const CodecFailure& f) { return {error(f.code), {}}; }
    }
    SHeaderEncodeResult insert(SHeaderField field) override {
        if (_poisoned) { return {error(ERET_INVALID_STATE), {}}; }
        try {
            if (!validField(field) || secret(field)) { bad(ERET_INVALID_ARGUMENT); }
            if (size(field) > _options.codec.maximumDecodedBytes) { bad(ERET_LIMIT); }
            SBytes wire; literal(wire, field.name, 5, 64); literal(wire, field.value);
            if (wire.size() > _options.codec.maximumEncodedBytes) { bad(ERET_LIMIT); }
            add(_encoder, std::move(field), true); return {error(ERET_OK), std::move(wire)};
        } catch (const CodecFailure& f) { return {error(f.code), {}}; }
    }
    SHeaderEncodeResult duplicate(std::uint64_t relative) override {
        if (_poisoned) { return {error(ERET_INVALID_STATE), {}}; }
        try { auto f = entry(false, relative, _encoder); SBytes wire; integer(wire, relative, 5, 0);
            add(_encoder, std::move(f), true); return {error(ERET_OK), std::move(wire)};
        } catch (const CodecFailure& f) { return {error(f.code), {}}; }
    }
    SHeaderEncodeResult encode(std::span<const SHeaderField> fields) override { return encodeSection(0, fields, false); }
    SHeaderEncodeResult encodeSection(std::uint64_t id, std::span<const SHeaderField> fields, bool allowBlocking) override {
        if (_poisoned) { return {error(ERET_INVALID_STATE), {}}; }
        try {
            if (id > maximumInteger) { bad(ERET_INVALID_ARGUMENT); }
            std::size_t blocked = 0, outstanding = 0;
            for (const auto& [stream, sections] : _outstanding) { (void)stream; bool waiting = false;
                for (const auto& section : sections) { ++outstanding; waiting |= section.required > _known; } blocked += waiting;
            }
            bool alreadyBlocked = false;
            if (auto it = _outstanding.find(id); it != _outstanding.end()) { for (const auto& s : it->second) { alreadyBlocked |= s.required > _known; } }
            const bool mayBlock = allowBlocking && (alreadyBlocked || blocked < _peerBlocked);
            SBytes wire; SSection section{0,{}}; std::size_t count = 0, decoded = 0;
            const auto base = _encoder.inserted;
            for (const auto& field : fields) {
                if (!validField(field)) { bad(ERET_INVALID_ARGUMENT); }
                boundField(field, count, decoded); std::optional<std::size_t> exact, name;
                for (std::size_t i = 0; i < std::size(qpackStatic); ++i) { if (qpackStatic[i].name == field.name) {
                    if (!name) { name = i; } if (qpackStatic[i].value == field.value) { exact = i; break; }
                } }
                std::optional<std::uint64_t> dynamic;
                if (!secret(field) && !exact) { for (auto it = _encoder.entries.rbegin(); it != _encoder.entries.rend(); ++it) {
                    if (it->field.name == field.name && it->field.value == field.value && (mayBlock || it->index < _known)) { dynamic = it->index; break; }
                } }
                if (exact && !secret(field)) { integer(wire, *exact, 6, 192); }
                else if (dynamic) { integer(wire, base-*dynamic-1, 6, 128); section.references.insert(*dynamic); section.required = (std::max)(section.required, *dynamic+1); }
                else if (name) { integer(wire, *name, 4, secret(field) ? 0x70 : 0x50); literal(wire, field.value); }
                else { literal(wire, field.name, 3, secret(field) ? 0x30 : 0x20); literal(wire, field.value); }
                if (wire.size() > _options.codec.maximumEncodedBytes) { bad(ERET_LIMIT); }
            }
            SBytes prefix;
            integer(prefix, section.required ? (section.required % (2*(_peerCapacity/32)))+1 : 0, 8, 0);
            integer(prefix, section.required ? base-section.required : 0, 7, 0);
            if (prefix.size() > _options.codec.maximumEncodedBytes || wire.size() > _options.codec.maximumEncodedBytes-prefix.size()) { bad(ERET_LIMIT); }
            prefix.insert(prefix.end(), wire.begin(), wire.end());
            if (section.required) {
                if (outstanding >= _options.maximumOutstandingSections) { bad(ERET_LIMIT); }
                _outstanding[id].push_back(std::move(section));
            }
            return {error(ERET_OK), std::move(prefix)};
        } catch (const CodecFailure& f) { return {error(f.code), {}}; }
    }
    SHeaderDecodeResult decode(std::span<const std::uint8_t> bytes) override { return decodeSection(0, bytes); }
    SHeaderDecodeResult decodeSection(std::uint64_t id, std::span<const std::uint8_t> bytes) override {
        if (_poisoned) { return {error(ERET_INVALID_STATE), {}}; }
        try {
            if (id > maximumInteger || bytes.size() > _options.codec.maximumEncodedBytes) { bad(ERET_LIMIT); }
            std::size_t pos = 0, count = 0, decoded = 0; const auto encoded = integer(bytes, pos, 8);
            std::uint64_t required = 0;
            if (encoded) {
                const auto entries = _options.codec.maximumDynamicTableBytes/32;
                const auto range = 2*entries;
                if (!entries || encoded > range) { bad(); }
                const auto maximum = _decoder.inserted+entries;
                required = (maximum/range)*range+encoded-1;
                if (required > maximum) { if (required <= range) { bad(); } required -= range; }
                if (!required) { bad(); }
            }
            if (pos == bytes.size()) { bad(ERET_TRUNCATED); }
            const auto negative = (bytes[pos] & 128) != 0; const auto delta = integer(bytes, pos, 7);
            if ((negative && delta >= required) || (!negative && delta > maximumInteger-required)) { bad(); }
            const auto base = negative ? required-delta-1 : required+delta;
            if (required > _decoder.inserted) {
                if (!_blocked.contains(id) && _blocked.size() >= _options.maximumBlockedStreams) { bad(ERET_LIMIT); }
                _blocked[id] = required; return {error(ERET_BLOCKED), {}};
            }
            auto dynamic = [&](std::uint64_t index, bool post) {
                if ((!post && index >= base) || (post && index > maximumInteger-base)) { bad(); }
                const auto absolute = post ? base+index : base-index-1;
                if (absolute >= required) { bad(); } return _decoder.at(absolute);
            };
            std::vector<SHeaderField> fields;
            while (pos < bytes.size()) {
                const auto first = bytes[pos]; SHeaderField field;
                if (first & 128) { const auto index = integer(bytes, pos, 6);
                    if (first & 64) { if (index >= std::size(qpackStatic)) { bad(); } field = qpackStatic[index]; }
                    else { field = dynamic(index, false); }
                } else if (first & 64) {
                    const auto index = integer(bytes, pos, 4);
                    if (first & 16) { if (index >= std::size(qpackStatic)) { bad(); } field.name = qpackStatic[index].name; }
                    else { field.name = dynamic(index, false).name; }
                    field.value = literal(bytes, pos, 7, 128, _options.codec.maximumDecodedBytes); field.indexing = (first & 32) ? EINDEX_NEVER : EINDEX_DEFAULT;
                } else if (first & 32) {
                    field.name = literal(bytes, pos, 3, 8, _options.codec.maximumDecodedBytes);
                    field.value = literal(bytes, pos, 7, 128, _options.codec.maximumDecodedBytes); field.indexing = (first & 16) ? EINDEX_NEVER : EINDEX_DEFAULT;
                } else if (first & 16) { field = dynamic(integer(bytes, pos, 4), true); }
                else { field.name = dynamic(integer(bytes, pos, 3), true).name;
                    field.value = literal(bytes, pos, 7, 128, _options.codec.maximumDecodedBytes); field.indexing = (first & 8) ? EINDEX_NEVER : EINDEX_DEFAULT;
                }
                boundField(field, count, decoded); fields.push_back(std::move(field));
            }
            if (required) { SBytes ack; integer(ack, id, 7, 128); queue(std::move(ack)); }
            _blocked.erase(id); return {error(ERET_OK), std::move(fields)};
        } catch (const CodecFailure& f) { _poisoned = true; return {error(f.code), {}}; }
    }
    SHttpError receiveEncoderInstructions(std::span<const std::uint8_t> bytes) override { return feed(bytes, true); }
    SHttpError receiveDecoderInstructions(std::span<const std::uint8_t> bytes) override { return feed(bytes, false); }
    SHeaderEncodeResult cancelStream(std::uint64_t id) override {
        if (_poisoned || id > maximumInteger) { return {error(ERET_INVALID_ARGUMENT), {}}; }
        _blocked.erase(id); SBytes wire; integer(wire, id, 6, 64); return {error(ERET_OK), std::move(wire)};
    }
    SBytes takeDecoderInstructions() override { auto result = std::move(_decoderOutput); _decoderOutput.clear(); return result; }
    std::uint64_t receivedInsertCount() const noexcept override { return _decoder.inserted; }
};
}
}
namespace hnhttp {
std::shared_ptr<IQpackCodec> IQpackCodec::create(SQpackOptions options) { return std::make_shared<detail::CQpackCodec>(std::move(options)); }
std::shared_ptr<IHeaderCodec> IHeaderCodec::createQpack(SHeaderCodecOptions options) { SQpackOptions o; o.codec = options; return IQpackCodec::create(o); }
}
