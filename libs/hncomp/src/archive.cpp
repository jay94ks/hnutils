#include <hncomp/archive.hpp>

#include "archive_internal.hpp"
#include "archive_crypto.hpp"

#include <charconv>
#include <coroutine>
#include <map>
#include <unordered_set>

namespace hncomp {
namespace {

using namespace detail;

struct Entry {
    SArchiveEntry info;
    std::uint64_t offset = 0, packed = 0, local = 0;
    unsigned method = 0;
    std::uint32_t crc = 0;
    EZipEncryption encryption = EZIP_NONE;
    unsigned aesVersion = 0;
};

class SpanStore final : public ArchiveStore {
private:
    SReadOnlyByteSpan _input;
public:
    explicit SpanStore(SReadOnlyByteSpan input) : _input(input) {}
    std::uint64_t size() const noexcept override { return _input.size(); }
    void read(std::uint64_t offset, SByteSpan output) const override {
        if (offset > _input.size() || output.size() > _input.size() - offset) { invalid("span archive range exceeds input"); }
        std::copy_n(_input.begin() + static_cast<std::size_t>(offset), output.size(), output.begin());
    }
    void append(SReadOnlyByteSpan) override { throw std::logic_error("read-only source span"); }
    void flush() override {}
};

[[noreturn]] void stateError(const char* message) { throw CCompressionError(EERROR_STATE, message); }

std::string safeName(std::string name, bool directory) {
    if (name.size() > 4096) { limit("archive name exceeds 4096 bytes"); }
    while (name.starts_with("./")) { name.erase(0, 2); }
    if (directory && !name.empty() && name.back() == '/') { name.pop_back(); }
    if (name.empty() || name.front() == '/' || name.find_first_of("\\:") != std::string::npos || name.find('\0') != std::string::npos) { invalid("unsafe archive entry name"); }
    for (std::size_t start = 0; start < name.size();) {
        auto end = name.find('/', start); if (end == std::string::npos) { end = name.size(); }
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") { invalid("unsafe archive path component"); } start = end + 1;
    }
    if (directory) { name.push_back('/'); }
    else if (name.back() == '/') { invalid("file name ends with slash"); }
    return name;
}

void insertEntry(std::vector<Entry>& entries, std::unordered_set<std::string>& names, std::size_t& nameBytes, Entry entry) {
    if (entries.size() == ARCHIVE_ENTRIES) { limit("archive metadata exceeds one million entries"); }
    if (entry.info.name.size() > 64 * 1024 * 1024 - nameBytes) { limit("archive name metadata exceeds 64 MiB"); }
    nameBytes += entry.info.name.size();
    auto key = entry.info.name; if (entry.info.directory) { key.pop_back(); }
    if (!names.insert(key).second) { invalid("duplicate archive entry name"); }
    entries.push_back(std::move(entry));
}

std::uint64_t little(SReadOnlyByteSpan bytes, std::size_t offset, unsigned count) {
    if (offset > bytes.size() || count > bytes.size() - offset) { throw NeedInput{}; }
    Reader reader(bytes.subspan(offset, count)); return reader.le(count);
}

void aesExtra(Entry& entry, SReadOnlyByteSpan bytes) {
    if (bytes.size()!=7) { invalid("invalid ZIP AES extra length"); }
    const auto version=little(bytes,0,2),method=little(bytes,5,2); const auto strength=bytes[4];
    if ((version!=1 && version!=2) || bytes[2]!='A' || bytes[3]!='E' || strength<1 || strength>3 || (method!=0 && method!=8)) { unsupported("unsupported WinZip AES parameters"); }
    entry.aesVersion=static_cast<unsigned>(version); entry.method=static_cast<unsigned>(method);
    entry.encryption=strength==1?EZIP_AES128:strength==2?EZIP_AES192:EZIP_AES256;
    entry.info.encrypted=true;
}

SBytes aesExtra(const Entry& entry) {
    SBytes extra; if (entry.encryption==EZIP_NONE) { return extra; }
    putLe(extra,0x9901,2); putLe(extra,7,2); putLe(extra,entry.aesVersion,2); extra.insert(extra.end(),{'A','E'});
    extra.push_back(entry.encryption==EZIP_AES128?1:entry.encryption==EZIP_AES192?2:3); putLe(extra,entry.method,2); return extra;
}

void readZip(const ArchiveStore& source, std::vector<Entry>& entries) {
    if (source.size() < 22) { invalid("missing ZIP end record"); }
    const auto tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(65557, source.size()));
    const auto tail = source.bytes(source.size() - tailSize, tailSize);
    std::size_t end = tail.size() - 22;
    for (;;) {
        if (little(tail, end, 4) == 0x06054b50 && end + 22 + little(tail, end + 20, 2) == tail.size()) { break; }
        if (!end) { invalid("missing ZIP central directory terminator"); } --end;
    }
    const auto endOffset = source.size() - tailSize + end;
    if (little(tail, end + 4, 2) || little(tail, end + 6, 2)) { unsupported("split ZIP archives are unsupported"); }
    auto count = little(tail, end + 10, 2), centralSize = little(tail, end + 12, 4), central = little(tail, end + 16, 4);
    if (little(tail, end + 8, 2) != count) { invalid("inconsistent ZIP entry counts"); }
    auto centralLimit = endOffset;
    if (count == 65535 || centralSize == UINT32_MAX || central == UINT32_MAX) {
        if (endOffset < 20) { invalid("missing ZIP64 locator"); }
        const auto locator = source.bytes(endOffset - 20, 20);
        if (little(locator, 0, 4) != 0x07064b50 || little(locator, 4, 4) || little(locator, 16, 4) != 1) { invalid("invalid ZIP64 locator"); }
        const auto at = little(locator, 8, 8); const auto record = source.bytes(at, 56);
        const auto recordSize = little(record, 4, 8);
        if (at > endOffset - 20 || recordSize < 44 || recordSize > endOffset - 20 - at || recordSize + 12 != endOffset - 20 - at ||
            little(record, 0, 4) != 0x06064b50 || little(record, 16, 4) || little(record, 20, 4)) { invalid("invalid ZIP64 end record"); }
        count = little(record, 32, 8); if (little(record, 24, 8) != count) { invalid("ZIP64 entry count mismatch"); }
        centralSize = little(record, 40, 8); central = little(record, 48, 8); centralLimit = at;
    }
    if (count > ARCHIVE_ENTRIES) { limit("too many ZIP entries"); }
    if (central > centralLimit || centralSize > centralLimit - central || central + centralSize != centralLimit) { invalid("invalid ZIP central range"); }
    std::uint64_t pos = central; std::unordered_set<std::string> names; std::size_t nameBytes = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (pos > centralLimit || centralLimit - pos < 46) { invalid("truncated ZIP central header"); }
        const auto header = source.bytes(pos, 46);
        if (little(header, 0, 4) != 0x02014b50) { invalid("invalid ZIP central signature"); }
        const auto flags = little(header, 8, 2), method = little(header, 10, 2);
        if (flags & ~UINT64_C(0x080f)) { unsupported("unsupported ZIP flags"); }
        if (method != 0 && method != 8 && method != 99) { unsupported("ZIP compression method is not STORE/DEFLATE/AES"); }
        if (bool(flags&1)!=(method==99)) { unsupported("only WinZip AES encryption is supported"); }
        const auto nameSize = little(header, 28, 2), extraSize = little(header, 30, 2), commentSize = little(header, 32, 2);
        const auto variableSize = nameSize + extraSize + commentSize;
        if (variableSize > centralLimit - pos - 46) { invalid("ZIP variable fields exceed central directory"); }
        const auto variable = source.bytes(pos + 46, static_cast<std::size_t>(variableSize));
        std::string original(variable.begin(), variable.begin() + static_cast<std::size_t>(nameSize));
        if (!(flags & 0x800) && std::any_of(original.begin(), original.end(), [](unsigned char c) { return c >= 128; })) { unsupported("non-UTF8 ZIP names require an explicit legacy codepage"); }
        Entry entry; entry.packed = little(header, 20, 4); entry.info.size = little(header, 24, 4); entry.local = little(header, 42, 4);
        entry.crc = static_cast<std::uint32_t>(little(header, 16, 4)); entry.method = static_cast<unsigned>(method);
        auto disk = little(header, 34, 2); bool found64 = false, foundAes = false;
        Reader extras(SReadOnlyByteSpan(variable).subspan(static_cast<std::size_t>(nameSize), static_cast<std::size_t>(extraSize)));
        while (extras.remaining()) {
            const auto id = extras.le(2), length = extras.le(2); Reader value(extras.span(static_cast<std::size_t>(length)));
            if (id == 1) {
                if (found64) { invalid("duplicate ZIP64 extra field"); } found64 = true;
                if (entry.info.size == UINT32_MAX) { entry.info.size = value.le(8); }
                if (entry.packed == UINT32_MAX) { entry.packed = value.le(8); }
                if (entry.local == UINT32_MAX) { entry.local = value.le(8); }
                if (disk == 65535) { disk = value.le(4); }
            } else if (id==0x9901) {
                if (foundAes || method!=99) { invalid("unexpected or duplicate ZIP AES field"); }
                foundAes=true; aesExtra(entry,value.span(value.remaining()));
            }
        }
        if ((method==99)!=foundAes || (entry.aesVersion==2 && entry.crc)) { invalid("missing ZIP AES field or nonzero AE-2 CRC"); }
        if ((!found64 && (entry.info.size == UINT32_MAX || entry.packed == UINT32_MAX || entry.local == UINT32_MAX)) || disk) { invalid("missing ZIP64 sizes or unsupported disk"); }
        if (entry.info.size > INT64_MAX || entry.packed > INT64_MAX) { limit("ZIP entry exceeds 63-bit policy"); }
        const auto attributes = little(header, 38, 4), unixType = (attributes >> 16) & 0xf000;
        if ((little(header, 4, 2) >> 8) == 3 && unixType && unixType != 0x8000 && unixType != 0x4000) { unsupported("ZIP links and special files are unsupported"); }
        entry.info.directory = !original.empty() && original.back() == '/';
        if ((unixType == 0x4000 || (attributes & 16)) && !entry.info.directory) { invalid("ZIP directory name must end with slash"); }
        entry.info.name = safeName(original, entry.info.directory);
        if (entry.info.directory && (entry.info.size || entry.crc)) { invalid("nonempty ZIP directory entry"); }
        if (entry.local > central || central - entry.local < 30) { invalid("ZIP local header outside data area"); }
        const auto local = source.bytes(entry.local, 30);
        if (little(local, 0, 4) != 0x04034b50 || little(local, 6, 2) != flags || little(local, 8, 2) != method || little(local, 26, 2) != nameSize) { invalid("ZIP local/central mismatch"); }
        const auto localExtra = little(local, 28, 2);
        if (nameSize + localExtra > central - entry.local - 30) { invalid("ZIP local fields exceed data area"); }
        const auto localName = source.bytes(entry.local + 30, static_cast<std::size_t>(nameSize));
        if (std::string(localName.begin(), localName.end()) != original) { invalid("ZIP local/central name mismatch"); }
        if (foundAes) {
            const auto data=source.bytes(entry.local+30+nameSize,static_cast<std::size_t>(localExtra)); Reader fields(data); bool found=false;
            while (fields.remaining()) {
                const auto id=fields.le(2),len=fields.le(2); const auto value=fields.span(static_cast<std::size_t>(len));
                if (id==0x9901) {
                    if (found) { invalid("duplicate local ZIP AES field"); } found=true; Entry localEntry; aesExtra(localEntry,value);
                    if (localEntry.method!=entry.method || localEntry.encryption!=entry.encryption || localEntry.aesVersion!=entry.aesVersion) { invalid("ZIP local/central AES mismatch"); }
                }
            }
            if (!found) { invalid("missing local ZIP AES field"); }
        }
        entry.offset = entry.local + 30 + nameSize + localExtra;
        if (entry.packed > central - entry.offset) { invalid("ZIP payload outside data area"); }
        if (!(flags & 8)) {
            auto raw = little(local, 22, 4), packed = little(local, 18, 4);
            if (raw == UINT32_MAX || packed == UINT32_MAX) {
                const auto data = source.bytes(entry.local + 30 + nameSize, static_cast<std::size_t>(localExtra)); Reader fields(data); bool found = false;
                while (fields.remaining()) {
                    const auto id = fields.le(2), len = fields.le(2); Reader value(fields.span(static_cast<std::size_t>(len)));
                    if (id == 1) { if (found) { invalid("duplicate local ZIP64 field"); } found = true; if (raw == UINT32_MAX) { raw = value.le(8); } if (packed == UINT32_MAX) { packed = value.le(8); } }
                }
            }
            if (raw != entry.info.size || packed != entry.packed || little(local, 14, 4) != entry.crc) { invalid("ZIP local/central size or CRC mismatch"); }
        }
        const auto overhead=entry.encryption==EZIP_NONE?0u:entry.encryption==EZIP_AES128?20u:entry.encryption==EZIP_AES192?24u:28u;
        if (entry.packed<overhead || (entry.method==0 && entry.packed-overhead!=entry.info.size)) { invalid("stored ZIP entry length mismatch"); }
        ranges.emplace_back(entry.local, entry.offset + entry.packed); insertEntry(entries, names, nameBytes, std::move(entry)); pos += 46 + variableSize;
    }
    if (pos != centralLimit) { invalid("unexpected ZIP central directory data"); }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) { if (ranges[i].first < ranges[i - 1].second) { invalid("overlapping ZIP records"); } }
}

