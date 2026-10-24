# hnhttp

C++20 HTTP 프로토콜 라이브러리의 **초기 구현 단계**입니다.
최종 범위는 **HTTP/1.0, HTTP/1.1, HTTP/2, HTTP/3**이고 네임스페이스는 `hnhttp`입니다.
socketpp처럼 공개 상태를 숨기고, 비동기 연산은 `asyncpp::task`로 선언합니다.

`hnhttp::hnhttp`는 socketpp·asyncpp와 저장소의 hntls·hnquic 인터페이스를 사용합니다.
HTTP/2·3 연결 엔진, Huffman, QPACK dynamic/control stream과 multipart를 구현했습니다.
hntls·hnquic은 실제 TLS 1.3/QUIC v1 초기 엔진과 클라이언트를 제공하며, 기능·검증 제한은 각 README에 명시합니다.
다음 표가 실행 가능한 범위이며 전체 HTTP 표준 준수·production-ready 상태를 주장하지 않습니다.

| 구성 | 현재 구현·제한 |
| --- | --- |
| HTTP/1.0·1.1 | incremental 요청/응답 head, Content-Length·chunked·close-delimited 수신, bounded body reader, HTTP/1.1 chunked 송신, binary source, query codec, 중복 헤더 보존, 순차 재사용 |
| HTTP/2 | prior-knowledge TCP, preface·SETTINGS/ACK·HEADERS/CONTINUATION·DATA·PING·WINDOW_UPDATE·RST_STREAM·GOAWAY, 동시 교환과 연결/스트림 flow control |
| HTTP/3 | native QUIC stream 위 request/control/QPACK pumps, SETTINGS·GOAWAY·FIN·stream reset, 차단된 QPACK section 재개; 실제 UDP/QUIC/TLS는 hnquic/hntls |
| 평문 TCP | `IHttpClient::create()` / `IHttpServer::create()`, socketpp와 caller-owned reactor; HTTP/2 서버는 `acceptConnection()` 후 `run()/ready()/accept()` |
| 보안 endpoint | `createTls()`로 HTTPS HTTP/1·2, `createQuic()`로 HTTP/3; 검증된 peer·ALPN 확인 후에만 HTTP connection 공개 |
| HPACK | 61개 static table, raw/Huffman literal, 정수 prefix, dynamic insert/reference/eviction/size-update 디코더; 인코더는 static reference와 non-indexed/never-indexed literal |
| QPACK | 99개 static table, raw/Huffman literal, required insert count/base, relative/post-base 참조, dynamic capacity/insert/duplicate/eviction, section ACK/cancellation/increment |
| form | multipart·URL-encoded 송수신, replayable multipart source의 경계 충돌 preflight, 한 part씩 bounded 수신, binary source |
| 리소스 조건 | `CHttpResource::evaluate()`의 ETag strong/weak 비교, If-Match/None-Match, 날짜 조건, If-Range, 단일 bytes Range의 206/416 선택 |
| 연동 | `IHttpStream::asTlsTransport()/fromTls()`, `IHttpQuicTransport::fromQuic()`; 미확립 TLS/미확인 QUIC는 I/O 전에 거부 |
| 아직 미구현 | Expect, HTTP/1 outgoing trailer·informational·chunk extension, CONNECT/Upgrade tunnel, client absolute/authority target, push |

미지원 factory는 `system_error(errc::not_supported)`를 던지고, async 기능은 `ERET_UNSUPPORTED`를 반환합니다.
HTTP/3 옵션으로 평문 TCP endpoint를 만드는 것은 거부합니다. 버전을 HTTP/1로 몰래 바꾸지 않습니다.
현재 operation 정책은 live stop-token/deadline을 지원하지 않으며 I/O 전에 거부합니다.
이미 취소된 token은 즉시 `ERET_CANCELLED`입니다. close에 의한 전체 연결 중단·pending I/O join은 구현되어 있습니다.
production-ready 또는 HTTP 전체 표준 준수를 주장하지 않습니다.

## 경계와 구성

| 버전 | 외부에서 제공할 전송 | hnhttp 연결 계약 | 헤더 압축 | 동시성·수신 제어 |
| --- | --- | --- | --- | --- |
| HTTP/1.0·1.1 | `IHttpStream` (TCP/TLS 등) | `IHttp1Connection` | 없음 | 한 교환씩, framing 완료 후 연결 재사용 |
| HTTP/2 | `IHttpStream` | `IHttp2Connection` | HPACK | 단일 byte-stream 위의 stream ID, 연결/스트림 window |
| HTTP/3 | `IHttpQuicTransport` + native QUIC streams | `IHttp3Connection` | QPACK | QUIC 다중 스트림, HTTP/3 control/QPACK stream 진행 |

HTTP/3을 HTTP/2의 단일 byte-stream parser로 흉내 내지 않습니다.
TCP/UDP, SSL/TLS handshake·인증서 검증, QUIC packet/ACK/loss/congestion/transport flow control은
**hnhttp 구현 범위 밖**입니다. 평문 TCP 소켓·연결 수립은 socketpp를 사용합니다. TLS·QUIC는 별도
라이브러리 [hntls](../hntls/README.md)·[hnquic](../hnquic/README.md)의 자체 엔진에 둡니다.
전송 어댑터 계약은 타사 QUIC/TLS 라이브러리 도입을 의미하지 않습니다.
현재 평문 endpoint는 TLS를 수행하지 않습니다. DNS와 origin pooling/coalescing 권한 판단은 호출자 책임입니다.

