#include "archive_internal.hpp"
#include "archive_crypto.hpp"

namespace hncomp {
namespace detail {
namespace {

constexpr std::array<std::uint8_t, 6> XZ_MAGIC{0xfd, '7', 'z', 'X', 'Z', 0};

unsigned filterAlignment(EXzFilter filter) {
    switch (filter) {
        case EXZ_DELTA: return 1;
        case EXZ_ARMTHUMB: return 2;
        case EXZ_POWERPC: case EXZ_ARM: case EXZ_SPARC: case EXZ_ARM64: return 4;
        default: unsupported("unsupported XZ prefilter (supported: Delta, PowerPC, ARM, Thumb, SPARC, ARM64)");
    }
}

/** A size-preserving filter keeps at most three tail bytes between LZMA2 chunks. */
class Prefilter {
private:
    SXzFilterOptions _options;
    bool _encode;
    std::uint32_t _position;
    std::array<std::uint8_t,256> _history{};
    unsigned _historyPos=0;
    SBytes _pending;
    std::uint32_t adjust(std::uint32_t value,std::uint32_t pc) const { return _encode?value+pc:value-pc; }
public:
    Prefilter(SXzFilterOptions options,bool encode) : _options(options),_encode(encode),_position(options.startOffset) {}
    SBytes process(SReadOnlyByteSpan bytes,bool final) {
        SBytes output=std::move(_pending); output.insert(output.end(),bytes.begin(),bytes.end()); std::size_t at=0;
        if (_options.filter==EXZ_DELTA) {
            for (auto& byte:output) {
                const auto prior=_history[(_historyPos+256-_options.deltaDistance)&255];
                const auto original=_encode?byte:static_cast<std::uint8_t>(byte+prior);
                if (_encode) { byte=static_cast<std::uint8_t>(byte-prior); } else { byte=original; }
                _history[_historyPos]=original; _historyPos=(_historyPos+1)&255;
            }
            return output;
        }
        const auto be=_options.filter==EXZ_POWERPC || _options.filter==EXZ_SPARC;
        auto read=[&](std::size_t pos) { std::uint32_t word=0; for (unsigned i=0;i<4;++i) { word|=std::uint32_t(output[pos+i])<<(8*(be?3-i:i)); } return word; };
        auto write=[&](std::size_t pos,std::uint32_t word) { for (unsigned i=0;i<4;++i) { output[pos+i]=static_cast<std::uint8_t>(word>>(8*(be?3-i:i))); } };
        while (at+4<=output.size()) {
            const auto pc=_position+static_cast<std::uint32_t>(at);
            auto word=read(at);
            switch (_options.filter) {
                case EXZ_POWERPC:
                    if ((word&0xfc000003)==0x48000001) { write(at,(word&0xfc000003)|(adjust(word&0x03fffffc,pc)&0x03fffffc)); }
                    break;
                case EXZ_ARM:
                    if ((word>>24)==0xeb) { write(at,0xeb000000|((adjust(word<<2,pc+8)>>2)&0x00ffffff)); }
                    break;
                case EXZ_ARMTHUMB:
                    if ((output[at+1]&0xf8)==0xf0 && (output[at+3]&0xf8)==0xf8) {
                        const auto value=((std::uint32_t(output[at+1]&7)<<19)|(std::uint32_t(output[at])<<11)|(std::uint32_t(output[at+3]&7)<<8)|output[at+2])<<1;
                        const auto dest=adjust(value,pc+4)>>1;
                        output[at]=static_cast<std::uint8_t>(dest>>11); output[at+1]=static_cast<std::uint8_t>(0xf0|((dest>>19)&7));
                        output[at+2]=static_cast<std::uint8_t>(dest); output[at+3]=static_cast<std::uint8_t>(0xf8|((dest>>8)&7)); at+=4; continue;
                    }
                    at+=2; continue;
                case EXZ_SPARC:
                    if ((word>>22)==0x100 || (word>>22)==0x1ff) {
                        const auto dest=adjust(word<<2,pc)>>2;
                        write(at,0x40000000|(dest&0x003fffff)|((0u-((dest>>22)&1))&0x3fc00000));
                    }
                    break;
                case EXZ_ARM64:
                    if ((word>>26)==0x25) { write(at,0x94000000|(adjust(word,pc>>2)&0x03ffffff)); }
                    else if ((word&0x9f000000)==0x90000000) {
                        const auto value=((word>>29)&3)|((word>>3)&0x001ffffc);
                        if (((value+0x20000)&0x1c0000)==0) {
                            const auto dest=adjust(value,pc>>12);
                            write(at,(word&0x9000001f)|((dest&3)<<29)|((dest&0x3fffc)<<3)|((0u-(dest&0x20000))&0xe00000));
                        }
                    }
                    break;
                default: unsupported("unsupported XZ prefilter");
            }
            at+=4;
        }
        _position+=static_cast<std::uint32_t>(at);
        if (!final) { _pending.assign(output.begin()+at,output.end()); output.resize(at); }
        return output;
    }
};

class FilterChain {
private:
    std::vector<Prefilter> _filters;
public:
    FilterChain(const std::vector<SXzFilterOptions>& options,bool encode) {
        if (encode) { for (const auto& option:options) { _filters.emplace_back(option,true); } }
        else { for (auto it=options.rbegin();it!=options.rend();++it) { _filters.emplace_back(*it,false); } }
    }
    SBytes process(SReadOnlyByteSpan bytes,bool final) {
        SBytes output(bytes.begin(),bytes.end());
        for (auto& filter:_filters) { output=filter.process(output,final); } return output;
    }
};

void vli(SBytes& bytes, std::uint64_t value) {
    if (value > INT64_MAX) { limit("XZ integer exceeds 63 bits"); }
    while (value >= 128) { bytes.push_back(static_cast<std::uint8_t>(value) | 128); value >>= 7; }
    bytes.push_back(static_cast<std::uint8_t>(value));
}

class Cursor {
private:
    const ArchiveStore& _source;
    std::array<std::uint8_t, ARCHIVE_CHUNK> _buffer{};
    std::uint64_t _pos = 0, _base = 0;
    std::size_t _count = 0;
public:
    explicit Cursor(const ArchiveStore& source) : _source(source) {}
    std::uint64_t pos() const noexcept { return _pos; }
    std::uint8_t byte() {
        if (_pos >= _source.size()) { throw CCompressionError(EERROR_TRUNCATED, "truncated XZ container"); }
        if (_pos < _base || _pos - _base >= _count) {
            _base = _pos; _count = static_cast<std::size_t>(std::min<std::uint64_t>(_buffer.size(), _source.size() - _pos));
            _source.read(_pos, SByteSpan(_buffer).first(_count));
        }
        return _buffer[static_cast<std::size_t>(_pos++ - _base)];
    }
    SBytes bytes(std::size_t count) {
        if (_pos > _source.size() || count > _source.size() - _pos) { throw CCompressionError(EERROR_TRUNCATED, "truncated XZ data"); }
        auto data = _source.bytes(_pos, count); _pos += count; return data;
    }
    std::uint64_t little(unsigned count) { std::uint64_t value = 0; for (unsigned i = 0; i < count; ++i) { value |= std::uint64_t(byte()) << (8 * i); } return value; }
    std::uint64_t integer() {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 9; ++i) {
            const auto next = byte(); value |= std::uint64_t(next & 127) << (7 * i);
            if (!(next & 128)) { if (i && next == 0) { invalid("noncanonical XZ integer"); } return value; }
        }
        invalid("oversized XZ integer");
    }
};

std::uint64_t headerVli(Reader& reader) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 9; ++i) {
        const auto byte = reader.byte(); value |= std::uint64_t(byte & 127) << (7 * i);
        if (!(byte & 128)) { if (i && byte == 0) { invalid("noncanonical XZ header integer"); } return value; }
    }
    invalid("oversized XZ header integer");
}

