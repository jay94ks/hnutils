#include <hnfs/filesystem.hpp>
#include "internal.hpp"
#include <map>

namespace hnfs {
namespace detail {
namespace {

unsigned alignment(EFormat format) {
    return format==EFORMAT_CPIO_ODC?1:format>=EFORMAT_CPIO_NEWC?4:2;
}

std::string canonical(std::string path) {
    std::size_t prefix=0; while (path.compare(prefix,2,"./")==0) { prefix+=2; }
    if (prefix) { path.erase(0,prefix); if (path.empty()) { return {}; } }
    if (path==".") { return {}; }
    if (path.empty() || path.front()=='/' || path.back()=='/' || path.find_first_of("\\:")!=std::string::npos || path.find('\0')!=std::string::npos) { fail(EERROR_INVALID,"unsafe CPIO pathname"); }
    for (std::size_t at=0;at<path.size();) {
        auto end=path.find('/',at); if (end==std::string::npos) { end=path.size(); }
        const auto part=path.substr(at,end-at);
        if (part.empty() || part=="." || part=="..") { fail(EERROR_INVALID,"unsafe CPIO path component"); }
        at=end+1;
    }
    return path;
}

std::uint64_t number(SReadOnlyByteSpan bytes,unsigned base) {
    std::uint64_t value=0;
    for (auto byte:bytes) {
        const auto digit=byte>='0' && byte<='9'?unsigned(byte-'0'):byte>='a' && byte<='f'?unsigned(byte-'a'+10):byte>='A' && byte<='F'?unsigned(byte-'A'+10):base;
        if (digit>=base || value>(UINT64_MAX-digit)/base) { fail(EERROR_INVALID,"invalid CPIO numeric field"); }
        value=value*base+digit;
    }
    return value;
}

SBytes digits(std::uint64_t value,unsigned count,unsigned base) {
    constexpr char alphabet[]="0123456789abcdef"; SBytes output(count,'0');
    for (auto at=count;at>0;--at) { output[at-1]=alphabet[value%base]; value/=base; }
    if (value) { fail(EERROR_LIMIT,"CPIO value does not fit the selected format"); } return output;
}

struct Header {
    std::uint64_t mode=0,uid=0,gid=0,links=0,time=0,size=0,name=0,check=0;
};

Header decodeHeader(EFormat format,SReadOnlyByteSpan bytes) {
    Header header;
    if (format==EFORMAT_CPIO_BINARY_LE || format==EFORMAT_CPIO_BINARY_BE) {
        auto word=[&](unsigned at) { const auto first=bytes[at*2],second=bytes[at*2+1]; return format==EFORMAT_CPIO_BINARY_LE?unsigned(first)|(unsigned(second)<<8):(unsigned(first)<<8)|second; };
        if (word(0)!=070707) { fail(EERROR_INVALID,"mixed or invalid binary CPIO magic"); }
        header.mode=word(3); header.uid=word(4); header.gid=word(5); header.links=word(6);
        header.time=(std::uint64_t(word(8))<<16)|word(9); header.name=word(10); header.size=(std::uint64_t(word(11))<<16)|word(12);
    } else if (format==EFORMAT_CPIO_ODC) {
        if (number(bytes.first(6),8)!=070707) { fail(EERROR_INVALID,"mixed or invalid odc CPIO magic"); }
        header.mode=number(bytes.subspan(18,6),8); header.uid=number(bytes.subspan(24,6),8); header.gid=number(bytes.subspan(30,6),8);
        header.links=number(bytes.subspan(36,6),8); header.time=number(bytes.subspan(48,11),8);
        header.name=number(bytes.subspan(59,6),8); header.size=number(bytes.subspan(65,11),8);
        // --> Validate ignored numeric fields too, without importing a packed host struct.
        number(bytes.subspan(6,6),8); number(bytes.subspan(12,6),8); number(bytes.subspan(42,6),8);
    } else {
        if (number(bytes.first(6),8)!=(format==EFORMAT_CPIO_CRC?070702u:070701u)) { fail(EERROR_INVALID,"mixed or invalid newc CPIO magic"); }
        std::array<std::uint64_t,13> fields{}; for (unsigned i=0;i<13;++i) { fields[i]=number(bytes.subspan(6+8*i,8),16); }
        header.mode=fields[1]; header.uid=fields[2]; header.gid=fields[3]; header.links=fields[4]; header.time=fields[5];
        header.size=fields[6]; header.name=fields[11]; header.check=fields[12];
        if (format==EFORMAT_CPIO_NEWC && header.check) { fail(EERROR_INVALID,"newc check field must be zero"); }
    }
    return header;
}

SBytes encodeHeader(EFormat format,const SEntry& entry,std::uint64_t inode,std::size_t nameLength,bool trailer=false) {
    SBytes output;
    const auto mode=trailer?0u:entry.permissions|(entry.kind==ENODE_DIRECTORY?0040000u:0100000u);
    const auto links=entry.kind==ENODE_DIRECTORY && !trailer?2u:1u;
    auto append=[&](std::uint64_t value,unsigned count,unsigned base) { const auto field=digits(value,count,base); output.insert(output.end(),field.begin(),field.end()); };
    if (format==EFORMAT_CPIO_BINARY_LE || format==EFORMAT_CPIO_BINARY_BE) {
        auto word=[&](std::uint64_t value) {
            if (value>65535) { fail(EERROR_LIMIT,"binary CPIO field exceeds 16 bits"); }
            if (format==EFORMAT_CPIO_BINARY_LE) { output.push_back(static_cast<std::uint8_t>(value)); output.push_back(static_cast<std::uint8_t>(value>>8)); }
            else { output.push_back(static_cast<std::uint8_t>(value>>8)); output.push_back(static_cast<std::uint8_t>(value)); }
        };
        if (entry.size>UINT32_MAX || entry.modifiedTime>UINT32_MAX) { fail(EERROR_LIMIT,"binary CPIO size/time exceeds 32 bits"); }
        word(070707); word(0); word(inode); word(mode); word(entry.uid); word(entry.gid); word(links); word(0);
        word(entry.modifiedTime>>16); word(entry.modifiedTime&65535); word(nameLength); word(entry.size>>16); word(entry.size&65535);
    } else if (format==EFORMAT_CPIO_ODC) {
        append(070707,6,8); append(0,6,8); append(inode,6,8); append(mode,6,8); append(entry.uid,6,8); append(entry.gid,6,8);
        append(links,6,8); append(0,6,8); append(entry.modifiedTime,11,8); append(nameLength,6,8); append(entry.size,11,8);
    } else {
        const auto* magic=format==EFORMAT_CPIO_CRC?"070702":"070701"; output.insert(output.end(),magic,magic+6);
        append(inode,8,16); append(mode,8,16); append(entry.uid,8,16); append(entry.gid,8,16); append(links,8,16);
        append(entry.modifiedTime,8,16); append(entry.size,8,16);
        for (unsigned i=0;i<4;++i) { append(0,8,16); } append(nameLength,8,16); append(0,8,16);
    }
    return output;
}

std::size_t headerSize(EFormat format) { return format==EFORMAT_CPIO_ODC?76:format>=EFORMAT_CPIO_NEWC?110:26; }

/** One entry at a time; memory is independent from file count and file data length. */
class CpioReader final : public IArchiveReader {
private:
    struct Impl {
        std::shared_ptr<IBlockDevice> device;
        SOpenOptions options;
        EFormat format;
        std::uint64_t position=0,remaining=0,totalData=0,nameBytes=0,entries=0,used=0,payloadOffset=0;
        std::uint32_t checksum=0,expected=0;
        bool haveEntry=false,finalized=false,ended=false,failed=false;
        std::mutex mutex;