HTTP framing·chunk codec·URI/form encoding·multipart 탐색·HPACK/QPACK·stream registry·field index 등
프로토콜 알고리즘과 내부 자료구조는 자체 구현합니다. 외부 HTTP parser, nghttp2/nghttp3,
curl 또는 시스템 HTTP engine으로 동작을 위임하지 않습니다. 공개 복사 값에는 표준 라이브러리의
string/vector/optional/smart pointer를 사용합니다. 압축 본문의 codec이 필요해지는 경우 `hncomp`를
활용하며 HTTP field compression(HPACK/QPACK)은 hnhttp에 둡니다.
hnhttp 자체는 payload 압축/해독이나 암호 연산을 하지 않습니다. 실제 libcertpp 암호 연산과
런타임 의존성은 hntls/hnquic에 두며, hncomp payload codec은 아직 연결하지 않습니다.
향후 payload 압축은 hncomp, 암호·인증서 처리는 libcertpp를 사용하고 TLS engine은 추가하지 않습니다.

### 공개 헤더

- `common.hpp`: 버전·메서드·오류·범위와 자원 제한
- `fields.hpp`: duplicate/order를 보존하는 헤더와 query의 read-only/mutable facade
- `transport.hpp`: 외부 byte channel, stream 및 QUIC transport 계약·인증서 전달
- `body.hpp`: outgoing source와 incoming reader 분리
- `form.hpp`: multipart/URL-encoded source, 순차 form reader
- `message.hpp`: 서버/클라이언트 역할별 request/response
- `connection.hpp`: read-ahead·압축 상태 소유자와 버전별 연결 capability
- `context.hpp`: 교환 수명·완료·중단·CONNECT/Upgrade takeover
- `endpoint.hpp`: 평문·TLS·QUIC `IHttpClient`·`IHttpServer`, private-state `CSocketHttpStream`
- `resource.hpp`: 애플리케이션 리소스의 조건부 요청과 단일 bytes Range 평가
- `header_codec.hpp`: 자체 구현 header-block codec 및 resource bounds
- `qpack.hpp`: dynamic table·instruction stream·차단/ACK/cancellation capability

umbrella header는 `<hnhttp.hpp>`입니다. 모든 stateful concrete 구현은 private PIMPL 또는
공개 interface 뒤의 private implementation으로 제공합니다. 헤더/본문의 내부 컨테이너나 native handle은
노출하지 않습니다. field facade reference의 수명은 소유 message와 같으며 복사 조회 결과는 독립 값입니다.

## 스트림 소유권과 교환 수명

1. `setStream(std::shared_ptr<IHttpStream>)`은 HTTP/1 convenience 경로입니다. 스트림의 실행기에서
   수명을 공유하며 bind하고, 실패하면 context·기존 binding을 변경하거나 stream을 닫지 않습니다.
   null·교체·중복 bind는 거부합니다. `createStream()`도 같은 shared pointer 계약입니다.
2. 연결 객체가 transport의 **논리적 I/O 사용권**과 read-ahead를 소유하고 context는 **한 교환**을 나타냅니다.
   호출자가 shared pointer를 보관할 수 있지만 직접 I/O하거나 같은 stream으로 두 번째 연결을 만들 수는 없습니다.
   shared pointer는 수명 공유이지 동시 I/O 허가나 연결 상태의 자동 공유가 아닙니다. 숨겨진 global connection cache는 없습니다.
3. `getRequest()/getResponse()/getBody()`는 shared facade를 반환합니다. context wrapper가 사라져도
   이미 얻은 facade와 생성된 task는 자신의 exchange 상태를 유지해야 합니다. 순환 소유는 금지합니다.
4. `parseOnce()`는 요청 **헤더까지만** 파싱합니다. 본문 전체를 메모리에 모으지 않습니다.
   헤더가 준비된 뒤 재호출은 idempotent OK이며 다음 요청으로 넘어가지 않습니다.
5. client request `exec()`는 요청을 보내고 최종 response header를 얻습니다. response body는 별도 reader입니다.
   서버의 response `exec()`는 응답 본문과 outbound trailer까지 완료합니다.
6. `finish()`는 inbound body/trailer와 outbound finalization이 모두 완료되어야 성공합니다.
   기본은 자동 discard가 아니며 미소비 본문이 있으면 `ERET_BODY_PENDING`입니다.
   명시적으로 `SFinishOptions`를 주면 바이트 상한과 operation deadline 안에서만 drain합니다.
7. HTTP/1은 성공한 finish와 keep-alive 정책을 만족한 뒤 **동일 connection에 새 context**를 만듭니다.
   이전 facade를 덮어쓰는 reset은 제공하지 않습니다. over-read한 다음 요청 바이트는 연결에 남깁니다.
8. HTTP/2·3에서는 서로 다른 context가 독립 스트림입니다. 하나의 미완료 body가 다른 요청 전체를
   reset해서는 안 됩니다. 단, 연결 수준 framing/compression 오류는 모든 교환에 영향을 줍니다.

HTTP/1은 동시 pipelining 실행을 제공하지 않습니다. 파이프라인으로 도착한 bytes를 보존하되
한 교환씩 파싱·응답합니다. HTTP/1.0 persistence는 보수적으로 close를 기본 동작으로 설계합니다.
`keepAlive=true`는 정책 요청이지 실제 재사용을 보장하는 값이 아닙니다.

미완료 exchange의 마지막 소유자가 사라지면 HTTP/1은 닫고, HTTP/2·3은 supervised pump에 해당
stream reset을 예약합니다. pump가 진행할 수 없다면 안전한 connection close로 종료합니다.
소멸자는 무제한 drain, blocking I/O, 예외를 던지는 응답 전송이나 detached 작업을 시작하지 않습니다.

## socketpp·reactor·worker 실행 계약

`IAsyncByteChannel::executor()`, `IHttpQuicTransport::executor()`와 `IHttpConnection::executor()`는
안정적인 `shared_ptr<asyncpp::dispatcher>`를 반환합니다. 연결은 외부 transport의 실행기를 그대로
사용합니다. null은 허용하지 않으며 실행 중 callbacks를 직렬화하고 `dispatcher::current()`를 설정해야 합니다.
`executor()` 조회와 `close()` 요청, 참조 소유권 해제만 임의 스레드에서 허용합니다.
공유 소유가 loop의 실행까지 보장하지는 않으므로 호출자가 직접 구동하고 종료 순서를 관리해야 합니다.

