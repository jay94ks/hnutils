#include "multiplexed_internal.hpp"
#include <hnhttp/qpack.hpp>
#include <asyncpp/launch.h>
#include <set>

namespace hnhttp::detail::mux {
namespace {
constexpr auto maximum = (UINT64_C(1)<<62)-1;
struct SFailure { ERetCode code; std::uint64_t protocol; };
[[noreturn]] void bad(ERetCode code = ERET_MALFORMED, std::uint64_t protocol = 0x106) { throw SFailure{code,protocol}; }
void varint(SBytes& bytes, std::uint64_t value) {
    if (value > maximum) { bad(ERET_INVALID_ARGUMENT); }
    const unsigned n = value < 64 ? 1 : value < 16384 ? 2 : value < (UINT64_C(1)<<30) ? 4 : 8;
    const auto start = bytes.size(); for (unsigned shift = n*8; shift; shift -= 8) { bytes.push_back(static_cast<std::uint8_t>(value>>(shift-8))); }
    bytes[start] |= static_cast<std::uint8_t>(n == 1 ? 0 : n == 2 ? 0x40 : n == 4 ? 0x80 : 0xc0);
}
std::uint64_t readVarint(const SBytes& bytes, std::size_t& at) {
    if (at == bytes.size()) { bad(); } const unsigned n = 1u<<(bytes[at]>>6);
    if (bytes.size()-at < n) { bad(); } std::uint64_t value = bytes[at++]&63;
    for (unsigned i = 1; i < n; ++i) { value = (value<<8)|bytes[at++]; } return value;
}
SBytes frame(std::uint64_t type, SBytes payload) { SBytes result; varint(result,type); varint(result,payload.size()); result.insert(result.end(),payload.begin(),payload.end()); return result; }
struct SOutput { bool encoder; SBytes bytes; };
class CHttp3Wire final : public IWire {
private:
    std::weak_ptr<SConnection> _owner;
    std::unique_ptr<IHttpQuicTransport> _transport;
    std::shared_ptr<IHttpQuicStream> _control, _encoder, _decoder;
    std::shared_ptr<IQpackCodec> _codec;
    asyncpp::async_launch_scope* _tasks = nullptr;
    asyncpp::mutex _controlSend;
    asyncpp::multi_consumer_event _outputChanged, _compressionChanged, _sendersDone{true};
    std::deque<SOutput> _output;
    std::size_t _outputBytes = 0, _blockedBytes = 0, _pumps = 0, _senders = 0;
    bool _controlSeen = false, _encoderSeen = false, _decoderSeen = false, _settingsSeen = false;
    std::uint64_t _peerFieldLimit = maximum, _capacity = 0;
    std::optional<std::uint64_t> _maximumPushId;
    std::map<std::uint64_t,std::uint64_t> _settings;
    std::shared_ptr<SConnection> owner() const { return _owner.lock(); }
    void queue(bool encoder, SBytes bytes) {
        auto s = owner(); if (bytes.empty() || !s || s->closed) { return; }
        if (_output.size() >= s->options.limits.maximumPendingControlFrames || bytes.size() > s->options.limits.maximumConnectionBufferedBytes-_outputBytes) { fail(s,ERET_LIMIT,0x107); return; }
        _outputBytes += bytes.size(); _output.push_back({encoder,std::move(bytes)}); _outputChanged.set();
    }
    void feedback() { queue(false,_codec->takeDecoderInstructions()); }
    asyncpp::task<void> write(std::shared_ptr<SConnection> s, IAsyncByteChannel& channel, SBytes bytes) {
        std::size_t at = 0;
        while (at < bytes.size()) {
            if (s->closed) { bad(ERET_CLOSED,0x100); }
            const auto count = (std::min)(s->options.limits.ioChunkBytes,bytes.size()-at);
            SBytes part(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+count));
            auto result = co_await channel.writeSome(std::move(part));
            if (result.result.code != ERET_OK || !result.acceptedBytes || result.acceptedBytes > count) { bad(result.result.code == ERET_OK ? ERET_IO : result.result.code,0x102); }
            at += result.acceptedBytes; co_await budget(s,result.acceptedBytes);
        }
    }
    asyncpp::task<SBytes> exact(std::shared_ptr<SConnection> s, IAsyncByteChannel& channel, std::size_t n) {
        SBytes bytes;
        while (bytes.size() < n) {
            const auto count = (std::min)(s->options.limits.ioChunkBytes,n-bytes.size()); auto result = co_await channel.readSome(count);
            if (result.result.code == ERET_EOF) { bad(bytes.empty() ? ERET_EOF : ERET_TRUNCATED); }
            if (result.result.code != ERET_OK || result.bytes.empty() || result.bytes.size() > count) { bad(result.result.code == ERET_OK ? ERET_IO : result.result.code,0x102); }
            bytes.insert(bytes.end(),result.bytes.begin(),result.bytes.end()); co_await budget(s,result.bytes.size());
        }
        co_return bytes;
    }
    asyncpp::task<std::uint64_t> number(std::shared_ptr<SConnection> s, IAsyncByteChannel& channel) {
        auto first = co_await exact(s,channel,1); const unsigned n = 1u<<(first[0]>>6); std::uint64_t value = first[0]&63;
        if (n > 1) {
            try { auto rest = co_await exact(s,channel,n-1); for (const auto c : rest) { value = (value<<8)|c; } }
            catch (const SFailure& failure) { if (failure.code == ERET_EOF) { bad(ERET_TRUNCATED); } throw; }
        } co_return value;
    }
    asyncpp::task<void> skip(std::shared_ptr<SConnection> s, IAsyncByteChannel& channel, std::uint64_t count) {
        while (count) { const auto n = static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(s->options.limits.ioChunkBytes),count)); (void)co_await exact(s,channel,n); count -= n; }
    }
    asyncpp::task<void> outputs(std::shared_ptr<SConnection> s) {
        ERetCode result = ERET_OK; std::uint64_t protocol = 0;
        try {
            while (!s->closed) {
                if (_output.empty()) { _outputChanged.reset(); co_await _outputChanged; continue; }
                auto output = std::move(_output.front()); _output.pop_front(); _outputBytes -= output.bytes.size();
                co_await write(s,output.encoder ? *_encoder : *_decoder,std::move(output.bytes));
            }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol; }
        catch (...) { result = ERET_IO; protocol = 0x102; }
        if (result != ERET_OK && !s->closed) { fail(s,result,protocol); }
    }
    void settingsFrame(std::shared_ptr<SConnection> s, const SBytes& bytes) {
        if (_settingsSeen) { bad(ERET_MALFORMED,0x105); }
        std::size_t at = 0; std::uint64_t capacity = 0, blocked = 0;
        while (at < bytes.size()) {
            const auto id = readVarint(bytes,at), value = readVarint(bytes,at);
            if ((id >= 2 && id <= 5) || _settings.contains(id)) { bad(ERET_MALFORMED,0x109); }
            if (_settings.size() >= s->options.limits.maximumPeerSettings) { bad(ERET_LIMIT,0x107); } _settings[id] = value;
            if (id == 1) { capacity = value; } else if (id == 7) { blocked = value; } else if (id == 6) { _peerFieldLimit = value; }
        }
        if (_codec->configurePeer(capacity,blocked) != ERET_OK) { bad(ERET_MALFORMED,0x109); }
        _capacity = (std::min)(capacity,s->options.http3.qpackMaximumTableCapacity);
        if (_capacity) { auto instruction = _codec->setEncoderCapacity(static_cast<std::size_t>(_capacity)); if (instruction.result.code != ERET_OK) { bad(instruction.result.code,0x201); } queue(true,std::move(instruction.bytes)); }
        _settingsSeen = true; s->ready = true; s->changed.set();
    }
    asyncpp::task<void> control(std::shared_ptr<SConnection> s, IHttpQuicStream& channel) {
        while (!s->closed) {
            const auto type = co_await number(s,channel); const auto count = co_await number(s,channel);
            if (!_settingsSeen && type != 4) { bad(ERET_MALFORMED,0x10a); }
            if (count > s->options.http3.maximumControlFrameBytes) { bad(ERET_LIMIT,0x107); }
            if (type == 0 || type == 1 || type == 2 || type == 5 || type == 6 || type == 8 || type == 9) { bad(ERET_MALFORMED,0x105); }
            if (type == 4) { settingsFrame(s,co_await exact(s,channel,static_cast<std::size_t>(count))); }
            else if (type == 7) {
                auto payload = co_await exact(s,channel,static_cast<std::size_t>(count)); std::size_t at = 0; const auto boundary = readVarint(payload,at);
                if (at != payload.size() || (s->role == HTTP_CLIENT && (boundary&3)) || (s->goaway && boundary > s->goaway->boundary)) { bad(ERET_MALFORMED,0x108); }
                s->goaway = SGoawayInfo{s->role == HTTP_CLIENT ? EGOAWAY_H3_FIRST_REJECTED_REQUEST_STREAM : EGOAWAY_H3_FIRST_REJECTED_PUSH_ID,boundary,{}};
                if (s->role == HTTP_CLIENT) { for (const auto& [id,weak] : s->streams) { if (id >= boundary) { if (auto e = weak.lock()) { streamFail(e,ERET_GOAWAY,0x10b); e->failure.retry = ERETRY_NOT_PROCESSED; e->quic->abortRead(0x10b); e->quic->abortWrite(0x10b); } } } }
                s->changed.set();
            } else if (type == 3) { bad(ERET_MALFORMED,0x108); }
            else if (type == 13) {
                if (s->role != HTTP_SERVER) { bad(ERET_MALFORMED,0x105); }
                auto payload = co_await exact(s,channel,static_cast<std::size_t>(count)); std::size_t at = 0; const auto id = readVarint(payload,at);
                if (at != payload.size()) { bad(); } if (_maximumPushId && id < *_maximumPushId) { bad(ERET_MALFORMED,0x108); } _maximumPushId = id;
            } else { co_await skip(s,channel,count); }
            co_await budget(s,static_cast<std::size_t>(count));
        }
    }
    asyncpp::task<void> unidirectional(std::shared_ptr<SConnection> s, std::shared_ptr<IHttpQuicStream> channel) {
        struct Guard { std::size_t& n; ~Guard() { --n; } } guard{_pumps};
        ERetCode result = ERET_OK; std::uint64_t protocol = 0; bool critical = false;
        try {
            const auto type = co_await number(s,*channel);
            if (type == 0) { critical = true; if (_controlSeen) { bad(ERET_MALFORMED,0x103); } _controlSeen = true; co_await control(s,*channel); }
            else if (type == 2 || type == 3) {
                critical = true; auto& seen = type == 2 ? _encoderSeen : _decoderSeen; if (seen) { bad(ERET_MALFORMED,0x103); } seen = true;
                while (!s->closed) {
                    auto bytes = co_await channel->readSome(s->options.limits.ioChunkBytes);
                    if (bytes.result.code == ERET_EOF) { bad(ERET_EOF,0x104); }
                    if (bytes.result.code != ERET_OK || bytes.bytes.empty() || bytes.bytes.size() > s->options.limits.ioChunkBytes) { bad(ERET_IO,0x102); }
                    const auto decoded = type == 2 ? _codec->receiveEncoderInstructions(bytes.bytes) : _codec->receiveDecoderInstructions(bytes.bytes);
                    if (decoded.code != ERET_OK) { bad(decoded.code,type == 2 ? 0x201 : 0x202); }
                    if (type == 2) { feedback(); _compressionChanged.set(); } co_await budget(s,bytes.bytes.size());
                }
            } else if (type == 1) { bad(ERET_MALFORMED,0x108); }
            else {
                std::uint64_t count = 0;
                while (!s->closed) { auto bytes = co_await channel->readSome(s->options.limits.ioChunkBytes);
                    if (bytes.result.code == ERET_EOF) { break; } if (bytes.result.code != ERET_OK || bytes.bytes.empty()) { break; }
                    if (bytes.bytes.size() > s->options.http3.maximumUnknownStreamBytes-count) { channel->abortRead(0x100); break; }
                    count += bytes.bytes.size(); co_await budget(s,bytes.bytes.size());
                }
            }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol; if (critical && (result == ERET_EOF || result == ERET_STREAM_RESET)) { protocol = 0x104; } }
        catch (...) { result = ERET_IO; protocol = 0x102; }
        if (result != ERET_OK && !s->closed && !(result == ERET_EOF && !critical)) { fail(s,result,protocol); }
    }
    asyncpp::task<void> request(std::shared_ptr<SConnection> s, std::shared_ptr<SExchange> e) {
        struct Guard { std::size_t& n; std::shared_ptr<SConnection> s; ~Guard() { --n; s->changed.set(); } } guard{_pumps,s};
        ERetCode result = ERET_OK; std::uint64_t protocol = 0; std::uint64_t unknown = 0; bool atBoundary = true;
        try {
            while (!s->closed && e->failure.code == ERET_OK) {
                atBoundary = true; const auto type = co_await number(s,*e->quic); atBoundary = false; const auto count = co_await number(s,*e->quic);
                if (++e->frames > s->options.limits.maximumMessageFrames) { bad(ERET_LIMIT,0x107); }
                if (type == 1) {
                    if (e->receivedTrailers || e->receivedEnd) { bad(ERET_MALFORMED,0x105); }
                    if (count > s->options.http3.maximumEncodedFieldSectionBytes) { bad(ERET_LIMIT,0x107); }
                    auto block = co_await exact(s,*e->quic,static_cast<std::size_t>(count)); auto decoded = _codec->decodeSection(*e->id,block);
                    if (decoded.result.code == ERET_BLOCKED) {
                        if (block.size() > s->options.http3.maximumBlockedHeaderBytes-_blockedBytes) { bad(ERET_LIMIT,0x107); }
                        _blockedBytes += block.size(); struct Blocked { std::size_t& all; std::size_t n; ~Blocked() { all -= n; } } blocked{_blockedBytes,block.size()};
                        while (decoded.result.code == ERET_BLOCKED && !s->closed && e->failure.code == ERET_OK) {
                            _compressionChanged.reset(); co_await _compressionChanged; decoded = _codec->decodeSection(*e->id,block);
                        }
                    }
                    if (s->closed || e->failure.code != ERET_OK) { break; }
                    if (decoded.result.code != ERET_OK) { fail(s,decoded.result.code,0x200); break; }
                    feedback(); result = receiveHeaders(e,std::move(decoded.fields),false);
                    if (result != ERET_OK) { bad(result,0x10e); }
                } else if (type == 0) {
                    std::uint64_t left = count;
                    if (!e->head || e->receivedTrailers || e->receivedEnd) { bad(ERET_MALFORMED,0x105); }
                    if (e->noBody && count) { bad(ERET_MALFORMED,0x10e); }
                    while (left && !s->closed && e->failure.code == ERET_OK) {
                        while ((e->buffered == s->options.limits.maximumStreamBufferedBytes || s->buffered+s->reservedReads == s->options.limits.maximumConnectionBufferedBytes) && !s->closed && e->failure.code == ERET_OK) { s->changed.reset(); co_await s->changed; }
                        if (s->closed || e->failure.code != ERET_OK) { break; }
                        // --> Reserve before suspension. A slow stream receives only a fair share,
                        // not the entire connection budget while other native reads are pending.
                        const auto quota = (std::max)(std::size_t{1},s->options.limits.maximumConnectionBufferedBytes/s->options.limits.maximumPendingRequests);
                        const auto n = static_cast<std::size_t>((std::min)({left,static_cast<std::uint64_t>(s->options.limits.ioChunkBytes),static_cast<std::uint64_t>(quota),static_cast<std::uint64_t>(s->options.limits.maximumStreamBufferedBytes-e->buffered),static_cast<std::uint64_t>(s->options.limits.maximumConnectionBufferedBytes-s->buffered-s->reservedReads)}));
                        SReadResult chunk;
                        {
                            s->reservedReads += n;
                            struct Reservation { SConnection& state; std::size_t bytes; ~Reservation() { state.reservedReads -= bytes; state.changed.set(); } } reservation{*s,n};
                            chunk = co_await e->quic->readSome(n);
                            if (chunk.result.code == ERET_EOF) { bad(ERET_TRUNCATED,0x106); }
                            if (chunk.result.code != ERET_OK) { bad(chunk.result.code,0x102); }
                            if (chunk.bytes.empty() || chunk.bytes.size() > n) { bad(ERET_IO,0x102); }
                        }
                        left -= chunk.bytes.size(); const auto delivered = chunk.bytes.size(); result = receiveData(e,std::move(chunk.bytes),false);
                        if (result != ERET_OK) { bad(result,result == ERET_LIMIT ? 0x107 : 0x10e); } co_await budget(s,delivered);
                    }
                } else if (type == 2 || type == 3 || type == 4 || type == 6 || type == 7 || type == 8 || type == 9 || type == 13) { bad(ERET_MALFORMED,0x105); }
                else if (type == 5) { bad(ERET_MALFORMED,s->role == HTTP_SERVER ? 0x105 : 0x108); }
                else { if (count > s->options.http3.maximumUnknownStreamBytes-unknown) { bad(ERET_LIMIT,0x107); } unknown += count; co_await skip(s,*e->quic,count); }
                co_await budget(s,static_cast<std::size_t>(count));
            }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol;
            if (result == ERET_EOF && atBoundary && e->head) { result = receiveData(e,{},true); protocol = 0x10e; }
            else if (result == ERET_EOF) { result = ERET_TRUNCATED; }
        } catch (...) { result = ERET_IO; protocol = 0x102; }
        if (result != ERET_OK && !s->closed && e->failure.code == ERET_OK) {
            if (protocol == 0x105 || protocol == 0x108 || protocol == 0x103) { fail(s,result,protocol); }
            else { reset(e,protocol); streamFail(e,result,protocol); }
        }
    }
    asyncpp::task<std::shared_ptr<IHttpQuicStream>> local(std::shared_ptr<SConnection> s, std::uint64_t type) {
        auto opened = co_await _transport->openStream(EQUIC_UNIDIRECTIONAL);
        if (opened.result.code != ERET_OK || !opened.stream) { bad(opened.result.code == ERET_OK ? ERET_IO : opened.result.code,0x103); }
        if (opened.stream->executor() != s->executor || opened.stream->kind() != EQUIC_UNIDIRECTIONAL || opened.stream->initiator() != s->role || opened.stream->id() > maximum
            || (opened.stream->id()&3) != static_cast<std::uint64_t>(s->role == HTTP_CLIENT ? 2 : 3)) { bad(ERET_INVALID_ARGUMENT,0x103); }
        std::shared_ptr<IHttpQuicStream> channel(std::move(opened.stream)); SBytes bytes; varint(bytes,type); co_await write(s,*channel,std::move(bytes)); co_return channel;
    }