        Impl(std::shared_ptr<IBlockDevice> source,SOpenOptions policy) : device(std::move(source)),options(policy),format(IFileSystem::probe(*device).format) {}
        void valid() const { if (failed) { fail(EERROR_STATE,"archive cursor failed"); } }
        void available(std::uint64_t amount) const {
            if (position>device->size() || amount>device->size()-position) { fail(EERROR_TRUNCATED,"truncated CPIO record"); }
        }
        SBytes take(std::size_t count) { available(count); auto bytes=device->readBytes(position,count); position+=count; return bytes; }
        void padding() {
            const auto count=static_cast<std::size_t>((alignment(format)-position%alignment(format))%alignment(format));
            const auto bytes=take(count); if (std::any_of(bytes.begin(),bytes.end(),[](auto b) { return b!=0; })) { fail(EERROR_INVALID,"nonzero CPIO alignment padding"); }
        }
        void complete() {
            if (finalized || remaining) { return; }
            if (format==EFORMAT_CPIO_CRC && checksum!=expected) { fail(EERROR_CHECKSUM,"CPIO byte-sum mismatch"); }
            padding(); finalized=true;
        }
        std::size_t read(SByteSpan output) {
            if (!haveEntry) { fail(EERROR_STATE,"select a CPIO record before reading data"); }
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(remaining,output.size()));
            available(count); device->readAt(position,output.first(count)); position+=count; remaining-=count;
            if (format==EFORMAT_CPIO_CRC) { for (auto byte:output.first(count)) { checksum+=byte; } }
            complete(); return count;
        }
        std::optional<SEntry> next() {
            if (ended) { return std::nullopt; }
            if (haveEntry) { std::array<std::uint8_t,65536> buffer{}; while (remaining) { read(buffer); } complete(); haveEntry=false; }
            const auto header=decodeHeader(format,take(headerSize(format)));
            if (!header.name || header.name>std::max<std::size_t>(11,options.maximumPathBytes)) { fail(EERROR_LIMIT,"CPIO pathname exceeds policy"); }
            const auto name=take(static_cast<std::size_t>(header.name));
            const auto terminator=std::find(name.begin(),name.end(),0);
            if (terminator==name.end() || std::any_of(terminator,name.end(),[](auto b) { return b!=0; })) { fail(EERROR_INVALID,"CPIO pathname is not NUL terminated"); }
            std::string path(name.begin(),terminator); padding(); available(header.size);
            if (path=="TRAILER!!!") {
                if (header.size || header.check) { fail(EERROR_INVALID,"nonempty CPIO trailer"); }
                used=position;
                if (device->size()-position>options.maximumTrailingPaddingBytes) { fail(EERROR_LIMIT,"CPIO trailing padding exceeds policy; use a bounded device view"); }
                while (position<device->size()) {
                    const auto tail=take(static_cast<std::size_t>(std::min<std::uint64_t>(65536,device->size()-position)));
                    if (std::any_of(tail.begin(),tail.end(),[](auto b) { return b!=0; })) { fail(EERROR_INVALID,"nonzero data after CPIO trailer; concatenated images are unsupported"); }
                }
                ended=true; return std::nullopt;
            }
            if (header.name>options.maximumPathBytes || nameBytes>options.maximumNameBytes || header.name>options.maximumNameBytes-nameBytes || entries>=options.maximumEntries) { fail(EERROR_LIMIT,"CPIO metadata exceeds policy"); }
            if (totalData>options.maximumDataBytes || header.size>options.maximumDataBytes-totalData) { fail(EERROR_LIMIT,"CPIO file bytes exceed policy"); }
            SEntry entry; entry.path=canonical(std::move(path)); entry.size=header.size;
            const auto type=header.mode&0170000;
            if (type==0100000) { entry.kind=ENODE_FILE; if (header.links!=1) { fail(EERROR_UNSUPPORTED,"CPIO hard-linked regular files are not implemented"); } }
            else if (type==0040000) { entry.kind=ENODE_DIRECTORY; if (header.size) { fail(EERROR_INVALID,"nonempty CPIO directory payload"); } }
            else { fail(EERROR_UNSUPPORTED,"CPIO symbolic links and special records are not implemented"); }
            if (entry.path.empty() && entry.kind!=ENODE_DIRECTORY) { fail(EERROR_INVALID,"CPIO root must be a directory"); }
            entry.permissions=static_cast<std::uint32_t>(header.mode&07777); entry.uid=static_cast<std::uint32_t>(header.uid);
            entry.gid=static_cast<std::uint32_t>(header.gid); entry.modifiedTime=header.time;
            ++entries; nameBytes+=header.name; totalData+=header.size;
            remaining=header.size; payloadOffset=position; expected=static_cast<std::uint32_t>(header.check); checksum=0; haveEntry=true; finalized=false;
            if (!remaining) { complete(); } return entry;
        }
    };
    std::unique_ptr<Impl> _impl;
public:
    CpioReader(std::shared_ptr<IBlockDevice> device,SOpenOptions options) : _impl(std::make_unique<Impl>(std::move(device),options)) {}
    EFormat format() const noexcept override { return _impl->format; }
    std::optional<SEntry> next() override {
        std::lock_guard lock(_impl->mutex); _impl->valid();
        try { return _impl->next(); } catch (...) { _impl->failed=true; throw; }
    }
    std::size_t read(SByteSpan output) override {
        std::lock_guard lock(_impl->mutex); _impl->valid();
        try { return _impl->read(output); } catch (...) { _impl->failed=true; throw; }
    }
    std::uint64_t usedBytes() const override {
        std::lock_guard lock(_impl->mutex); _impl->valid(); if (!_impl->ended) { fail(EERROR_STATE,"scan to the trailer before querying image length"); } return _impl->used;
    }
    std::uint64_t payloadOffset() const {
        std::lock_guard lock(_impl->mutex); _impl->valid(); if (!_impl->haveEntry) { fail(EERROR_STATE,"no current payload"); } return _impl->payloadOffset;
    }
};

class CpioFile final : public IFileReader {
private:
    std::shared_ptr<IBlockDevice> _device;
    std::uint64_t _offset,_size;
public:
    CpioFile(std::shared_ptr<IBlockDevice> device,std::uint64_t offset,std::uint64_t size) : _device(std::move(device)),_offset(offset),_size(size) {}
    std::uint64_t size() const override { return _size; }
    std::size_t readAt(std::uint64_t offset,SByteSpan output) const override {
        if (offset>=_size || output.empty()) { return 0; }
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),_size-offset));
        _device->readAt(_offset+offset,output.first(count)); return count;
    }
};

