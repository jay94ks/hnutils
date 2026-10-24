# HTTP QUIC TLS 라이브러리 경계

HTTP 프로토콜 구현은 `hnhttp`, 자체 QUIC 전송은 `hnquic`, TLS는 `hntls`로 분리합니다.
hntls/hnquic의 더미는 실제 TLS 1.3/QUIC v1 초기 엔진과 클라이언트 구현으로 교체했습니다. 타사 QUIC/TLS engine을
추가하지 않으며, hnhttp 안에 이 두 프로토콜을 중복 구현하지 않습니다.

## 계층별 책임

| 경로 | 프로토콜 조합 | 소켓 |
| --- | --- | --- |
| 평문 HTTP/1과 HTTP/2 prior knowledge | hnhttp | socketpp TCP |
| HTTPS HTTP/1과 HTTP/2 | hnhttp, hntls record mode | socketpp TCP |
| HTTP/3 | hnhttp, hnquic, hntls QUIC mode | socketpp UDP |

hntls는 handshake·인증·키 스케줄을 담당하고, hnquic은 packet protection·ACK·재전송·혼잡 및
transport flow control을 담당합니다. 암호 원시 연산·키·인증서·secure storage는 libcertpp에 둡니다.
HTTP payload 압축은 hncomp, HPACK/QPACK는 hnhttp에 둡니다.

QUIC의 TLS 통합은 TCP TLS record 경로와 다릅니다. CRYPTO bytes와 encryption-level별 secret 이벤트를
교환하고, QUIC의 handshake confirmation은 TLS의 handshake completion과 구분해야 합니다.
[RFC 9001](https://www.rfc-editor.org/rfc/rfc9001.html)

## 단계별 완료 기준

1. 공개 contract는 역할별 헤더로 분리하고 실제 구현과 옵션·오류 테스트를 유지합니다.
2. hnhttp의 HTTP/2·3 protocol engine을 각각 ordered byte channel과 native-QUIC-stream 계약으로 구현·검증합니다.
   메모리 transport 테스트가 UDP·TLS·실제 QUIC wire 호환성을 입증한다고 표시하지 않습니다.
3. hntls는 독립 RFC key/IV 벡터, 실제 TCP 및 Python/OpenSSL 독립 피어 상호운용을 검증했습니다.
4. hnquic은 독립 RFC packet 벡터, loss/PTO/reorder/duplicate 및 실제 UDP 통신을 검증했습니다.
   독립 QUIC peer 상호운용과 전체 RFC 적합성·보안 검토는 아직 완료하지 않았습니다.
5. 실제 HTTP/3 네트워크 통신 완료 표시는 hntls와 hnquic까지 연결하여 검증한 뒤에만 사용합니다.
   현재 저장소의 두 endpoint 사이에서 hntls handshake와 실제 hnquic UDP를 통한
   400KB HTTP/3 본문 왕복을 검증했습니다. 독립 QUIC/HTTP/3 구현과의 상호운용 검증과는 별개입니다.
6. hnhttp의 `createTls()`/`createQuic()` endpoint는 인증·ALPN 확인 후 연결을 공개하며,
   native pump와 HTTP multiplexed pump의 수명을 구분합니다. 같은 연결에서 binary 업로드·다운로드,
   multipart/URL-encoded form, Range·ETag 조건부 응답 뒤의 후속 요청을 검증합니다.
   리소스 조건 평가는 hnhttp에 있지만 캐시 저장·파일 시스템·원자적인 리소스 변경은 애플리케이션 책임입니다.

각 라이브러리의 세부 요건은 [hntls](../libs/hntls/README.md)와 [hnquic](../libs/hnquic/README.md)에 둡니다.
