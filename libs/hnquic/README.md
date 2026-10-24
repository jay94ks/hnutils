# hnquic

socketpp UDP, hntls의 자체 TLS 1.3, libcertpp 암호 원시 연산과 asyncpp만 사용하는 QUIC v1의
초기 구현입니다. 더미는 제거했고 실제 암호화 UDP 통신을 수행합니다. 외부 QUIC/TLS 엔진은
연결하지 않습니다. HTTP/3/QPACK은 hnhttp의 책임입니다.

전체 RFC 적합성·일반 인터넷 배포·독립 QUIC 구현과의 상호운용·보안 감사를 완료한 상태는 아닙니다.

## 구현 범위

- v1 long/short header, varint, CID, packet number 복원, coalesced packet 처리.
- Initial key derivation, AES-128-GCM packet protection과 AES header protection.
  모든 AES/HKDF 연산은 libcertpp를 사용합니다.
- hntls의 record-free CRYPTO handshake, 순서가 바뀐 CRYPTO 재조립, transport parameter 검사,
  Initial/Handshake/1-RTT packet-number space, handshake confirmation와 이전 level key 폐기.
- bounded ACK range·중복 억제, RTT/ACK delay 추정, packet/time threshold loss detection,
  PTO probe, frame 재전송, bytes-in-flight와 초기 NewReno 형태 congestion window.
- 양/단방향 62-bit native stream ID, offset 재조립/overlap 검사, FIN/final size,
  RESET_STREAM/STOP_SENDING, stream/connection flow control과 MAX_DATA/MAX_STREAM_DATA.
- UDP 서버 CID demultiplexing·admission ceiling·3배 anti-amplification,
  caller reactor의 timer, idle timeout, 실제 client/server I/O와 종료 wake/join.

### 현재 제한

Retry/version negotiation, stateless reset, key phase update, resumption/0-RTT, migration/새 경로 검증,
PMTU/ECN metadata, 정교한 pacing/persistent-congestion 처리 및 완전한 closing/draining 타이머는
아직 지원하지 않습니다. 단일 경로, 1200-byte datagram budget, TLS_AES_128_GCM_SHA256/X25519
프로파일입니다. 지원하지 않는 Retry/0-RTT packet은 처리하지 않고, 인증된 미지원 frame/key phase는
오류로 거부합니다. 원격 peer가 이 기능들을 요구하면 연결되지 않거나 종료될 수 있습니다.

stream 객체 수는 connection 수명 전체에 걸쳐 로컬 옵션으로 제한합니다. peer MAX_STREAMS는
해석하지만 stream 수명 종료에 따른 로컬 credit 재발급은 아직 없습니다. openStream의 credit
소진은 무한 대기하지 않고 LIMIT입니다. RESET/STOP_SENDING 뒤 connection credit 회수 등 모든
stream 종료 조합의 적합성 검증은 후속 작업입니다. 고부하/공개망 서비스에 사용하지 마세요.

## 역할별 헤더와 실행 계약

`common.hpp`, `options.hpp`, `stream.hpp`, `connection.hpp`, `client.hpp`, `server.hpp`를 분리했습니다.
`hnquic.hpp`에는 include만 있습니다. 각 헤더는 독립 컴파일하고 상태는 비공개로 유지합니다.

- `IQuicClient`의 실제 `CQuicClient`: client connection의 run/connect/close/join facade.
- `IQuicConnection`의 실제 구현: confirmed 연결과 stream open/accept.
- `IQuicServer`: 하나의 UDP socket을 소유하고 연결별 CID로 demultiplexing합니다.
- `IQuicStream`: owned I/O, 방향, FIN과 reset을 유지합니다. 한 read/한 write를 동시 진행합니다.

client는 caller가 `run()`을 먼저 시작한 뒤 `connect()`를 await합니다. run과 reactor를 계속
구동해야 합니다. `connect()`는 TLS Finished뿐 아니라 HANDSHAKE_DONE까지 확인한 연결만 반환합니다.
서버 `run()`은 accepted connection을 포함해 공유 UDP endpoint의 수신·송신·복구 timer를 소유합니다.
accepted connection의 별도 run은 선택적 lifecycle supervisor이며 두 번째 UDP receive loop를 만들지 않습니다.
서버를 close하면 공유 endpoint와 연결들도 중단됩니다. native run과 hnhttp의 HTTP run은 별개입니다.

```cpp
// Run on watcher; supervise tasks with asyncpp::async_launch_scope.
hntls::STlsOptions tlsOptions;
tlsOptions.serverName = "example.test";
tlsOptions.alpn = {"h3"};
hntls::STrustOptions trust;
trust.anchors = trustedCertificates;
auto policy = hntls::ITlsContext::create(tlsOptions, {}, trust);
auto client = hnquic::IQuicClient::create(watcher, policy);
asyncpp::async_launch_scope tasks;
tasks.launch(client->run());
auto result = co_await client->connect(peerAddress);
if (result.result.code == hnquic::ERET_OK) {
    auto stream = co_await result.connection->openStream(hnquic::ESTREAM_BIDIRECTIONAL);
    // Native stream I/O or hnhttp::IHttpQuicTransport::fromQuic(result.connection).
}
client->close();
co_await tasks.join();
co_await client->waitClosed();
```

policy/role/ALPN/resource limits은 factory에서 검증합니다. application error와 stream ID는 62-bit입니다.
factory 성공이나 isImplemented는 인증 성공을 의미하지 않습니다. wrong executor·BUSY·EOF·reset·native
실패를 구분합니다. live cancellation은 대기를 깨우고, 미래 deadline은 명시적 UNSUPPORTED입니다.
close 후 대기 작업과 run/waitClosed를 join한 뒤 watcher를 정지합니다. 숨겨진 timer 스레드는 없습니다.

## 검증

```sh
cmake -S . -B build
cmake --build build --target hnquic_tests
ctest --test-dir build -R '^hnquic\.' --output-on-failure
```

doctest는 Windows/Linux에서 독립 RFC 9001 서버 Initial 암호문과 header protection 벡터,
변조 tag 거부, 실제 TLS handshake, deterministic clock의 packet loss/PTO/reorder/duplicate,
실제 socketpp UDP client/server와 native FIN을 검사합니다. 테스트용 in-memory HTTP transport와
실제 QUIC packet 엔진 검증을 구분합니다. Windows 오류 진단은 console로만 출력합니다.
hnhttp 통합 테스트는 실제 UDP/TLS/QUIC 경로로 400KB HTTP/3 본문과 native flow control을 검사합니다.
Windows/Linux 관련 회귀 89개와 Linux ASan/UBSan/누수 검사 62개가 통과했습니다.

전송과 보호 기준은 [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000.html),
[RFC 9001](https://www.rfc-editor.org/rfc/rfc9001.html), recovery 기준은
[RFC 9002](https://www.rfc-editor.org/rfc/rfc9002.html)입니다. 현재 일부 구현을 전체 적합성으로
간주하면 안 됩니다. 특히 독립 QUIC peer 및 폭넓은 공격/손실/종료 조합 검증이 더 필요합니다.