socketpp를 확인한 결과 `CSocketWatcher`는 이미 `asyncpp::dispatcher`를 상속하고, `push()`로 작업을
깨우며 `run()/runOne()`에서 current dispatcher를 설정합니다. 다음 구성으로 응용할 수 있습니다.

| 구성 요소 | 역할·조건 |
| --- | --- |
| `CSocket` | 동기 accept/open 이후 `CSocketAsync(watcher, std::move(socket))`로 소유권 이전; blocking receive/send를 reactor에서 직접 호출하지 않음 |
| `CSocketAsync` | `receive/send`를 외부 `IHttpStream` 어댑터에서 await; 소켓이 사용하는 byte span의 저장 공간은 native 완료까지 소유 |
| `CSocketWatcher` | 연결의 serial executor로 사용; watcher 하나는 동시에 한 runner만 구동하고 서로 다른 watcher로 연결을 분산 |
| worker pool | 복사한 path/header/body chunk로 CPU 작업; HTTP facade에 접근하기 전에 protocol executor로 복귀 |

worker 기반 자체 event loop도 같은 dispatcher 계약을 충족하면 연결 실행기로 사용할 수 있습니다.
일반적인 다중 스레드 pool 자체는 직렬 실행기가 아니므로 직접 연결 실행기로 사용하지 않습니다.
필요하면 외부 serial dispatcher/strand를 두고 socket completion을 그쪽으로 marshal합니다.
이때 실제 소켓의 watcher도 별도로 계속 구동해야 합니다. native handle이나 watcher 내부 등록 정보는 공개하지 않습니다.

bound context/request/response/body와 field facade의 조회·수정, async 메서드 호출·await는 모두
연결 실행기에서 합니다. 실행기가 다른 async 진입은 상태/전송에 손대기 전에 `ERET_WRONG_EXECUTOR`를
반환합니다. 예외를 허용하는 동기 메서드는 `logic_error`로 거부하고 noexcept 관찰 메서드는 호출자에게
실행기 준수를 요구합니다. 이 검사는 foreign thread에서의 내부 상태 읽기 자체가 race가 되지 않도록 설계합니다.
unbound draft는 단일 호출자가 독점해서 준비할 수 있고, bind 시점부터 실행기에 귀속됩니다.

`co_await asyncpp::defer(*workers)`로 복사 데이터를 worker로 옮기고, 결과를 계산한 후
`co_await asyncpp::defer(*connection->executor())`로 돌아옵니다. worker 예외도 먼저 보관한 다음
실행기로 돌아와 처리합니다. callback·body source는 block하거나 HTTP 상태를 재진입하지 않으며,
다른 executor에서 완료되는 작업을 이용하면 복귀를 marshal해야 합니다. `asyncpp::task`가 자동으로
원래 스레드로 돌아온다고 가정하지 않습니다. foreign-executor 거부를 제외한 정상 작업의 완료는 연결 실행기에서 합니다.

`SHttpConnectionOptions::scheduling`은 한 turn당 operation 수와 incremental 처리 바이트 수를
제한합니다. 두 값은 양수여야 하며 0이면 생성/bind를 거부합니다. 기본은 64회·256 KiB입니다.
socket이 계속 ready이거나 read-ahead/body source가 즉시 결과를 내더라도 budget 소진 시 동일 실행기에
continuation을 post합니다. HTTP/1 parser/sender, multipart preflight와 HTTP/2·3 pump에 모두 적용합니다.
budget은 연결 내 작업끼리 공유하며 즉시 완료된 nested await마다 초기화하지 않습니다.
한 단계가 큰 메시지를 처리한다는 이유로 budget을 우회하지 않습니다. 자체 source/callback의 CPU 시간까지
강제로 선점할 수는 없으므로 외부 구현도 협조해야 합니다. accept queue, body buffering, handler 수와
worker 제출량은 별도로 제한해야 하며 한 스트림의 애플리케이션 처리가 control frame 진행을 막아서는 안 됩니다.

### 취소와 종료에서 아직 필요한 어댑터 작업

`SAsyncCapabilities`는 외부 channel과 QUIC accept/open의 deadline·stop-token 지원 여부를 나타냅니다.
지원하지 않는 요청은 I/O 전에 `ERET_UNSUPPORTED`로 거부해야 하며 timeout을 무시한 채 대기하지 않습니다.
같은 방향의 중복 read/write는 `ERET_BUSY`이며 반대 방향 read/write는 동시에 진행할 수 있습니다.
stop callback은 임의 스레드에서 실행되므로 상태 변경/코루틴 직접 resume 대신 실행기에 취소 요청만 post합니다.
실행 중 task/frame을 파괴하는 것은 취소가 아닙니다. native completion과 취소가 경합해도 정확히 한 번 완료하고
native I/O가 사용하는 buffer/state는 실제 completion이 끝날 때까지 유지해야 합니다.

`CSocketAsync`에는 per-operation deadline·stop-token API가 없어 어댑터의 optional capabilities는 false입니다.
socketpp의 POSIX close는 kernel 등록을 제거한 다음 private readiness waiter를 취소 오류로 깨우도록 보강했습니다.
Windows는 closesocket 이후 IOCP completion을 소비합니다. `CSocketHttpStream`은 read/write buffer를 native 완료까지
소유하고 close 요청을 watcher로 marshal하며 native pending 작업을 추적해 `waitClosed()`에서 join합니다.
서로 다른 watcher나 datagram socket을 잘못 연결하는 것은 생성 단계에서 거부합니다.
listener close는 native accept뿐 아니라 요청 head를 기다리는 아직 전달되지 않은 연결도 중단·join합니다.
이미 caller에게 반환된 context/connection은 listener close와 별개로 caller가 close/join해야 합니다.