std::uint64_t crc64(std::uint64_t crc, SReadOnlyByteSpan input) {
    static const auto table = [] {
        std::array<std::uint64_t, 256> values{};
        for (unsigned i = 0; i < 256; ++i) {
            auto value = std::uint64_t(i);
            for (unsigned b = 0; b < 8; ++b) { value = (value >> 1) ^ (UINT64_C(0xc96c5795d7870f42) & (0 - (value & 1))); }
            values[i] = value;
        }
        return values;
    }();
    for (auto value : input) { crc = (crc >> 8) ^ table[(crc ^ value) & 255]; } return crc;
}

} // namespace

void archiveValidateXz(const SXzOptions& options) {
    if (options.filters.size()>3) { throw std::invalid_argument("XZ supports at most three prefilters"); }
    if (options.check!=EXZ_CHECK_NONE && options.check!=EXZ_CHECK_CRC32 && options.check!=EXZ_CHECK_CRC64 && options.check!=EXZ_CHECK_SHA256) { throw std::invalid_argument("unsupported XZ integrity check"); }
    for (const auto& filter:options.filters) {
        const auto alignment=filterAlignment(filter.filter);
        if (filter.startOffset%alignment || (filter.filter==EXZ_DELTA && (filter.deltaDistance<1 || filter.deltaDistance>256 || filter.startOffset))) { throw std::invalid_argument("invalid XZ filter alignment/distance"); }
    }
}

