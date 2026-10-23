# hnutils

재사용 가능한 라이브러리와 CLI 도구를 함께 개발하기 위한 모노레포입니다.

## 저장소 구조

```text
.
|-- libs/   공용 코드와 재사용 가능한 라이브러리
|-- tools/  독립적으로 실행하거나 배포할 수 있는 CLI 도구
|-- docs/   저장소 전체의 설계, 계획, 사용법 문서
`-- refs/   외부 Git 저장소와 해당 저장소에 대한 패치 및 문서
```

- 새 라이브러리는 `libs/<name>`에 둡니다.
- 새 CLI 도구는 `tools/<name>`에 둡니다.
- 여러 프로젝트에 걸친 문서는 `docs`에 둡니다.
- 외부 저장소는 `refs/<repository>/repo`에 Git submodule로 연결하고,
  로컬 패치와 관련 문서는 각각 `patches`와 `docs`에서 관리합니다.

## 시작하기

submodule을 포함해 저장소를 복제합니다.

```sh
git clone --recurse-submodules https://github.com/jay94ks/hnutils.git
```

이미 저장소를 복제했다면 다음 명령으로 외부 저장소를 가져올 수 있습니다.

```sh
git submodule update --init --recursive
```

구체적인 빌드 및 테스트 명령은 도구와 라이브러리가 추가될 때 각 프로젝트의
README에 기록합니다.

## 현재 프로젝트

- [`libs/socketpp`](libs/socketpp/README.md): BSD 스타일 동기 소켓 API와
  IOCP, epoll, kqueue 기반 비동기 코루틴 API
- [`libs/daemonizer`](libs/daemonizer/README.md): Windows 재실행·Unix fork 기반
  데몬 실행, 비동기 양방향 파이프 및 signal 관리
