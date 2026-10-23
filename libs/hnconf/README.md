# hnconf

nginx 스타일 설정 문서를 읽고 편집하고 쓰는 C++20 라이브러리입니다.
네임스페이스는 `hnconf`, CMake target은 `hnconf::hnconf`, umbrella header는
`<hnconf.hpp>`입니다. socketpp처럼 상태 객체는 private PIMPL을 사용하며,
OS 타입이나 내부 컨테이너의 mutable reference를 노출하지 않습니다.

## 책임과 문법

- `CConfigNode`: 이름, 순서 있는 문자열 인자, 지시어/블록 구분, 자식 노드.
- `CConfig`: 문서 파싱, 조회·편집, 정규화된 직렬화, 동기 파일 읽기·쓰기.
- `CConfigAsync`: asyncpp 실행기에 작업을 넘기는 coroutine 어댑터.
- `CParseError`: source, 1-based line/byte column, 0-based byte offset 및 `what()`.
- `SParseOptions`, `SWriteOptions`: 파싱 리소스 제한 및 출력 정책.

단순 지시어는 `name arg1 arg2;`, 블록은 `name args { ... }` 형태입니다.
이 구조는 [nginx 공식 설정 문법 안내](https://nginx.org/en/docs/beginners_guide.html#conf_structure)를
기준으로 합니다. 같은 이름의 지시어와 블록, 인자의 순서를 보존합니다.
빈 블록과 인자가 없는 지시어, 빈 인자 `""`/`''`를 구분합니다.

```nginx
worker_processes auto;
http {
    server {
        listen 8080;
        listen 8443 ssl;
        server_name localhost;
        location / { root "./www root"; }
    }
}
```

문자열 처리 규칙은 [nginx의 토큰 처리 구현](https://github.com/nginx/nginx/blob/master/src/core/ngx_conf_file.c)을
참고합니다. 따옴표는 토큰 시작에서 인용 구문을 시작하며, 인접 문자열의 shell식
결합은 하지 않습니다. `#`는 토큰 사이에서 줄 끝까지 주석이며, `foo#bar`와
인용부호 안의 `#`는 값입니다. `\n`, `\r`, `\t`, `\\`, `\"`, `\'`는 디코딩하고,
`\d`, `\$`, `\ `, `\;` 같은 다른 escape는 **backslash까지 보존**합니다.
backslash-newline을 줄 연결로 해석하지 않습니다. `$name`, `${name}`은 해석하지
않는 문자열이며, 정규식에 쓰는 구분용 중괄호는 인용하는 것이 안전합니다.

입력은 NUL을 제외한 바이트열입니다. UTF-8을 검증하거나 locale에 따라 변환하지
않습니다. CRLF와 LF를 읽으며 시작 위치의 UTF-8 BOM을 허용합니다. offset과 column은
BOM을 포함한 원본 바이트 기준이고 LF가 line을 증가시킵니다. 파일 경로는
`std::filesystem::path`로 받아 Windows의 native Unicode 경로를 지원합니다.

**nginx 자체가 아니며 설정 의미를 검증하지 않습니다.** `include`는 일반 지시어로
남고 파일을 자동으로 읽지 않습니다. 변수/환경변수 치환, glob, 모듈·context 검사,
타입 변환, 실행·reload 기능은 없습니다. 주석·공백·원래 따옴표 스타일은 보존하지
않습니다. 출력은 4칸 들여쓰기와 LF를 기본으로 하며 필요한 토큰을 안전하게
인용하므로 다시 읽었을 때 동일한 트리 값을 얻습니다.

## 동기 사용과 편집

```cpp
#include <hnconf.hpp>

auto config = hnconf::CConfig::load("app.conf");
// 메모리 입력: CConfig::parse("worker_processes auto;")
for (const auto index : config.find("worker_processes")) {
    auto node = config.nodes().at(index);
    node.setArguments({"4"});
    config.replaceNode(index, node);
}
config.addNode(hnconf::CConfigNode::block("events", {}, {
    hnconf::CConfigNode::directive("worker_connections", {"1024"})
}));
config.save("app.generated.conf");
auto text = config.serialize();
```

`nodes()`, `children()`, `arguments()`, `name()`은 소유권 있는 복사본을 반환합니다.
문서/노드 복사는 내부 immutable storage를 공유하지만, setter는 새 storage를
만들어 교체합니다. 따라서 복사본 변경이 원본이나 다른 snapshot을 변경하지 않습니다.
자식 수정은 `setChildren()`으로 부모에 반영하고, 마지막으로 문서의 `replaceNode()`를
호출합니다. 실행 가능한 중첩 편집 예제는 [`examples/config.cpp`](examples/config.cpp)에 있습니다.

별도 복사본은 독립적으로 사용할 수 있습니다. **같은 객체를 동시에 수정하거나
수정 중 읽는 것은 외부 동기화가 필요**합니다. 조회는 정확한 이름에 대한 모든
index를 반환하며, 점 경로·슬래시 경로 해석으로 이름을 제한하지 않습니다.

## 비동기 사용

```cpp
#include <hnconf.hpp>
#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>

asyncpp::thread_pool workers(2);
hnconf::CConfigAsync async(workers);
auto config = asyncpp::as_promise(async.load("app.conf")).get();
config.addNode(hnconf::CConfigNode::directive("daemon", {"off"}));
asyncpp::as_promise(async.save(config, "app.generated.conf")).get();
// coroutine 내부에서는 co_await async.load(...) / async.save(...) 사용.
```

`parse`, `load`, `serialize`, `save`는 모두 `asyncpp::task<T>`를 반환합니다.
작업 시작 시 항상 전달된 dispatcher에 enqueue하며 파싱과 파일 I/O를 그 실행기에서
수행합니다. OS의 native async file I/O가 아닌 **worker offload** 방식입니다.
파일 I/O에 네트워크 reactor를 전달하면 그 reactor를 막으므로 worker를 사용해야 합니다.
호출자 coroutine도 완료한 worker에서 이어지며 자동으로 원래 실행기로 돌아오지 않습니다.

문서 snapshot·텍스트·경로·옵션은 메서드 호출 시 task에 보관하므로 원본 문서나
어댑터를 먼저 파괴해도 됩니다. **dispatcher는 모든 task 완료까지 유지하고 구동해야
하며, 한 번 받은 continuation을 정확히 한 번 실행해야 합니다.** 실행 중인 task를
외부에서 파괴하는 취소는 지원하지 않습니다. pool 안에서 같은 pool 작업을 `.get()`으로
막지 말고 `co_await`하세요. 예제의 `.get()`은 worker 밖의 main thread에서만 사용합니다.

## 제한과 오류

기본 파싱 제한: 입력 16 MiB, 디코딩된 토큰 1 MiB, 전체 노드 100,000개,
지시어당 인자 4,096개, 중첩 블록 128단계입니다. `SParseOptions`로 조정할 수 있으나
중첩의 hard ceiling은 128입니다. 프로그램으로 만든 트리에도 이 깊이 제한을 적용합니다.
출력 기본 제한은 16 MiB이며 `SWriteOptions`로 변경합니다. 출력 들여쓰기는 0~16칸입니다.
0 제한은 해당 리소스를 허용하지 않는다는 의미이며, 입력 0바이트/노드 0개인 빈 문서는
가능합니다. 출력 크기와 입력 크기·토큰 제한은 별개이므로 큰 데이터를 읽으려면
읽기 옵션도 맞추어야 합니다.

- 구문/파싱 제한: `CParseError`. 부분 트리를 반환하지 않습니다.
- 잘못된 옵션·이름·NUL 값/경로: `std::invalid_argument`.
- 출력/트리 깊이 제한: `std::length_error`.
- 파일 열기·읽기·쓰기·닫기 실패: `std::system_error`.
- 잘못된 index: `std::out_of_range`; 지시어에 자식을 설정하면 `std::logic_error`.
- 할당 실패: `std::bad_alloc`. 비동기 작업에서도 예외가 task를 통해 전달됩니다.

`save()`는 직렬화를 성공시킨 뒤 파일을 생성/덮어씁니다. 직렬화 실패는 기존 파일에
영향을 주지 않지만, **파일 쓰기는 원자적 교체나 crash durability를 제공하지 않습니다.**
I/O 실패 시 부분 파일이 남을 수 있습니다. 기존 파일을 보존해야 한다면 다른 경로에
저장하고 호출자 정책으로 교체하세요. 디렉터리 생성과 파일 권한 관리는 호출자 책임입니다.

## 빌드와 검증

루트 CMake에서 C++20 컴파일러로 빌드합니다. 공개 의존성은 asyncpp뿐이며 socketpp나
daemonizer에 의존하지 않습니다. doctest는 테스트에만 사용합니다. Windows/Linux 지원.

```sh
cmake -S . -B build -DHNCONF_BUILD_EXAMPLES=ON
cmake --build build --config Debug --target hnconf_tests hnconf_example
ctest --test-dir build -C Debug -R hnconf --output-on-failure
```

`HNCONF_BUILD_EXAMPLES` 기본값은 OFF입니다. 테스트는 문법·escaping·순서·복사 격리,
자원 제한·byte corpus·파일 오류·Unicode 경로, 실제 asyncpp worker 및 lazy task 수명을
검증합니다. 출력은 stdout/stderr로 제한하고 Windows 실패 대화상자와 debugger break를
비활성화합니다. `BUILD_SHARED_LIBS=ON`도 지원합니다.

설치 후에는 `find_package(hnconf CONFIG REQUIRED)`와 `hnconf::hnconf`를 사용합니다.