/* Independent compressed LZMA2 chunks, standard XZ blocks and a checked index/footer. */
void archiveWriteXz(const ArchiveStore& source, const SArchiveSink& sink, unsigned level,const SXzOptions& options) {
    archiveValidateXz(options);
    SBytes header(XZ_MAGIC.begin(), XZ_MAGIC.end()); header.push_back(0); header.push_back(static_cast<std::uint8_t>(options.check));
    putLe(header, crc32(SReadOnlyByteSpan(header).subspan(6)), 4); sink(header);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> records;
    for (std::uint64_t offset = 0; offset < source.size();) {
        if (records.size() == ARCHIVE_ENTRIES) { limit("too many XZ blocks"); }
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(1024 * 1024, source.size() - offset));
        const auto original=source.bytes(offset,amount); FilterChain filters(options.filters,true);
        auto input = filters.process(original,true); SBytes compressed;
        for (std::size_t pos = 0; pos < input.size();) {
            const auto chunk = SReadOnlyByteSpan(input).subspan(pos, std::min<std::size_t>(65536, input.size() - pos));
            auto payload = encodeLzma2Chunk(chunk, level);
            if (payload.size() < chunk.size() && payload.size() <= 65536) {
                compressed.push_back(static_cast<std::uint8_t>(0xe0 | ((chunk.size() - 1) >> 16)));
                putBe(compressed, chunk.size() - 1, 2); putBe(compressed, payload.size() - 1, 2); compressed.push_back(93);
                compressed.insert(compressed.end(), payload.begin(), payload.end());
            } else { compressed.push_back(1); putBe(compressed, chunk.size() - 1, 2); compressed.insert(compressed.end(), chunk.begin(), chunk.end()); }
            pos += chunk.size();
        }
        compressed.push_back(0);
        SBytes block{0,static_cast<std::uint8_t>(options.filters.size())};
        for (const auto& filter:options.filters) {
            vli(block,filter.filter);
            if (filter.filter==EXZ_DELTA) { vli(block,1); block.push_back(static_cast<std::uint8_t>(filter.deltaDistance-1)); }
            else { vli(block,filter.startOffset?4:0); if (filter.startOffset) { putLe(block,filter.startOffset,4); } }
        }
        vli(block,0x21); vli(block,1); block.push_back(16);
        while (block.size()%4) { block.push_back(0); } block[0]=static_cast<std::uint8_t>(block.size()/4); putLe(block, crc32(block), 4); sink(block); sink(compressed);
        const std::array<std::uint8_t, 3> zeros{};
        sink(SReadOnlyByteSpan(zeros).first((4 - compressed.size() % 4) % 4));
        SBytes check;
        if (options.check==EXZ_CHECK_CRC32) { putLe(check,crc32(original),4); }
        else if (options.check==EXZ_CHECK_CRC64) { putLe(check,~crc64(UINT64_MAX,original),8); }
        else if (options.check==EXZ_CHECK_SHA256) { ArchiveHash hash(true); hash.update(original); check=hash.finish(); }
        sink(check);
        records.emplace_back(block.size() + compressed.size() + check.size(), amount); offset += amount;
    }
    SBytes index{0}; vli(index, records.size());
    for (const auto& [packed, raw] : records) { vli(index, packed); vli(index, raw); }
    while (index.size() % 4) { index.push_back(0); } putLe(index, crc32(index), 4); sink(index);
    SBytes footer; putLe(footer, index.size() / 4 - 1, 4); footer.push_back(0); footer.push_back(static_cast<std::uint8_t>(options.check));
    SBytes final; putLe(final, crc32(footer), 4); final.insert(final.end(), footer.begin(), footer.end()); final.insert(final.end(), {'Y','Z'}); sink(final);
}

