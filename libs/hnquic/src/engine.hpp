#ifndef __SRC_HNQUIC_ENGINE_HPP__
#define __SRC_HNQUIC_ENGINE_HPP__
#include "wire.hpp"
#include <asyncpp/event.h>
#include <atomic>
#include <functional>

namespace hnquic {
namespace detail {
/** Private QUIC v1 state machine. Clock and datagrams are supplied by the reactor owner. */
class Engine {
  private:
    SQuicOptions _options;
    std::shared_ptr<hntls::ITlsContext> _context;
    std::shared_ptr<hntls::ITlsSession> _tls;
    std::shared_ptr<asyncpp::dispatcher> _executor;
    std::array<Space, 3> _spaces;
    std::map<std::uint64_t, Stream> _streams;
    std::deque<std::uint64_t> _accepted;
    std::deque<SBytes> _deferred;
    SBytes _localCid, _remoteCid, _originalCid;
    EConnectionState _state = ESTATE_IDLE;
    SQuicError _error{ERET_OK, {}, {}, {}, {}};
    bool _confirmed = false, _peerParameters = false, _addressValidated = false;
    Time _activity{}, _recoveryStart{}, _lastSent{};
    std::chrono::microseconds _latestRtt{333000}, _smoothedRtt{333000}, _rttVariance{166500},
        _minimumRtt{std::chrono::microseconds::max()};
    std::uint64_t _receivedBytes = 0, _sentBytes = 0, _flightBytes = 0, _cwnd = 12000,
                  _threshold = UINT64_MAX;
    std::uint64_t _peerMaxData = 0, _sentData = 0, _receiveData = 0, _receiveLimit = 0, _consumedData = 0;
    std::uint64_t _peerStreamBidiLocal = 0, _peerStreamBidiRemote = 0, _peerStreamUni = 0, _peerBidi = 0,
                  _peerUni = 0, _openedBidi = 0, _openedUni = 0;
    unsigned _ackExponent = 3, _ptoCount = 0, _probes = 0;
    std::chrono::microseconds _maxAckDelay{25000};
    std::array<std::uint64_t, 3> _receiveFloor{};
    std::array<bool, 3> _discarded{};
    bool _rttMeasured = false;

    void discard(unsigned level) {
        auto &space = _spaces[level];
        for (auto &[pn, sent] : space.sent) {
            _flightBytes -= sent.bytes;
        }
        space.sent.clear();
        space.pending.clear();
        space.received.clear();
        space.crypto = Reassembly{};
        space.ack = false;
        space.read.clear();
        space.write.clear();
        _discarded[level] = true;
    }