void readZipEntry(const ArchiveStore& source, const Entry& entry, const std::string& password, const SArchiveSink& sink) {
    std::uint64_t produced = 0; std::uint32_t crc = 0xffffffffu;
    auto output = [&](SReadOnlyByteSpan bytes) {
        if (bytes.size() > entry.info.size - produced) { invalid("ZIP expansion exceeds declared length"); }
        produced += bytes.size(); archiveCrcUpdate(crc, bytes); if (!bytes.empty()) { sink(bytes); }
    };
    if (entry.method == 0) { archiveDecryptZip(source,entry.offset,entry.packed,password,entry.encryption,output); }
    else {
        SCompressionOptions options; options.format = EFORMAT_RAW;
        options.maxInputBytes = options.maxOutputBytes = std::numeric_limits<std::size_t>::max();
        auto decoder = IStreamDecompressor::create(options); std::array<std::uint8_t, ARCHIVE_CHUNK> buffer{};
        bool finished=false;
        auto process=[&](SReadOnlyByteSpan input,EInputMode mode) {
            std::size_t pos=0;
            if (finished) { if (!input.empty()) { invalid("trailing ZIP compressed bytes"); } return; }
            for (;;) {
                const auto result=decoder->process(input.subspan(pos),buffer,mode); pos+=result.consumed; output(SReadOnlyByteSpan(buffer).first(result.produced));
                if (result.status==ESTREAM_FINISHED) { finished=true; if (pos!=input.size()) { invalid("trailing ZIP compressed bytes"); } break; }
                if (result.status==ESTREAM_NEED_INPUT) { break; }
                if (!result.consumed && !result.produced) { invalid("ZIP decoder made no progress"); }
            }
        };
        archiveDecryptZip(source,entry.offset,entry.packed,password,entry.encryption,[&](auto bytes) { process(bytes,EINPUT_MORE); });
        process({},EINPUT_FINAL); if (!finished) { invalid("truncated ZIP compressed data"); }
    }
    if (produced != entry.info.size || (entry.aesVersion!=2 && ~crc != entry.crc)) { invalid("ZIP entry CRC/size mismatch"); }
}

