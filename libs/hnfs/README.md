# hnfs

파일 시스템의 설계 차이를 보존하는 C++20 이미지 라이브러리입니다.
네임스페이스 `hnfs`, umbrella header `<hnfs.hpp>`, CMake target `hnfs::hnfs`를 사용합니다.

**현재 CPIO, ISO9660/Joliet·물리 파티션 UDF 읽기, MBR/EBR·GPT 읽기/쓰기,
VHD/VHDX 읽기와 동적 이미지 copy-out compaction을 제공합니다.**
요청한 8개 포맷 계열 전체의 생성·수정·조회 드라이버가 완성된 상태가 아닙니다.
시그니처 인식만 있는 포맷을 지원 완료로 표시하지 않습니다.

## 인터페이스 계열

`IFileSystem`은 포맷 식별과 구현 진입점만 제공합니다. 모든 포맷에 CRUD,
랜덤 접근, 저널 재생, 트랜잭션을 강제하지 않습니다. 구현 객체가 실제로 상속한
인터페이스를 `dynamic_pointer_cast`로 발견합니다. 공개 상태 객체는 private PIMPL이며
mutable 버퍼·OS handle·드라이버 내부 자료구조를 노출하지 않습니다.

| 계열 | 인터페이스 | 목적 |
| --- | --- | --- |
| 장치 | `IBlockDevice`, `CMemoryDevice`, `CFileDevice`, `CDeviceView` | 64비트 위치 기반 I/O와 파티션 범위 제한 |
| 가상 디스크 | `CVirtualDisk` | VHD/VHDX를 논리 블록 장치로 변환, 동적 이미지 compact |
| 파티션 | `CPartitionTable` | MBR/EBR·GPT 목록, 명시적 메타데이터 생성·수정, 장치 뷰 |
| 파일 | `IFileReader`, `IFileEditor` | 파일 데이터 조회와 수정 분리 |
| 경로 트리 | `ITreeReader`, `ITreeEditor` | 경로 조회·페이지 나열과 namespace 수정 분리 |
| 순차 이미지 | `IArchiveReader`, `IArchiveBuilder` | CPIO와 같은 레코드 스트림 조회·재생성 |
| 읽기 전용 이미지 | `ITreeReader` + 별도 builder | SquashFS는 가짜 in-place editor를 제공하지 않음 |
| CoW | `ICowTree`, `ICowTransaction` | staging flush와 root/checkpoint publication 구분 |
| 스냅샷 | `ISnapshotCatalog` | 읽기 전용 스냅샷 목록·트리 접근 |
| Btrfs | `ISubvolumeCatalog` | 고유 namespace를 갖는 subvolume root |
| APFS | `IVolumeContainer` | 컨테이너와 내부 volume을 별개로 취급 |
| NTFS | `INamedStreams` | ADS를 위치 기반 파일 스트림으로 취급 |
| 메타데이터 | `IExtendedAttributes` | 작은 name/value 속성; ADS와 분리 |
| 저널 | `IJournalInspector` | 읽기 전용 복구 상태 조회; 암묵적인 replay/repair 없음 |

파일·트리 조회는 광학 이미지의 `COpticalFileSystem`과 CPIO의 명시적인 `CIndexedArchive` 어댑터로 구현했습니다.
트리/파일 수정·CoW·확장 인터페이스는 아직 계약만 선언되어 있습니다.
저널 복구를 일반 애플리케이션 트랜잭션으로 취급하지 않습니다.
CoW commit 실패는 `ETXN_INDETERMINATE`일 수 있습니다. 그 경우 `abort()`가 이미
공개된 root를 되돌린다고 보장하지 않습니다. 스냅샷 생성·복구·배치 편집의 옵션은
실제 포맷 드라이버 구현 시 해당 포맷의 계약에 맞춰 추가합니다.
SquashFS의 builder 구현 또한 아직 제공하지 않습니다.

## 실제 구현 상태

