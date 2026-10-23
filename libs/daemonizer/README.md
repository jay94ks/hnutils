# daemonizer

`daemonizer`는 `daemonz` 네임스페이스의 C++20 데몬 실행 라이브러리입니다.
asyncpp 코루틴을 사용하며, Windows 재실행과 Unix fork를 같은 생명주기 및
부모·자식 IPC 인터페이스로 제공합니다.

## 공개 API

| 타입 | 역할 |
| --- | --- |
| `IDaemon` | 실행, signal 입력, 협력 종료 인터페이스와 `launch()` 진입점 |
| `CDaemon` | private PIMPL 기반 기본 구현과 비동기 생명주기 훅 |
| `IPipe` | 양방향 바이트 스트림의 `read`, `write`, `readExact`, `writeAll` |
| `IPipeServer` | 파이프 생성·소유·취소·종료를 담당하는 OS별 서버 |
| `SSignal` | 종류, 네이티브 코드, 발신 PID를 읽기 전용으로 보관하는 값 |
| `SDaemonOptions` | 호출자가 작성하고 실행 전에 검증·복사하는 설정 |
| `CDaemonContext` | 인자·프로세스 슬롯·부모/자식 파이프·worker 실행 API |

Windows에서는 `args[0]`의 실행 파일에 내부 플래그를 붙여 `CreateProcessW`로
다시 실행합니다. 자식 프로세스의 `launch()`가 내부 플래그를 해석하며, 훅에
전달하는 context에서는 이를 제거합니다. 부모 메모리의 변경은 Windows 자식에
복제되지 않으므로 필요한 상태는 원래 인자 또는 파이프로 전달해야 합니다.

Unix는 `fork()`와 비동기 non-blocking `socketpair`를 사용합니다. Windows는
overlapped named pipe를 사용하며 파이프·종료 event만 명시적으로 상속합니다.

## 사용 예제

```cpp
#include <daemonizer.hpp>

class CMyDaemon final : public daemonz::CDaemon {
protected:
    asyncpp::task<void> onStart(const daemonz::CDaemonContext& context) override {
        // Open process-local resources here, after fork/re-exec.
        co_await context.work([] { /* Perform startup work. */ });
    }

    asyncpp::task<void> onRun(const daemonz::CDaemonContext&) override {
        // Run asynchronous application work here.
        co_await waitForStop();
    }

    asyncpp::task<void> onSignal(daemonz::SSignal signal) override {
        if (signal.type() == daemonz::ESIG_RELOAD) {
            // Reload application configuration.
            co_return;
        }
        co_await daemonz::CDaemon::onSignal(signal);
    }

    asyncpp::task<void> onStop() override {
        // Release application-owned resources here.
        co_return;
    }
};

int main(int argc, char** argv) {
    daemonz::SDaemonOptions options;
    for (int i = 0; i < argc; ++i) options.args.emplace_back(argv[i]);
    options.max_fork = 2;
    options.max_threads = -1;
    options.max_reactors = 2;
    options.max_workers = 2;
    // options.detach = true; // Enable terminal detachment for deployment.

    CMyDaemon daemon;
    return daemonz::IDaemon::launch(daemon, options);
}
```

`onStart()`는 초기화를 마쳐야 합니다. 긴 작업은 signal 처리와 독립적으로
실행되는 `onRun()`에 두며, 종료 요청에 협력하도록 작성합니다. `onRun()`이
끝나면 인스턴스가 종료됩니다. `onStop()`은 시작·실행·signal 훅의 예외 경로에서도
호출됩니다. 비동기 훅에서 reactor를 블로킹하지 않도록 주의합니다.

## 설정 및 실행 계약

- `max_fork`는 추가 관리 자식 수입니다. `0`은 현재 인스턴스만 실행합니다.
  부모의 슬롯은 `0`, 자식은 `1..max_fork`입니다.
- 스레드 제한은 프로세스마다 적용합니다. 호출 스레드가 첫 reactor를 맡으므로
  `max_reactors=2`, `max_workers=2`이면 호출 스레드를 포함해 런타임 스레드가
  총 4개입니다. 사용자가 별도로 만든 스레드와 OS 내부 스레드는 관리하지 않습니다.
- `max_threads=-1`은 reactor·worker 합계를 사용합니다. 작은 예산, 음수 개수,
  reactor `0`, 합산 overflow는 실행 전에 stderr 오류와 종료 코드 `1`로 처리합니다.
  여유 예산이 있어도 불필요한 스레드를 만들지 않습니다.
- `max_workers=0`이면 worker 실행 요청은 `std::logic_error`로 실패합니다.
- 기본값은 foreground입니다. `detach=true`는 Unix에서 double fork와 `setsid`,
  Windows에서 내부 root 플래그를 사용한 detached 재실행을 수행합니다.
  Unix bootstrap fork는 관리 자식 슬롯에 포함하지 않습니다.
- 분리 모드의 원래 호출자는 프로세스 분리 성공 시 반환합니다. 이후 애플리케이션
  초기화 성공까지 확인하는 readiness 프로토콜은 애플리케이션에서 구성해야 합니다.
- Unix에서는 애플리케이션 스레드나 fork 이전에 유지해야 할 자원을 만들기 전에
  호출합니다. 인스턴스 자원은 `onStart()`에서 엽니다. Linux는 fork가 필요한
  경우 이미 다른 스레드가 있으면 거부합니다.
- Windows 인자와 `working_directory`는 UTF-8입니다. 비 ASCII 인자는 wide argv를
  UTF-8로 변환해서 넣어야 합니다. `args[0]`은 현재 실행 파일의 경로여야 합니다.

## 빌드 및 테스트

외부 의존성은 [`asyncpp`](../../refs/asyncpp/README.md)이며, 테스트만
[`doctest`](../../refs/doctest/README.md)를 사용합니다.

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

테스트는 stdout/stderr로 진단하며 Windows 오류 대화상자와 디버거 중단을
비활성화합니다. 실제 자식 프로세스와 분리 모드 테스트도 포함합니다.

설치 후에는 `find_package(daemonizer CONFIG REQUIRED)`와
`daemonizer::daemonizer` CMake target을 사용합니다.

자세한 [설계 및 수명 계약](docs/design.md)을 참고하세요.