종료는 다음 순서를 지킵니다. 먼저 admission을 중단하고 connection `close()`를 요청합니다.
그다음 caller가 supervise한 handler/run task를 join하고 `connection->waitClosed()`로 내부 sender·protocol
작업과 transport native callback까지 정리된 것을 확인합니다. 마지막에 watcher를 stop하고 실행 스레드를 join합니다.
connection/transport는 공유 watcher의 `stop()`을 호출하지 않습니다. waitClosed의 deadline/cancellation은
해당 waiter만 끝내며 cleanup은 계속되므로 성공한 join 없이 자원을 파괴하면 안 됩니다.
소켓/adapter/connection·task·watcher 사이 strong-reference cycle도 금지합니다.

## HTTP/2·3의 비동기 구동

외부 serial executor/event loop에서 `IHttpMultiplexedConnection::run()`을 하나만 구동합니다.
호출자가 task를 supervision하고 종료 시 join해야 합니다. 연결은 worker/thread를 몰래 만들지 않습니다.
같은 연결의 API 진입과 내부 protocol continuation은 이 executor에서 직렬화합니다. 서로 다른 exchange의
동시 co_await는 허용하지만 같은 exchange의 중복 parse/exec/read는 `BUSY/INVALID_STATE`입니다.
외부 adapter는 그 executor로 continuation을 복귀시키거나 적절히 marshal해야 합니다.

`ready()`는 preface·SETTINGS/control setup을 기다립니다. run이 없으면 `NOT_RUNNING`을 반환합니다.
서버는 `accept()`로 헤더가 준비된 새 context를 얻습니다. 클라이언트는 connection을 공유하는
`IHttpClientContext::create(connection)`을 여러 개 생성합니다. `accept()` waiter의 취소는 연결을 닫지 않습니다.
한 스트림의 deadline/cancellation은 해당 스트림에 적용하고, 공유 byte-stream read를 취소하여
다른 스트림을 함께 중단하지 않습니다. `run()`의 deadline만 연결 수명을 제어합니다.

### HTTP/2

client/server preface, SETTINGS/ACK, HEADERS/CONTINUATION, DATA, PING, WINDOW_UPDATE,
RST_STREAM, GOAWAY와 HPACK static/dynamic table·정수·Huffman을 자체 구현했습니다.
HPACK 송신/수신 상태는 연결·방향별로 분리합니다. stream body delivery/discard에 맞춰 receive
credit을 반환하며 단순한 수신만으로 무제한 WINDOW_UPDATE하지 않습니다.
server push는 기본 설계 API에 넣지 않고 client는 ENABLE_PUSH=0을 광고하도록 설계합니다.
protocol이 요구하는 알 수 없는 frame 처리, 금지된 push 처리와 압축 상태 동기화는 검증 범위입니다.

`IHttp2Connection`에만 SETTINGS update/ACK, PING/ACK, send-window observation을 둡니다.
현재 local runtime SETTINGS update는 ID 3(MAX_CONCURRENT_STREAMS)과 6(MAX_HEADER_LIST_SIZE)의
hard limit 이내 광고를 지원하며, decoder/window 재설정이 필요한 나머지 ID는 I/O 전에 UNSUPPORTED입니다.
peer가 보내는 table/window/frame SETTINGS 변경은 처리합니다. 연결/스트림 flow window를
애플리케이션이 직접 수정하지 못합니다. stream window는 SETTINGS 감소 후 음수일 수 있습니다.

### HTTP/3

`createQuic()`은 이미 연결된 외부 QUIC adapter를 받습니다. confirmed handshake와 ALPN `h3`를
요구하고 초기 계약에서는 0-RTT replay를 사용하지 않습니다. HTTP/2-over-TLS는 외부 ALPN `h2`를
확인하며 cleartext HTTP/2는 명시적인 prior knowledge 경로입니다. 버전을 sniff해서 silently downgrade하지 않습니다.
HTTP/1 Upgrade를 통한 h2c 전환, HTTP/3 endpoint discovery/Alt-Svc·QUIC version negotiation은 이 단계에 넣지 않습니다.

HTTP/3 request bidi stream, 단일 control stream, QPACK encoder/decoder uni stream을 구분합니다.
request stream ID 0은 유효하므로 `optional<uint64_t>`로 unbound와 구분합니다. QUIC의 62-bit ID를
HTTP/2의 31-bit ID에 축소하지 않습니다. transport adapter는 uni stream 방향과 stream initiator,
FIN/RESET_STREAM/STOP_SENDING 및 별도 byte ordering을 정확하게 제공해야 합니다.

HTTP/3 frame·QUIC varint를 프로토콜 내부에서 파싱하고 QPACK의 서로 다른 static/dynamic table,
insert count, blocked section, section acknowledgment와 cancellation instruction을 구현했습니다.
HPACK와 QPACK는 공통 primitive 일부를 공유할 수 있으나 동일한 상태 기계로 대체하지 않습니다.
critical stream과 SETTINGS 중복/순서/종료 오류는 연결 수준으로 처리합니다.
HTTP/3 SETTINGS는 한 번만 보내므로 HTTP/2의 runtime update API를 상속시키지 않습니다.
server push는 MAX_PUSH_ID를 발행하지 않는 정책으로 비활성화하며 push/unknown uni stream도 무시해서
상태를 잃는 것이 아니라 제한·오류로 처리합니다. critical stream pump는 body consumer와 별개로 진행합니다.

QPACK 때문에 대기하는 header bytes/streams, DATA 대기량, control frame과 unknown stream 수신을
각각 제한합니다. QUIC adapter도 자체 버퍼를 제한하고 critical control/QPACK stream의 진행을 위한
credit을 확보해야 합니다. hnhttp의 한도만으로 외부 QUIC engine 메모리까지 제한한다고 주장하지 않습니다.

### hntls·hnquic 직접 연동

HTTP/3 DATA read는 대기 전에 connection 공간을 예약하고 stream별로 나누어 사용합니다.
작은 연결 버퍼에서도 여러 read가 같은 공간을 중복 배정받지 않으며, 이는 HTTP/2 window 상태와 별개입니다.