| 포맷 계열 | 현재 상태 |
| --- | --- |
| FAT12/16/32, exFAT | 헤더/geometry 기반 probe만 제공, VFAT 이름·할당·파일 트리 드라이버 미구현 |
| NTFS | probe만 제공, MFT/runlist/속성·ADS·수정 미구현 |
| ext2/ext3/ext4 | superblock feature 기반 probe만 제공, inode·extent·directory·journal 드라이버 미구현 |
| XFS | probe만 제공, allocation group·B+tree·log 드라이버 미구현 |
| Btrfs | probe만 제공, chunk mapping·CoW tree·subvolume·snapshot 드라이버 미구현 |
| SquashFS | probe만 제공, metadata/data block reader와 builder 미구현 |
| CPIO | binary LE/BE, odc, newc, crc 순차 읽기·생성 및 선택적 읽기 전용 tree adapter |
| APFS | 컨테이너 signature probe만 제공, checkpoint·object map·volume/tree·암호화 미구현 |
| ISO 9660 | 읽기 전용 tree, Joliet Unicode, Level 3 multi-extent 파일과 64비트 랜덤 읽기 |
| UDF | 2048바이트 sector, type-1 물리 파티션, FE/EFE, short/long AD·embedded data·hole 읽기 |
| MBR/EBR·GPT | 별도 파티션 API, 읽기·생성·메타데이터 교체, GPT CRC/중복 사본 검증 |
| VHD/VHDX | 고정·동적 standalone 읽기, 동적 이미지의 copy-out compaction; in-place 쓰기 미구현 |

`IFileSystem::support(format)`는 이 상태를 반환합니다. 현재 CPIO 계열만
`archiveRead/archiveCreate`가 true입니다. ISO9660/UDF는 `treeRead=true`, 모든 `treeEdit`는 false입니다.
CPIO의 기본 open 결과는 순차 cursor이며 `archiveIndexRead=true`인 포맷은 별도 인덱스 어댑터를
요청할 수 있습니다. 이 플래그는 native mutable tree의 존재를 뜻하지 않습니다.
`probe` 성공은 유효한 볼륨, 완전한 호환성, 쓰기 가능성을 의미하지 않습니다.
ext4를 legacy feature만으로 만든 경우 ext2/ext3와 헤더상 구분되지 않을 수 있어
ext-family 판정은 휴리스틱임을 명시합니다. 파일시스템 probe는 파티션을 암묵적으로 선택하지 않습니다.

## 광학 이미지: ISO·DVD와 대용량

`IFileSystem::open`은 ISO/UDF에 대해 실제 `ITreeReader`를 반환합니다.
`COpticalFileSystem::open(image, options, EFORMAT_ISO9660)`은 bridge 이미지에서도 ISO 트리를
명시적으로 선택합니다. 자동 인식은 UDF를 우선하며 지원되지 않는 UDF를 ISO로 조용히 대체하지 않습니다.
ISO는 Joliet가 있으면 Unicode 트리를, 없으면 ISO 식별자를 사용합니다. Rock Ridge의 POSIX
별도 namespace/심볼릭 링크·권한은 구현하지 않았습니다. ISO 파일 버전 `;n`은 제거하며
동일한 표시 이름이 충돌하면 조회 중 오류를 반환합니다. 파일/디렉터리 생성은 지원하지 않습니다.

DVD-ROM UDF의 물리 type-1 partition map, 일반 파일·디렉터리, FE/EFE, short/long allocation,
embedded bytes와 sparse hole을 읽습니다. VAT·sparing·UDF 2.50 metadata partition, allocation
continuation·indirect ICB·다중 파일셋은 명시적으로 거부합니다. anchor는 sector 256에서 검증하며
손상 anchor를 자동 복구하지 않습니다. 모든 DVD/BD, CSS/AACS 암호화나 DRM을 지원한다는 뜻은 아닙니다.
물리 광학 드라이브의 OS 장치 접근이 아닌 **regular image 파일**을 대상으로 합니다.

디렉터리는 요청 시 순회하며 전체 트리를 열 때 적재하지 않습니다. 페이지는 최대 4096개,
정렬은 매체 기록 순서이며 offset 페이지는 앞부분을 다시 스캔할 수 있습니다. 오류 검증도 lazy입니다.
한 디렉터리 스캔에 entry/name/data 정책을 적용하며 전역 완전 검증 도구를 대신하지 않습니다.
기본 단일 디렉터리 64 MiB, 파일 extent 65536개, 경로 깊이 128, descriptor 256개입니다.
ISO Level 3의 연속 directory record를 결합하여 4 GiB를 넘는 파일을 읽습니다.
64비트 I/O로 DVD·대용량 이미지를 통째로 할당하지 않으며 기본 데이터 정책은 64 GiB이고 조절 가능합니다.