std::uint64_t tarNumber(SReadOnlyByteSpan input) {
    std::uint64_t value = 0;
    if (input.front() & 128) {
        if (input.front() & 64) { invalid("negative TAR numeric field"); }
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto digit = i ? input[i] : input[i] & 127;
            if (value > (UINT64_C(0x7fffffffffffffff) - digit) / 256) { limit("TAR numeric overflow"); } value = value * 256 + digit;
        }
    } else {
        std::size_t i = 0; while (i < input.size() && input[i] == ' ') { ++i; }
        bool ended = false;
        for (; i < input.size(); ++i) {
            const auto digit = input[i];
            if (!digit || digit == ' ') { ended = true; continue; }
            if (ended || digit < '0' || digit > '7') { invalid("invalid TAR octal field"); }
            if (value > (UINT64_C(0x7fffffffffffffff) - (digit - '0')) / 8) { limit("TAR numeric overflow"); } value = value * 8 + digit - '0';
        }
    }
    return value;
}

std::string tarText(SReadOnlyByteSpan bytes) {
    auto end = std::find(bytes.begin(), bytes.end(), 0); return std::string(bytes.begin(), end);
}

bool tarHeader(SReadOnlyByteSpan header) {
    if (header.size() != 512) { return false; }
    if (std::all_of(header.begin(), header.end(), [](auto value) { return value == 0; })) { return true; }
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < 512; ++i) { sum += i >= 148 && i < 156 ? 32 : header[i]; }
    try { return sum == tarNumber(header.subspan(148,8)); } catch (const CCompressionError&) { return false; }
}

class TarScanner {
private:
    std::vector<Entry>& _entries;
    std::unordered_set<std::string> _names;
    std::size_t _nameBytes = 0;
    SBytes _header, _extra;
    std::map<std::string, std::string> _global, _next;
    std::uint64_t _position = 0, _remaining = 0;
    std::size_t _padding = 0;
    unsigned _zeros = 0, _extraType = 0;
    bool _ended = false, _pending = false;

    void extended() {
        if (_extraType == 'L') { _next["path"] = tarText(_extra); _pending = true; return; }
        std::size_t pos = 0;
        while (pos < _extra.size()) {
            auto space = std::find(_extra.begin() + pos, _extra.end(), ' ');
            if (space == _extra.end()) { invalid("invalid PAX record length"); }
            const auto count = static_cast<std::size_t>(space - (_extra.begin() + pos));
            std::uint64_t length = 0;
            const char* first = reinterpret_cast<const char*>(_extra.data() + pos);
            const auto parsed = std::from_chars(first, first + count, length);
            if (parsed.ec != std::errc{} || parsed.ptr != first + count || length < count + 4 || length > _extra.size() - pos || _extra[pos + length - 1] != '\n') { invalid("invalid PAX record"); }
            const std::string text(_extra.begin() + pos + count + 1, _extra.begin() + pos + static_cast<std::size_t>(length) - 1);
            const auto equals = text.find('='); if (equals == std::string::npos || !equals) { invalid("invalid PAX key/value"); }
            auto key = text.substr(0, equals); auto value = text.substr(equals + 1);
            if (key.starts_with("GNU.sparse") || key == "SCHILY.realsize") { unsupported("sparse TAR files are unsupported"); }
            if (key == "path" || key == "size") { (_extraType == 'g' ? _global : _next)[key] = std::move(value); }
            pos += static_cast<std::size_t>(length);
        }
        if (_extraType != 'g') { _pending = true; }
    }