TLS record 처리 코드를 hnhttp에 넣지 않습니다. hntls의 `IByteStream`/`ITlsStream` 계약이
암호화 transport와 복호화된 application bytes의 경계를 소유합니다.

```cpp
#include <hnhttp.hpp>
#include <hntls/stream.hpp>

// Enter the raw stream's serial executor first.
auto records = hnhttp::IHttpStream::asTlsTransport(rawSocketStream);
auto tls = hntls::ITlsStream::create(records, tlsContext);
if (co_await tls->handshake() != hntls::ERET_OK) { tls->close(); co_await tls->waitClosed(); co_return; }
auto stream = hnhttp::IHttpStream::fromTls(tls);
```

HTTP/3는 `IHttpQuicTransport::fromQuic(nativeConnection)`으로 저장소의 `hnquic::IQuicConnection`을
연결합니다. caller가 native QUIC `run()`과 HTTP `run()`을 각각 감독·join합니다. HTTP 쪽이 UDP packet,
TLS handshake 또는 숨겨진 native run task를 만들지 않습니다. confirmed handshake와 ALPN `h3`가 필요하고
client는 transport가 보고한 peer authentication도 확인합니다. 미확립/미확인 native 연결은 어댑터 생성 시 거부됩니다.

`IQpackCodec`는 transport를 소유하지 않습니다. encoder instruction bytes를 encoder stream에 보내고,
peer instruction fragments를 `receiveEncoderInstructions/receiveDecoderInstructions()`에 전달합니다.
`decodeSection()`의 BLOCKED는 손상 오류가 아니며 caller가 bounded block을 보관해 insert 진행 후 재시도합니다.
`takeDecoderInstructions()`로 ACK/increment를 가져옵니다. HTTP/3 엔진이 이 연결 작업을 수행합니다.

## 인증서·오류·재시도

transport가 `STransportSecurity`에 `shared_ptr<const certpp::x509::CCert>` peer chain을 제공합니다.
leaf-first snapshot을 context에서 조회하며 인증서 객체가 외부 mutable alias로 변경되지 않는다는
계약이 필요합니다. hnhttp는 인증서 parse·trust store·hostname·path validation을 수행하지 않습니다.
`EPEER_TRANSPORT_ACCEPTED`도 **transport가 보고한 결과**이며 hnhttp의 별도 검증 결과가 아닙니다.
형식 전달은 forward declaration으로 가능하며 인증서 메서드를 사용하는 애플리케이션이 libcertpp 헤더/target을 연결합니다.

프로토콜/전송/limit/deadline/cancellation은 `ERetCode`와 복사 `SHttpError`로 보고합니다.
상세 정보에는 native error, 오류 범위와 stream ID, 원래 protocol error, retry evidence가 있습니다.
4xx/5xx는 정상적으로 받은 HTTP 응답이므로 `exec()==ERET_OK`입니다.
로컬 builder의 잘못된 값/동결 후 변경은 `invalid_argument/length_error/logic_error`로 보고합니다.
메모리 할당·외부 callback 같은 예외는 관련 작업을 실패 상태로 만들고 정리하며, 내부 pump에서는 오류로 변환합니다.

H1의 중간 메시지 EOF, 모호한 framing, 파싱 중 취소는 연결을 재사용하지 않습니다.
H2/H3의 stream error와 compression/connection error는 분리합니다.
GOAWAY는 H2의 last-processed stream, H3 서버의 first-rejected request, H3 클라이언트의
first-rejected push boundary가 다르므로 `SGoawayInfo`에 그 의미를 보존합니다.
처리되지 않았다는 근거가 있어도 non-idempotent 요청이나 body를 **자동 재전송하지 않습니다**.

클라이언트는 최종 response header가 도착해도 업로드가 진행 중일 수 있습니다. `exec()`는
연결/교환이 소유하는 supervised sender를 시작하고 헤더를 반환하며, H1 sender도 unowned detached
task가 아니어야 합니다. 먼저 response body를 소비하고 `waitSent()`로 송신 완료/오류를 확인합니다.
응답을 소비하지 않고 업로드부터 전부 기다리면 양방향 backpressure가 교착할 수 있으므로 그 순서를
강요하지 않습니다. 최종 헤더 이후에 발생한 송신 실패도 waitSent/finish에서 관찰 가능해야 합니다.
아직 송신 중인 finish는 BODY_PENDING을 반환할 수 있습니다. 필요하면 명시적인 abort로 교환을 끝냅니다.
최종 외부 facade 소유자가 사라질 때 private task state의 참조 때문에 취소가 영원히 지연되는 순환 소유도 금지합니다.

## 헤더·대상·본문 규칙

- 헤더 이름은 case-insensitive lookup, field line 순서와 duplicates는 보존합니다.
  `get`은 첫 값, `values/entries`는 최대 4096개의 페이지 복사이며 Set-Cookie를 comma-join하지 않습니다.
- mutable facade는 CR/LF/NUL injection, pseudo-fields와 Host/Content-Length/Transfer-Encoding/
  Connection/Trailer/Expect 같은 serializer-managed field의 수동 변경을 거부합니다.
  authority는 `request->authority(...)`, body framing은 send options/source metadata로 설정합니다.
  source Content-Type과 충돌하는 임의 override도 보내기 전에 거부합니다.
- HTTP/2·3의 pseudo-fields는 target/method/status에 매핑하고 ordinary header로 노출하거나 삽입하지 않습니다.
  연결 전용 field·금지된 transfer coding·pseudo-field 순서/중복/필수 항목을 버전별로 검사합니다.
- HTTP/1은 strict CRLF, obs-fold 거부, TE+CL 거부, duplicate CL 거부를 기본 정책으로 둡니다.
  Content-Length/chunked/close-delimited, HEAD·1xx·204·304·CONNECT의 framing을 분리합니다.
  HTTP/2·3도 Content-Length가 있으면 실제 entity length와 비교하고 chunked를 사용하지 않습니다.
