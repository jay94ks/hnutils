#include <hnhttp/header_codec.hpp>
#include "codec_internal.hpp"

namespace hnhttp::detail {
static const SHeaderField hpackStatic[] = {
    {":authority",""},{":method","GET"},{":method","POST"},{":path","/"},{":path","/index.html"},
    {":scheme","http"},{":scheme","https"},{":status","200"},{":status","204"},{":status","206"},
    {":status","304"},{":status","400"},{":status","404"},{":status","500"},{"accept-charset",""},
    {"accept-encoding","gzip, deflate"},{"accept-language",""},{"accept-ranges",""},{"accept",""},
    {"access-control-allow-origin",""},{"age",""},{"allow",""},{"authorization",""},{"cache-control",""},
    {"content-disposition",""},{"content-encoding",""},{"content-language",""},{"content-length",""},
    {"content-location",""},{"content-range",""},{"content-type",""},{"cookie",""},{"date",""},
    {"etag",""},{"expect",""},{"expires",""},{"from",""},{"host",""},{"if-match",""},
    {"if-modified-since",""},{"if-none-match",""},{"if-range",""},{"if-unmodified-since",""},
    {"last-modified",""},{"link",""},{"location",""},{"max-forwards",""},{"proxy-authenticate",""},
    {"proxy-authorization",""},{"range",""},{"referer",""},{"refresh",""},{"retry-after",""},
    {"server",""},{"set-cookie",""},{"strict-transport-security",""},{"transfer-encoding",""},
    {"user-agent",""},{"vary",""},{"via",""},{"www-authenticate",""}
};

class CHpackCodec final : public IHeaderCodec {
private:
    SHeaderCodecOptions _options;
    bool _poisoned = false;
    std::vector<SHeaderField> _dynamic;
    std::size_t _capacity;
    static std::size_t tableSize(const std::vector<SHeaderField>& table) {
        std::size_t size = 0; for (const auto& f : table) { size += f.name.size()+f.value.size()+32; } return size;
    }
    static void evict(std::vector<SHeaderField>& table, std::size_t maximum) {
        auto size = tableSize(table);
        while (size > maximum) { size -= table.back().name.size()+table.back().value.size()+32; table.pop_back(); }
    }
    static SHeaderField lookup(std::uint64_t index, const std::vector<SHeaderField>& table) {
        if (!index) { bad(); } if (index <= std::size(hpackStatic)) { return hpackStatic[index-1]; }
        index -= std::size(hpackStatic)+1; if (index >= table.size()) { bad(); } return table[index];
    }
public:
    explicit CHpackCodec(SHeaderCodecOptions options) : _options(options), _capacity(options.maximumDynamicTableBytes) {
        if (!options.maximumEncodedBytes || !options.maximumDecodedBytes || !options.maximumFields) { throw std::invalid_argument("invalid codec bounds"); }
    }
    SHeaderEncodeResult encode(std::span<const SHeaderField> fields) override {
        SBytes output; std::size_t decoded = 0;
        try {
            if (fields.size() > _options.maximumFields) { bad(ERET_LIMIT); }
            for (const auto& f : fields) {
                if (!validField(f)) { bad(ERET_INVALID_ARGUMENT); }
                if (f.name.size()+f.value.size()+32 > _options.maximumDecodedBytes-decoded) { bad(ERET_LIMIT); }
                decoded += f.name.size()+f.value.size()+32;
                std::size_t name = 0, exact = 0;
                for (std::size_t i = 0; i < std::size(hpackStatic); ++i) {
                    if (hpackStatic[i].name == f.name) { if (!name) { name = i+1; } if (hpackStatic[i].value == f.value) { exact = i+1; break; } }
                }
                if (exact && !secret(f)) { integer(output, exact, 7, 128); }
                else { integer(output, name, 4, secret(f) ? 16 : 0); if (!name) { literal(output, f.name); } literal(output, f.value); }
                if (output.size() > _options.maximumEncodedBytes) { bad(ERET_LIMIT); }
            }
        } catch (const CodecFailure& f) { return {error(f.code), {}}; }
        return {error(ERET_OK), std::move(output)};
    }
    SHeaderDecodeResult decode(std::span<const std::uint8_t> bytes) override {
        if (_poisoned) { return {error(ERET_INVALID_STATE), {}}; }
        std::size_t pos = 0, decoded = 0, capacity = _capacity; auto table = _dynamic;
        std::vector<SHeaderField> fields;
        try {
            if (bytes.size() > _options.maximumEncodedBytes) { bad(ERET_LIMIT); }
            bool anyField = false;
            while (pos < bytes.size()) {
                const auto first = bytes[pos]; SHeaderField value;
                if (first & 128) { value = lookup(integer(bytes, pos, 7), table); }
                else if ((first & 0xe0) == 0x20) {
                    if (anyField) { bad(); } const auto requested = integer(bytes, pos, 5);
                    if (requested > _options.maximumDynamicTableBytes) { bad(); }
                    capacity = static_cast<std::size_t>(requested); evict(table, capacity); continue;
                } else {
                    const bool insert = (first & 64) != 0;
                    const auto index = integer(bytes, pos, insert ? 6 : 4);
                    value.name = index ? lookup(index, table).name : literal(bytes, pos, 7, 128, _options.maximumDecodedBytes);
                    value.value = literal(bytes, pos, 7, 128, _options.maximumDecodedBytes);
                    value.indexing = (!insert && (first & 16)) ? EINDEX_NEVER : EINDEX_DEFAULT;
                    if (insert) {
                        if (value.name.size()+value.value.size()+32 > capacity) { table.clear(); }
                        else { table.insert(table.begin(), value); evict(table, capacity); }
                    }
                }
                anyField = true;
                // --> Compression syntax is independent from HTTP field semantics/scope.
                if (fields.size() >= _options.maximumFields || value.name.size()+value.value.size()+32 > _options.maximumDecodedBytes-decoded) { bad(ERET_LIMIT); }
                decoded += value.name.size()+value.value.size()+32; fields.push_back(std::move(value));
            }
        } catch (const CodecFailure& f) { _poisoned = true; return {error(f.code), {}}; }
        _dynamic = std::move(table); _capacity = capacity; return {error(ERET_OK), std::move(fields)};
    }
};
}
namespace hnhttp {
std::shared_ptr<IHeaderCodec> IHeaderCodec::createHpack(SHeaderCodecOptions options) { return std::make_shared<detail::CHpackCodec>(options); }
}