    void header() {
        const SReadOnlyByteSpan bytes(_header);
        if (std::all_of(bytes.begin(), bytes.end(), [](auto value) { return value == 0; })) {
            if (++_zeros == 2) { if (_pending) { invalid("orphan TAR extended header"); } _ended = true; } return;
        }
        if (_zeros || !tarHeader(bytes)) { invalid("TAR header checksum/terminator mismatch"); }
        auto name = tarText(bytes.first(100));
        const auto magic = tarText(bytes.subspan(257,6));
        if (!magic.empty() && magic != "ustar" && magic != "ustar ") { unsupported("unsupported TAR header variant"); }
        if (magic == "ustar") { const auto prefix = tarText(bytes.subspan(345,155)); if (!prefix.empty()) { name = prefix + '/' + name; } }
        auto size = tarNumber(bytes.subspan(124,12)); const auto type = bytes[156];
        _extraType = 0;
        if (type == 'x' || type == 'g' || type == 'L') {
            if (size > 1024 * 1024) { limit("TAR extended header exceeds 1 MiB"); }
            _extraType = type; _extra.clear();
        } else {
            if (type != 0 && type != '0' && type != '5') { unsupported("TAR links/special/sparse entries are unsupported"); }
            auto attributes = _global;
            for (const auto& [key, value] : _next) { if (value.empty()) { attributes.erase(key); } else { attributes[key] = value; } }
            if (auto it = attributes.find("path"); it != attributes.end()) { name = it->second; }
            if (auto it = attributes.find("size"); it != attributes.end()) {
                const auto& value = it->second; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || size > INT64_MAX) { invalid("invalid PAX file size"); }
            }
            _next.clear(); _pending = false;
            const bool directory = type == '5';
            if (directory && size) { invalid("TAR directory has a body"); }
            if (!(directory && (name == "." || name == "./"))) {
                Entry entry; entry.info = {safeName(name, directory), directory, size}; entry.offset = _position; entry.packed = size;
                insertEntry(_entries, _names, _nameBytes, std::move(entry));
            }
        }
        _remaining = size; _padding = static_cast<std::size_t>((512 - size % 512) % 512);
        if (!_remaining && _extraType) { extended(); _extraType = 0; }
    }

public:
    explicit TarScanner(std::vector<Entry>& entries) : _entries(entries) { _header.reserve(512); }
    void consume(SReadOnlyByteSpan input) {
        if (input.size() > INT64_MAX - _position) { limit("TAR stream exceeds 63-bit policy"); }
        while (!input.empty()) {
            if (_ended) {
                if (std::any_of(input.begin(), input.end(), [](auto value) { return value != 0; })) { invalid("trailing nonzero TAR bytes"); }
                _position += input.size(); return;
            }
            if (_remaining) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(_remaining, input.size()));
                if (_extraType) { _extra.insert(_extra.end(), input.begin(), input.begin() + count); }
                _position += count; input = input.subspan(count); _remaining -= count;
                if (!_remaining && _extraType) { extended(); _extraType = 0; } continue;
            }
            if (_padding) { const auto count = std::min(_padding, input.size()); _padding -= count; _position += count; input = input.subspan(count); continue; }
            const auto count = std::min<std::size_t>(512 - _header.size(), input.size());
            _header.insert(_header.end(), input.begin(), input.begin() + count); _position += count; input = input.subspan(count);
            if (_header.size() == 512) { header(); _header.clear(); }
        }
    }
    void finish() const { if (!_ended || _position % 512) { throw CCompressionError(EERROR_TRUNCATED, "TAR is missing complete zero terminators"); } }
};

void uncompress(const ArchiveStore& source, EArchiveFormat format, const SArchiveSink& sink) {
    if (format == EARCHIVE_TAR_XZ || format == EARCHIVE_XZ) { archiveReadXz(source, sink); }
    else if (format == EARCHIVE_TAR_GZIP) { archiveGzip(source, sink, false, 5); }
    else { archiveCopy(source, 0, source.size(), sink); }
}

EArchiveFormat scan(const ArchiveStore& source, std::vector<Entry>& entries, std::uint64_t maximum) {
    auto signature = source.bytes(0, static_cast<std::size_t>(std::min<std::uint64_t>(12, source.size())));
    if (signature.size() >= 4 && signature[0] == 'P' && signature[1] == 'K') {
        readZip(source, entries); std::uint64_t total = 0;
        for (const auto& entry : entries) { if (entry.info.size > maximum - total) { limit("ZIP declared expansion exceeds opening policy"); } total += entry.info.size; }
        return EARCHIVE_ZIP;
    }
    const bool xz = signature.size() >= 6 && std::equal(signature.begin(), signature.begin() + 6, std::array<std::uint8_t,6>{0xfd,'7','z','X','Z',0}.begin());
    const bool gzip = signature.size() >= 2 && signature[0] == 31 && signature[1] == 139;
    if (xz || gzip) {
        SBytes prefix; bool decided = false, tar = false; std::uint64_t total = 0; TarScanner parser(entries);
        auto sink = [&](SReadOnlyByteSpan input) {
            if (input.size() > INT64_MAX - total) { limit("archive expansion exceeds 63 bits"); } total += input.size();
            if (total > maximum) { limit("compressed archive expansion exceeds opening policy"); }
            if (!decided) {
                const auto count = std::min<std::size_t>(512 - prefix.size(), input.size());
                prefix.insert(prefix.end(), input.begin(), input.begin() + count); input = input.subspan(count);
                if (prefix.size() < 512) { return; }
                tar = tarHeader(prefix); decided = true; if (tar) { parser.consume(prefix); } prefix.clear();
            }
            if (tar) { parser.consume(input); }
        };
        uncompress(source, xz ? EARCHIVE_XZ : EARCHIVE_TAR_GZIP, sink);
        if (tar) { parser.finish(); return xz ? EARCHIVE_TAR_XZ : EARCHIVE_TAR_GZIP; }
        if (gzip) { unsupported("gzip archive does not contain TAR"); }
        Entry entry; entry.info = {"data", false, total}; entries.push_back(std::move(entry)); return EARCHIVE_XZ;
    }
    if (source.size() >= 1024 && tarHeader(source.bytes(0,512))) {
        if (source.size() > maximum) { limit("TAR size exceeds opening policy"); }
        TarScanner parser(entries); archiveCopy(source, 0, source.size(), [&](auto bytes) { parser.consume(bytes); }); parser.finish(); return EARCHIVE_TAR;
    }
    unsupported("unrecognized archive signature");
}

void tarOctal(SBytes& header, std::size_t pos, unsigned count, std::uint64_t value) {
    for (unsigned i = count - 1; i--;) { header[pos + i] = static_cast<std::uint8_t>('0' + (value & 7)); value >>= 3; }
    header[pos + count - 1] = 0; if (value) { limit("TAR octal field overflow"); }
}

