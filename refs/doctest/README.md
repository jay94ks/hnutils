# doctest reference

## 목적

`doctest/doctest`는 C++ 테스트 프레임워크입니다. `libs/socketpp`의 주소, 소켓
옵션, 동기 및 비동기 네트워크 테스트를 작성하고 CTest에서 케이스별로 실행하기
위해 사용합니다. upstream 저장소는 [`repo`](repo)에 Git submodule로 연결하며,
v2.5.3 릴리스 commit `2d0a935`에 고정합니다.

## 로컬 구성

- `repo`: `https://github.com/doctest/doctest`의 submodule
- `patches`: upstream에 적용할 로컬 patch
- `docs`: 테스트 프레임워크 통합과 사용에 관한 문서

doctest는 `HNUTILS_BUILD_TESTS=ON`일 때만 빌드에 포함합니다. 라이브러리의 공개
의존성 또는 설치 산출물에는 포함하지 않습니다.

## Patches

현재 patch는 없습니다. patch를 추가하면 적용 순서, 대상 upstream commit,
필요한 이유를 이 절에 기록합니다.

통합 방식은 [테스트 통합 문서](docs/integration.md)에 정리합니다.