    void bounded() const {
        std::size_t count = _deferred.size(), bytes = 0;
        for (auto &p : _deferred) {
            need(p.size() <= _options.maximumBufferedBytes - bytes, "deferred packet byte limit", 13);
            bytes += p.size();
        }
        for (auto &space : _spaces) {
            count += space.sent.size() + space.pending.size();
            for (auto &p : space.pending) {
                need(p.size() <= _options.maximumBufferedBytes - bytes, "pending packet byte limit", 13);
                bytes += p.size();
            }
            for (auto &[pn, p] : space.sent) {
                need(p.frames.size() <= _options.maximumBufferedBytes - bytes, "recovery byte limit", 13);
                bytes += p.frames.size();
            }
            need(space.crypto.buffered() <= _options.maximumBufferedBytes - bytes, "CRYPTO buffer limit", 13);
            bytes += space.crypto.buffered();
        }
        for (auto &[id, s] : _streams) {
            need(s.receive.buffered() <= _options.maximumBufferedBytes - bytes, "stream buffer limit", 13);
            bytes += s.receive.buffered();
        }
        need(count <= _options.maximumPendingDatagrams, "pending datagram count limit", 13);
    }
    void queue(unsigned level, SBytes frame) {
        need(frame.size() <= 1000, "frame exceeds packet budget", 1);
        _spaces[level].pending.push_back(std::move(frame));
        bounded();
        changed.set();
    }
    void parameter(SBytes &out, std::uint64_t id, std::uint64_t value) {
        SBytes data;
        varint(data, value);
        varint(out, id);
        varint(out, data.size());
        append(out, data);
    }
    void parameter(SBytes &out, std::uint64_t id, View data) {
        varint(out, id);
        varint(out, data.size());
        append(out, data);
    }
    SBytes parameters() {
        SBytes p;
        parameter(p, 15, _localCid);
        if (_options.role == EROLE_SERVER) {
            parameter(p, 0, _originalCid);
        }
        parameter(p, 1, _options.idleTimeout.count());
        parameter(p, 3, 1200);
        parameter(p, 4, _options.maximumConnectionData);
        parameter(p, 5, _options.maximumStreamData);
        parameter(p, 6, _options.maximumStreamData);
        parameter(p, 7, _options.maximumStreamData);
        parameter(p, 8, _options.maximumBidirectionalStreams);
        parameter(p, 9, _options.maximumUnidirectionalStreams);
        parameter(p, 12, View{});
        parameter(p, 14, 2);
        return p;
    }
    void peerParameters() {
        auto p = _tls->peerInfo().quicTransportParameters;
        Reader r(p);
        std::set<std::uint64_t> seen;
        bool initial = false, original = false;
        while (r.size()) {
            auto id = r.integer();
            auto v = r.take(r.integer());
            need(seen.insert(id).second, "duplicate transport parameter", 8);
            Reader value(v);
            if (id == 15) {
                need(std::equal(v.begin(), v.end(), _remoteCid.begin(), _remoteCid.end()),
                     "initial source CID mismatch", 8);
                initial = true;
                continue;
            }
            if (id == 0) {
                need(_options.role == EROLE_CLIENT &&
                         std::equal(v.begin(), v.end(), _originalCid.begin(), _originalCid.end()),
                     "original destination CID mismatch", 8);
                original = true;
                continue;
            }
            if (id == 12) {
                need(v.empty(), "invalid disable_active_migration", 8);
                continue;
            }
            if (id == 2 || id == 13 || id == 16) {
                need(_options.role == EROLE_CLIENT, "server-only transport parameter", 8);
                if (id == 2) {
                    need(v.size() == 16, "invalid stateless reset token", 8);
                }
                if (id == 16) {
                    need(v.size() <= 20, "invalid retry source CID", 8);
                }
                continue;
            }
            if (id == 1 || id == 3 || id == 4 || id == 5 || id == 6 || id == 7 || id == 8 || id == 9 ||
                id == 10 || id == 11 || id == 14) {
                auto n = value.integer();
                need(!value.size(), "invalid numeric transport parameter", 8);
                switch (id) {
                case 1:
                    if (n && (_options.idleTimeout.count() == 0 ||
                              n < static_cast<std::uint64_t>(_options.idleTimeout.count()))) {
                        _options.idleTimeout = std::chrono::milliseconds(n);
                    }
                    break;
                case 3:
                    need(n >= 1200, "invalid max_udp_payload_size", 8);
                    break;
                case 4:
                    _peerMaxData = n;
                    break;
                case 5:
                    _peerStreamBidiLocal = n;
                    break;
                case 6:
                    _peerStreamBidiRemote = n;
                    break;
                case 7:
                    _peerStreamUni = n;
                    break;
                case 8:
                    need(n <= (UINT64_C(1) << 60), "bidi stream limit overflow", 8);
                    _peerBidi = n;
                    break;
                case 9:
                    need(n <= (UINT64_C(1) << 60), "uni stream limit overflow", 8);
                    _peerUni = n;
                    break;
                case 10:
                    need(n <= 20, "ack delay exponent overflow", 8);
                    _ackExponent = n;
                    break;
                case 11:
                    need(n < 16384, "max ack delay overflow", 8);
                    _maxAckDelay = std::chrono::microseconds(n * 1000);
                    break;
                case 14:
                    need(n >= 2, "active CID limit too small", 8);
                    break;
                default:
                    break;
                }
            }
        }
        need(initial && (_options.role != EROLE_CLIENT || original), "missing QUIC connection ID parameters",
             8);
        _peerParameters = true;
    }
    asyncpp::task<void> handshake(hntls::SHandshakeResult r) {
        if (r.result.code != hntls::ERET_OK) {
            throw SQuicError{
                ERET_INVALID_STATE, UINT64_C(0x100) + r.result.alert.value_or(80), {}, {}, r.result.message};
        }
        for (auto &secret : r.secrets) {
            need(secret.cipherSuite == 0x1301 && secret.value, "unsupported QUIC cipher suite", 1);
            unsigned level = secret.level == hntls::ELEVEL_HANDSHAKE ? 1 : 2;
            auto &key =
                secret.direction == hntls::EDIRECTION_READ ? _spaces[level].read : _spaces[level].write;
            auto v = secret.value->toSpan();
            key.install({v.data, v.size});
        }
        for (auto &f : r.outgoing) {
            unsigned level = f.level == hntls::ELEVEL_INITIAL     ? 0
                             : f.level == hntls::ELEVEL_HANDSHAKE ? 1
                                                                  : 2;
            auto &s = _spaces[level];
            for (std::size_t offset = 0; offset < f.bytes.size();) {
                auto n = std::min<std::size_t>(900, f.bytes.size() - offset);
                SBytes frame{6};
                varint(frame, s.cryptoOffset);
                varint(frame, n);
                append(frame, View(f.bytes).subspan(offset, n));
                queue(level, std::move(frame));
                s.cryptoOffset += n;
                offset += n;
            }
        }
        if (r.handshakeComplete) {
            peerParameters();
            _state = ESTATE_ESTABLISHED;
            if (_options.role == EROLE_SERVER) {
                _confirmed = true;
                _addressValidated = true;
                queue(2, SBytes{0x1e});
                discard(1);
            }
            changed.set();
        }
        co_return;
    }
    Stream &stream(std::uint64_t id, bool fromPeer) {
        auto it = _streams.find(id);
        if (it != _streams.end()) {
            return it->second;
        }
        const bool peer = (id & 1) != static_cast<unsigned>(_options.role);
        const bool uni = id & 2;
        need(fromPeer && peer, "frame references unopened local stream", 5);
        need(id / 4 < (uni ? _options.maximumUnidirectionalStreams : _options.maximumBidirectionalStreams),
             "peer stream count limit", 4);
        // --> Opening N implicitly opens all lower peer streams of the same type.
        for (std::uint64_t current = id & 3; current <= id; current += 4) {
            if (_streams.contains(current)) {
                continue;
            }
            Stream s;
            s.id = current;
            s.receiveLimit = _options.maximumStreamData;
            s.sendLimit = uni ? 0 : _peerStreamBidiLocal;
            _streams.emplace(current, std::move(s));
            _accepted.push_back(current);
        }
        return _streams.at(id);
    }
    std::chrono::microseconds pto() const {
        auto delay = _smoothedRtt + std::max(_rttVariance * 4, std::chrono::microseconds{1000}) +
                     (_confirmed ? _maxAckDelay : std::chrono::microseconds{0});
        return delay * (UINT64_C(1) << std::min(_ptoCount, 10u));
    }
    void congestion(std::uint64_t lost, Time sent, Time now) {
        if (!lost || sent <= _recoveryStart) {
            return;
        }
        _recoveryStart = now;
        _cwnd = std::max<std::uint64_t>(_cwnd / 2, 2400);
        _threshold = _cwnd;
    }
    void acknowledge(unsigned level, Reader &r, Time now, bool ecn) {
        auto &space = _spaces[level];
        auto largest = r.integer(), delay = r.integer(), count = r.integer(), first = r.integer();
        need(largest < space.next && first <= largest && count < _options.maximumAckRanges,
             "invalid ACK range", 10);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{largest - first, largest}};
        auto smallest = largest - first;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto gap = r.integer(), length = r.integer();
            need(smallest >= 2 && gap <= smallest - 2, "ACK gap underflow", 7);
            auto end = smallest - gap - 2;
            need(length <= end, "ACK range underflow", 7);
            smallest = end - length;
            ranges.emplace_back(smallest, end);
        }
        if (ecn) {
            r.integer();
            r.integer();
            r.integer();
        }
        auto latest = space.sent.find(largest);
        if (latest != space.sent.end()) {
            auto sample = std::chrono::duration_cast<std::chrono::microseconds>(now - latest->second.time);
            _latestRtt = sample;
            _minimumRtt = std::min(_minimumRtt, sample);
            auto ackDelay = delay > (UINT64_MAX >> _ackExponent) ? UINT64_MAX : delay << _ackExponent;
            if (_confirmed && level == 2) {
                auto adjust = std::min(
                    std::chrono::microseconds(std::min<std::uint64_t>(ackDelay, INT64_MAX)), _maxAckDelay);
                if (sample >= _minimumRtt + adjust) {
                    sample -= adjust;
                }
            }
            if (!_rttMeasured) {
                _smoothedRtt = sample;
                _rttVariance = sample / 2;
                _rttMeasured = true;
            } else {
                auto delta = _smoothedRtt > sample ? _smoothedRtt - sample : sample - _smoothedRtt;
                _rttVariance = (_rttVariance * 3 + delta) / 4;
                _smoothedRtt = (_smoothedRtt * 7 + sample) / 8;
            }
        }
        bool newly = false;
        for (auto it = space.sent.begin(); it != space.sent.end();) {
            bool ack = false;
            for (auto [a, b] : ranges) {
                if (it->first >= a && it->first <= b) {
                    ack = true;
                    break;
                }
            }
            if (ack) {
                auto &sent = it->second;
                _flightBytes -= sent.bytes;
                if (sent.time > _recoveryStart) {
                    _cwnd += _cwnd < _threshold ? sent.bytes
                                                : std::max<std::uint64_t>(1, 1200 * sent.bytes / _cwnd);
                }
                it = space.sent.erase(it);
                newly = true;
                continue;
            }
            auto lossDelay =
                std::max(std::max(_latestRtt, _smoothedRtt) * 9 / 8, std::chrono::microseconds{1000});
            if (it->first < largest && (largest - it->first >= 3 || it->second.time + lossDelay <= now)) {
                auto sent = std::move(it->second);
                _flightBytes -= sent.bytes;
                congestion(sent.bytes, sent.time, now);
                space.pending.push_front(std::move(sent.frames));
                it = space.sent.erase(it);
            } else {
                ++it;
            }
        }
        if (newly) {
            _ptoCount = 0;
        }
        _probes = 0;
        changed.set();
    }
    asyncpp::task<void> frames(unsigned level, View input, Time now) {
        Reader r(input);
        bool eliciting = false;
        while (r.size()) {
            auto type = r.integer();
            if (type == 0) {
                continue;
            }
            if (type == 2 || type == 3) {
                acknowledge(level, r, now, type == 3);
                continue;
            }
            eliciting = true;
            if (type == 1) {
                continue;
            }
            if (type == 6) {
                need(level < 2 || _state == ESTATE_ESTABLISHED, "unexpected application CRYPTO", 10);
                auto offset = r.integer();
                auto data = r.take(r.integer());
                auto &cryptoStream = _spaces[level].crypto;
                cryptoStream.insert(offset, data, _options.maximumBufferedBytes,
                                    _options.maximumReassemblyRanges);
                bounded();
                auto next = cryptoStream.take(_options.maximumBufferedBytes);
                if (!next.empty()) {
                    co_await handshake(co_await _tls->receive(level == 0   ? hntls::ELEVEL_INITIAL
                                                              : level == 1 ? hntls::ELEVEL_HANDSHAKE
                                                                           : hntls::ELEVEL_APPLICATION,
                                                              std::move(next)));
                }
                continue;
            }
            if (type == 0x1c || type == 0x1d) {
                auto code = r.integer();
                if (type == 0x1c) {
                    r.integer();
                }
                auto reason = r.take(r.integer());
                _error = {ERET_CLOSED,
                          type == 0x1c ? std::optional<std::uint64_t>{code} : std::nullopt,
                          type == 0x1d ? std::optional<std::uint64_t>{code} : std::nullopt,
                          {},
                          std::string(reason.begin(), reason.end())};
                _state = ESTATE_CLOSED;
                changed.set();
                continue;
            }
            need(level == 2 && _state == ESTATE_ESTABLISHED, "frame illegal at encryption level", 10);
            if (type >= 8 && type <= 15) {
                auto id = r.integer();
                auto offset = (type & 4) ? r.integer() : 0;
                auto length = (type & 2) ? r.integer() : r.size();
                auto data = r.take(length);
                auto &s = stream(id, true);
                need(!(id & 2) || ((id & 1) != static_cast<unsigned>(_options.role)),
                     "STREAM on send-only stream", 5);
                need(offset <= (UINT64_C(1) << 62) - 1 && data.size() <= (UINT64_C(1) << 62) - 1 - offset,
                     "stream offset overflow", 3);
                auto end = offset + data.size();
                need(!s.finalSize || end <= *s.finalSize, "data beyond final size", 6);
                if (type & 1) {
                    need(!s.finalSize || *s.finalSize == end, "inconsistent final size", 6);
                    need(end >= s.receiveHighest, "FIN below received data", 6);
                    s.finalSize = end;
                }
                need(end <= s.receiveLimit, "stream flow control exceeded", 3);
                auto extra = end > s.receiveHighest ? end - s.receiveHighest : 0;
                need(extra <= _receiveLimit - _receiveData, "connection flow control exceeded", 3);
                _receiveData += extra;
                s.receiveHighest = std::max(s.receiveHighest, end);
                if (!s.readReset && !data.empty()) {
                    s.receive.insert(offset, data, _options.maximumBufferedBytes,
                                     _options.maximumReassemblyRanges);
                }
                bounded();
                changed.set();
                continue;
            }
            if (type == 4) {
                auto id = r.integer(), code = r.integer(), final = r.integer();
                auto &s = stream(id, true);
                need(!(id & 2) || ((id & 1) != static_cast<unsigned>(_options.role)),
                     "RESET on send-only stream", 5);
                need(final >= s.receiveHighest && (!s.finalSize || *s.finalSize == final),
                     "RESET final size mismatch", 6);
                need(final <= s.receiveLimit && final - s.receiveHighest <= _receiveLimit - _receiveData,
                     "RESET flow control exceeded", 3);
                _receiveData += final - s.receiveHighest;
                s.receiveHighest = final;
                s.finalSize = final;
                s.readReset = true;
                s.applicationError = code;
                changed.set();
                continue;
            }
            if (type == 5) {
                auto id = r.integer(), code = r.integer();
                auto &s = stream(id, true);
                need(!(id & 2) || ((id & 1) == static_cast<unsigned>(_options.role)),
                     "STOP_SENDING on receive-only stream", 5);
                s.writeReset = true;
                s.applicationError = code;
                SBytes reset{4};
                varint(reset, id);
                varint(reset, code);
                varint(reset, s.sendOffset);
                queue(2, std::move(reset));
                changed.set();
                continue;
            }
            if (type == 0x10) {
                _peerMaxData = std::max(_peerMaxData, r.integer());
                changed.set();
                continue;
            }
            if (type == 0x11) {
                auto id = r.integer(), limit = r.integer();
                auto &s = stream(id, true);
                need(!(id & 2) || ((id & 1) == static_cast<unsigned>(_options.role)),
                     "MAX_STREAM_DATA on receive-only stream", 5);
                s.sendLimit = std::max(s.sendLimit, limit);
                changed.set();
                continue;
            }
            if (type == 0x12 || type == 0x13) {
                auto count = r.integer();
                need(count <= (UINT64_C(1) << 60), "MAX_STREAMS overflow", 4);
                auto &limit = type == 0x12 ? _peerBidi : _peerUni;
                limit = std::max(limit, count);
                changed.set();
                continue;
            }
            if (type == 0x14 || type == 0x16 || type == 0x17) {
                r.integer();
                continue;
            }
            if (type == 0x15) {
                r.integer();
                r.integer();
                continue;
            }
            if (type == 0x1a) {
                auto challenge = r.take(8);
                SBytes response{0x1b};
                append(response, challenge);
                queue(2, std::move(response));
                continue;
            }
            if (type == 0x1b) {
                r.take(8);
                continue;
            }
            if (type == 0x18) {
                auto sequence = r.integer(), retire = r.integer();
                auto length = r.get(1);
                need(length && length <= 20 && retire <= sequence, "invalid NEW_CONNECTION_ID", 10);
                r.take(length);
                r.take(16);
                SBytes retireFrame{0x19};
                varint(retireFrame, sequence);
                queue(2, std::move(retireFrame));
                continue;
            }
            if (type == 0x19) {
                need(r.integer() != 0, "cannot retire only active connection ID", 10);
                continue;
            }
            if (type == 0x1e) {
                need(_options.role == EROLE_CLIENT, "server received HANDSHAKE_DONE", 10);
                _confirmed = true;
                discard(1);
                changed.set();
                continue;
            }
            need(false, "unsupported or unknown QUIC frame", 7);
        }
        if (eliciting && !_discarded[level]) {
            _spaces[level].ack = true;
        }
        co_return;
    }
    SBytes ack(unsigned level) {
        auto &received = _spaces[level].received;
        SBytes frame;
        if (received.empty()) {
            return frame;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        auto it = received.rbegin();
        auto high = *it, low = high;
        ++it;
        for (; it != received.rend(); ++it) {
            if (*it + 1 == low) {
                low = *it;
            } else {
                ranges.emplace_back(low, high);
                high = low = *it;
            }
        }
        ranges.emplace_back(low, high);
        frame.push_back(2);
        varint(frame, ranges[0].second);
        varint(frame, 0);
        varint(frame, ranges.size() - 1);
        varint(frame, ranges[0].second - ranges[0].first);
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            varint(frame, ranges[i - 1].first - ranges[i].second - 2);
            varint(frame, ranges[i].second - ranges[i].first);
        }
        return frame;
    }