- `path`는 escaped path이며 `?`·fragment나 percent triplet을 임의로 decode/normalize하지 않습니다.
  query facade는 decoded octets를 받고 '%20' 방식으로 URI component escaping합니다. query '+'는
  literal이고 URL-encoded form의 '+'=space와 별개입니다. 원본 target도 별도 보존합니다.
- HTTP/1.1·2·3 client는 명시적 authority가 필요합니다. 이미 열린 stream만으로 Host를 추측하지 않습니다.
  CONNECT authority-form, OPTIONS '*', proxy absolute-form은 target 값과 메서드·버전에 맞춰 검증합니다.
- `status(404)`와 `status(404,"Not Found")`를 모두 제공합니다. H2/H3에는 reason phrase를 전송하지 않습니다.
  101은 HTTP/1.1 `acceptUpgrade()` 전용이며 일반 정보 응답은 `sendInformational()`로 분리합니다.
- entity source는 `prepare()` 후 metadata를 고정하고 작은 owned chunk로 읽습니다.
  known length와 실제 EOF를 검증하며 unknown length를 0으로 취급하지 않습니다.
  H1.1 unknown length는 chunked, H1.0 unknown-length request는 unsupported, H1.0 response는 close-delimited;
  H2/H3는 DATA/END_STREAM 또는 QUIC FIN과 trailer HEADERS로 종료합니다.
- 현재 `Expect: 100-continue` 송신 옵션은 UNSUPPORTED입니다. H2/H3 provisional responses는 bounded
  observer로 전달하며 서버의 `sendInformational()`도 구현했습니다. 임의 timeout 후 자동 재시도하지 않습니다.
- incoming body는 transfer framing만 제거합니다. gzip/br/zstd 등 Content-Encoding이나 JSON은
  자동 해독하지 않습니다. 필요한 payload codec은 별도의 hncomp 어댑터로 추가할 수 있습니다.

## form과 binary

binary source와 multipart·URL-encoded 송수신이 구현되어 있습니다.

`IFormData::create()`는 multipart/form-data, `create(EFORM_URLENCODED)`는 urlencoded source입니다.
`addField`는 중복 이름을 보존하고 `addFile`은 host filename이 아닌 **전송할 filename metadata**와
별도 body source를 받습니다. URL-encoded form에 파일을 넣는 것은 거부합니다.
raw binary는 `IHttpBodySource::fromBytes(bytes, "application/octet-stream")` 또는 사용자 source로 처리합니다.

multipart boundary는 한 번 선택해서 충돌을 무시하지 않습니다. 전송 전 bounded candidate preflight로
모든 part를 검사하고 rewind 가능한 source만 form part로 허용합니다. one-shot file part는 명시적으로
거부하며, 이를 지원하려고 본문 전체를 몰래 RAM/파일에 spool하지 않습니다. 일반 raw body는 one-shot일 수 있습니다.
경계 탐지는 보수적으로 `--boundary` substring 충돌도 거부합니다. 사용자 지정 경계 충돌은 BOUNDARY_COLLISION이고,
자동 경계는 maximumBoundaryAttempts 내에서 다시 선택합니다. 사용자 source는 prepare 이후 같은 bytes를 재생해야 합니다.
이후 파일-backed source를 추가한다면 mmap/MemoryMapped IO의 bounded sliding window와 런타임 크기 옵션을 사용해야 합니다.

수신 `IHttpFormReader::next()`는 한 part를 반환하고 이전 part를 소비/명시적 discard하기 전에는
BODY_PENDING을 반환합니다. part body는 다음 part로 넘어갈 수 없습니다. form EOF 전 closing boundary,
bounded epilogue와 enclosing HTTP body EOF를 확인합니다. filename은 경로로 신뢰하거나 자동 저장하지 않습니다.
중첩 multipart와 응용 데이터 decode는 자동 수행하지 않습니다. form payload 오류와 HTTP framing 오류를 구분합니다.

## CONNECT·Upgrade

아래는 향후 계약이며 현재 tunnel/handoff 구현은 UNSUPPORTED입니다. CONNECT client 송신도 거부합니다.

성공한 CONNECT는 `takeTunnel()`로 application DATA channel을 넘깁니다. HTTP/2·3의 터널은 해당
stream만 차지하며 다른 교환을 중단하지 않습니다. extended CONNECT/WebSocket handshake·capsule·HTTP Datagram
확장은 이 초기 계약에 포함하지 않습니다.
HTTP/1의 검증된 101/CONNECT에는 별도 `IHttp1Handoff` capability가 있어 raw stream을 detach할 수 있습니다.
반드시 read-ahead bytes를 함께 반환하며 H2/H3 공유 연결을 raw stream으로 탈취할 수 없습니다.
`SStreamHandoff::stream`도 shared pointer이지만 handoff 결과는 move-only입니다. 기존 connection이
I/O 사용권을 포기한 뒤 takeover 소비자에게만 사용권이 넘어가며 기존 shared alias는 I/O하면 안 됩니다.

## 예제와 검증 단계

`examples/http1.cpp`는 서버/클라이언트 흐름과 기본 multipart source를 컴파일합니다. HTTP/1.1의
필수 authority 설정, response body 소비와 finish를 예제에 보충했습니다.
`examples/multiplexed.cpp`는 HTTP/2/3 transport 차이와 supervised run/join을 보여줍니다.
event loop 안에서 future.get()으로 pump를 막지 않으며, 예외 경로도 close 후 join합니다.
`examples/native_transport.cpp`는 hntls·hnquic 인스턴스를 실제 HTTP 어댑터 factory로 연결합니다.
이미 handshake된 hntls 또는 confirmed hnquic 연결을 요구합니다.
`examples/socketpp_reactor.cpp`는 CSocket → CSocketAsync → 호출자 제공 IHttpStream 어댑터 연결,
worker 왕복과 동일 HTTP/1 연결의 두 요청 처리를 컴파일로 확인합니다. 어댑터 factory는 호출자가 주입하는
계약이며 예제 자체는 어댑터를 주입받습니다. 구현된 `CSocketHttpStream::create()`를 주입할 수 있습니다.
public endpoint 헤더가 socketpp 타입을 사용하므로 hnhttp target은 socketpp를 public dependency로 연결합니다.
`examples/plaintext.cpp`는 현재 구현된 factory만 사용하는 URL-encoded POST/echo 흐름입니다.
호출자가 handler/task를 supervise하고 watcher를 계속 구동해야 하며 HTTP 호출 안에서 event loop를 중첩 실행하지 않습니다.
`examples/http2_server.cpp`는 loopback에서 한 연결·한 요청을 처리하는 실행 예제입니다. 포트를 인자로 받으며
0이면 OS가 선택한 포트를 `port=N`으로 출력합니다. TLS 없이 `curl --http2-prior-knowledge` 등으로 검증할 수 있습니다.

