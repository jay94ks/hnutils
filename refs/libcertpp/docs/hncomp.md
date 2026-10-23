# hncomp 암호화 통합

기준 upstream commit: `3d3690360d9a14b25f22155c735d5e3be2f54d07`.
서브모듈 소스 변경이나 로컬 patch 없이 공개 API만 사용한다.

- `IHasher`: XZ SHA-256, 테스트용 SHA-1/SHA-256 어댑터.
- `CHmac`, `CPbkdf2`: WinZip 규격의 SHA-1 HMAC과 PBKDF2 (1000회).
- `AES` + unpadded CBC: WinZip little-endian CTR 카운터에 사용할 AES 블록.
  입력을 `counter XOR previousCipher`로 보정하면 CBC 내부 XOR와 상쇄되어
  출력이 정확히 `AES(counter)`가 된다. 내부 AesCore 헤더에는 접근하지 않는다.
- `CRng`: salt 생성. OS CSPRNG 실패 시 실패하며 fallback을 활성화하지 않는다.
- `CSecure`: 임시 키 지우기, 비밀값 비교. hncomp에 constant-time 감사 완료를 주장하지 않는다.

루트 CMake에서 `certpp::certpp`를 제공하고 hncomp는 private 링크한다.
공개 hncomp 헤더에는 certpp 타입을 노출하지 않는다. 설치 패키지는
`find_dependency(certpp CONFIG)`로 의존 대상을 복원한다. Windows DLL 빌드는
certpp DLL을 hncomp 실행 파일 디렉터리에 복사한다. 기본 shared/static 선택은
`BUILD_SHARED_LIBS`를 따르며 `CERTPP_BUILD_SHARED`로 별도 지정할 수 있다.

upstream 테스트·예제는 모노레포 빌드에서 비활성화한다. 보안 메모리 잠금과 OS 전용
난수 설정은 그대로 유지한다. GNU/Clang Debug에서는 -O0에서 upstream RORX
inline assembly의 immediate 조건을 만족하지 못하므로 공식 SIMD-disable 스위치를
기본 ON으로 선택한다. SHA/AES acceleration 스위치는 별개로 유지한다.

OpenSSL ECB 기반 독립 카운터 벡터, RFC 6070 PBKDF2, RFC 2202 HMAC, FIPS SHA-256
KAT와 7-Zip ZIP 양방향 교차 검증을 사용한다. WinZip의 낮은 KDF 비용과
메타데이터 비인증은 규격 한계다. libcertpp 보안 수정의 영향도 함께 추적해야 한다.