/** Indexing is a separate, immutable view; the native cursor remains sequential. */
class CpioTree final : public ITreeReader {
private:
    struct Record { SEntry info; std::uint64_t offset=0; bool implicit=false; };
    struct Impl {
        std::shared_ptr<IBlockDevice> device;
        EFormat format;
        std::map<std::string,Record> records;
        SOpenOptions options;
        std::uint64_t names=0;

        Impl(std::shared_ptr<IBlockDevice> source,SOpenOptions policy) : device(std::move(source)),format(IFileSystem::probe(*device).format),options(policy) {
            add({"",ENODE_DIRECTORY,0,0755},0,true);
            auto cursor=std::make_shared<CpioReader>(device,options);
            while (auto entry=cursor->next()) {
                for (std::size_t at=entry->path.find('/');at!=std::string::npos;at=entry->path.find('/',at+1)) {
                    const auto parent=entry->path.substr(0,at); const auto found=records.find(parent);
                    if (found==records.end()) { add({parent,ENODE_DIRECTORY,0,0755},0,true); }
                    else if (found->second.info.kind!=ENODE_DIRECTORY) { fail(EERROR_INVALID,"CPIO parent path is not a directory"); }
                }
                const auto offset=cursor->payloadOffset(); auto found=records.find(entry->path);
                if (found!=records.end()) {
                    if (!found->second.implicit || entry->kind!=ENODE_DIRECTORY) { fail(EERROR_INVALID,"ambiguous duplicate CPIO pathname in tree adapter"); }
                    found->second.info=std::move(*entry); found->second.implicit=false;
                } else { add(std::move(*entry),offset,false); }
            }
        }
        void add(SEntry entry,std::uint64_t offset,bool implicit) {
            // --> Count synthesized ancestors too; a deep filename must not evade index limits.
            if (records.size()>=options.maximumEntries || names>options.maximumNameBytes || entry.path.size()>options.maximumNameBytes-names) { fail(EERROR_LIMIT,"archive tree index exceeds metadata policy"); }
            names+=entry.path.size(); auto key=entry.path; records.emplace(std::move(key),Record{std::move(entry),offset,implicit});
        }
        static std::string path(const std::string& path) {
            if (path.empty() || path=="/") { return {}; }
            return canonical(path.front()=='/'?path.substr(1):path);
        }
        const Record& find(const std::string& name) const {
            const auto found=records.find(path(name)); if (found==records.end()) { fail(EERROR_NOT_FOUND,"archive path not found"); } return found->second;
        }
    };
    std::unique_ptr<Impl> _impl;
public:
    CpioTree(std::shared_ptr<IBlockDevice> device,SOpenOptions options) : _impl(std::make_unique<Impl>(std::move(device),options)) {}
    EFormat format() const noexcept override { return _impl->format; }
    SEntry stat(const std::string& path) const override { return _impl->find(path).info; }
    std::vector<SEntry> list(const std::string& directory,std::uint64_t offset,std::size_t count) const override {
        if (count>4096) { fail(EERROR_LIMIT,"directory page exceeds 4096 entries"); }
        const auto& record=_impl->find(directory);
        if (record.info.kind!=ENODE_DIRECTORY) { fail(EERROR_NOT_DIRECTORY,"archive path is not a directory"); }
        std::vector<SEntry> output; if (!count) { return output; }
        const auto prefix=record.info.path.empty()?std::string{}:record.info.path+"/";
        for (auto it=_impl->records.lower_bound(prefix);it!=_impl->records.end();++it) {
            const auto& path=it->first;
            if (!path.starts_with(prefix)) { break; }
            if (path==record.info.path || path.find('/',prefix.size())!=std::string::npos) { continue; }
            if (offset) { --offset; continue; }
            output.push_back(it->second.info); if (output.size()==count) { break; }
        }
        return output;
    }
    std::shared_ptr<IFileReader> openFile(const std::string& path) const override {
        const auto& record=_impl->find(path);
        if (record.info.kind!=ENODE_FILE) { fail(EERROR_IS_DIRECTORY,"archive path is not a regular file"); }
        return std::make_shared<CpioFile>(_impl->device,record.offset,record.info.size);
    }
};

