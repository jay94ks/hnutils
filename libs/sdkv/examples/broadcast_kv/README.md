# Four-host UDP broadcast / shared SDKV example

이 예제의 목적은 **발견된 호스트들이 같은 애플리케이션 KV를 보유**하는 것입니다.
IP 목록만 SDKV에 넣는 예제가 아닙니다.

- 발견·전파: socketpp의 IPv4 UDP broadcast
- 공유 데이터 저장·조회·변경 이벤트: SDKV
- 비동기 실행: asyncpp coroutine + CSocketWatcher
- 광고 주기: **1초**
- 원격 레코드 TTL: **소유자의 마지막 새 비콘 수신부터 5초**

## 공유되는 데이터

| 키 | 작성자 | 모든 호스트에서 보이는 기본 값 |
| --- | --- | --- |
| `demo/host1` | host1 | `hello-from-host1` |
| `demo/host2` | host2 | `hello-from-host2` |
| `demo/host3` | host3 | `hello-from-host3` |
| `demo/host4` | host4 | `hello-from-host4` |
| `cluster/message` | host1 | `shared-start` → 2초 뒤 `shared-updated` |

각 프로세스는 독립된 `ITable::create()` 인스턴스를 갖습니다. 정상 수렴 후
네 테이블 모두 위 다섯 키와 같은 값들을 보유합니다. 값은 IP 주소가 아닌
실제 애플리케이션 데이터입니다. `--value TEXT`로 해당 호스트의
`demo/<id>` 값을 지정할 수 있습니다.

IP는 UDP 수신 source address에서 얻어 통신 계층의 별도 discovery metadata로
관리합니다. 패킷에 자기가 주장하는 IP를 싣지 않습니다. 키의 소유자는 고유한
`--id`로 구분하므로 한 머신에서도 네 프로세스를 실행할 수 있습니다.

## 빌드

저장소 루트에서:

```sh
cmake -S . -B build -DSDKV_BUILD_EXAMPLES=ON
cmake --build build --config Debug
```

Ninja 기준 실행 파일:

- Linux: `build/libs/sdkv/examples/sdkv_example_broadcast_kv`
- Windows: `build/libs/sdkv/examples/sdkv_example_broadcast_kv.exe`
- 다중 구성 생성기는 `examples/Debug/` 등 구성별 하위 폴더를 사용합니다.

예제 옵션은 기본 OFF입니다. 예제만 socketpp에 의존하며 SDKV 라이브러리
자체에는 네트워크 의존성이나 프로토콜이 추가되지 않습니다. 예제 실행 파일과
지원 코드는 설치하지 않습니다.

## 실제 4개 호스트에서 실행

동일 IPv4 broadcast 도메인의 각 호스트에서 하나씩 실행합니다.
동일한 UDP 포트를 사용하고, `--id`는 중복되지 않게 지정합니다.

```sh
# 머신 1
sdkv_example_broadcast_kv --id host1 --port 45454
# 머신 2
sdkv_example_broadcast_kv --id host2 --port 45454
# 머신 3
sdkv_example_broadcast_kv --id host3 --port 45454
# 머신 4: 3초 후 종료하여 TTL 만료를 관찰
sdkv_example_broadcast_kv --id host4 --port 45454 --duration 3
```

기본 broadcast 목적지는 `255.255.255.255`입니다. 인터페이스가 여러 개라면
`--interface 192.168.1.10`으로 **해당 머신의 실제 로컬 IP**를 지정하고,
필요하면 `--broadcast 192.168.1.255`처럼 실제 서브넷의 broadcast 주소를
선택합니다. 인터페이스·서브넷 주소는 애플리케이션이 자동 추정하지 않습니다.

UDP 송수신을 방화벽에서 허용해야 합니다. 라우터, VPN, WSL/가상화 모드,
무선 AP의 client isolation 때문에 broadcast가 전달되지 않을 수 있습니다.
예제는 방화벽이나 네트워크 설정을 변경하지 않습니다.

host4가 멈추면 마지막 비콘으로부터 5초 뒤 나머지 테이블에서
`demo/host4`가 삭제됩니다. 다른 호스트의 비콘이 이 레코드를 재전파하여
유효기간을 무한 연장하지 않습니다.

## SDKV 이벤트로 상태 관찰

출력 예:

