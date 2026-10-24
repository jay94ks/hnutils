#include "multiplexed_internal.hpp"
#include "transport_lease.hpp"
#include <asyncpp/launch.h>
#include <asyncpp/mutex.h>

namespace hnhttp::detail::mux {
namespace {
constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
struct SFailure { ERetCode code; std::uint64_t protocol; };
[[noreturn]] void bad(ERetCode code = ERET_MALFORMED, std::uint64_t protocol = 1) { throw SFailure{code,protocol}; }
std::uint32_t read32(const SBytes& b, std::size_t at = 0) {
    return (static_cast<std::uint32_t>(b[at])<<24) | (static_cast<std::uint32_t>(b[at+1])<<16) | (static_cast<std::uint32_t>(b[at+2])<<8) | b[at+3];
}
void u32(SBytes& b, std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) { b.push_back(static_cast<std::uint8_t>(value>>shift)); } }
struct SFrame { std::uint8_t type, flags; std::uint32_t id; SBytes payload; };
struct SFlow { std::int64_t send = 65535, receive = 65535; bool localEnd = false, remoteEnd = false, opened = false; };
struct SWait { asyncpp::multi_consumer_event event; bool done = false; };
class CHttp2Wire final : public IWire {
private:
    std::weak_ptr<SConnection> _owner;
    std::shared_ptr<IHttpStream> _stream;
    std::shared_ptr<IHeaderCodec> _decoder, _encoder;
    asyncpp::mutex _send;
    asyncpp::multi_consumer_event _controlChanged;
    std::deque<SFrame> _control;
    std::deque<std::shared_ptr<SWait>> _settingsAcks;
    std::map<std::array<std::uint8_t,8>,std::shared_ptr<SWait>> _pings;
    std::map<std::uint32_t,SFlow> _flows;
    std::map<std::uint64_t,std::uint64_t> _settings;
    std::uint32_t _next = 1, _peerFrame = 16384, _peerWindow = 65535, _peerConcurrent = UINT32_MAX;
    std::uint64_t _peerHeaderList = UINT64_MAX;
    std::int64_t _sendWindow = 65535, _receiveWindow = 65535;
    bool _firstSettings = false, _encoderShrink = false;
    std::size_t _activeSenders = 0;
    asyncpp::multi_consumer_event _sendersDone{true};
    std::shared_ptr<SConnection> owner() const { return _owner.lock(); }
    void prune(const std::shared_ptr<SConnection>& s) {
        std::erase_if(s->streams, [&](const auto& pair) { if (pair.second.expired()) { _flows.erase(static_cast<std::uint32_t>(pair.first)); return true; } return false; });
        std::erase_if(_flows,[&](const auto& pair) { return !s->streams.contains(pair.first); });
    }
    void queue(SFrame frame) {
        auto s = owner(); if (!s || s->closed) { return; }
        if (frame.type == 8) {
            for (auto& pending : _control) { if (pending.type == 8 && pending.id == frame.id) {
                const auto a = read32(pending.payload), b = read32(frame.payload); if (b <= 0x7fffffff-a) { pending.payload.clear(); u32(pending.payload,a+b); return; }
            } }
        }
        if (_control.size() >= s->options.limits.maximumPendingControlFrames) { fail(s,ERET_LIMIT,11); return; }
        _control.push_back(std::move(frame)); _controlChanged.set();
    }
    void resetId(std::uint32_t id, std::uint32_t code) { SBytes bytes; u32(bytes,code); queue({3,0,id,std::move(bytes)}); }
    asyncpp::task<void> raw(std::shared_ptr<SConnection> s, SBytes bytes) {
        std::size_t position = 0;
        while (position < bytes.size()) {
            if (s->closed) { bad(ERET_CLOSED,0); }
            const auto n = (std::min)(s->options.limits.ioChunkBytes,bytes.size()-position);
            SBytes chunk(bytes.begin()+static_cast<std::ptrdiff_t>(position),bytes.begin()+static_cast<std::ptrdiff_t>(position+n));
            auto result = co_await _stream->writeSome(std::move(chunk));
            if (result.result.code != ERET_OK || !result.acceptedBytes || result.acceptedBytes > n) { bad(result.result.code == ERET_OK ? ERET_IO : result.result.code,2); }
            position += result.acceptedBytes; co_await budget(s,result.acceptedBytes);
        }
    }
    asyncpp::task<void> frameUnlocked(std::shared_ptr<SConnection> s, SFrame f) {
        SBytes wire{static_cast<std::uint8_t>(f.payload.size()>>16),static_cast<std::uint8_t>(f.payload.size()>>8),static_cast<std::uint8_t>(f.payload.size()),f.type,f.flags};
        u32(wire,f.id); wire.insert(wire.end(),f.payload.begin(),f.payload.end()); co_await raw(std::move(s),std::move(wire));
    }
    asyncpp::task<void> emit(std::shared_ptr<SConnection> s, SFrame frame) {
        auto lock = co_await _send.lock_scoped(); co_await frameUnlocked(std::move(s),std::move(frame));
    }
    asyncpp::task<SBytes> exact(std::shared_ptr<SConnection> s, std::size_t n, bool allowEof = false) {
        SBytes result; result.reserve(n);
        while (result.size() < n) {
            const auto maximum = (std::min)(n-result.size(),s->options.limits.ioChunkBytes);
            auto read = co_await _stream->readSome(maximum);
            if (read.result.code == ERET_EOF) { bad(result.empty() && allowEof ? ERET_EOF : ERET_TRUNCATED,0); }
            if (read.result.code != ERET_OK || read.bytes.empty() || read.bytes.size() > maximum) { bad(read.result.code == ERET_OK ? ERET_IO : read.result.code,2); }
            result.insert(result.end(),read.bytes.begin(),read.bytes.end()); co_await budget(s,read.bytes.size());
        }
        co_return result;
    }
    asyncpp::task<SFrame> next(std::shared_ptr<SConnection> s) {
        auto header = co_await exact(s,9,true); const auto n = (static_cast<std::size_t>(header[0])<<16) | (static_cast<std::size_t>(header[1])<<8) | header[2];
        if (n > s->options.http2.maximumFrameSize) { bad(ERET_MALFORMED,6); }
        SFrame f{header[3],header[4],read32(header,5)&0x7fffffff,{}};
        if (n) { f.payload = co_await exact(s,n); } co_return f;
    }
    asyncpp::task<void> controls(std::shared_ptr<SConnection> s) {
        ERetCode code = ERET_OK; std::uint64_t protocol = 0;
        try {
            while (!s->closed) {
                if (_control.empty()) { _controlChanged.reset(); co_await _controlChanged; continue; }
                auto f = std::move(_control.front()); _control.pop_front(); co_await emit(s,std::move(f));
            }
        } catch (const SFailure& failure) { code = failure.code; protocol = failure.protocol; }
        catch (...) { code = ERET_IO; protocol = 2; }
        if (code != ERET_OK && !s->closed) { fail(s,code,protocol); }
    }
    void setting(std::shared_ptr<SConnection> s, std::uint16_t id, std::uint32_t value) {
        switch (id) {
        case 1: if (value < 4096) { _encoderShrink = true; } break;
        case 2: if (s->role == HTTP_CLIENT || value > 1) { bad(); } break;
        case 3: _peerConcurrent = value; break;
        case 4: {
            if (value > 0x7fffffff) { bad(ERET_MALFORMED,3); }
            const auto delta = static_cast<std::int64_t>(value)-_peerWindow;
            for (auto& [stream,flow] : _flows) { (void)stream; if (flow.send+delta > 0x7fffffff) { bad(ERET_MALFORMED,3); } flow.send += delta; }
            _peerWindow = value; break;
        }
        case 5: if (value < 16384 || value > 16777215) { bad(); } _peerFrame = value; break;
        case 6: _peerHeaderList = value; break;
        default: break;
        }
        if (!_settings.contains(id) && _settings.size() >= s->options.limits.maximumPeerSettings) { bad(ERET_LIMIT,11); }
        _settings[id] = value;
    }
    asyncpp::task<void> receive(std::shared_ptr<SConnection> s, SFrame f) {
        prune(s);
        if (!_firstSettings && (f.type != 4 || (f.flags & 1))) { bad(); }
        switch (f.type) {
        case 4: {
            if (f.id) { bad(); }
            if (f.flags & 1) {
                if (!f.payload.empty()) { bad(ERET_MALFORMED,6); }
                if (_settingsAcks.empty()) { bad(); } auto waiter = _settingsAcks.front(); _settingsAcks.pop_front();
                if (waiter) { waiter->done = true; waiter->event.set(); }
            } else {
                if (f.payload.size()%6) { bad(ERET_MALFORMED,6); }
                if (f.payload.size()/6 > s->options.limits.maximumPeerSettings) { bad(ERET_LIMIT,11); }
                for (std::size_t p = 0; p < f.payload.size(); p += 6) { setting(s,static_cast<std::uint16_t>((f.payload[p]<<8)|f.payload[p+1]),read32(f.payload,p+2)); }
                _firstSettings = true; queue({4,1,0,{}}); s->ready = true; s->changed.set();
            }
            break;
        }
        case 1: {
            if (!f.id) { bad(); }
            const auto headerId = f.id;
            auto found = s->streams.find(f.id); auto e = found == s->streams.end() ? nullptr : found->second.lock();
            bool rejected = false;
            if (!e) {
                if (s->role != HTTP_SERVER || !(f.id&1)) { bad(); }
                if (f.id <= s->highestPeer) { rejected = true; }
                else {
                    s->highestPeer = f.id; std::size_t active = 0;
                    for (const auto& [id,weak] : s->streams) { (void)id; if (auto x = weak.lock(); x && x->failure.code == ERET_OK && (!x->receivedEnd || !x->sentEnd)) { ++active; } }
                    rejected = s->draining || active >= s->options.http2.maximumConcurrentStreams;
                    if (!rejected) { e = incoming(s,f.id); _flows[f.id] = {static_cast<std::int64_t>(_peerWindow),static_cast<std::int64_t>(s->options.http2.initialStreamWindow),false,false,true}; }
                }
            }
            std::size_t begin = 0, padding = 0;
            if (f.flags & 8) { if (f.payload.empty()) { bad(); } padding = f.payload[begin++]; }
            if (f.flags & 32) {
                if (f.payload.size()-begin < 5) { bad(ERET_MALFORMED,6); }
                if ((read32(f.payload,begin)&0x7fffffff) == f.id) { if (e) { streamFail(e,ERET_MALFORMED,1); } resetId(f.id,1); }
                begin += 5;
            }
            if (padding > f.payload.size()-begin) { bad(); }
            SBytes block(f.payload.begin()+static_cast<std::ptrdiff_t>(begin),f.payload.end()-static_cast<std::ptrdiff_t>(padding));
            if (block.size() > s->options.http2.maximumEncodedHeaderBytes) { bad(ERET_LIMIT,11); }
            const bool end = (f.flags&1) != 0; std::size_t continuations = 0;
            while (!(f.flags&4)) {
                if (++continuations > s->options.http2.maximumContinuationFrames) { bad(ERET_LIMIT,11); }
                f = co_await next(s); if (f.type != 9 || f.id != headerId) { bad(); }
                // --> Rejected streams still consume their compression block to preserve HPACK state.
                if (f.payload.size() > s->options.http2.maximumEncodedHeaderBytes-block.size()) { bad(ERET_LIMIT,11); }
                block.insert(block.end(),f.payload.begin(),f.payload.end());
            }
            if (block.size() > s->options.http2.maximumEncodedHeaderBytes) { bad(ERET_LIMIT,11); }
            auto decoded = _decoder->decode(block); if (decoded.result.code != ERET_OK) { bad(decoded.result.code,9); }
            if (rejected || !e) { resetId(f.id,7); break; }
            if (e->failure.code != ERET_OK) { break; }
            if (++e->frames > s->options.limits.maximumMessageFrames) { streamFail(e,ERET_LIMIT,11); resetId(f.id,11); break; }
            const auto result = receiveHeaders(e,std::move(decoded.fields),end);
            if (result != ERET_OK) { streamFail(e,result,result == ERET_LIMIT ? 7 : 1); resetId(f.id,result == ERET_LIMIT ? 7 : 1); }
            if (end) { _flows[f.id].remoteEnd = true; } break;
        }
        case 0: {
            if (!f.id) { bad(); }
            if (static_cast<std::int64_t>(f.payload.size()) > _receiveWindow) { bad(ERET_MALFORMED,3); }
            _receiveWindow -= static_cast<std::int64_t>(f.payload.size());
            auto found = s->streams.find(f.id); auto e = found == s->streams.end() ? nullptr : found->second.lock();
            if (!e) { if ((s->role == HTTP_SERVER && f.id > s->highestPeer) || (s->role == HTTP_CLIENT && f.id >= _next)) { bad(); }
                _receiveWindow += static_cast<std::int64_t>(f.payload.size()); SBytes credit; u32(credit,static_cast<std::uint32_t>(f.payload.size())); if (!f.payload.empty()) { queue({8,0,0,std::move(credit)}); } resetId(f.id,5); break;
            }
            auto& flow = _flows[f.id]; flow.receive -= static_cast<std::int64_t>(f.payload.size());
            if (flow.receive < 0) { consumed(e,f.payload.size()); streamFail(e,ERET_MALFORMED,3); resetId(f.id,3); break; }
            std::size_t start = 0, padding = 0;
            if (f.flags&8) { if (f.payload.empty()) { bad(); } padding = f.payload[start++]; if (padding > f.payload.size()-start) { bad(); } }
            SBytes bytes(f.payload.begin()+static_cast<std::ptrdiff_t>(start),f.payload.end()-static_cast<std::ptrdiff_t>(padding));
            if (start+padding) { consumed(e,start+padding); }
            if (e->failure.code != ERET_OK) { consumed(e,bytes.size()); break; }
            if (flow.remoteEnd) { consumed(e,bytes.size()); streamFail(e,ERET_STREAM_RESET,5); resetId(f.id,5); break; }
            auto result = ++e->frames > s->options.limits.maximumMessageFrames ? ERET_LIMIT : receiveData(e,bytes,(f.flags&1) != 0);
            if (result != ERET_OK) { consumed(e,bytes.size()); streamFail(e,result,result == ERET_LIMIT ? 11 : 1); resetId(f.id,result == ERET_LIMIT ? 11 : 1); }
            if (f.flags&1) { flow.remoteEnd = true; } break;
        }
        case 8: {
            if (f.payload.size() != 4) { bad(ERET_MALFORMED,6); } const auto delta = read32(f.payload)&0x7fffffff;
            if (!f.id) { if (!delta || _sendWindow+delta > 0x7fffffff) { bad(ERET_MALFORMED,!delta ? 1 : 3); } _sendWindow += delta; }
            else {
                if (auto it = _flows.find(f.id); it != _flows.end()) {
                    if (!delta || it->second.send+delta > 0x7fffffff) { auto e = s->streams[f.id].lock(); if (e) { streamFail(e,ERET_MALFORMED,!delta ? 1 : 3); resetId(f.id,!delta ? 1 : 3); } }
                    else { it->second.send += delta; }
                } else if ((s->role == HTTP_CLIENT && f.id >= _next) || (s->role == HTTP_SERVER && f.id > s->highestPeer)) { bad(); }
            }
            s->changed.set(); break;
        }
        case 3: {
            if (!f.id) { bad(); } if (f.payload.size() != 4) { bad(ERET_MALFORMED,6); }
            auto found = s->streams.find(f.id); auto e = found == s->streams.end() ? nullptr : found->second.lock();
            if (e) { streamFail(e,ERET_STREAM_RESET,read32(f.payload)); if (read32(f.payload) == 7) { e->failure.retry = ERETRY_NOT_PROCESSED; } }
            else if ((s->role == HTTP_CLIENT && f.id >= _next) || (s->role == HTTP_SERVER && f.id > s->highestPeer)) { bad(); }
            break;
        }
        case 6: {
            if (f.id) { bad(); } if (f.payload.size() != 8) { bad(ERET_MALFORMED,6); }
            std::array<std::uint8_t,8> key; std::copy(f.payload.begin(),f.payload.end(),key.begin());
            if (f.flags&1) { if (auto it = _pings.find(key); it != _pings.end()) { it->second->done = true; it->second->event.set(); _pings.erase(it); } }
            else { queue({6,1,0,std::move(f.payload)}); } break;
        }
        case 7: {
            if (f.id) { bad(); } if (f.payload.size() < 8) { bad(ERET_MALFORMED,6); }
            const auto boundary = read32(f.payload)&0x7fffffff, code = read32(f.payload,4);
            if (s->goaway && boundary > s->goaway->boundary) { bad(); }
            s->goaway = SGoawayInfo{EGOAWAY_H2_LAST_PROCESSED_STREAM,boundary,code};
            if (s->role == HTTP_CLIENT) { for (const auto& [id,weak] : s->streams) { if (id > boundary) { if (auto e = weak.lock()) { streamFail(e,ERET_GOAWAY,code); e->failure.retry = ERETRY_NOT_PROCESSED; } } } }
            s->changed.set(); break;
        }
        case 2: if (!f.id) { bad(); } if (f.payload.size() != 5) { bad(ERET_MALFORMED,6); }
            if ((read32(f.payload)&0x7fffffff) == f.id) { resetId(f.id,1); if (auto it = s->streams.find(f.id); it != s->streams.end()) { if (auto e = it->second.lock()) { streamFail(e,ERET_MALFORMED,1); } } } break;
        case 5: case 9: bad();
        default: break;
        }
        co_await budget(s,f.payload.size());
    }
    static SBytes localSettings(const SHttpConnectionOptions& o, EHttpRole role) {
        SBytes bytes;
        auto add = [&](std::uint16_t id, std::uint32_t value) { bytes.push_back(static_cast<std::uint8_t>(id>>8)); bytes.push_back(static_cast<std::uint8_t>(id)); u32(bytes,value); };
        add(1,o.http2.headerTableSize); if (role == HTTP_CLIENT) { add(2,0); }
        add(3,o.http2.maximumConcurrentStreams); add(4,o.http2.initialStreamWindow); add(5,o.http2.maximumFrameSize); add(6,o.http2.maximumHeaderListSize); return bytes;
    }
public:
    CHttp2Wire(std::shared_ptr<SConnection> s, std::shared_ptr<IHttpStream> stream) : _owner(s),_stream(std::move(stream)) {
        SHeaderCodecOptions o; o.maximumDynamicTableBytes = s->options.http2.headerTableSize;
        o.maximumDecodedBytes = (std::min)(s->options.limits.maximumHeaderBytes,static_cast<std::size_t>(s->options.http2.maximumHeaderListSize));
        o.maximumFields = s->options.limits.maximumHeaderFields; o.maximumEncodedBytes = s->options.http2.maximumEncodedHeaderBytes;
        _decoder = IHeaderCodec::createHpack(o); _encoder = IHeaderCodec::createHpack(o);
    }
    asyncpp::task<ERetCode> run(std::shared_ptr<SConnection> s) override {
        ERetCode result = ERET_OK; std::uint64_t protocol = 0;
        asyncpp::async_launch_scope tasks;
        try {
            if (s->role == HTTP_CLIENT) { auto lock = co_await _send.lock_scoped(); co_await raw(s,SBytes(preface.begin(),preface.end())); }
            _settingsAcks.push_back({}); queue({4,0,0,localSettings(s->options,s->role)}); tasks.launch(controls(s));
            if (s->role == HTTP_SERVER) { auto bytes = co_await exact(s,preface.size()); if (!std::equal(bytes.begin(),bytes.end(),preface.begin())) { bad(); } }
            while (!s->closed) { co_await receive(s,co_await next(s)); }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol; }
        catch (...) { result = ERET_IO; protocol = 2; }
        if (!s->closed) { fail(s,result == ERET_EOF ? ERET_CLOSED : result,protocol); }
        _controlChanged.set(); co_await tasks.join(); co_await _sendersDone; co_return s->failure.code;
    }
    asyncpp::task<ERetCode> open(std::shared_ptr<SExchange> e) override {
        auto s = e->c; if (s->closed) { co_return ERET_CLOSED; }
        if (s->draining || s->goaway || _next > 0x7fffffff) { co_return ERET_GOAWAY; }
        prune(s); std::size_t active = 0;
        for (const auto& [id,weak] : s->streams) { (void)id; if (auto x = weak.lock(); x && x->failure.code == ERET_OK && (!x->sentEnd || !x->receivedEnd)) { ++active; } }
        if (active >= _peerConcurrent) { co_return ERET_LIMIT; }
        e->id = _next; _next += 2; s->streams[*e->id] = e;
        _flows[static_cast<std::uint32_t>(*e->id)] = {static_cast<std::int64_t>(_peerWindow),static_cast<std::int64_t>(s->options.http2.initialStreamWindow),false,false}; co_return ERET_OK;
    }
    asyncpp::task<ERetCode> headers(std::shared_ptr<SExchange> e, std::vector<SHeaderField> fields, bool end) override {
        auto s = e->c; ++_activeSenders; _sendersDone.reset();
        struct Guard { CHttp2Wire& self; ~Guard() { if (!--self._activeSenders) { self._sendersDone.set(); } } } guard{*this};
        ERetCode result = ERET_OK; std::uint64_t protocol = 2;
        try {
            std::size_t bytes = 0; for (const auto& f : fields) { bytes += f.name.size()+f.value.size()+32; }
            if (bytes > _peerHeaderList) { co_return ERET_LIMIT; }
            auto lock = co_await _send.lock_scoped(); if (e->failure.code != ERET_OK || s->closed) { co_return ERET_CLOSED; }
            auto encoded = _encoder->encode(fields); if (encoded.result.code != ERET_OK) { co_return encoded.result.code; }
            if (_encoderShrink) {
                if (encoded.bytes.size() == s->options.http2.maximumEncodedHeaderBytes) { co_return ERET_LIMIT; }
                encoded.bytes.insert(encoded.bytes.begin(),0x20); _encoderShrink = false;
            }
            std::size_t at = 0; bool first = true;
            do {
                const auto count = (std::min)(static_cast<std::size_t>(_peerFrame),encoded.bytes.size()-at);
                const auto last = at+count == encoded.bytes.size();
                SBytes block(encoded.bytes.begin()+static_cast<std::ptrdiff_t>(at),encoded.bytes.begin()+static_cast<std::ptrdiff_t>(at+count));
                co_await frameUnlocked(s,{static_cast<std::uint8_t>(first ? 1 : 9),static_cast<std::uint8_t>((last ? 4 : 0) | (first && end ? 1 : 0)),static_cast<std::uint32_t>(*e->id),std::move(block)});
                if (first) { _flows[static_cast<std::uint32_t>(*e->id)].opened = true; }
                at += count; first = false;
            } while (at < encoded.bytes.size());
            if (end) { _flows[static_cast<std::uint32_t>(*e->id)].localEnd = true; }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol; }
        catch (...) { result = ERET_IO; }
        if (result != ERET_OK) { fail(s,result,protocol); } co_return result;
    }
    asyncpp::task<ERetCode> data(std::shared_ptr<SExchange> e, SBytes bytes, bool end) override {
        auto s = e->c; ++_activeSenders; _sendersDone.reset();
        struct Guard { CHttp2Wire& self; ~Guard() { if (!--self._activeSenders) { self._sendersDone.set(); } } } guard{*this};
        ERetCode result = ERET_OK; std::uint64_t protocol = 2;
        try {
            std::size_t at = 0;
            do {
                auto& flow = _flows.at(static_cast<std::uint32_t>(*e->id));
                while (at < bytes.size() && (_sendWindow <= 0 || flow.send <= 0) && !s->closed && e->failure.code == ERET_OK) { s->changed.reset(); co_await s->changed; }
                if (s->closed || e->failure.code != ERET_OK) { co_return e->failure.code == ERET_OK ? ERET_CLOSED : e->failure.code; }
                const auto count = (std::min)({bytes.size()-at,static_cast<std::size_t>(_peerFrame),static_cast<std::size_t>((std::max)(INT64_C(0),(std::min)(_sendWindow,flow.send)))});
                _sendWindow -= static_cast<std::int64_t>(count); flow.send -= static_cast<std::int64_t>(count);
                SBytes part(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+count)); at += count;
                co_await emit(s,{0,static_cast<std::uint8_t>(end && at == bytes.size() ? 1 : 0),static_cast<std::uint32_t>(*e->id),std::move(part)});
            } while (at < bytes.size());
            if (end) { _flows[static_cast<std::uint32_t>(*e->id)].localEnd = true; }
        } catch (const SFailure& failure) { result = failure.code; protocol = failure.protocol; }
        catch (...) { result = ERET_IO; }
        if (result != ERET_OK) { fail(s,result,protocol); } co_return result;
    }
    void consumed(const std::shared_ptr<SExchange>& e, std::size_t n) override {
        if (!n || e->c->closed) { return; } _receiveWindow += static_cast<std::int64_t>(n);
        SBytes connection; u32(connection,static_cast<std::uint32_t>(n)); queue({8,0,0,std::move(connection)});
        if (auto it = _flows.find(static_cast<std::uint32_t>(*e->id)); it != _flows.end()) {
            it->second.receive += static_cast<std::int64_t>(n);
            if (!it->second.remoteEnd) { SBytes stream; u32(stream,static_cast<std::uint32_t>(n)); queue({8,0,static_cast<std::uint32_t>(*e->id),std::move(stream)}); }
        }
    }
    void reset(const std::shared_ptr<SExchange>& e, std::uint64_t code) override {
        if (e->id) { if (auto it = _flows.find(static_cast<std::uint32_t>(*e->id)); it != _flows.end() && it->second.opened) { resetId(static_cast<std::uint32_t>(*e->id),static_cast<std::uint32_t>(code)); } }
        e->c->changed.set();
    }
    void stop(std::uint64_t) noexcept override {
        _stream->close(); _controlChanged.set();
        for (const auto& waiter : _settingsAcks) { if (waiter) { waiter->event.set(); } }
        for (const auto& [key,waiter] : _pings) { (void)key; waiter->event.set(); }
    }
    asyncpp::task<ERetCode> join(SOperationOptions o) override { co_await _sendersDone; co_return co_await _stream->waitClosed(o); }
    asyncpp::task<ERetCode> goaway(std::shared_ptr<SConnection> s) override {
        ERetCode result = ERET_OK;
        try { SBytes bytes; u32(bytes,static_cast<std::uint32_t>(s->highestPeer)); u32(bytes,0); co_await emit(s,{7,0,0,std::move(bytes)}); }
        catch (...) { result = ERET_IO; } co_return result;
    }
    STransportSecurity security() const override { return _stream->security(); }
    std::vector<SProtocolSetting> settings() const override { std::vector<SProtocolSetting> result; for (const auto& [id,value] : _settings) { result.push_back({id,value}); } return result; }
    asyncpp::task<ERetCode> update(std::vector<SProtocolSetting> values, SOperationOptions o) override {
        auto s = owner(); auto p = entry(s,o); if (p != ERET_OK) { co_return p; }
        if (!s->running || !s->ready) { co_return ERET_NOT_RUNNING; }
        if (_settingsAcks.size() >= s->options.http2.maximumOutstandingSettings || values.size() > s->options.limits.maximumPeerSettings) { co_return ERET_LIMIT; }
        SBytes bytes;
        for (const auto& value : values) {
            // --> Limits that alter live decoder/window state require a separate policy transition.
            if (value.identifier != 3 && value.identifier != 6) { co_return ERET_UNSUPPORTED; }
            if (value.value > (value.identifier == 3 ? s->options.http2.maximumConcurrentStreams : s->options.http2.maximumHeaderListSize)) { co_return ERET_INVALID_ARGUMENT; }
            bytes.push_back(0); bytes.push_back(static_cast<std::uint8_t>(value.identifier)); u32(bytes,static_cast<std::uint32_t>(value.value));
        }
        auto waiter = std::make_shared<SWait>(); _settingsAcks.push_back(waiter); queue({4,0,0,std::move(bytes)});
        co_await waiter->event; co_return waiter->done ? ERET_OK : ERET_CLOSED;
    }
    asyncpp::task<ERetCode> ping(std::array<std::uint8_t,8> key, SOperationOptions o) override {
        auto s = owner(); auto p = entry(s,o); if (p != ERET_OK) { co_return p; }
        if (!s->running || !s->ready) { co_return ERET_NOT_RUNNING; }
        if (_pings.contains(key)) { co_return ERET_BUSY; } if (_pings.size() >= s->options.http2.maximumOutstandingPings) { co_return ERET_LIMIT; }
        auto waiter = std::make_shared<SWait>(); _pings[key] = waiter; queue({6,0,0,SBytes(key.begin(),key.end())});
        co_await waiter->event; co_return waiter->done ? ERET_OK : ERET_CLOSED;
    }
    SHttp2FlowState flow(std::optional<std::uint32_t> id) const override { SHttp2FlowState result; result.connectionSendWindow = _sendWindow;
        if (id) { if (auto it = _flows.find(*id); it != _flows.end()) { result.streamSendWindow = it->second.send; } } return result;
    }
};
}
std::shared_ptr<IHttpConnection> h2(std::shared_ptr<IHttpStream> stream, EHttpRole role, SHttpConnectionOptions options) {
    validate(options); if (!stream || !stream->executor() || (role != HTTP_CLIENT && role != HTTP_SERVER)) { throw std::invalid_argument("invalid HTTP/2 transport"); }
    require(stream->executor()); const auto security = stream->security();
    if (security.tls && security.alpn != "h2") { throw std::invalid_argument("HTTP/2 requires h2 ALPN on TLS"); }
    auto s = std::make_shared<SConnection>(); s->options = options; s->role = role; s->executor = stream->executor();
    claimTransport(stream,s);
    s->wire = std::make_shared<CHttp2Wire>(s,std::move(stream)); return facade(s);
}
}