/** Sequential builder may backpatch CRC header bytes, but never buffers whole files. */
class CpioBuilder final : public IArchiveBuilder {
private:
    struct Impl {
        std::shared_ptr<IBlockDevice> device;
        SArchiveCreateOptions options;
        std::uint64_t position=0,remaining=0,entries=0,totalData=0,nameBytes=0,headerOffset=0;
        std::uint32_t checksum=0;
        bool haveEntry=false,finished=false,failed=false;
        mutable std::mutex mutex;

        Impl(std::shared_ptr<IBlockDevice> target,SArchiveCreateOptions policy) : device(std::move(target)),options(policy) {
            const auto trailer=headerSize(options.format)+11;
            if (device->size()<trailer+(alignment(options.format)-trailer%alignment(options.format))%alignment(options.format)) { fail(EERROR_RANGE,"device cannot hold a CPIO trailer"); }
            if (!options.allowOverwrite) {
                for (std::uint64_t at=0;at<device->size();) {
                    const auto bytes=device->readBytes(at,static_cast<std::size_t>(std::min<std::uint64_t>(65536,device->size()-at)));
                    if (std::any_of(bytes.begin(),bytes.end(),[](auto b) { return b!=0; })) { fail(EERROR_STATE,"creation requires an empty device or explicit allowOverwrite"); }
                    at+=bytes.size();
                }
            }
        }
        void valid() const { if (failed || finished) { fail(EERROR_STATE,"archive builder is finished or failed"); } }
        void append(SReadOnlyByteSpan bytes) { device->writeAt(position,bytes); position+=bytes.size(); }
        void padding() {
            const std::array<std::uint8_t,3> zeros{};
            append(SReadOnlyByteSpan(zeros).first(static_cast<std::size_t>((alignment(options.format)-position%alignment(options.format))%alignment(options.format))));
        }
        void complete() {
            if (options.format==EFORMAT_CPIO_CRC) { const auto field=digits(checksum,8,16); device->writeAt(headerOffset+102,field); }
            padding();
        }
        void begin(SEntry entry) {
            valid(); if (remaining) { fail(EERROR_STATE,"finish the current payload before beginning another entry"); }
            if (entry.kind!=ENODE_FILE && entry.kind!=ENODE_DIRECTORY) { fail(EERROR_UNSUPPORTED,"CPIO builder supports regular files and directories only"); }
            entry.path=canonical(entry.path.empty()?".":std::move(entry.path));
            if (entry.path.empty() && entry.kind!=ENODE_DIRECTORY) { fail(EERROR_INVALID,"root entry must be a directory"); }
            if (entry.path=="TRAILER!!!" || entry.permissions>07777 || (entry.kind==ENODE_DIRECTORY && entry.size)) { fail(EERROR_INVALID,"invalid CPIO entry metadata"); }
            auto name=entry.path.empty()?std::string("."):entry.path; name.push_back('\0');
            const auto& limits=options.limits;
            if (name.size()>limits.maximumPathBytes || nameBytes>limits.maximumNameBytes || name.size()>limits.maximumNameBytes-nameBytes || entries>=limits.maximumEntries || totalData>limits.maximumDataBytes || entry.size>limits.maximumDataBytes-totalData) { fail(EERROR_LIMIT,"CPIO creation exceeds policy"); }
            auto header=encodeHeader(options.format,entry,entries+1,name.size());
            // --> Check all arithmetic before the first write of this entry.
            auto end=position; const auto a=alignment(options.format);
            auto advance=[&](std::uint64_t size) { range(device->size(),end,size); end+=size; };
            advance(header.size()+name.size()); advance((a-end%a)%a); advance(entry.size); advance((a-end%a)%a);
            const auto trailer=headerSize(options.format)+11; advance(trailer); advance((a-end%a)%a);
            try {
                headerOffset=position; append(header); append({reinterpret_cast<const std::uint8_t*>(name.data()),name.size()}); padding();
                haveEntry=true; remaining=entry.size; checksum=0; ++entries; nameBytes+=name.size(); totalData+=entry.size;
                if (!remaining) { complete(); }
            } catch (...) { failed=true; throw; }
        }
        void write(SReadOnlyByteSpan bytes) {
            valid(); if (!haveEntry || bytes.size()>remaining) { fail(EERROR_STATE,"payload exceeds or precedes the current entry"); }
            if (bytes.empty()) { return; }
            try {
                append(bytes); remaining-=bytes.size();
                if (options.format==EFORMAT_CPIO_CRC) { for (auto byte:bytes) { checksum+=byte; } }
                if (!remaining) { complete(); }
            } catch (...) { failed=true; throw; }
        }
        void finish() {
            if (finished) { return; } valid(); if (remaining) { fail(EERROR_STATE,"incomplete CPIO entry payload"); }
            try {
                SEntry trailer; const auto header=encodeHeader(options.format,trailer,0,11,true); append(header);
                constexpr std::array<std::uint8_t,11> name{'T','R','A','I','L','E','R','!','!','!',0}; append(name); padding();
                if (options.allowOverwrite) {
                    const std::array<std::uint8_t,65536> zeros{};
                    for (auto at=position;at<device->size();) { const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(zeros.size(),device->size()-at)); device->writeAt(at,SReadOnlyByteSpan(zeros).first(count)); at+=count; }
                }
                device->flush(); finished=true;
            } catch (...) { failed=true; throw; }
        }
    };
    std::unique_ptr<Impl> _impl;