SBytes tarBlock(const std::string& name, std::uint64_t size, bool directory, unsigned type = 0) {
    SBytes block(512); auto shortName = name; std::string prefix;
    if (name.size() > 100) {
        auto slash = name.rfind('/', std::min<std::size_t>(155, name.size() - 1));
        if (slash != std::string::npos && name.size() - slash - 1 <= 100) { prefix = name.substr(0, slash); shortName = name.substr(slash + 1); }
        else { shortName = "entry"; }
    }
    std::copy(shortName.begin(), shortName.end(), block.begin()); std::copy(prefix.begin(), prefix.end(), block.begin() + 345);
    tarOctal(block, 100, 8, directory ? 0755 : 0644); tarOctal(block, 108, 8, 0); tarOctal(block, 116, 8, 0);
    tarOctal(block, 124, 12, size); tarOctal(block, 136, 12, 0);
    std::fill(block.begin() + 148, block.begin() + 156, ' '); block[156] = static_cast<std::uint8_t>(type ? type : directory ? '5' : '0');
    const std::string magic("ustar\00000", 8); std::copy(magic.begin(), magic.end(), block.begin() + 257);
    std::uint64_t sum = 0; for (auto value : block) { sum += value; }
    tarOctal(block, 148, 7, sum); block[155] = ' '; return block;
}

std::string paxRecord(const std::string& key, const std::string& value) {
    const auto suffix = " " + key + "=" + value + "\n"; auto length = suffix.size() + 1;
    while (length != std::to_string(length).size() + suffix.size()) { length = std::to_string(length).size() + suffix.size(); }
    return std::to_string(length) + suffix;
}

void zeros(ArchiveStore& store, std::size_t count) { const std::array<std::uint8_t, 1024> bytes{}; store.append(SReadOnlyByteSpan(bytes).first(count)); }

Entry writeTar(ArchiveStore& destination, const ArchiveStore& source, std::string name, bool directory) {
    std::string pax;
    if (name.size() > 100) { pax += paxRecord("path", name); }
    if (source.size() >= UINT64_C(8589934592)) { pax += paxRecord("size", std::to_string(source.size())); }
    if (!pax.empty()) {
        destination.append(tarBlock("PaxHeader", pax.size(), false, 'x'));
        destination.append(SReadOnlyByteSpan(reinterpret_cast<const std::uint8_t*>(pax.data()), pax.size())); zeros(destination, (512 - pax.size() % 512) % 512);
    }
    destination.append(tarBlock(name, source.size() >= UINT64_C(8589934592) ? 0 : source.size(), directory));
    Entry entry; entry.info = {std::move(name), directory, source.size()}; entry.offset = destination.size(); entry.packed = source.size();
    archiveCopy(source, 0, source.size(), [&](auto bytes) { destination.append(bytes); }); zeros(destination, static_cast<std::size_t>((512 - source.size() % 512) % 512)); return entry;
}

Entry writeZip(ArchiveStore& destination, const ArchiveStore& source, std::string name, bool directory, unsigned level, const SZipOptions& zip) {
    Entry entry; entry.info = {std::move(name), directory, source.size()}; entry.local = destination.size(); entry.method = directory ? 0 : 8;
    entry.encryption=directory?EZIP_NONE:zip.encryption; entry.info.encrypted=entry.encryption!=EZIP_NONE; entry.aesVersion=entry.info.encrypted?2:0;
    const auto aes=aesExtra(entry);
    SBytes header; putLe(header, 0x04034b50, 4); putLe(header, entry.info.encrypted?51:45, 2); putLe(header, entry.info.encrypted?0x809:0x808, 2); putLe(header, entry.info.encrypted?99:entry.method, 2);
    putLe(header, 0, 2); putLe(header, 33, 2); putLe(header, 0, 4); putLe(header, UINT32_MAX, 4); putLe(header, UINT32_MAX, 4);
    putLe(header, entry.info.name.size(), 2); putLe(header, 20+aes.size(), 2); header.insert(header.end(), entry.info.name.begin(), entry.info.name.end());
    putLe(header, 1, 2); putLe(header, 16, 2); putLe(header, entry.info.size, 8); putLe(header, 0, 8); header.insert(header.end(),aes.begin(),aes.end()); destination.append(header); entry.offset = destination.size();
    std::uint32_t crc = 0xffffffffu;
    if (!directory) {
        ZipEncryptor cipher(zip,[&](auto bytes) { destination.append(bytes); });
        SCompressionOptions options; options.level = level; options.format = EFORMAT_RAW;
        options.maxInputBytes = options.maxOutputBytes = std::numeric_limits<std::size_t>::max();
        auto encoder = IStreamCompressor::create(options); std::array<std::uint8_t, ARCHIVE_CHUNK> output{};
        auto process = [&](SReadOnlyByteSpan input, EFlushMode mode) {
            std::size_t pos = 0;
            for (;;) {
                auto result = encoder->process(input.subspan(pos), output, mode); pos += result.consumed;
                cipher.process(SReadOnlyByteSpan(output).first(result.produced));
                if (result.status == ESTREAM_NEED_INPUT || result.status == ESTREAM_FINISHED) { break; }
            }
        };
        archiveCopy(source, 0, source.size(), [&](auto input) { archiveCrcUpdate(crc, input); process(input, EFLUSH_NONE); }); process({}, EFLUSH_FINISH);
        cipher.finish();
    }
    entry.crc = entry.aesVersion==2?0:~crc; entry.packed = destination.size() - entry.offset;
    SBytes descriptor; putLe(descriptor, 0x08074b50, 4); putLe(descriptor, entry.crc, 4); putLe(descriptor, entry.packed, 8); putLe(descriptor, entry.info.size, 8); destination.append(descriptor);
    return entry;
}