  public:
    asyncpp::multi_consumer_event changed;
    Engine(std::shared_ptr<asyncpp::dispatcher> e, std::shared_ptr<hntls::ITlsContext> tls, SQuicOptions o)
        : _options(o), _context(std::move(tls)), _executor(std::move(e)), _localCid(random(8)),
          _receiveLimit(o.maximumConnectionData) {}
    EConnectionState state() const { return _state; }
    bool confirmed() const { return _confirmed; }
    const SBytes &localCid() const { return _localCid; }
    const SBytes &originalCid() const { return _originalCid; }
    SQuicError error() const { return _error; }
    hntls::SPeerInfo peerInfo() const { return _tls ? _tls->peerInfo() : hntls::SPeerInfo{}; }
    void fail(SQuicError e) {
        _error = std::move(e);
        _state = ESTATE_FAILED;
        changed.set();
    }
    void close(std::uint64_t code = 0) {
        if (_state == ESTATE_CLOSED || _state == ESTATE_FAILED) {
            return;
        }
        for (auto &s : _spaces) {
            s.pending.clear();
            s.ack = false;
        }
        if (_spaces[2].write.ready()) {
            SBytes frame{0x1d};
            varint(frame, code);
            varint(frame, 0);
            queue(2, std::move(frame));
        }
        _state = ESTATE_CLOSED;
        _error = {ERET_CLOSED, {}, code, {}, {}};
        changed.set();
    }
    asyncpp::task<void> start(Time now) {
        need(_state == ESTATE_IDLE, "QUIC connection already started", 1);
        _state = ESTATE_HANDSHAKING;
        _activity = now;
        if (_options.role == EROLE_CLIENT) {
            _originalCid = random(8);
            _remoteCid = _originalCid;
            auto c = initialSecret(_originalCid, true), s = initialSecret(_originalCid, false);
            _spaces[0].write.install(view(c));
            _spaces[0].read.install(view(s));
            _tls = _context->createSession(_executor, hntls::EMODE_QUIC, parameters());
            co_await handshake(co_await _tls->start());
        }
        co_return;
    }
    asyncpp::task<void> receive(SBytes datagram, Time now) {
        if (_state == ESTATE_CLOSED || _state == ESTATE_FAILED) {
            co_return;
        }
        std::size_t offset = 0;
        bool counted = false;
        while (offset < datagram.size()) {
            PacketHeader h;
            try {
                h = parseHeader(View(datagram).subspan(offset));
            } catch (const SQuicError &) {
                co_return;
            }
            auto packet = View(datagram).subspan(offset, h.length);
            offset += h.length;
            if (_discarded[h.level]) {
                continue;
            }
            if (!_tls && _options.role == EROLE_SERVER) {
                if (h.level != 0 || datagram.size() < 1200 || h.destination.size() < 8) {
                    co_return;
                }
                _originalCid = h.destination;
                _remoteCid = h.source;
                auto c = initialSecret(_originalCid, true), s = initialSecret(_originalCid, false);
                _spaces[0].read.install(view(c));
                _spaces[0].write.install(view(s));
                _tls = _context->createSession(_executor, hntls::EMODE_QUIC, parameters());
                co_await handshake(co_await _tls->start());
            }
            if (h.destination != _localCid &&
                !(h.level == 0 && _options.role == EROLE_SERVER && h.destination == _originalCid)) {
                continue;
            }
            auto &space = _spaces[h.level];
            if (!space.read.ready() || (h.level == 2 && _state != ESTATE_ESTABLISHED)) {
                if (_deferred.size() < _options.maximumPendingDatagrams) {
                    _deferred.emplace_back(packet.begin(), packet.end());
                    bounded();
                }
                continue;
            }
            SBytes plaintext;
            std::uint64_t pn = 0;
            if (!space.read.open(packet, h.pnOffset, space.expected, plaintext, pn)) {
                continue;
            }
            if (h.level == 0 && _options.role == EROLE_CLIENT && _remoteCid == _originalCid) {
                _remoteCid = h.source;
            }
            if (h.longHeader && h.source != _remoteCid) {
                continue;
            }
            if (pn < _receiveFloor[h.level] || space.received.contains(pn)) {
                space.ack = true;
                continue;
            }
            if (!counted) {
                _receivedBytes += datagram.size();
                counted = true;
            }
            _activity = now;
            space.expected = std::max(space.expected, pn + 1);
            space.received.insert(pn);
            // --> Bound ACK state, retaining a floor so discarded duplicates never re-enter parsing.
            while (space.received.size() > std::min<std::size_t>(_options.maximumAckRanges, 64)) {
                _receiveFloor[h.level] = *space.received.begin() + 1;
                space.received.erase(space.received.begin());
            }
            if (h.level == 1) {
                if (_options.role == EROLE_SERVER) {
                    _addressValidated = true;
                }
                if (!_discarded[0]) {
                    discard(0);
                }
            }
            co_await frames(h.level, plaintext, now);
        }
        auto waiting = std::move(_deferred);
        _deferred.clear();
        for (auto &packet : waiting) {
            auto h = parseHeader(packet);
            if (_discarded[h.level]) {
                continue;
            }
            if (_spaces[h.level].read.ready() && (h.level != 2 || _state == ESTATE_ESTABLISHED)) {
                co_await receive(std::move(packet), now);
            } else {
                _deferred.push_back(std::move(packet));
            }
        }
        bounded();
        changed.set();
        co_return;
    }
    void tick(Time now) {
        if (_state == ESTATE_CLOSED || _state == ESTATE_FAILED) {
            return;
        }
        if (_options.idleTimeout.count() && now - _activity >= _options.idleTimeout) {
            fail({ERET_TIMEOUT, {}, {}, {}, "QUIC idle timeout"});
            return;
        }
        Time oldest = Time::max();
        unsigned selected = 0;
        for (unsigned i = 0; i < 3; ++i) {
            for (auto &[pn, s] : _spaces[i].sent) {
                if (s.time < oldest) {
                    oldest = s.time;
                    selected = i;
                }
            }
        }
        if (oldest != Time::max() && now >= _lastSent + pto()) {
            auto &s = _spaces[selected];
            need(!s.sent.empty(), "empty PTO space", 1);
            s.pending.push_front(s.sent.begin()->second.frames);
            _probes = 2;
            ++_ptoCount;
            _lastSent = now;
            bounded();
            changed.set();
        }
    }
    std::vector<SBytes> datagrams(Time now) {
        std::vector<SBytes> out;
        if (_state == ESTATE_FAILED) {
            return out;
        }
        for (unsigned level = 0; level < 3; ++level) {
            auto &s = _spaces[level];
            if (!s.write.ready()) {
                continue;
            }
            for (std::size_t turn = 0;
                 turn < _options.maximumOperationsPerTurn && (s.ack || !s.pending.empty()); ++turn) {
                bool data = !s.pending.empty();
                if (_state != ESTATE_CLOSED && data && _flightBytes + 1200 > _cwnd && !_probes) {
                    break;
                }
                if (_options.role == EROLE_SERVER && !_addressValidated &&
                    _sentBytes + 1200 > _receivedBytes * 3) {
                    break;
                }
                SBytes payload;
                if (s.ack) {
                    payload = ack(level);
                }
                if (data && payload.size() + s.pending.front().size() <= 1100) {
                    append(payload, s.pending.front());
                } else {
                    data = false;
                }
                if (payload.empty()) {
                    break;
                }
                SBytes header;
                header.push_back(level == 2 ? 0x43 : level == 0 ? 0xc3 : 0xe3);
                if (level != 2) {
                    number(header, 1, 4);
                    header.push_back(static_cast<std::uint8_t>(_remoteCid.size()));
                    append(header, _remoteCid);
                    header.push_back(static_cast<std::uint8_t>(_localCid.size()));
                    append(header, _localCid);
                    if (level == 0) {
                        header.push_back(0);
                    }
                    // --> Encode length in two bytes to keep the Initial padding calculation stable.
                    if (level == 0) {
                        auto overhead = header.size() + 2 + 4 + 16;
                        need(overhead <= 1200, "Initial header budget", 1);
                        payload.resize(std::max(payload.size(), 1200 - overhead), 0);
                    }
                    auto length = payload.size() + 4 + 16;
                    need(length < 16384, "packet too large", 1);
                    number(header, length | 0x4000, 2);
                } else {
                    append(header, _remoteCid);
                }
                auto pnOffset = header.size();
                need(s.next < (UINT64_C(1) << 62), "packet number exhaustion", 15);
                auto pn = s.next++;
                number(header, pn, 4);
                auto packet = s.write.protect(std::move(header), payload, pnOffset, pn);
                s.ack = false;
                if (data) {
                    auto frames = std::move(s.pending.front());
                    s.pending.pop_front();
                    s.sent.emplace(pn, Sent{now, std::move(frames), packet.size()});
                    _flightBytes += packet.size();
                    _lastSent = now;
                    if (_probes) {
                        --_probes;
                    }
                }
                _sentBytes += packet.size();
                out.push_back(std::move(packet));
            }
        }
        bounded();
        return out;
    }
    std::optional<std::uint64_t> open(EStreamKind kind) {
        if (!_confirmed || _state != ESTATE_ESTABLISHED) {
            return {};
        }
        bool uni = kind == ESTREAM_UNIDIRECTIONAL;
        auto &count = uni ? _openedUni : _openedBidi;
        auto peer = uni ? _peerUni : _peerBidi;
        // --> Local policy also bounds how many stream objects this connection retains.
        peer = std::min(peer,
                        uni ? _options.maximumUnidirectionalStreams : _options.maximumBidirectionalStreams);
        if (count >= peer) {
            return {};
        }
        auto id = count++ * 4 + static_cast<unsigned>(_options.role) + (uni ? 2 : 0);
        Stream s;
        s.id = id;
        s.receiveLimit = _options.maximumStreamData;
        s.sendLimit = uni ? _peerStreamUni : _peerStreamBidiRemote;
        _streams.emplace(id, std::move(s));
        return id;
    }
    std::optional<std::uint64_t> accept() {
        if (_accepted.empty()) {
            return {};
        }
        auto id = _accepted.front();
        _accepted.pop_front();
        return id;
    }
    Stream &at(std::uint64_t id) { return _streams.at(id); }
    std::size_t write(std::uint64_t id, View input, bool fin = false) {
        auto &s = at(id);
        need(!(id & 2) || (id & 1) == static_cast<unsigned>(_options.role), "write on receive-only stream",
             5);
        need(!s.sendFin && !s.writeReset, "stream write is closed", 5);
        auto available = std::min<std::uint64_t>(s.sendLimit - s.sendOffset, _peerMaxData - _sentData);
        auto size = std::min<std::size_t>(
            {input.size(), static_cast<std::size_t>(std::min<std::uint64_t>(available, 900)), 900});
        if (!size && !fin) {
            return 0;
        }
        if (fin && !input.empty() && size < input.size()) {
            fin = false;
        }
        SBytes frame{static_cast<std::uint8_t>(0x0e | (fin ? 1 : 0))};
        varint(frame, id);
        varint(frame, s.sendOffset);
        varint(frame, size);
        append(frame, input.first(size));
        queue(2, std::move(frame));
        s.sendOffset += size;
        _sentData += size;
        s.sendFin |= fin;
        return size;
    }
    SBytes read(std::uint64_t id, std::size_t maximum) {
        auto &s = at(id);
        auto bytes = s.receive.take(maximum);
        if (bytes.empty()) {
            return bytes;
        }
        s.consumed += bytes.size();
        _consumedData += bytes.size();
        constexpr auto max = (UINT64_C(1) << 62) - 1;
        s.receiveLimit = std::min(max, s.consumed + _options.maximumStreamData);
        _receiveLimit = std::min(max, _consumedData + _options.maximumConnectionData);
        SBytes streamFrame{0x11};
        varint(streamFrame, id);
        varint(streamFrame, s.receiveLimit);
        queue(2, std::move(streamFrame));
        SBytes connectionFrame{0x10};
        varint(connectionFrame, _receiveLimit);
        queue(2, std::move(connectionFrame));
        return bytes;
    }
    void abort(std::uint64_t id, std::uint64_t code, bool read) {
        need(code < (UINT64_C(1) << 62), "application error exceeds 62 bits", 1);
        auto &s = at(id);
        SBytes frame{static_cast<std::uint8_t>(read ? 5 : 4)};
        varint(frame, id);
        varint(frame, code);
        if (read) {
            s.readReset = true;
        } else {
            s.writeReset = true;
            varint(frame, s.sendOffset);
        }
        queue(2, std::move(frame));
        changed.set();
    }
};
} // namespace detail
} // namespace hnquic
#endif