```text
[host2] discovered host1 ip=192.168.1.10
[host2] ADD key="cluster/message" prev=<absent> now="shared-start" revision=1
[host2] CHANGE key="cluster/message" prev="shared-start" now="shared-updated" revision=2
[host2] REMOVE key="demo/host4" prev="hello-from-host4" now=<absent> revision=2
```

`CConsoleListener`는 SDKV가 전달하는 `prev/now`를 그대로 소비합니다.
같은 값의 주기적 비콘은 TTL만 갱신하며 SDKV revision이나 변경 이벤트를
추가하지 않습니다. 원격 수신은 `addAsync/setAsync`, 만료는 `removeAsync`를
통하므로 조회·revision·추가/변경/삭제 이벤트가 SDKV에 의해 일관되게 관리됩니다.
애플리케이션은 테이블에서 `get()`으로 원하는 공통 값을 조회할 수 있습니다.

각 테이블의 revision은 로컬 변경 이력입니다. 서로 다른 호스트의 revision
숫자가 같아야 한다는 보장은 없으며, 이 예제는 동일 키/값으로의 수렴을 보여줍니다.

## 네 프로세스 자동 시나리오

```sh
ctest --test-dir build -C Debug -R sdkv.example --output-on-failure
```

상태 전이/코덱/TTL 단위 테스트는 doctest입니다. 추가 smoke test는 실제
프로세스 4개를 동시에 실행해 실제 UDP broadcast로 통신시킵니다.

1. 운영체제가 선택한 사용 가능한 공통 포트로 네 프로세스를 시작합니다.
2. 모든 테이블에 다섯 키가 나타나고 `cluster/message` 갱신이 전달되는지 확인합니다.
3. host4는 3초 후 종료합니다.
4. 나머지는 12초 동안 실행해 host4의 키가 만료되고 나머지 네 키는 유지되는지 확인합니다.

같은 머신의 네 프로세스는 IP가 같을 수 있습니다. 이것은 네 물리 머신의
방화벽·라우팅을 검증하는 테스트가 아닙니다. stdout/stderr로만 결과를 보고하며
Windows 오류 대화상자와 debugger break를 끕니다.

## 설계 경계

- 키마다 작성자는 하나입니다. `demo/<id>`는 그 id, `cluster/message`는
  host1만 작성합니다. 임의의 동시 작성 충돌 해결이나 합의 프로토콜은 없습니다.
- 비콘은 **자기가 소유한 키들의 전체 스냅샷**입니다. 캐시된 원격 키는
  재광고하지 않습니다. 새 스냅샷에서 빠진 소유 키는 다음 수신 때 삭제합니다.
- 손실된 갱신/삭제는 다음 1초 스냅샷에서 복구됩니다. 새로 시작하거나
  TTL 만료 후 재발견한 호스트도 별도의 연결 없이 현재 값을 받습니다.
- incarnation과 증가 sequence로 이미 수신한 중복/역순 패킷이 TTL을
  갱신하지 않게 합니다. 이미 관찰한 이전 incarnation으로의 회귀도 무시합니다.
- TTL은 로컬 steady clock을 사용하며 원격 시계 동기화에 의존하지 않습니다.
  이벤트 루프가 대략 20ms마다 실행될 때 만료를 확인합니다.
- 수신은 기존 `CSocket`의 nonblocking API로 최대 64개씩 처리하고, 송신은
  `CUdpBroadcastAsync`를 await합니다. 종료 시 취소해야 하는 pending receive가
  없으며, main은 마지막 tick 완료 후 소켓과 watcher를 해제합니다.
- 데이터 인코딩·디코딩, 소유권, 네트워크, TTL은 **예제 애플리케이션 정책**입니다.
  SDKV 코어는 계속 불투명 바이트 저장소이며 socketpp는 소켓 계층으로 남습니다.
- 예제의 node/replica 작업은 단일 watcher 이벤트 루프에서 직렬 실행합니다.
  각 coroutine 완료 전에 다른 작업이나 소멸을 시작하지 않습니다. 애플리케이션
  쓰기는 `setOwned()`를 사용하며, `table()`은 조회·구독 용도로 사용합니다.
- 인증·암호화·영속성·원자적 다중 키 갱신은 없습니다. 신뢰할 수 있는 LAN의
  데모이며, owner id를 위조하는 악성 송신자를 방어하는 보안 프로토콜이 아닙니다.
- SDKV 값 제한은 4096바이트지만 이 데모는 조각화를 피하기 위해 값 512바이트,
  패킷 1200바이트로 더 작게 제한합니다. 대형 UDP 패킷은 안전하게 버립니다.
