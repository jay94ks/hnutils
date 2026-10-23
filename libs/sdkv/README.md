# sdkv - Simple Distributed KV

C++20의 바이트 기반 Key-Value Collection 라이브러리입니다.
네임스페이스는 `distkv`, 공개 umbrella 헤더는 `<sdkv.hpp>`,
CMake 타깃은 `sdkv::sdkv`입니다.

현재 구현 범위는 메모리 내 테이블, 현재 로컬 상태에 대한 원자적 연산,
리스너 기반 테이블 간 동기화입니다. 네트워크/P2P, 데이터 인코딩·디코딩,
분산 토폴로지·합의·충돌 해결·영속 저장소는 제공하지 않습니다.

## 인터페이스

- `ITable::create()`: 내부 데이터가 캡슐화된 thread-safe 테이블을 생성합니다.
- `ITableListener`: 추가·실제 값 변경·삭제 콜백을 정의합니다.
- `IRemoteTable`: `ITable`을 상속하며 `isOnline()`, `isSynced()`를 정의합니다.
  구체적인 원격 어댑터와 상태 정책은 사용자가 구현합니다.
- `CTableChange`: 저장 가능한 불변 이벤트입니다. `prev()`, `now()`에 값과
  revision이 들어 있으며, 추가는 prev 부재, 삭제는 now 부재로 표현됩니다.
- `CTableSubscription`: 리스너 등록의 move-only RAII 핸들입니다.

## 현재 상태 기준 연산

| 연산 | 의미 |
| --- | --- |
| `get(key, output)` | 현재 값의 복사본을 반환. 없으면 output 유지 |
| `add(key, value)` | 없을 때만 등록. 존재하면 `ERET_ALREADY_EXISTS` |
| `remove(key)` | 존재하는 키 삭제. 없으면 `ERET_NOT_FOUND` |
| `set(key, value)` | 존재하는 값만 변경. 없으면 `ERET_NOT_FOUND` |
| `setIf(key, expected, value)` | 기대 상태와 일치할 때 원자적으로 등록·변경 |

`setIf`의 `expected = std::nullopt`는 키가 없어야 한다는 뜻입니다.
빈 바이트열은 **존재하는 빈 값**을 기대한다는 뜻으로, 부재와 다릅니다.
실패한 비교는 `ERET_CONDITION_FAILED`를 반환합니다.

키와 값은 모두 불투명한 바이트열이며 빈 키/값과 NUL 바이트도 허용합니다.
값은 최대 **4096바이트**입니다. 초과하면 `ERET_VALUE_TOO_LARGE`를 반환하며
조용히 자르거나 분할하지 않습니다. 큰 데이터의 여러 키 분할과 복원은
애플리케이션이 담당합니다. 키 길이 정책도 애플리케이션의 책임입니다.

## Revision과 리스너

각 키는 실제 추가·변경·삭제 때 1씩 증가하는 로컬 revision을 가집니다.
다른 키의 변경은 해당 키의 revision에 영향을 주지 않습니다.
삭제 이력을 유지하므로 삭제 후 재등록해도 revision이 1로 돌아가지 않습니다.
예를 들어 추가 1 → 변경 2 → 삭제 3 → 재등록 4가 됩니다.

동일 바이트열 재설정, 중복 전파, 실패한 연산은 revision을 증가시키거나
변경 콜백을 발생시키지 않습니다. 따라서 리스너가 직접 값의 변동을 비교할
필요가 없습니다. 삭제 이벤트의 새 revision은 `change.revision()`으로
확인할 수 있습니다. `ITable::revision()`은 별도로 테이블 전체의 실제
변경 횟수를 나타냅니다.

`get(key, STableValue&)`는 값과 revision을 함께 반환합니다.
`get(key, SBytes&)`는 값만 반환합니다. 반환 데이터·스냅샷·이벤트는 모두
소유한 복사본이며 내부 가변 데이터에 대한 접근을 제공하지 않습니다.

## 사용 예

```cpp
#include <sdkv.hpp>

distkv::SBytes key{0x01, 0x00};
distkv::SBytes first{0x10};
distkv::SBytes second{0x20};

auto source = distkv::ITable::create();
auto local = distkv::ITable::create();
source->add(key, first);
local->addSyncFrom(source); // 현재 항목을 overlay한 뒤 이후 변동을 수신
source->setIf(key, distkv::SReadOnlyByteSpan(first), second);

distkv::STableValue value;
if (local->get(key, value) == distkv::ERET_OK) {
    // 애플리케이션에서 value.bytes를 디코딩합니다.
}
local->removeSyncFrom(source);
```

`ITableListener`를 상속한 객체 역시 `listener->addSyncFrom(local)`으로
테이블을 구독할 수 있습니다. 이를 사용자 네트워크 전파 어댑터나 디코더에
사용하면 됩니다. 리스너는 `std::shared_ptr`로 생성하고 애플리케이션이
소유권을 유지해야 합니다.

## 비동기

`getAsync`, `addAsync`, `removeAsync`, `setAsync`, `setIfAsync`는
`asyncpp::task<T>`를 반환합니다. 인자 바이트열과 테이블 소유권을 task 생성
시점에 확보하므로 임시 인자나 호출자 소유권 해제로 인한 dangling을 방지합니다.

마지막 인자로 `asyncpp::dispatcher*`를 주면 그 실행기로 전환한 뒤 연산을
수행합니다. 실행기를 지정하지 않으면 await하는 스레드에서 수행하며 별도
스레드를 임의로 만들지 않습니다. 실행기는 작업이 끝날 때까지 유지하고
구동해야 합니다. 이 작업들은 로컬 연산의 완료이지 원격 합의나 전파 완료
acknowledgement가 아닙니다.

명시한 실행기는 현재 실행 중인 실행기와 같아도 항상 큐를 거칩니다.
그 실행기의 콜백 안에서 future를 동기적으로 기다리지 말고 코루틴으로 await하세요.

자세한 동기화·수명 계약은 [설계 문서](docs/design.md)를 참고하세요.

## 빌드·테스트·설치

저장소 루트에서:

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug -R sdkv --no-tests=error --output-on-failure
cmake --install build --prefix ./out/install --config Debug
```

소비자 프로젝트는 `find_package(sdkv CONFIG REQUIRED)`와
`target_link_libraries(app PRIVATE sdkv::sdkv)`를 사용합니다.
asyncpp는 공개 의존성, doctest는 테스트 전용 의존성입니다.
테스트 실패는 stdout/stderr로 출력하며 Windows 대화상자와 debugger break를
사용하지 않습니다.