public:
    CHttp3Wire(std::shared_ptr<SConnection> s, std::unique_ptr<IHttpQuicTransport> transport) : _owner(s),_transport(std::move(transport)) {
        SQpackOptions options; options.codec.maximumDynamicTableBytes = static_cast<std::size_t>(s->options.http3.qpackMaximumTableCapacity);
        options.codec.maximumDecodedBytes = (std::min)(s->options.limits.maximumHeaderBytes,static_cast<std::size_t>(s->options.http3.maximumFieldSectionSize));
        options.codec.maximumEncodedBytes = s->options.http3.maximumEncodedFieldSectionBytes; options.codec.maximumFields = s->options.limits.maximumHeaderFields;
        options.maximumBlockedStreams = static_cast<std::size_t>(s->options.http3.qpackBlockedStreams); options.maximumOutstandingSections = s->options.limits.maximumPendingRequests*2;
        _codec = IQpackCodec::create(options);
    }
    asyncpp::task<ERetCode> run(std::shared_ptr<SConnection> s) override {
        asyncpp::async_launch_scope tasks; _tasks = &tasks; ERetCode result = ERET_OK; std::uint64_t protocol = 0;
        try {
            _control = co_await local(s,0); _encoder = co_await local(s,2); _decoder = co_await local(s,3);
            SBytes settings; varint(settings,1); varint(settings,s->options.http3.qpackMaximumTableCapacity);
            varint(settings,7); varint(settings,s->options.http3.qpackBlockedStreams); varint(settings,6); varint(settings,s->options.http3.maximumFieldSectionSize);
            co_await write(s,*_control,frame(4,std::move(settings))); tasks.launch(outputs(s));
            while (!s->closed) {
                auto accepted = co_await _transport->acceptStream();
                if (accepted.result.code != ERET_OK || !accepted.stream) { bad(accepted.result.code == ERET_OK ? ERET_IO : accepted.result.code,0x102); }
                auto& channel = accepted.stream;
                if (channel->executor() != s->executor || channel->initiator() == s->role || channel->id() > maximum
                    || ((channel->id()&1) != static_cast<std::uint64_t>(channel->initiator() == HTTP_SERVER))
                    || (((channel->id()>>1)&1) != static_cast<std::uint64_t>(channel->kind() == EQUIC_UNIDIRECTIONAL))) { bad(ERET_INVALID_ARGUMENT,0x103); }
                if (_pumps >= s->options.limits.maximumPendingRequests+3) { bad(ERET_LIMIT,0x107); }
                if (channel->kind() == EQUIC_UNIDIRECTIONAL) { ++_pumps; tasks.launch(unidirectional(s,std::shared_ptr<IHttpQuicStream>(std::move(channel)))); }
                else {
                    if (s->role != HTTP_SERVER || (channel->id()&3)) { bad(ERET_MALFORMED,0x103); }
                    if (s->draining) { channel->abortRead(0x10b); channel->abortWrite(0x10b); continue; }
                    const auto id = channel->id(); if (auto found = s->streams.find(id); found != s->streams.end() && !found->second.expired()) { bad(ERET_INVALID_ARGUMENT,0x103); }
                    s->highestPeer = (std::max)(s->highestPeer,id); s->hasPeerStream = true; auto e = incoming(s,id); e->quic = std::move(channel); ++_pumps; tasks.launch(request(s,e));
                }
                std::erase_if(s->streams,[](const auto& pair) { return pair.second.expired(); }); co_await budget(s,0);
            }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol; }
        catch (...) { result = ERET_IO; protocol = 0x102; }
        if (!s->closed) { fail(s,result,protocol); } _outputChanged.set(); _compressionChanged.set();
        co_await tasks.join(); co_await _sendersDone; _tasks = nullptr; co_return s->failure.code;
    }
    asyncpp::task<ERetCode> open(std::shared_ptr<SExchange> e) override {
        auto s = e->c; if (s->closed) { co_return ERET_CLOSED; } if (s->draining || s->goaway) { co_return ERET_GOAWAY; }
        if (!_tasks || !s->ready) { co_return ERET_NOT_RUNNING; }
        if (_pumps >= s->options.limits.maximumPendingRequests+3) { co_return ERET_LIMIT; }
        auto opened = co_await _transport->openStream(EQUIC_BIDIRECTIONAL);
        if (opened.result.code != ERET_OK || !opened.stream) { co_return opened.result.code == ERET_OK ? ERET_IO : opened.result.code; }
        if (opened.stream->executor() != s->executor || opened.stream->kind() != EQUIC_BIDIRECTIONAL || opened.stream->initiator() != HTTP_CLIENT
            || opened.stream->id() > maximum || (opened.stream->id()&3)) { co_return ERET_INVALID_ARGUMENT; }
        if (s->closed) { opened.stream->abortRead(0x10c); opened.stream->abortWrite(0x10c); co_return ERET_CLOSED; }
        e->id = opened.stream->id(); e->quic = std::move(opened.stream); s->streams[*e->id] = e; ++_pumps; _tasks->launch(request(s,e)); co_return ERET_OK;
    }
    asyncpp::task<ERetCode> headers(std::shared_ptr<SExchange> e, std::vector<SHeaderField> fields, bool end) override {
        auto s = e->c; ++_senders; _sendersDone.reset(); struct Guard { CHttp3Wire& self; ~Guard() { if (!--self._senders) { self._sendersDone.set(); } } } guard{*this};
        ERetCode result = ERET_OK;
        try {
            auto lock = co_await e->sendMutex.lock_scoped(); if (s->closed || e->failure.code != ERET_OK) { co_return ERET_CLOSED; }
            std::size_t bytes = 0; for (const auto& field : fields) { bytes += field.name.size()+field.value.size()+32; }
            if (bytes > _peerFieldLimit) { co_return ERET_LIMIT; }
            if (_capacity) { for (const auto& field : fields) {
                if (!field.name.starts_with(':') && field.name != "authorization" && field.name != "proxy-authorization" && field.name != "cookie" && field.name != "set-cookie" && field.name != "password" && field.indexing != EINDEX_NEVER) {
                    auto inserted = _codec->insert(field); if (inserted.result.code == ERET_OK) { queue(true,std::move(inserted.bytes)); }
                }
            } }
            auto encoded = _codec->encodeSection(*e->id,fields,true); if (encoded.result.code != ERET_OK) { co_return encoded.result.code; }
            co_await write(s,*e->quic,frame(1,std::move(encoded.bytes))); if (end) { result = co_await e->quic->finishWrite(); }
        } catch (const SFailure& failure) { result = failure.code; } catch (...) { result = ERET_IO; }
        co_return result;
    }
    asyncpp::task<ERetCode> data(std::shared_ptr<SExchange> e, SBytes bytes, bool end) override {
        auto s = e->c; ++_senders; _sendersDone.reset(); struct Guard { CHttp3Wire& self; ~Guard() { if (!--self._senders) { self._sendersDone.set(); } } } guard{*this};
        ERetCode result = ERET_OK;
        try { auto lock = co_await e->sendMutex.lock_scoped();
            if (!bytes.empty()) { co_await write(s,*e->quic,frame(0,std::move(bytes))); } if (end) { result = co_await e->quic->finishWrite(); }
        } catch (const SFailure& failure) { result = failure.code; } catch (...) { result = ERET_IO; } co_return result;
    }
    void consumed(const std::shared_ptr<SExchange>& e, std::size_t) override { e->c->changed.set(); }
    void reset(const std::shared_ptr<SExchange>& e, std::uint64_t code) override {
        if (e->quic) { e->quic->abortRead(code); e->quic->abortWrite(code); }
        if (e->id) { auto instruction = _codec->cancelStream(*e->id); if (instruction.result.code == ERET_OK) { queue(false,std::move(instruction.bytes)); } }
        _compressionChanged.set(); e->c->changed.set();
    }
    void stop(std::uint64_t code) noexcept override { _transport->close(code ? code : 0x100); _outputChanged.set(); _compressionChanged.set(); }
    asyncpp::task<ERetCode> join(SOperationOptions o) override { co_await _sendersDone; co_return co_await _transport->waitClosed(o); }
    asyncpp::task<ERetCode> goaway(std::shared_ptr<SConnection> s) override {
        ERetCode result = ERET_OK;
        try { auto lock = co_await _controlSend.lock_scoped(); SBytes bytes;
            varint(bytes,s->role == HTTP_SERVER && s->hasPeerStream ? s->highestPeer+4 : 0); co_await write(s,*_control,frame(7,std::move(bytes)));
        } catch (...) { result = ERET_IO; } co_return result;
    }
    STransportSecurity security() const override { return _transport->security(); }
    std::vector<SProtocolSetting> settings() const override { std::vector<SProtocolSetting> result; for (const auto& [id,value] : _settings) { result.push_back({id,value}); } return result; }
    asyncpp::task<ERetCode> update(std::vector<SProtocolSetting>, SOperationOptions) override { co_return ERET_UNSUPPORTED; }
    asyncpp::task<ERetCode> ping(std::array<std::uint8_t,8>, SOperationOptions) override { co_return ERET_UNSUPPORTED; }
    SHttp2FlowState flow(std::optional<std::uint32_t>) const override { return {}; }
};
}
std::shared_ptr<IHttpConnection> h3(std::unique_ptr<IHttpQuicTransport> transport, EHttpRole role, SHttpConnectionOptions options) {
    validate(options); if (!transport || !transport->executor() || (role != HTTP_CLIENT && role != HTTP_SERVER)) { throw std::invalid_argument("invalid HTTP/3 transport"); }
    require(transport->executor()); const auto security = transport->security();
    if (!transport->handshakeConfirmed() || !security.tls || security.alpn != "h3" || security.peerAuthentication == EPEER_TRANSPORT_REJECTED
        || (role == HTTP_CLIENT && security.peerAuthentication != EPEER_TRANSPORT_ACCEPTED)) { throw std::invalid_argument("HTTP/3 requires confirmed authenticated h3 transport"); }
    auto s = std::make_shared<SConnection>(); s->options = options; s->role = role; s->executor = transport->executor();
    s->wire = std::make_shared<CHttp3Wire>(s,std::move(transport)); return facade(s);
}
}
