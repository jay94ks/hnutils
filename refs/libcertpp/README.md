# libcertpp reference

## 목적

`jay94ks/libcertpp`를 C++17 기반 X.509, ASN.1 및 암호화 기능에 활용하기 위한
외부 reference로 보관합니다. 자체 DER codec, 암호화 primitive, 인증서 계층을
제공하며 Windows와 Linux를 지원합니다. upstream 저장소는 [`repo`](repo)에 Git
submodule로 연결합니다.

## 로컬 구성

- `repo`: `https://github.com/jay94ks/libcertpp`의 submodule
- `patches`: upstream에 적용할 로컬 patch
- `docs`: 통합 및 사용 과정에서 작성한 문서

## Patches

현재 patch는 없습니다. patch를 추가하면 적용 순서, 대상 upstream commit,
필요한 이유를 이 절에 기록합니다.

## 사용 라이브러리

`libs/hncomp`의 ZIP AES 및 XZ SHA-256에서 공개 API를 사용합니다.
통합 방식·빌드 설정·보안 경계는 [hncomp 통합](docs/hncomp.md)을 참고하십시오.
