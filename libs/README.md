# Libraries

여러 CLI 도구에서 재사용할 공용 코드와 독립적인 라이브러리를 둡니다.

각 라이브러리는 `libs/<name>` 디렉터리를 사용하며, 최소한 다음 내용을 담은
README를 포함해야 합니다.

- 라이브러리의 책임과 공개 API
- 빌드 및 테스트 방법
- 의존성 및 지원 환경
- 사용하는 도구와 간단한 예제

특정 도구에만 필요한 구현은 해당 `tools/<name>` 안에 두고, 실제로 재사용할
경계가 확인되었을 때 라이브러리로 분리합니다.

## 현재 라이브러리

- [`socketpp`](socketpp/README.md): 동기 BSD 스타일 소켓과 asyncpp 기반 비동기
  코루틴 API를 함께 제공하는 크로스 플랫폼 소켓 래퍼
- [`daemonizer`](daemonizer/README.md): asyncpp 기반 데몬 생명주기, 프로세스 생성,
  양방향 IPC, signal 전달 및 종료 관리
- [`hncomp`](hncomp/README.md): 자체 구현한 압축 코덱, 일괄/스트리밍 인터페이스와
  asyncpp 기반 비동기 압축·해제