void finishZip(ArchiveStore& destination, const std::vector<Entry>& entries) {
    const auto central = destination.size();
    for (const auto& entry : entries) {
        const auto aes=aesExtra(entry);
        SBytes header; putLe(header, 0x02014b50, 4); putLe(header, 0x032d, 2); putLe(header, entry.info.encrypted?51:45, 2); putLe(header, entry.info.encrypted?0x809:0x808, 2); putLe(header, entry.info.encrypted?99:entry.method, 2);
        putLe(header, 0, 2); putLe(header, 33, 2); putLe(header, entry.crc, 4); putLe(header, UINT32_MAX, 4); putLe(header, UINT32_MAX, 4);
        putLe(header, entry.info.name.size(), 2); putLe(header, 28+aes.size(), 2); putLe(header, 0, 2); putLe(header, 0, 2); putLe(header, 0, 2);
        putLe(header, entry.info.directory ? UINT32_C(0x41ed0010) : UINT32_C(0x81a40000), 4); putLe(header, UINT32_MAX, 4);
        header.insert(header.end(), entry.info.name.begin(), entry.info.name.end()); putLe(header, 1, 2); putLe(header, 24, 2);
        putLe(header, entry.info.size, 8); putLe(header, entry.packed, 8); putLe(header, entry.local, 8); header.insert(header.end(),aes.begin(),aes.end()); destination.append(header);
    }
    const auto size = destination.size() - central, zip64 = destination.size(); SBytes end;
    putLe(end, 0x06064b50, 4); putLe(end, 44, 8); putLe(end, 45, 2); putLe(end, 45, 2); putLe(end, 0, 4); putLe(end, 0, 4);
    putLe(end, entries.size(), 8); putLe(end, entries.size(), 8); putLe(end, size, 8); putLe(end, central, 8);
    putLe(end, 0x07064b50, 4); putLe(end, 0, 4); putLe(end, zip64, 8); putLe(end, 1, 4);
    putLe(end, 0x06054b50, 4); putLe(end, 0, 4); putLe(end, 65535, 2); putLe(end, 65535, 2); putLe(end, UINT32_MAX, 4); putLe(end, UINT32_MAX, 4); putLe(end, 0, 2); destination.append(end);
}

const Entry& findEntry(const std::vector<Entry>& entries, const std::string& name) {
    for (const auto& entry : entries) { if (entry.info.name == name) { if (entry.info.directory) { invalid("cannot read a directory as a file"); } return entry; } }
    throw std::out_of_range("archive entry not found");
}

std::vector<SArchiveEntry> metadata(const std::vector<Entry>& entries, std::uint64_t offset, std::size_t count) {
    if (count > 4096) { throw std::invalid_argument("archive metadata page exceeds 4096 entries"); }
    std::vector<SArchiveEntry> result;
    if (offset >= entries.size()) { return result; }
    count = std::min(count,entries.size()-static_cast<std::size_t>(offset)); result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) { result.push_back(entries[static_cast<std::size_t>(offset)+i].info); } return result;
}

struct Operation {
    bool& busy;
    explicit Operation(bool& value) : busy(value) { if (busy) { stateError("serialize archive operations; callbacks must not reenter"); } busy = true; }
    ~Operation() { busy = false; }
};

class ArchiveReader final : public IArchive {
private:
    struct Impl {
        std::shared_ptr<ArchiveStore> source;
        std::vector<Entry> entries;
        EArchiveFormat format;
        std::uint64_t maximum;
        std::string password;
        mutable bool busy = false;
        explicit Impl(std::shared_ptr<ArchiveStore> store, std::uint64_t ceiling, std::string secret) : source(std::move(store)), format(scan(*source, entries, ceiling)), maximum(ceiling), password(std::move(secret)) {}
        ~Impl() { eraseSecret({reinterpret_cast<std::uint8_t*>(password.data()),password.size()}); }
    };
    std::unique_ptr<Impl> _impl;
public:
    ArchiveReader(std::shared_ptr<ArchiveStore> source, std::uint64_t maximum, std::string password) : _impl(std::make_unique<Impl>(std::move(source),maximum,std::move(password))) {}
    EArchiveFormat format() const noexcept override { return _impl->format; }
    bool isReadOnly() const noexcept override { return true; }
    SMappedIOOptions mappingOptions() const override { return _impl->source->mappingOptions(); }
    void setMappingOptions(SMappedIOOptions options) override { Operation operation(_impl->busy); _impl->source->setMappingOptions(options); }
    std::uint64_t entryCount() const noexcept override { return _impl->entries.size(); }
    std::vector<SArchiveEntry> entries(std::uint64_t offset, std::size_t count) const override { return metadata(_impl->entries,offset,count); }
    void addDirectory(std::string) override { stateError("archive is read-only"); }
    void addFile(std::string, SReadOnlyByteSpan) override { stateError("archive is read-only"); }
    void addFileFromFile(std::string, const std::filesystem::path&) override { stateError("archive is read-only"); }
    void finish() override { stateError("archive is read-only"); }
    void readFile(const std::string& name, SArchiveSink sink) const override {
        if (!sink) { throw std::invalid_argument("archive sink is empty"); }
        Operation operation(_impl->busy); const auto& entry = findEntry(_impl->entries, name);
        if (_impl->format == EARCHIVE_ZIP) { readZipEntry(*_impl->source, entry, _impl->password, sink); }
        else if (_impl->format == EARCHIVE_TAR) { archiveCopy(*_impl->source, entry.offset, entry.info.size, sink); }
        else {
            std::uint64_t pos = 0, produced = 0;
            uncompress(*_impl->source, _impl->format, [&](SReadOnlyByteSpan bytes) {
                if (pos > _impl->maximum || bytes.size() > _impl->maximum - pos) { limit("archive expansion exceeds opening policy"); }
                const auto start = std::max(pos, entry.offset), end = std::min(pos + bytes.size(), entry.offset + entry.info.size);
                if (end > start) { sink(bytes.subspan(static_cast<std::size_t>(start - pos), static_cast<std::size_t>(end - start))); produced += end - start; }
                pos += bytes.size();
            });
            if (produced != entry.info.size) { invalid("compressed archive entry size mismatch"); }
        }
    }
};