```cpp
auto image = hnfs::CFileDevice::open("dvd.iso", false, {256 * 1024});
auto optical = hnfs::COpticalFileSystem::open(image);
auto page = optical->list("VIDEO_TS", 0, 32);
image->setMappingOptions({4 * 1024 * 1024}); // 다음 I/O부터 적용
auto file = optical->openFile("VIDEO_TS/VTS_01_1.VOB");
std::array<std::uint8_t, 65536> bytes{};
auto count = file->readAt(0, bytes);
```

## 파티션 테이블

`CPartitionTable::open`은 filesystem과 독립적인 validated catalog를 반환합니다.
geometry는 `SPartitionOpenOptions.logicalSectorSize`로 명시하며 512/1024/2048/4096을 지원합니다.
MBR primary와 05/0F/85 extended container, EBR logical partition을 해석합니다.
LBA 범위·겹침·EBR cycle·메타데이터 충돌을 검사하고 CHS는 주소 결정에 사용하지 않습니다.
GPT는 protective MBR, primary/backup header와 entry array CRC32, 상호 위치·GUID·데이터 일치를
검증합니다. hybrid MBR은 거부합니다. `useBackupGpt=true`는 backup만 검증하는 **명시적인 읽기 복구**이며
원본을 repair하지 않습니다. GUID 값은 on-disk byte order 배열이며 이름은 UTF-8 복사본입니다.

`SPartitionLayout`에 slot/LBA/length/type/이름을 지정하고 `CPartitionTable::write`로 기록합니다.
logical MBR partition은 각각의 `ebrLba`도 지정합니다. GPT writer는 128바이트 entry, primary/backup
구조와 protective MBR을 생성합니다. 호출자 제공 disk/partition GUID는 nonzero여야 합니다.
제안한 전체 배치를 먼저 검증하고 비어 있는 metadata 영역에만 기본 기록하며,
기존 nonzero metadata 교체는 `SPartitionWriteOptions.allowOverwrite=true`가 필요합니다.
MBR bootstrap/disk signature는 기본 보존하며 `preserveBootCode=false`로만 지웁니다.
GPT backup→primary→MBR 순으로 flush하지만 **전원 장애 원자성은 보장하지 않습니다**.
기존 파티션 내용 이동·파일시스템 리사이즈·포맷·old metadata secure erase는 수행하지 않습니다.

```cpp
auto disk = hnfs::CFileDevice::open("disk.img");
auto partitions = hnfs::CPartitionTable::open(disk);
auto page = partitions->list(0, 16);
auto partition = partitions->openPartition(page.at(0).index); // 기본 read-only
auto filesystem = hnfs::IFileSystem::open(partition); // 실제 구현된 포맷만 open
```

`openPartition(slot, true)`는 명시적으로 writable view를 요청하며 read-only parent를 우회하지 않습니다.
extended container 자체는 파일시스템처럼 열 수 없습니다. `openAsync/writeAsync`는 worker에서 동작합니다.
파티션 표 변경 동안 기존 catalog/view를 사용하지 말고 변경 후 다시 열어야 합니다.

## VHD/VHDX와 compact

`CVirtualDisk`는 `IBlockDevice`이므로 `CPartitionTable`에 바로 전달할 수 있습니다.
standalone fixed/dynamic VHD와 VHDX를 읽습니다. VHD는 footer/header checksum, BAT 범위·겹침,
sector bitmap을 검증하고 존재하지 않는 sector를 0으로 반환합니다. VHDX는 current header 선택,
CRC32C·region·metadata·BAT·64비트 offset을 검증합니다. 미할당/zero/undefined/unmapped block은
일관되게 0으로 반환합니다. 차등 디스크·parent chain·부분 할당 bitmap·pending recovery log는 거부합니다.
현재 가상 디스크 view는 **읽기 전용**이고, in-place allocation/write·로그 재생·임의 이미지 생성은 미구현입니다.

```cpp
auto disk = hnfs::CVirtualDisk::openFile("dynamic.vhdx");
disk->setMappingOptions({2 * 1024 * 1024});
auto partitions = hnfs::CPartitionTable::open(disk,
    {.logicalSectorSize = disk->logicalSectorSize()});
hnfs::SCompactOptions options;
options.scanZeroBlocks = true;
options.mapping.windowBytes = 256 * 1024;
auto result = disk->compactToFile("packed.vhdx", options); // 새 이름, 기존 파일은 덮어쓰지 않음
```