/* Decode bounded LZMA2 chunks, including retained dictionaries/probabilities and CRC64. */
std::uint64_t archiveReadXz(const ArchiveStore& source, const SArchiveSink& sink) {
    Cursor input(source); std::uint64_t total = 0;
    try {
        do {
            const auto header = input.bytes(12);
            if (!std::equal(XZ_MAGIC.begin(), XZ_MAGIC.end(), header.begin()) || header[6] || (header[7] & 0xf0)) { invalid("invalid XZ stream header"); }
            Reader checksum(SReadOnlyByteSpan(header).subspan(8));
            if (checksum.le(4) != crc32(SReadOnlyByteSpan(header).subspan(6, 2))) { invalid("XZ stream header CRC mismatch"); }
            const auto check = header[7];
            if (check != 0 && check != 1 && check != 4 && check!=10) { unsupported("unsupported XZ check type"); }
            const unsigned checkBytes = check == 1 ? 4 : check == 4 ? 8 : check==10?32:0;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> records;
            std::uint64_t indexStart;
            for (;;) {
                const auto blockStart = input.pos(); const auto encodedSize = input.byte();
                if (!encodedSize) { indexStart = blockStart; break; }
                if (records.size() == ARCHIVE_ENTRIES) { limit("too many XZ blocks"); }
                const auto headerSize = (unsigned(encodedSize) + 1) * 4;
                SBytes block{encodedSize}; auto rest = input.bytes(headerSize - 1); block.insert(block.end(), rest.begin(), rest.end());
                Reader crc(SReadOnlyByteSpan(block).last(4));
                if (crc.le(4) != crc32(SReadOnlyByteSpan(block).first(headerSize - 4))) { invalid("XZ block header CRC mismatch"); }
                Reader fields(SReadOnlyByteSpan(block).subspan(1, headerSize - 5)); const auto flags = fields.byte();
                if (flags & 0x3c) { invalid("reserved XZ block flags"); }
                const auto declaredPacked = flags & 64 ? headerVli(fields) : UINT64_MAX;
                const auto declaredRaw = flags & 128 ? headerVli(fields) : UINT64_MAX;
                SXzOptions options;
                for (unsigned i=0;i<(flags&3);++i) {
                    const auto id=headerVli(fields),length=headerVli(fields); SXzFilterOptions filter; filter.filter=static_cast<EXzFilter>(id);
                    if (id!=3 && id!=5 && id!=7 && id!=8 && id!=9 && id!=10) { unsupported("unsupported XZ prefilter identifier"); }
                    if (id==3) { if (length!=1) { invalid("invalid XZ delta properties"); } filter.deltaDistance=unsigned(fields.byte())+1; }
                    else { if (length!=0 && length!=4) { invalid("invalid XZ BCJ properties"); } if (length) { filter.startOffset=static_cast<std::uint32_t>(fields.le(4)); } }
                    options.filters.push_back(filter);
                }
                try { archiveValidateXz(options); } catch (const std::invalid_argument&) { invalid("invalid XZ filter properties"); }
                FilterChain filters(options.filters,false);
                if (headerVli(fields) != 0x21 || headerVli(fields) != 1) { unsupported("XZ requires the LZMA2 filter"); }
                Lzma2Decoder decoder(fields.byte());
                while (fields.remaining()) { if (fields.byte()) { invalid("nonzero XZ header padding"); } }
                const auto payloadStart = input.pos(); std::uint64_t raw = 0, crc64value = UINT64_MAX; std::uint32_t crc32value = 0xffffffffu;
                ArchiveHash hash(true);
                auto output=[&](SReadOnlyByteSpan bytes) { archiveCrcUpdate(crc32value,bytes); crc64value=crc64(crc64value,bytes); if (check==10) { hash.update(bytes); } if (!bytes.empty()) { sink(bytes); } };
                for (;;) {
                    const auto control = input.byte(); if (!control) { break; }
                    if (control > 2 && control < 0x80) { invalid("reserved LZMA2 control byte"); }
                    const auto high = input.byte(); const auto low = input.byte();
                    const auto rawSize = ((control >= 0x80 ? (control & 31u) << 16 : 0) | (unsigned(high) << 8) | low) + 1;
                    unsigned packedSize = rawSize, properties = 0;
                    if (control >= 0x80) { const auto hi = input.byte(); const auto lo = input.byte(); packedSize = (unsigned(hi) << 8 | lo) + 1; }
                    if (control >= 0xc0) { properties = input.byte(); }
                    const auto decoded = decoder.chunk(control, input.bytes(packedSize), rawSize, properties);
                    if (rawSize > INT64_MAX - raw || rawSize > INT64_MAX - total) { limit("XZ uncompressed size overflow"); }
                    raw += rawSize; total += rawSize;
                    if (declaredRaw != UINT64_MAX && raw > declaredRaw) { invalid("XZ block exceeds declared output"); }
                    output(filters.process(decoded,false));
                }
                output(filters.process({},true));
                const auto packed = input.pos() - payloadStart;
                if ((declaredPacked != UINT64_MAX && declaredPacked != packed) || (declaredRaw != UINT64_MAX && declaredRaw != raw)) { invalid("XZ block declared size mismatch"); }
                while (input.pos() % 4) { if (input.byte()) { invalid("nonzero XZ block padding"); } }
                if ((check == 1 && input.little(4) != ~crc32value) || (check == 4 && input.little(8) != ~crc64value)) { invalid("XZ block checksum mismatch"); }
                if (check==10 && !equalSecret(input.bytes(32),hash.finish())) { invalid("XZ SHA-256 mismatch"); }
                records.emplace_back(headerSize + packed + checkBytes, raw);
            }
            if (input.integer() != records.size()) { invalid("XZ index record count mismatch"); }
            for (const auto& [packed, raw] : records) {
                const auto p = input.integer(); const auto r = input.integer();
                if (p != packed || r != raw) { invalid("XZ index record mismatch"); }
            }
            while (input.pos() % 4) { if (input.byte()) { invalid("nonzero XZ index padding"); } }
            const auto indexDataSize = input.pos() - indexStart;
            if (input.little(4) != archiveCrc(source, indexStart, indexDataSize)) { invalid("XZ index CRC mismatch"); }
            const auto footer = input.bytes(12); Reader footerReader(footer);
            const auto footerCrc = footerReader.le(4), backwards = footerReader.le(4);
            if (footerCrc != crc32(SReadOnlyByteSpan(footer).subspan(4,6)) || backwards != indexDataSize / 4 ||
                footer[8] || footer[9] != check || footer[10] != 'Y' || footer[11] != 'Z') { invalid("XZ footer mismatch"); }
            while (input.pos() < source.size()) {
                const auto one = source.bytes(input.pos(), 1); if (one[0]) { break; }
                for (unsigned i = 0; i < 4; ++i) { if (input.byte()) { invalid("invalid XZ stream padding"); } }
            }
        } while (input.pos() < source.size());
    } catch (const NeedInput&) { throw CCompressionError(EERROR_TRUNCATED, "truncated XZ header or LZMA2 chunk"); }
    return total;
}

