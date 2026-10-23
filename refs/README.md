# External references

프로젝트에서 참고하거나 사용하는 외부 Git 저장소를 모아 둡니다.

각 외부 저장소는 다음 구조를 사용합니다.

```text
refs/<repository>/
|-- repo/       Git submodule로 연결한 upstream 저장소
|-- patches/    upstream 소스에 적용해야 하는 재현 가능한 patch
|-- docs/       통합, 빌드, 사용에 관한 로컬 문서
|-- README.md   도입 이유와 patch 목록
`-- AGENTS.md   해당 reference를 다룰 때의 작업 지침
```

`repo` 안의 소스는 upstream 소유로 취급합니다. 수정이 필요하면 변경을 patch로
내보내 `patches`에 보존하고, 적용 순서와 이유를 상위 README에 기록합니다.