compact는 미할당 블록을 생략하고 선택적으로 전부 0인 할당 블록을 회수하여 살아 있는 블록을
밀집 배치합니다. 파일시스템의 삭제 데이터가 남은 free space를 자동 추정하지 않습니다.
가상 capacity/논리 내용은 유지하며 VHDX 가상 ID와 data-write GUID도 보존합니다.
VHDX metadata/log/region 위치는 유지하므로 최소 파일 길이는 이 구조들의 마지막 위치에 제한됩니다.
알 수 없는 optional VHDX metadata는 읽을 수 있지만 compact는 안전을 위해 거부합니다.
검사·쓰기의 두 단계 동안 원본은 변경하면 안 됩니다. 결과를 재검증하고 명시적으로 flush하며
실패 시 부분 출력이 남을 수 있습니다. 결과의 `imageBytes`는 파일 길이이고 실제 host allocated bytes는 아닙니다.
`compactToFileAsync/openFileAsync`도 asyncpp worker를 사용하며 원본/경로를 task가 소유합니다.

## 장치와 안전 경계

- 메모리는 소유 복사하며 호출자 span을 보관하지 않습니다. `readBytes`는 한 번에 최대 64 MiB.
- 파일은 기존 regular image만 열거나 `create`로 새 파일을 배타 생성합니다.
  기존 파일을 truncate하지 않습니다. 기본 열기는 read-only입니다.
- Windows device namespace·reparse point, POSIX symlink·block/character device·FIFO는
  파일 backend에서 허용하지 않습니다. raw disk 접근이나 OS mount API는 제공하지 않습니다.
- 모든 hnfs 파일 내용 I/O는 mmap/MapViewOfFile **슬라이딩 윈도우**를 사용합니다.
  `SMappedIOOptions.windowBytes`는 기본 1 MiB, 범위 4 KiB..64 MiB의 useful bytes이고 OS 정렬
  prefix가 추가될 수 있습니다. 전체 파일을 매핑하지 않습니다. open/create 옵션과
  `mappingOptions/setMappingOptions`로 런타임 변경하며 device mutex와 직렬화합니다.
  dirty window 교체/옵션 변경/명시적 flush에서 매핑 동기화 오류를 보고합니다.
  파일 생성 중 I/O 실패 시 부분 파일이 남을 수 있습니다.
- 용량은 고정되어 있고 모든 범위는 overflow 없이 확인합니다. read-only view는
  상위 장치의 읽기 전용 정책을 약화할 수 없습니다.
- 기본 구현은 단일 호출을 mutex로 보호합니다. `next/read`, `beginEntry/write/finish` 같은
  복합 순서는 호출자가 직렬화해야 합니다. 여러 reader나 외부 프로그램이 공유 장치를
  변경하면 안 됩니다. 외부 파일 resize나 동시 드라이버 수정도 지원하지 않습니다.
  특히 외부 truncate는 POSIX mmap의 SIGBUS를 일으킬 수 있고 이 계약 위반을 예외로 복구하지 않습니다.
- 장치 flush는 파일 시스템 트랜잭션이나 원자적 파일 교체가 아닙니다.

## CPIO 사용 예

```cpp
auto device = std::make_shared<hnfs::CMemoryDevice>(8192);
auto writer = hnfs::IArchiveBuilder::create(device, {hnfs::EFORMAT_CPIO_CRC});
writer->beginEntry({"hello.txt", hnfs::ENODE_FILE, 5});
writer->write(hnfs::SBytes{'h', 'e', 'l', 'l', 'o'});
writer->finish();

auto object = hnfs::IFileSystem::open(device);
auto reader = std::dynamic_pointer_cast<hnfs::IArchiveReader>(object);
while (auto entry = reader->next()) {
    std::array<std::uint8_t, 65536> chunk{};
    while (auto count = reader->read(chunk)) {
        // chunk[0..count)를 즉시 소비한다.
    }
}
```

CPIO는 기존 디렉터리를 in-place 수정하는 파일 시스템이 아닙니다. 수정하려면
입력을 순회하고 필요한 레코드를 변경해 **별도 출력 장치**의 builder로 재생성합니다.
현재 편집용 인덱스나 자동 호스트 디스크 추출 API는 제공하지 않습니다.