class ArchiveWriter final : public IArchive {
private:
    struct Impl {
        std::shared_ptr<ArchiveStore> output, staging;
        std::vector<Entry> entries;
        std::unordered_set<std::string> names;
        std::size_t nameBytes = 0;
        EArchiveFormat format;
        unsigned level;
        SZipOptions zip;
        SXzOptions xz;
        SArchiveCompletion completion;
        enum State { Writable, Finishing, Finished, Failed } state = Writable;
        mutable bool busy = false;
        Impl(std::shared_ptr<ArchiveStore> store, std::shared_ptr<ArchiveStore> scratch, EArchiveFormat container,
            unsigned effort, SArchiveCompletion callback, SZipOptions zipOptions, SXzOptions xzOptions) : output(std::move(store)), staging(std::move(scratch)), format(container), level(effort), zip(std::move(zipOptions)), xz(std::move(xzOptions)), completion(std::move(callback)) {}
        ~Impl() { eraseSecret({reinterpret_cast<std::uint8_t*>(zip.password.data()),zip.password.size()}); }
        void writable() const { if (state != Writable) { stateError("archive is sealed or failed"); } }
        void add(std::string name, const ArchiveStore& source, bool directory) {
            writable(); name = safeName(std::move(name), directory);
            if (entries.size() == ARCHIVE_ENTRIES) { limit("too many archive entries"); }
            if (name.size() > 64 * 1024 * 1024 - nameBytes) { limit("archive name metadata exceeds 64 MiB"); }
            auto key = name; if (directory) { key.pop_back(); }
            if (names.contains(key)) { invalid("duplicate archive entry name"); }
            if (format == EARCHIVE_XZ && (directory || !entries.empty())) { unsupported("XZ holds one file; use TAR_XZ for multiple entries"); }
            Operation operation(busy);
            try {
                Entry entry;
                if (format == EARCHIVE_ZIP) { entry = writeZip(*output, source, std::move(name), directory, level, zip); }
                else if (format == EARCHIVE_XZ) {
                    archiveCopy(source, 0, source.size(), [&](auto bytes) { staging->append(bytes); });
                    entry.info = {std::move(name), false, source.size()}; entry.packed = source.size();
                } else { entry = writeTar(*staging, source, std::move(name), directory); }
                nameBytes += entry.info.name.size(); names.insert(std::move(key)); entries.push_back(std::move(entry));
            } catch (...) { state = Failed; throw; }
        }
    };
    std::unique_ptr<Impl> _impl;
public:
    ArchiveWriter(std::shared_ptr<ArchiveStore> output, std::shared_ptr<ArchiveStore> staging, EArchiveFormat format,
        unsigned level, SArchiveCompletion completion, SZipOptions zip, SXzOptions xz) : _impl(std::make_unique<Impl>(std::move(output), std::move(staging), format, level, std::move(completion),std::move(zip),std::move(xz))) {}
    EArchiveFormat format() const noexcept override { return _impl->format; }
    bool isReadOnly() const noexcept override { return false; }
    SMappedIOOptions mappingOptions() const override { return _impl->output->mappingOptions(); }
    void setMappingOptions(SMappedIOOptions options) override {
        Operation operation(_impl->busy); _impl->output->setMappingOptions(options);
        if (_impl->staging!=_impl->output) { _impl->staging->setMappingOptions(options); }
    }
    std::uint64_t entryCount() const noexcept override { return _impl->entries.size(); }
    std::vector<SArchiveEntry> entries(std::uint64_t offset, std::size_t count) const override { return metadata(_impl->entries,offset,count); }
    void addDirectory(std::string name) override { auto empty = archiveMemory(0); _impl->add(std::move(name), *empty, true); }
    void addFile(std::string name, SReadOnlyByteSpan content) override {
        _impl->writable(); SpanStore data(content); _impl->add(std::move(name), data, false);
    }
    void addFileFromFile(std::string name, const std::filesystem::path& source) override {
        _impl->writable(); auto file = archiveFile(source, false, mappingOptions()); _impl->add(std::move(name), *file, false);
    }
    void readFile(const std::string& name, SArchiveSink sink) const override {
        if (!sink) { throw std::invalid_argument("archive sink is empty"); }
        if (_impl->state == Impl::Failed) { stateError("archive writer failed"); }
        Operation operation(_impl->busy); const auto& entry = findEntry(_impl->entries, name);
        if (_impl->format == EARCHIVE_ZIP) { readZipEntry(*_impl->output, entry, _impl->zip.password, sink); }
        else { archiveCopy(*_impl->staging, entry.offset, entry.info.size, sink); }
    }
    void finish() override {
        Operation operation(_impl->busy); auto& s = *_impl;
        if (s.state == Impl::Finished) { return; } s.writable(); s.state = Impl::Finishing;
        try {
            if (s.format == EARCHIVE_ZIP) { finishZip(*s.output, s.entries); }
            else {
                if (s.format != EARCHIVE_XZ) { zeros(*s.staging, 1024); }
                if (s.format == EARCHIVE_XZ || s.format == EARCHIVE_TAR_XZ) { archiveWriteXz(*s.staging, [&](auto bytes) { s.output->append(bytes); }, s.level, s.xz); }
                else if (s.format == EARCHIVE_TAR_GZIP) { archiveGzip(*s.staging, [&](auto bytes) { s.output->append(bytes); }, true, s.level); }
            }
            s.output->flush(); s.state = Impl::Finished;
            if (s.completion) { auto callback = std::move(s.completion); callback(s.output->memory()); }
        } catch (...) { s.state = Impl::Failed; throw; }
    }
};

void validateArchive(EArchiveFormat format, unsigned level) {
    if (format < EARCHIVE_ZIP || format > EARCHIVE_TAR_GZIP || level < 1 || level > 9) { throw std::invalid_argument("invalid archive format/level"); }
}

void validateOptions(EArchiveFormat format,const SZipOptions& zip,const SXzOptions& xz) {
    if (zip.password.size()>4096 || zip.encryption<EZIP_NONE || zip.encryption>EZIP_AES256) { throw std::invalid_argument("invalid ZIP encryption/password"); }
    if (zip.encryption!=EZIP_NONE && (format!=EARCHIVE_ZIP || zip.password.empty())) { throw std::invalid_argument("ZIP encryption requires ZIP format and a nonempty password"); }
    if ((!xz.filters.empty() || xz.check!=EXZ_CHECK_CRC32) && format!=EARCHIVE_XZ && format!=EARCHIVE_TAR_XZ) { throw std::invalid_argument("XZ settings require XZ/TAR_XZ format"); }
    archiveValidateXz(xz);
}

EArchiveFormat fromExtension(const std::filesystem::path& path) {
    auto name = path.filename().string(); for (auto& c : name) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
    if (name.ends_with(".tar.xz") || name.ends_with(".txz")) { return EARCHIVE_TAR_XZ; }
    if (name.ends_with(".tar.gz") || name.ends_with(".tgz")) { return EARCHIVE_TAR_GZIP; }
    if (name.ends_with(".zip")) { return EARCHIVE_ZIP; }
    if (name.ends_with(".tar")) { return EARCHIVE_TAR; }
    if (name.ends_with(".xz")) { return EARCHIVE_XZ; }
    unsupported("archive creation requires .zip/.tar/.xz/.tar.xz/.tar.gz");
}

struct ArchiveDispatch {
    asyncpp::dispatcher* worker;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> continuation) const { worker->push([continuation] { continuation.resume(); }); }
    void await_resume() const noexcept {}
};