```sh
cmake -S . -B build
cmake --build build --target hnhttp_contract_tests
ctest --test-dir build -R hnhttp.contract --output-on-failure
# Optional single-request HTTP/2 echo server:
cmake --build build --target hnhttp_http2_server
./build/libs/hnhttp/hnhttp_http2_server 8080
```

각 헤더의 독립 컴파일, 예제의 타입 연결, role/transport 분리, coroutine signature와 기본 resource/trust/error 값을 확인합니다.
실제 CSocketWatcher와 worker thread의 coroutine 왕복·예외 복귀, native TCP의 binary 128 KiB 요청/응답과
동일 연결 두 번 재사용, pending accept/head-read 종료를 실행 검증합니다. 별도로 1-byte fragmentation,
partial write, chunk/trailer와 정확한 응답 bytes, buffered pipelined request 보존, ambiguous framing·bare LF·obs-fold·
truncation 거부를 테스트합니다. HPACK는 RFC 7541 C.4의 Huffman 연속 요청, QPACK는 RFC 9204 B.1~B.5의
독립 bytes·post-base·duplicate·eviction과 잘못된 padding/EOS/feedback를 검증합니다. H2/H3 동시 대용량 교환,
작은 stream window, PING/SETTINGS ACK, H2 native TCP, 독립 malformed frame·H3 critical FIN/중복 SETTINGS,
multipart 1-byte 분할·충돌·잘림과 4,096개 deterministic compression mutation도 포함합니다.
H2/H3 trailer HEADERS·정확한 collect 상한과, H3의 완료된 조기 응답이 이후 업로드 오류로 지워지지 않는지도 확인합니다.
Linux에서 실행 예제에 curl 8.5.0/nghttp2 1.59.0의 `--http2-prior-knowledge` POST를 보내
HTTP/2 200과 동일한 echo 본문을 확인했습니다. curl/nghttp2는 검증 도구일 뿐 라이브러리 의존성이 아닙니다.
doctest와 stderr/stdout runner를 사용하며 Windows dialog는 비활성화합니다.
메모리 전송 테스트와 별도로 실제 hntls TCP의 HTTPS HTTP/1·2 및 hnquic UDP의 HTTP/3 endpoint를 검증합니다.
독립 Python/OpenSSL 피어의 TLS 인증·ALPN 및 HTTP/1·2 wire echo는 `tests/secure_interop.py`로 검사합니다.
독립 QUIC/HTTP/3 구현과의 상호운용은 아직 검증하지 않았습니다.
테스트가 exhaustive close/cancel race, 성능·취약점·전체 표준 호환성을 입증하지 않습니다.

기본 H1/H2/H3 프로토콜 경로는 구현되었지만 전체 기능이 완료된 것은 아닙니다. 후속 범위는 다음과 같습니다.

1. 명시적으로 미지원인 Expect·HTTP/1 trailer/informational·CONNECT/Upgrade와 local live SETTINGS 정책
2. optional per-operation cancellation/deadline, 더 넓은 close/cancel race 및 fairness 검증
3. hntls·hnquic의 명시된 초기 profile 확장과 독립 QUIC/HTTP/3 피어 상호운용
4. 독립 구현 상호운용·추가 fuzz corpus·성능 계측·외부 보안 검토

HPACK는 RFC 7541 Appendix C, QPACK는 RFC 9204 Appendix B의 독립 예시와 negative vectors를 검증합니다.
HTTP/1 request smuggling 입력, H2 continuation/flow error, H3 critical-stream/SETTINGS/QUIC ID/QPACK blocking,
취소·GOAWAY·미완료 본문·use-after-context 수명을 각각 테스트해야 합니다. 자기 자신과의 round trip만으로
호환성이나 보안을 주장하지 않습니다.

## 보안 endpoint 실행

`IHttpClient::createTls(watcher, tlsPolicy, options)`와 서버의 동일 factory는 HTTP/1 또는 HTTP/2를
지원합니다. 정책의 ALPN은 해당 버전의 단일 값(`http/1.1`, `h2`)이어야 합니다. 클라이언트는
명시적인 trust anchor·serverName과 peer authentication을 요구합니다. connect/acceptConnection이
성공하기 전에는 HTTP connection을 공개하지 않습니다. 인증 실패는 `ERET_AUTHENTICATION`입니다.

HTTP/3은 `createQuic(watcher, tlsPolicy, options, quicOptions)`를 사용합니다. ALPN은 `h3`이며,
서버 QUIC role과 적어도 세 개의 uni-stream credit이 필요합니다. 호출자가 native endpoint의
`run()`을 supervise한 뒤 connect/acceptConnection을 진행하고, 획득한 HTTP multiplexed connection의
별도 `run()/ready()`도 supervise합니다. 숨겨진 스레드나 중첩 event loop는 없습니다.
평문/TLS endpoint의 `run()`은 `ERET_UNSUPPORTED`입니다.

TCP/TLS listener를 닫아도 이미 반환한 HTTP connection은 독립적으로 사용할 수 있습니다.
QUIC listener는 공유 UDP 소켓을 소유하므로 닫으면 그 소켓의 accepted connection도 종료됩니다.
종료 시 connection/endpoint를 close하고 모든 pump와 `waitClosed()`를 join한 뒤 watcher를 멈춥니다.
컴파일 검증되는 흐름은 `examples/secure_client.cpp`, 독립 wire 검사 서버는 `examples/secure_server.cpp`입니다.
예제의 임시 자체 서명 인증서는 테스트 목적이며 production trust policy가 아닙니다.