경로 조회가 필요하면 `CIndexedArchive::open(device, options)`를 명시적으로 호출하십시오.
전체 이미지를 순회해 checksum/EOF를 검증하고 metadata만 보관합니다. `ITreeReader`와
`IFileReader`를 제공하지만 `ITreeEditor`로는 변환되지 않습니다. 기본 순차 cursor에는
인덱스 비용이 붙지 않습니다. 어댑터는 누락된 부모 directory를 합성하며 합성 항목까지
entry/name 한도에 포함합니다. root도 항목 수에 포함하고, 중복 경로·file인 부모를 거부합니다.
기본 cursor가 허용하는 반복 레코드를 tree가 모호하게 덮어쓰지는 않습니다.
페이지 조회는 최대 4096개, 기본 128개이며 반환 metadata는 독립 복사본입니다.
파일 view는 index가 파괴돼도 장치 수명을 유지합니다. 읽기 중 외부 장치 변경은 여전히 금지입니다.

- 일반 파일과 디렉터리, mode/uid/gid/mtime, 정확한 data size를 보존합니다.
  링크·특수 노드·HP-UX/PWB 확장·concatenated initramfs는 명시적으로 미지원입니다.
  binary는 PWB 원본이 아닌 이후 Unix 형식이며 두 endian을 지원합니다.
- 형식별 숫자 폭을 초과하면 자르지 않고 실패합니다. odc 데이터 길이는 33비트,
  newc/crc 및 binary는 32비트 한도를 사용합니다. GNU cpio binary의 일부 구현은
  더 작은 signed 크기 한도를 사용할 수 있습니다.
- `next()`는 이전 레코드의 미소비 payload도 bounded buffer로 건너뛰며 checksum을 검사합니다.
  입력 전체 목록이나 파일 전체를 메모리에 모으지 않습니다.
- `crc`는 이름과 달리 unsigned byte 합이며 암호학적 인증이 아닙니다. CRC builder는
  스트리밍 후 해당 헤더의 check field만 backpatch합니다. newc nonzero check와
  nonzero alignment padding은 canonical 입력 정책상 거부합니다.
- `TRAILER!!!`와 뒤의 zero padding을 검증한 뒤에만 EOF를 반환합니다.
  checksum이 확인되기 전 전달된 데이터는 잠정적이며, 소멸 시 미소비 데이터는 검증하지 않습니다.
- 이름의 절대 경로·`..`·드라이브/역슬래시·내부 NUL은 거부합니다. `./` prefix는 정규화합니다.
  일반 CPIO의 반복된 경로 레코드는 순차적으로 보존하며 자동 추출/덮어쓰기를 하지 않습니다.
- 기본 정책은 백만 레코드, 개별 경로 4096바이트, 누적 이름 64 MiB, 누적 데이터 64 GiB,
  trailer 뒤 zero padding 16 MiB입니다. 큰 여유 용량의 장치라면 `CDeviceView`로
  이미지 길이를 제한하거나 명시적으로 padding 정책을 바꾸십시오.
- 생성 시 기본적으로 장치 전체가 zero인지 먼저 확인합니다. `allowOverwrite=true`를
  명시하면 기존 내용 교체를 허용하고 finish에서 남은 tail도 zero로 만듭니다.
  이는 파괴적 포맷 동작이며 rollback/원자성을 보장하지 않습니다.
- finish 전에 partial image가 기록됩니다. I/O 실패 후 builder는 재사용할 수 없습니다.
  성공한 finish는 재호출 가능하고 소멸자는 자동 finish하지 않습니다.
- reader의 next/read에서 파싱·I/O·상태 오류가 발생하면 새 cursor를 열어야 합니다.
  EOF 전 usedBytes 조회 거부는 cursor 상태를 바꾸지 않습니다. 정상 EOF와 native I/O
  실패를 동일하게 처리하지 않습니다.

## 비동기

`CDeviceAsync`, `CArchiveReaderAsync`, `CArchiveBuilderAsync`, `CTreeReaderAsync`가 각 인터페이스에 대응합니다.
`CTreeReaderAsync::indexArchive`는 선택적 인덱스 구축을 worker에서 수행합니다.
`CTreeReaderAsync::open`은 ISO/UDF native tree를 worker에서 엽니다.
공통 거대 async wrapper로 포맷에 없는 기능을 추가하지 않습니다.

```cpp
asyncpp::thread_pool worker(1);
auto reader = asyncpp::as_promise(
    hnfs::CArchiveReaderAsync::open(device, worker)).get();
hnfs::CArchiveReaderAsync async(reader, worker);
auto entry = asyncpp::as_promise(async.next()).get();
auto data = asyncpp::as_promise(async.read(65536)).get();
```