asyncpp::task<std::shared_ptr<IArchive>> openOn(std::filesystem::path path, SArchiveOpenOptions options, asyncpp::dispatcher* worker) {
    co_await ArchiveDispatch{worker}; co_return IArchive::createFromFile(path, options);
}
asyncpp::task<SBytes> readOn(std::shared_ptr<IArchive> archive, std::string name, std::size_t maximum, asyncpp::dispatcher* worker) {
    co_await ArchiveDispatch{worker}; co_return archive->readFile(name, maximum);
}
asyncpp::task<std::shared_ptr<IArchive>> memoryOn(SBytes bytes, std::uint64_t maximum, asyncpp::dispatcher* worker, std::string password) {
    co_await ArchiveDispatch{worker}; co_return IArchive::openFromMemory(bytes,maximum,std::move(password));
}
asyncpp::task<void> sinkOn(std::shared_ptr<IArchive> archive, std::string name, SArchiveSink sink, asyncpp::dispatcher* worker) {
    co_await ArchiveDispatch{worker}; archive->readFile(name,std::move(sink));
}
asyncpp::task<void> addOn(std::shared_ptr<IArchive> archive, std::string name, SBytes bytes, asyncpp::dispatcher* worker) {
    co_await ArchiveDispatch{worker}; archive->addFile(std::move(name), bytes);
}
asyncpp::task<void> addPathOn(std::shared_ptr<IArchive> archive, std::string name, std::filesystem::path path, asyncpp::dispatcher* worker) {
    co_await ArchiveDispatch{worker}; archive->addFileFromFile(std::move(name), path);
}
asyncpp::task<void> finishOn(std::shared_ptr<IArchive> archive, asyncpp::dispatcher* worker) {
    co_await ArchiveDispatch{worker}; archive->finish();
}

} // namespace

/* Select distinct immutable-reader and append-only-creator implementations. */
std::shared_ptr<IArchive> IArchive::createFromFile(const std::filesystem::path& filename, SArchiveOpenOptions options) {
    validateArchive(EARCHIVE_ZIP, options.compressionLevel);
    if (options.mapping.windowBytes<4096 || options.mapping.windowBytes>64*1024*1024) { limit("mapping window must be 4 KiB..64 MiB"); }
    if (options.mode != EARCHIVE_CREATE && options.mode != EARCHIVE_OPEN) { throw std::invalid_argument("invalid archive open mode"); }
    try {
        if (options.zip.password.size()>4096) { throw std::invalid_argument("ZIP password exceeds 4096 bytes"); }
        if (options.mode == EARCHIVE_OPEN) { return std::make_shared<ArchiveReader>(archiveFile(filename, false,options.mapping),options.maximumExpandedBytes,std::move(options.zip.password)); }
        const auto format = fromExtension(filename); validateOptions(format,options.zip,options.xz); auto output = archiveFile(filename, true,options.mapping);
        auto scratch = format == EARCHIVE_ZIP || format == EARCHIVE_TAR ? output : archiveTemporary(options.mapping);
        return std::make_shared<ArchiveWriter>(std::move(output), std::move(scratch), format, options.compressionLevel, SArchiveCompletion{},std::move(options.zip),std::move(options.xz));
    } catch (const NeedInput&) { throw CCompressionError(EERROR_TRUNCATED, "truncated archive metadata"); }
}

/* No hidden disk staging on the explicitly memory-only path. */
std::shared_ptr<IArchive> IArchive::createInMemory(std::size_t maximum, SArchiveMemoryOptions options, SArchiveCompletion completion) {
    validateArchive(options.format, options.compressionLevel);
    validateOptions(options.format,options.zip,options.xz);
    if (!completion) { throw std::invalid_argument("memory archive completion callback is empty"); }
    auto output = archiveMemory(maximum); auto scratch = options.format == EARCHIVE_ZIP || options.format == EARCHIVE_TAR ? output : archiveMemory(maximum);
    return std::make_shared<ArchiveWriter>(std::move(output), std::move(scratch), options.format, options.compressionLevel, std::move(completion),std::move(options.zip),std::move(options.xz));
}

/* Preserve ownership even if a caller destroys or mutates its span after opening. */
std::shared_ptr<IArchive> IArchive::openFromMemory(SReadOnlyByteSpan memory, std::uint64_t maximumExpandedBytes, std::string password) {
    if (password.size()>4096) { throw std::invalid_argument("ZIP password exceeds 4096 bytes"); }
    try { return std::make_shared<ArchiveReader>(archiveMemory(memory.size(), memory),maximumExpandedBytes,std::move(password)); }
    catch (const NeedInput&) { throw CCompressionError(EERROR_TRUNCATED, "truncated archive metadata"); }
}

IArchive::~IArchive() = default;

/* The allocation cap is also enforced for each produced chunk, not only declared metadata. */
SBytes IArchive::readFile(const std::string& name, std::size_t maximum) const {
    SBytes output; readFile(name, [&](auto bytes) { room(output, bytes.size(), maximum); output.insert(output.end(), bytes.begin(), bytes.end()); }); return output;
}

asyncpp::task<std::shared_ptr<IArchive>> IArchive::createFromFileAsync(std::filesystem::path filename, SArchiveOpenOptions options, asyncpp::dispatcher& worker) { return openOn(std::move(filename), options, &worker); }
asyncpp::task<SBytes> IArchive::readFileAsync(std::string name, std::size_t maximum, asyncpp::dispatcher& worker) { return readOn(shared_from_this(), std::move(name), maximum, &worker); }
asyncpp::task<std::shared_ptr<IArchive>> IArchive::openFromMemoryAsync(SBytes memory, std::uint64_t maximum, asyncpp::dispatcher& worker, std::string password) { return memoryOn(std::move(memory),maximum,&worker,std::move(password)); }
asyncpp::task<void> IArchive::readFileAsync(std::string name, SArchiveSink sink, asyncpp::dispatcher& worker) { return sinkOn(shared_from_this(),std::move(name),std::move(sink),&worker); }
asyncpp::task<void> IArchive::addFileAsync(std::string name, SBytes bytes, asyncpp::dispatcher& worker) { return addOn(shared_from_this(), std::move(name), std::move(bytes), &worker); }
asyncpp::task<void> IArchive::addFileFromFileAsync(std::string name, std::filesystem::path source, asyncpp::dispatcher& worker) { return addPathOn(shared_from_this(), std::move(name), std::move(source), &worker); }
asyncpp::task<void> IArchive::finishAsync(asyncpp::dispatcher& worker) { return finishOn(shared_from_this(), &worker); }

} // namespace hncomp