## 업로드·다운로드와 조건부 요청

multipart 파일은 `IFormData::addFile(name, filename, source)`로 지정합니다. filename은 신뢰할 수 없는
wire metadata이며 서버 경로로 자동 사용하지 않습니다. URL-encoded 형식에는 파일 part 개념이 없으므로
`addFile()`은 거부합니다. 필요한 octet data는 percent-encoded KV 값으로 전달할 수 있습니다.
`IHttpFormReader::next()`로 이름·filename·body를 한 part씩 읽으며 중복 이름과 빈 값도 보존합니다.
파일 시스템 저장·읽기는 애플리케이션 책임이고 현재 파일 경로를 여는 body source는 제공하지 않습니다.
따라서 파일 테스트는 실제 네트워크로 전달하는 binary file payload를 검증하며 디스크 저장 테스트가 아닙니다.

`CHttpResource::evaluate(method, request.headers(), metadata)`는 선택된 리소스에 대해 다음을 반환합니다.

- `If-Match` strong 비교, `If-None-Match` weak 비교, 목록·와일드카드, 우선순위에 따른 `304`·`412`
- `If-Modified-Since`·`If-Unmodified-Since` 날짜 비교와 세 가지 HTTP-date 수신 형식
- 단일 bytes Range의 시작/끝·suffix·열린 끝, `206` offset/length와 Content-Range, 만족 불가 `416`
- strong ETag 또는 명시적으로 strong으로 지정된 Last-Modified를 사용하는 `If-Range`

호출자는 인증·라우팅·일반 오류 검사 후 이 함수를 실행하고 반환 status/headers를 response에 적용한 뒤,
`sendBody`일 때 정확히 offset/length 범위의 source를 제공합니다. HEAD는 본문을 보내지 않습니다.
수정 요청의 validator 비교와 실제 변경의 원자성은 애플리케이션 책임입니다. ETag 생성, 캐시 보관,
freshness 계산·자동 재검증·파일 송신은 하지 않습니다. Cache-Control/Expires/Vary는 그대로 보존·송수신합니다.
다중 범위·알 수 없는 단위·잘못된 Range는 무시하고 전체 `200`을 선택하며 multipart/byteranges는 미구현입니다.
허용되지 않는 조건 태그 구문은 `400`입니다.

HTTP/1의 기본 strict 정책은 POST/PUT/PATCH 또는 Content-Type이 있는 요청에 Content-Length와 chunked
모두 없으면 `411 Length Required`, Connection: close를 전송하고 `ERET_LENGTH_REQUIRED`로 거부합니다.
`options.http1.requireExplicitRequestBodyLength=false`이면 RFC 9112의 헤더 뒤 길이 0 해석을 적용합니다.
이 정책은 도착한 뒤의 bytes를 추측하지 않습니다. 명시적인 Content-Length: 0과 chunked 요청은 유효합니다.
HTTP/2·3의 길이 미상 본문은 END_STREAM/FIN으로 구분하므로 Content-Length 없이도 허용합니다.
길이 미상 HTTP/1.1 응답은 자동 chunked 송신되며 마지막 `0\r\n\r\n`까지 전송합니다.
HTTP/1.0 연결 재사용은 양쪽에서 `options.http1.allowHttp10KeepAlive=true`를 지정하고
상대의 Connection: keep-alive를 확인한 경우만 허용합니다. 기본은 기존처럼 연결 종료입니다.

`tests/resource_scenario.hpp`는 실제 HTTPS HTTP/1·2 및 UDP HTTP/3의 동일 연결에서 20개 요청을 처리합니다.
100KB binary multipart·URL-encoded KV·원시 업로드·길이 미상 송수신·전체/범위 다운로드·조건부 요청·HEAD를
순차 실행하고 각 응답 본문과 헤더, finish 후 재사용 가능 상태를 확인합니다. 별도 테스트는 동시 H2/H3 요청,
HTTP/1.0 keep-alive, chunked 원시 출력과 다음 응답 경계, unframed 요청의 411/close를 검사합니다.

```sh
cmake --build build --target hnhttp_contract_tests hnhttp_secure_server
ctest --test-dir build -R hnhttp.contract --output-on-failure
python3 libs/hnhttp/tests/secure_interop.py build/libs/hnhttp/hnhttp_secure_server
```

Windows/IOCP와 Linux/epoll의 socketpp·hntls·hnquic·hnhttp 관련 회귀 89개,
Linux ASan/UBSan·누수 검사 62개를 통과했습니다. 설치된 CMake package만 사용하는 별도 소비자도
신규 endpoint factory와 조건부 요청 API를 컴파일·링크·실행 검증했습니다.

## 규격

- [RFC 9110: HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110.html)
- [RFC 9112: HTTP/1.1](https://www.rfc-editor.org/rfc/rfc9112.html)
- [RFC 9113: HTTP/2](https://www.rfc-editor.org/rfc/rfc9113.html), [RFC 7541: HPACK](https://www.rfc-editor.org/rfc/rfc7541.html)
- [RFC 9114: HTTP/3](https://www.rfc-editor.org/rfc/rfc9114.html), [RFC 9204: QPACK](https://www.rfc-editor.org/rfc/rfc9204.html)
- [RFC 7578: multipart/form-data](https://www.rfc-editor.org/rfc/rfc7578.html)
- [WHATWG URL: URL-encoded forms](https://url.spec.whatwg.org/#application/x-www-form-urlencoded)

이 문서는 설계 계약의 단일 소유 문서입니다. 구현이 시작되면 실제 availability와 검증 결과를 별도로
갱신하고 아직 구현되지 않은 기능을 지원 완료로 표시하지 않아야 합니다.