public:
    CpioBuilder(std::shared_ptr<IBlockDevice> device,SArchiveCreateOptions options) : _impl(std::make_unique<Impl>(std::move(device),options)) {}
    EFormat format() const noexcept override { return _impl->options.format; }
    void beginEntry(SEntry entry) override { std::lock_guard lock(_impl->mutex); _impl->begin(std::move(entry)); }
    void write(SReadOnlyByteSpan bytes) override { std::lock_guard lock(_impl->mutex); _impl->write(bytes); }
    void finish() override { std::lock_guard lock(_impl->mutex); _impl->finish(); }
    std::uint64_t usedBytes() const override { std::lock_guard lock(_impl->mutex); return _impl->position; }
};

} // namespace

std::shared_ptr<IArchiveReader> openCpio(std::shared_ptr<IBlockDevice> device,SOpenOptions options) { return std::make_shared<CpioReader>(std::move(device),options); }
std::shared_ptr<IArchiveBuilder> createCpio(std::shared_ptr<IBlockDevice> device,SArchiveCreateOptions options) { return std::make_shared<CpioBuilder>(std::move(device),options); }
std::shared_ptr<ITreeReader> indexCpio(std::shared_ptr<IBlockDevice> device,SOpenOptions options) { return std::make_shared<CpioTree>(std::move(device),options); }

} // namespace detail
} // namespace hnfs