/* Use the native incremental DEFLATE adapter for bounded gzip input/output. */
void archiveGzip(const ArchiveStore& source, const SArchiveSink& sink, bool encode, unsigned level) {
    SCompressionOptions options; options.format = EFORMAT_GZIP; options.level = level;
    options.maxInputBytes = options.maxOutputBytes = std::numeric_limits<std::size_t>::max();
    auto encoder = encode ? IStreamCompressor::create(options) : nullptr;
    auto decoder = !encode ? IStreamDecompressor::create(options) : nullptr;
    std::array<std::uint8_t, ARCHIVE_CHUNK> buffer{}; std::uint64_t offset = 0;
    SBytes input; std::size_t pos = 0;
    for (;;) {
        if (pos == input.size() && offset < source.size()) {
            input = source.bytes(offset, static_cast<std::size_t>(std::min<std::uint64_t>(ARCHIVE_CHUNK, source.size() - offset))); offset += input.size(); pos = 0;
        }
        const auto result = encode ? encoder->process(SReadOnlyByteSpan(input).subspan(pos), buffer, offset == source.size() ? EFLUSH_FINISH : EFLUSH_NONE) :
            decoder->process(SReadOnlyByteSpan(input).subspan(pos), buffer, offset == source.size() ? EINPUT_FINAL : EINPUT_MORE);
        pos += result.consumed; sink(SReadOnlyByteSpan(buffer).first(result.produced));
        if (result.status == ESTREAM_FINISHED) {
            if (offset != source.size() || pos != input.size()) { invalid("trailing bytes after gzip archive"); } break;
        }
        if (!result.consumed && !result.produced && pos != input.size()) { invalid("gzip archive made no progress"); }
    }
}

} // namespace detail
} // namespace hncomp
