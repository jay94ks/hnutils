# hntls

외부 TLS 엔진 없이 구현한 TLS 1.3의 초기 구현입니다. `src/dummy.cpp`는 제거했습니다.
런타임 의존성은 socketpp, libcertpp, asyncpp뿐이며 OpenSSL·Schannel을 연결하지 않습니다.
독립 보안 감사나 전체 TLS 표준 적합성 검증을 완료한 제품은 아닙니다.

## 구현 범위

- ClientHello, ServerHello, EncryptedExtensions, CertificateRequest, Certificate,
  CertificateVerify, Finished의 client/server 상태 기계와 증분 입력.
- TLS_AES_128_GCM_SHA256, X25519, HKDF-Extract/Expand-Label, transcript와 방향별 traffic key.
- Ed25519, ECDSA P-256/SHA-256, RSA-PSS-RSAE/SHA-256 CertificateVerify. RSA 키는 최소 2048 bit.
- libcertpp의 명시적 trust anchor 기반 chain·유효 기간·KeyUsage·EKU·name constraints 검증,
  SAN DNS/IP identity 확인, ALPN, 선택적 상호 인증. CN fallback은 없습니다.
- TLS record의 인증·암호화, sequence/사용 한도, padding 제거, incremental fragmentation,
  수신 KeyUpdate 및 요청에 대한 응답, close_notify와 잘린 transport EOF 구분.
- record-free QUIC CRYPTO handshake 및 encryption level별 immutable secure traffic-secret snapshot.
- socketpp TCP 클라이언트, accepted TCP socket 어댑터, 한 read와 한 write의 동시 진행 및 close/join.

암호 원시 연산과 secure key storage는 모두 libcertpp에 위임합니다. 이 라이브러리 안에 AES,
SHA, HKDF, ECC 등의 알고리즘을 복제하지 않습니다. secret을 로그에 출력하지 않습니다.

### 현재 제한

TLS 1.2 이하, 다른 cipher suite/group, HelloRetryRequest, 재개/PSK/0-RTT, post-handshake client
authentication, OCSP/CRL/revocation, OS trust store 및 policy-tree 검증은 미지원입니다.
티켓은 구조를 검사한 뒤 버립니다. unsupported 협상은 plaintext 또는 약한 버전으로 fallback하지 않습니다.
fatal alert 전송, 선제적 KeyUpdate, 더 넓은 extension/오류 상호운용 및 보안 검토는 후속 작업입니다.

`requirePeerAuthentication`은 기본 true입니다. 서버에서 client certificate를 요구하지 않을 때에만
false로 설정합니다. 클라이언트에서 false로 설정하면 chain/name 검증을 하지 않으며
`EPEER_ACCEPTED`도 반환하지 않습니다. 이 모드는 신뢰되는 인증 연결이 아닙니다.
context 생성 이후 전달한 identity/anchor 객체의 다른 alias를 통해 내용을 변경하면 안 됩니다.

## 분리된 공개 API

`common.hpp`, `options.hpp`, `credentials.hpp`, `context.hpp`, `session.hpp`, `stream.hpp`,
`socket_stream.hpp`, `client.hpp`로 책임을 분리했습니다. `hntls.hpp`는 include만 집계합니다.
각 헤더는 독립 translation unit으로 빌드합니다. 모든 상태는 비공개입니다.

- `ITlsContext`: 불변 policy/credential factory.
- `ITlsSession`: 소켓을 열지 않는 transport-neutral TLS 상태 기계.
- `ITlsStream`: `IByteStream` 위의 handshake와 암호화 application I/O.
- `CSocketByteStream`: 연결된 socketpp TCP socket을 소유하는 transport. 서버 accept에도 사용합니다.
- `ITlsClient`: TCP 연결과 TLS handshake를 수행하는 실제 `CTlsClient` 구현을 반환합니다.

factory와 `isImplemented()`는 연결 성공을 의미하지 않습니다. `ESTATE_ESTABLISHED`와 인증 결과를
검사해야 합니다. QUIC의 handshake confirmation은 hnquic에서 별도로 결정합니다.

```cpp
// Run inside the caller's serial socketpp reactor.
hntls::STlsOptions options;
options.serverName = "example.test";
options.alpn = {"h2"};
hntls::STrustOptions trust;
trust.anchors = trustedCertificates; // shared_ptr<const certpp::x509::CCert>
auto policy = hntls::ITlsContext::create(options, {}, trust);
auto client = hntls::ITlsClient::create(watcher, policy);
auto result = co_await client->connect(peerAddress);
if (result.result.code == hntls::ERET_OK) {
    auto stream = result.stream;
    // Use stream->readSome()/writeSome(), or hnhttp::IHttpStream::fromTls(stream).
}
client->close();
co_await client->waitClosed(); // before stopping watcher
```

공유 reactor는 caller가 구동합니다. read/write 각각 같은 방향 중첩은 BUSY이며,
coroutine 입력/결과는 소유권을 갖습니다. 대기 중 객체를 해제하지 말고 close 후 작업과 waitClosed를 join합니다.
live stop token은 close 또는 stream 중단을 통해 I/O를 깨웁니다. stream/client의 미래 deadline은
조용히 무시하지 않고 UNSUPPORTED를 반환합니다. 동기 계산만 하는 session은 시작 시 deadline을 확인합니다.

## 빌드와 검증

```sh
cmake -S . -B build
cmake --build build --target hntls_tests hntls_echo_server hntls_echo_client
ctest --test-dir build -R '^hntls\.' --output-on-failure
python libs/hntls/tests/interop.py build/libs/hntls/hntls_echo_server
```

Windows에서는 실행 파일에 `.exe`를 붙입니다. doctest 진단은 stdout/stderr로 출력하며
Windows assertion/CRT/WER 창을 비활성화합니다. 예제는 테스트용 ephemeral 인증서의 공개 DER만
출력하고 파일을 열지 않습니다. `HNTLS_BUILD_EXAMPLES=OFF`로 예제 빌드를 끌 수 있습니다.

독립 [RFC 8448](https://www.rfc-editor.org/info/rfc8448/) key/IV 벡터, 분할 handshake,
상호 인증, 잘못된 anchor/name, 변조 tag, close_notify, 실제 TCP echo의 세 signature scheme을 검사합니다.
`interop.py`는 Python/OpenSSL을 독립 검증 피어로만 사용합니다. TLS 1.3, 인증서/localhost,
ALPN, 정확한 echo와 close_notify 상호운용을 Windows/Linux에서 확인했습니다.
이 검증이 미지원 기능이나 전체 TLS 안전성을 증명하지는 않습니다.

설계 기준은 [RFC 8446](https://www.rfc-editor.org/rfc/rfc8446.html),
[RFC 7301](https://www.rfc-editor.org/rfc/rfc7301.html),
[RFC 9525](https://www.rfc-editor.org/rfc/rfc9525.html),
[RFC 9001](https://www.rfc-editor.org/rfc/rfc9001.html)입니다.