task는 호출 시 인자와 구현 객체를 소유합니다. adapter를 파괴해도 생성된 task는 유효합니다.
dispatcher와 callback 외부 상태는 작업이 끝날 때까지 살아 있어야 합니다.
blocking I/O는 네트워크 reactor가 아닌 worker에 배치합니다. continuation은 worker에서 이어지고
자동으로 원래 스레드로 복귀하지 않습니다. 취소·timeout·전원 장애 복구를 제공한다고 주장하지 않습니다.

## 의존성과 확장 순서

현재 런타임 의존성은 asyncpp와 C++ 표준 라이브러리·OS 파일 I/O뿐입니다.
포맷 parser/serializer·range 계산·checksum은 직접 구현했습니다. 표준 컨테이너를
사용하지만 외부 filesystem engine/libarchive/libfs 도구를 링크하거나 실행하지 않습니다.

포맷 드라이버 추가 시 압축 코덱은 `libs/hncomp`에서 구현/확장하고, 암호화는 libcertpp의
공개 API 또는 `refs/libcertpp/patches` 지침에 따른 패치를 사용합니다.
아직 압축·암호화 파일 시스템 드라이버가 없으므로 불필요한 링크 의존성은 추가하지 않았습니다.
특히 NTFS LZNT1, SquashFS LZO, APFS LZVN/LZFSE 등 현재 hncomp에 없는 코덱을
기존 다른 알고리즘으로 대체해서 호환성을 주장하면 안 됩니다.

후속 구현은 FAT/VFAT allocation·이름 규칙, ext2 inode/block 트리를 먼저 검증하고,
저널·extent·checksum 검증 후 ext3/ext4·NTFS·XFS로 확장합니다. Btrfs/APFS는
각각 chunk/object mapping과 root publication을 검증한 다음에만 writable 기능을 공개합니다.
SquashFS는 read-only tree와 전체 이미지 builder를 별도로 구현합니다.

## 빌드·검증

```sh
cmake -S . -B build -DHNFS_BUILD_EXAMPLES=ON
cmake --build build --target hnfs_tests hnfs_cpio_example
ctest --test-dir build -R hnfs --output-on-failure
```

Linux에서 테스트 전용 GNU cpio가 있으면 `-DHNFS_REFERENCE_TESTS=ON`으로 교차 검증합니다.
`-DHNFS_DISK_REFERENCE_TESTS=ON`은 테스트 전용 genisoimage·sfdisk·sgdisk·qemu-img가 필요합니다.
독립 ISO/Joliet/UDF 생성 이미지, 파티션 표의 외부 읽기, VHD/VHDX compact 후 qemu-img compare를 검증합니다.
binary LE/odc/newc/crc는 양방향, binary BE는 hnfs 출력의 GNU cpio 해제를 확인합니다.
실행 파일·ctest 실패·CRT assert는 stdout/stderr를 사용하고 Windows dialog를 띄우지 않습니다.
doctest는 소유권, 범위·overflow, read-only, 생성·덮어쓰기, checksum·손상·EOF,
비동기 수명·worker 전환, capability의 실제 노출을 검증합니다.

## 규격

- [ECMA-119 / ISO9660 및 Joliet](https://ecma-international.org/publications-and-standards/standards/ecma-119/),
  [ECMA-167 / UDF 기본 구조](https://ecma-international.org/publications-and-standards/standards/ecma-167/)
- [UEFI GPT/MBR](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html)
- [Microsoft VHD specification](https://www.microsoft.com/en-us/download/details.aspx?id=23850),
  [Microsoft MS-VHDX](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/)

- [Linux initramfs/CPIO](https://www.kernel.org/doc/html/latest/driver-api/early-userspace/buffer-format.html)
- [NetBSD CPIO format](https://man.netbsd.org/cpio.5),
  [GNU cpio format options](https://www.gnu.org/software/cpio/manual/html_chapter/Invoking-cpio.html)
- [UEFI FAT media format](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html),
  [Microsoft exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification)
- [ext structures](https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html),
  [XFS metadata](https://docs.kernel.org/filesystems/xfs/xfs-self-describing-metadata.html)
- [Btrfs](https://btrfs.readthedocs.io/),
  [SquashFS design](https://docs.kernel.org/filesystems/squashfs.html),
  [Apple File System Reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf)

자체 구현은 외부 engine의 코드 취약점을 그대로 가져오지 않는다는 뜻일 뿐,
동일한 결함 유형이나 새로운 취약점이 없다는 보장은 아닙니다. 이미지 수정은 반드시
복사본에서 검증하십시오. 보안 감사·fsck 대체·운영 디스크 손상 복구 완료 제품이 아닙니다.
