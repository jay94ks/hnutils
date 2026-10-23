# hncomp

외부 압축 라이브러리에 링크하거나 OS 압축 API·외부 명령을 호출하지 않는 C++20
압축 라이브러리입니다. 네임스페이스는 `hncomp`, umbrella header는 `<hncomp.hpp>`,
CMake target은 `hncomp::hncomp`입니다. 공개 의존성은 비동기 API에 사용하는 asyncpp입니다.
ZIP 암호화와 XZ SHA-256은 private 의존성인 `certpp::certpp`(refs/libcertpp)를 사용합니다.
AES·SHA·HMAC·PBKDF2·암호학적 난수·비밀값 비교/지우기를 중복 구현하지 않습니다.
구현 객체는 private PIMPL로 상태를 숨기고 mutable 내부 버퍼나 native handle을 노출하지 않습니다.

## 알고리즘과 포맷

| 알고리즘 | 기본 일괄 처리 포맷 | 추가 일괄 처리 포맷 |
| --- | --- | --- |
| LZ77 | 자체 `HNL7` triple 포맷 | HNCS |
| LZ78 | 자체 `HNL8` 사전 포맷 | HNCS |
| LZMA | LZMA1 `.lzma` / alone | HNCS |
| DEFLATE | zlib | raw DEFLATE, gzip, HNCS |
| bzip2 | `BZh` 스트림 | HNCS |
| Zstd | 표준 Zstandard frame | HNCS |
| Brotli | 표준 RFC 7932 stream | HNCS |
| Snappy | 표준 Snappy framed | raw Snappy block, HNCS |
| LZ4 | 표준 LZ4 frame | raw LZ4 block, HNCS |

`EFORMAT_DEFAULT`는 위 기본값을 선택합니다. `RAW`의 의미는 알고리즘에 따라 다릅니다.
bzip2/Zstd/Brotli에서는 해당 표준 자체의 스트림/프레임을 뜻하며, 컨테이너를 제거한다는
의미가 아닙니다. 잘못된 알고리즘/포맷 조합은 명시적으로 거부합니다.

모든 인코더는 실제 사전 참조나 entropy coding을 수행합니다. 단순히 다른 코덱으로
대체하거나 모든 데이터를 저장 블록으로 포장하는 구현이 아닙니다. 압축 수준은 1~9의
검색 노력 힌트이며 알고리즘별 동일 압축률이나 기존 라이브러리와 동일한 결과 바이트를
보장하지 않습니다. bzip2는 BWT/MTF/RLE와 단순한 canonical Huffman 인코더를,
Zstd/Brotli는 보수적인 LZ 매칭 정책을 사용합니다. 최적화된 기존 구현과 동등한 속도·압축률을
목표 달성했다고 주장하지 않습니다.

디코더는 표준의 stored/RLE/match/entropy 경로를 처리합니다. DEFLATE는 stored/fixed/dynamic
Huffman, Zstd는 Huffman/FSE/default/repeat tables와 repeat offsets, Brotli는 block switching,
context maps, 정적 사전 및 121개 transform을 처리합니다. 다음 부가기능은 지원하지 않습니다.

- 외부 preset/trained dictionary를 주입하는 API.
- 일괄 `EALGORITHM_LZMA`는 LZMA1만 제공하며 `lc + lp > 4`는 지원하지 않습니다.
  LZMA2와 `.xz`는 아래 `IArchive` 경로에서 별도로 제공합니다.
- 폐기된 randomized bzip2 블록.
- 위 표에 없는 legacy/확장 포맷.

zlib/gzip, bzip2, `.lzma`, LZ4 일괄 디코더는 정확히 한 스트림/프레임을 읽고 뒤따르는
바이트를 거부합니다. Zstd는 연결된 표준/스킵 프레임을, Snappy framed는 EOF까지의 청크를
읽습니다. 출력 크기를 모르는 raw LZ4도 반드시 출력 제한을 적용합니다.

## 일괄 처리

`ICompressor`와 `IDecompressor`는 스트리밍 인터페이스와 독립적입니다.

```cpp
#include <hncomp.hpp>

hncomp::SCompressionOptions options;
options.algorithm = hncomp::EALGORITHM_DEFLATE;
options.format = hncomp::EFORMAT_GZIP;

auto compressor = hncomp::ICompressor::create(options);
auto decompressor = hncomp::IDecompressor::create(options);
hncomp::SBytes input{'h', 'e', 'l', 'l', 'o'};
auto compressed = compressor->compress(input);
auto restored = decompressor->decompress(compressed);
```

입력 span은 동기 호출 중에만 빌리고 결과는 소유권 있는 `SBytes`로 반환합니다.
기본 구현의 일괄 객체에는 메시지 간 가변 상태가 없으므로 여러 독립 호출에 재사용할 수 있습니다.

## 별도 스트리밍 인터페이스

`IStreamCompressor` / `IStreamDecompressor`는 일괄 인터페이스를 상속하지 않습니다.
각각 `process`, `reset`, `state`를 제공하며 압축기는 `supportsFlush`도 제공합니다.

**현재 native incremental adapter는 DEFLATE의 raw/zlib/gzip에 제공됩니다.** 다른 알고리즘의
표준 포맷을 지정하여 native streaming factory를 호출하면 `EERROR_UNSUPPORTED`입니다.
모든 알고리즘에는 **명시적으로 선택하는 `EFORMAT_CHUNKED`(HNCS)** 스트리밍을 제공합니다.
HNCS는 hncomp 고유 포맷이며 Snappy/LZ4의 `EFORMAT_FRAMED`나 HTTP Content-Encoding의
`br`, `zstd`, `gzip` 등과 호환되는 외부 포맷이라고 간주하면 안 됩니다.
일괄 표준 포맷 API와 HNCS를 섞거나, 지원되지 않는 포맷을 자동 대체하지 않습니다.

```cpp
hncomp::SCompressionOptions options; // DEFLATE + zlib native stream.
auto encoder = hncomp::IStreamCompressor::create(options);
hncomp::SBytes output(4096);
auto remaining = hncomp::SReadOnlyByteSpan(input);
for (;;) {
    const auto result = encoder->process(remaining, output, hncomp::EFLUSH_FINISH);
    remaining = remaining.subspan(result.consumed);
    // output[0..result.produced) 를 파일/전송 계층에 전달.
    if (result.status == hncomp::ESTREAM_FINISHED) { break; }
}
```

- `consumed`만큼 입력을 버리고 나머지는 다음 호출에 다시 전달합니다.
- `produced`만큼만 출력이 초기화됩니다. `NEED_OUTPUT`이면 새 출력 공간으로 재호출합니다.
- `NEED_INPUT`이면 입력을 추가합니다. 정상적인 backpressure는 예외가 아닙니다.
- 출력 크기 0은 입력을 소비하지 않는 `NEED_OUTPUT`을 반환합니다.
- `FLUSHED`는 현재 flush 완료이지 스트림 종료가 아닙니다.
- `FINISHED` 뒤에는 `reset()`으로 새 독립 스트림을 시작합니다. 실패한 객체도 reset이 필요합니다.
- pending flush에서는 같은 모드와 아직 소비되지 않은 입력 suffix를 전달합니다.
  flush가 시작된 뒤 소비하지 않은 입력이 없다면 빈 입력으로 출력만 배출합니다.
- 해제기의 `EINPUT_FINAL`은 제공한 suffix 뒤에 입력이 없음을 뜻합니다. 종료에 필요한
  바이트가 부족하면 `EERROR_TRUNCATED`가 발생합니다. 정상 종료 시 남은 뒤쪽 입력은 소비하지 않습니다.
- 같은 스트림의 호출은 직렬화해야 합니다. 복사와 실행 중 취소는 지원하지 않습니다.

DEFLATE는 NONE/PARTIAL/SYNC/FULL/FINISH를 구분합니다. PARTIAL/SYNC는 문맥을 유지하며,
FULL은 인코더의 이전 참조 사전을 버립니다. FINISH는 최종 블록과 trailer를 출력합니다.
HNCS는 NONE/SYNC/FULL/FINISH를 지원하고 PARTIAL은 거부합니다. HNCS 블록은 원래부터
서로 독립적이므로 FULL에서 유지해야 할 압축 사전은 없습니다.

해제 중 출력된 데이터는 전체 checksum/trailer 검증 전까지 잠정적인 값입니다.
이전 출력이 있었더라도 나중에 checksum 오류가 발생할 수 있습니다. 일괄 API는 완전한 검증을
성공시킨 뒤에만 결과를 반환합니다. Snappy framed처럼 종료 표식이 없는 포맷은 온전한 마지막
청크 뒤의 전송 중단을 포맷만으로 검출할 수 없습니다.

## 비동기

각 인터페이스는 `compressAsync`, `decompressAsync` 또는 `processAsync`를 제공합니다.
worker는 호출자가 소유·구동하며 모든 task보다 오래 살아야 합니다.

```cpp
#include <asyncpp/thread_pool.h>
#include <asyncpp/sync_wait.h>

asyncpp::thread_pool worker(2);
auto compressed = asyncpp::as_promise(compressor->compressAsync(input, worker)).get();
// coroutine에서는 co_await compressor->compressAsync(input, worker).
```

메서드를 호출할 때 입력과 shared 객체 수명을 task에 보관합니다. lazy task가 시작하기 전에
호출자가 원래 버퍼나 객체의 shared pointer를 해제해도 됩니다. 외부에서 구현한 인터페이스도
비동기를 사용할 때는 `shared_ptr`로 소유해야 합니다. 항상 worker에 enqueue하고 dispatcher TLS에
의존하지 않습니다. 작업과 호출자 continuation은 worker에서 이어지며 자동 복귀하지 않습니다.
실행 중인 coroutine을 임의로 파괴하지 말고 완료까지 기다려야 합니다.

`processAsync(input, outputCapacity, mode, worker)`의 `SStreamChunk`는 실제 `output`과
소비하지 않은 `remainingInput`을 모두 소유합니다. 다음 호출에는 남은 입력을 전달하세요.
호출당 async 출력 용량은 최대 1 MiB이며 동일 스트림에서는 이전 task 완료 후 다음 작업을 시작합니다.
pool 내부에서 같은 pool 작업을 `.get()`으로 막지 말고 `co_await`를 사용하세요.

## 리소스와 오류

기본 제한은 입력/출력 각각 64 MiB(일괄 작업 또는 stream reset 구간의 누적값), 허용
window 8 MiB, LZ78 사전 65,536 entries, 선호 블록 32 KiB입니다. `maxWindowBytes`는
프레임의 history/dictionary 제한이지 전체 프로세스 메모리 할당 상한은 아닙니다.
블록 크기는 1~64 KiB, window 정책은 32 KiB~256 MiB 범위에서 설정합니다.

잘못된 정책은 `std::invalid_argument`, 포맷/상태/제한/잘린 입력은 `CCompressionError`로
보고하며 `code()`로 INVALID_DATA/TRUNCATED/UNSUPPORTED/LIMIT/STATE를 구분합니다.
할당 실패는 `std::bad_alloc`로 전달합니다. 압축 결과가 입력보다 커질 수도 있으므로 압축과
해제의 입력/출력 제한은 실제 사용할 크기에 맞추어 지정해야 합니다.

### LZ77 / LZ78 포맷

사용자가 지정한 [Dictionary 기반 압축 알고리즘 글](https://infossm.github.io/blog/2020/07/20/dictcomp/)의
슬라이딩 윈도우 triple과 증분 사전 pair를 기준으로 구현·테스트했습니다.
아래는 범용 표준 포맷이 아닌 hncomp의 명시적인 저장 규칙입니다. 정수는 little endian입니다.

- HNL7: `HNL7` + uint64 원본 길이 + `(uint16 offset, uint16 length, uint8 hasNext,
  [uint8 next])` 반복. literal은 `(0,0,1,c)`, 참조는 최대 길이 258과 최대 거리 65535입니다.
  마지막 참조가 정확히 EOF까지라면 hasNext=0으로 표현합니다. 길이가 거리보다 긴 겹침 복사를 지원합니다.
- HNL8: `HNL8` + uint64 원본 길이 + uint32 사전 entry ceiling + `(varint parent,
  uint8 hasNext, [uint8 next])` 반복. entry 0은 빈 문자열입니다. 각 새 pair를 사전에
  추가하며 한도 도달 후에는 freeze합니다. 마지막 기존 prefix는 hasNext=0으로 표현합니다.
  varint는 최대 5바이트의 uint32 LEB128입니다.

### HNCS v1 포맷

- 8바이트 header: `HNCS`, version=1, algorithm enum byte, inner format enum byte, reserved=0.
- 각 청크: uint32 compressed length + uint32 raw length + uint32 CRC-32(raw) + 압축 payload.
- raw 청크는 1~65536바이트, 압축 payload는 최대 1 MiB입니다.
- 종료: 0인 uint32 필드 세 개 + uint64 누적 raw length.
- inner format은 LZMA=ALONE, 그 외=RAW입니다. 중첩 HNCS는 허용하지 않습니다.

HNCS는 전체 입력을 쌓은 뒤 처리하는 가짜 스트리밍이 아닙니다. 인코더는 최대 한 원본 청크를,
디코더는 최대 한 압축 payload와 검증된 raw 청크를 처리하며 출력 공간에 따라 정지합니다.
전체 checksum이 아닌 청크별 검증이므로 인증·MAC이나 악성 재배열 방지를 제공하지 않습니다.

## 검증과 빌드

```sh
cmake -S . -B build -DHNCOMP_BUILD_EXAMPLES=ON
cmake --build build --config Debug --target hncomp_tests hncomp_example
ctest --test-dir build -C Debug -R hncomp --output-on-failure
```

기본 doctest는 외부 압축 코덱 없이 실행됩니다. 왕복뿐 아니라 독립 구현이 생성한 고정 벡터,
사용자 참조 글의 tuple/pair, 작은 버퍼/bytewise 입력, flush/reset/EOF/오류 및 비동기 수명,
임의 바이트와 손상된 스트림을 검증합니다. Windows 실패 대화상자와 debugger break는 비활성화합니다.

추가 양방향 교차 검증은 **테스트 전용** Python 3.11+와 시스템 참조 구현이 설치된 Linux에서 실행합니다.
라이브러리는 이들을 링크·호출하지 않으며 CMake가 자동 다운로드/설치하지 않습니다.

```sh
cmake -S . -B build -DHNCOMP_REFERENCE_TESTS=ON
cmake --build build --target hncomp_tests hncomp_oracle
ctest --test-dir build -R hncomp --output-on-failure
```

Python stdlib zlib/bz2/lzma와 시스템 libzstd/libbrotlienc/libbrotlidec/libsnappy/liblz4가 필요합니다.
교차 검증은 여러 Zstd/Brotli 압축 수준, 저엔트로피·바이너리·랜덤·다중 블록 데이터 및
지속 zlib PARTIAL flush를 검사합니다. HTTP·SSH·libssh2 코드나 의존성은 개발 범위에 넣지 않습니다.
zlib ABI 자체를 제공하는 drop-in 교체 라이브러리는 아닙니다.

Windows/Linux의 정적 및 공유 라이브러리를 지원하며 설치 후
`find_package(hncomp CONFIG REQUIRED)`로 사용합니다. 실행 가능한 예제는
[`examples/compression.cpp`](examples/compression.cpp)에 있습니다.

## 파일·메모리 아카이브

`<hncomp/archive.hpp>`의 `IArchive`는 일괄 압축/스트리밍 코덱과 별개의 컨테이너 인터페이스입니다.
구현은 읽기 전용 reader와 생성용 writer로 분리되며, 모두 private PIMPL을 사용합니다.

| 포맷 | 생성 확장자 | 지원 내용 |
| --- | --- | --- |
| ZIP | `.zip` | STORE/DEFLATE 읽기, DEFLATE 생성, ZIP64, WinZip AES |
| TAR | `.tar` | regular file/directory, USTAR/PAX, GNU long-name 읽기 |
| XZ | `.xz` | LZMA2 단일 데이터 스트림, 열었을 때 파일 이름은 `data` |
| TAR+XZ | `.tar.xz`, `.txz` | 여러 파일·디렉터리를 TAR로 묶어 XZ 압축 |
| TAR+gzip | `.tar.gz`, `.tgz` | 여러 파일·디렉터리를 TAR로 묶어 gzip 압축 |

ZIP/TAR/XZ 포맷 및 LZMA2 처리는 내부 구현만 사용합니다. libarchive/liblzma/zlib, OS 압축 API,
외부 압축 실행 파일을 런타임에 호출하지 않습니다. XZ는 LZMA2의 dictionary/state/properties
reset과 유지되는 probability/repeat-distance 상태, NONE/CRC32/CRC64/SHA-256 check, 여러 block과
연결된 stream 및 padding을 읽습니다. 생성 시 실제 LZMA 압축을 수행하며, 효과가 없는 청크만
표준 uncompressed LZMA2 chunk로 저장합니다.

### 파일 동작과 모드

```cpp
hncomp::SArchiveOpenOptions options;
options.mode = hncomp::EARCHIVE_CREATE;
options.compressionLevel = 5;
auto archive = hncomp::IArchive::createFromFile("backup.tar.xz", options);
archive->addDirectory("docs");
archive->addFile("docs/hello.txt", hncomp::SBytes{'h','e','l','l','o'});
archive->addFileFromFile("large.bin", "source.bin");
auto preview = archive->readFile("docs/hello.txt"); // 생성 중에도 읽기 가능.
archive->finish(); // 반드시 명시적으로 완료.
archive.reset();

auto reader = hncomp::IArchive::createFromFile("backup.tar.xz");
reader->readFile("large.bin", [](hncomp::SReadOnlyByteSpan chunk) {
    // chunk를 즉시 소비한다. 파일 전체를 RAM에 모으지 않는다.
});
```

- CREATE는 새 파일만 생성하며 기존 목적지가 있으면 실패합니다. 기존 파일을 덮어쓰지 않습니다.
- OPEN은 확장자가 아니라 파일 내용을 자동 인식하며 읽기만 허용합니다. 디렉터리/파일 추가와
  `finish()`는 `EERROR_STATE`로 거부합니다. 기존 파일의 수정/append 모드는 없습니다.
- 생성자는 `finish()` 전후에 이미 추가된 파일을 읽을 수 있습니다. 완료 후 추가는 불가능하며
  성공한 `finish()`의 재호출은 아무 동작도 하지 않습니다. 소멸자는 자동 완료하지 않습니다.
- 파일은 Windows `CreateFileMapping`/`MapViewOfFile`, POSIX `mmap`의 최대 1 MiB 창으로
  접근합니다. 64비트 오프셋을 사용하고 전체 파일 크기만큼 연속 메모리를 할당하지 않습니다.
  `addFileFromFile`과 sink 버전 `readFile`은 bounded buffer로 동작합니다.
- TAR+압축 및 단일 XZ의 생성은 생성 중 읽기를 위해 원본을 private 임시 디스크 파일에
  staging합니다. 원본 크기만큼의 추가 디스크 공간이 필요할 수 있으며 staging은 자동 삭제됩니다.
- 압축 TAR은 열 때 스트림을 순회해 인덱스를 만들고, 각 파일 읽기 때 다시 순회합니다.
  RAM 사용은 제한되지만 ZIP 같은 임의 접근 속도를 제공하지는 않습니다.
- 열려 있는 원본·아카이브를 외부에서 수정/축소하면 안 됩니다. 특히 POSIX mmap은 외부 truncate와의
  경쟁으로 SIGBUS가 발생할 수 있습니다. 동일 인스턴스의 호출도 직렬화해야 합니다.
- 실패한 생성자는 재사용할 수 없으며 완료 콜백이 호출되지 않습니다. 파일 생성 실패 시 부분 파일이
  남을 수 있습니다. `finish()`/flush의 I/O 실패는 예외로 보고하며 원자적 교체를 보장하지 않습니다.

### 메모리 동작과 콜백

```cpp
hncomp::SBytes content;
auto archive = hncomp::IArchive::createInMemory(8 * 1024 * 1024,
    {hncomp::EARCHIVE_ZIP, 5},
    [&](hncomp::SReadOnlyByteSpan result) {
        content.assign(result.begin(), result.end());
    });
archive->addFile("hello.txt", hncomp::SBytes{'h','i'});
archive->finish();
auto reader = hncomp::IArchive::openFromMemory(content);
```

메모리 생성 옵션은 포맷·압축 레벨과 선택적인 ZIP/XZ 설정입니다. `maximumSize`는 최종 컨테이너 및 필요한 경우
별도의 원본 staging 버퍼 각각에 적용됩니다. 전체 프로세스 RSS의 절대 한도는 아닙니다.
메모리 전용 경로는 임시 파일을 만들지 않습니다. 완료 콜백은 명시적 `finish()`에서 최대 한 번,
완성된 전체 컨텐트로 호출되며 span은 콜백 안에서만 유효합니다. 유지하려면 복사해야 합니다.
콜백 예외는 호출자에게 전달되고 writer는 실패 상태가 됩니다. 콜백에서 같은 아카이브로
재진입하면 거부됩니다. `openFromMemory`는 입력을 즉시 복사하므로 입력 span의 수명과 독립적입니다.

XZ 자체에는 파일 이름과 디렉터리 개념이 없습니다. 생성 시 파일 한 개만 추가할 수 있고,
열었을 때 이름은 항상 `data`입니다. 여러 항목이나 이름 보존이 필요하면 TAR_XZ를 사용합니다.
XZ 내부 데이터가 TAR라면 자동으로 TAR_XZ로 인식합니다.

### 엔트리 범위 조회

`entryCount()`는 복사 없이 항목 수를 반환합니다. `entries(offset, count = 128)`는 해당 범위만
복사하며, 한 번에 최대 4096개를 허용합니다. 끝을 넘는 범위는 빈 페이지, 마지막 페이지는
남은 항목만 반환합니다. 전체 목록을 복사하는 인자 없는 `entries()`는 제공하지 않습니다.
반환값 수정은 내부 인덱스에 영향을 주지 않습니다. 열 때 만드는 내부 인덱스에는 아래의
항목 수·이름 메모리 제한이 계속 적용됩니다. 디스크 기반 지연 인덱스는 아닙니다.

```cpp
for (std::uint64_t offset = 0; offset < reader->entryCount(); offset += 128) {
    for (const auto& entry : reader->entries(offset, 128)) {
        // 이 페이지의 항목만 소비한다.
    }
}
```

### ZIP 암호화와 XZ 필터

```cpp
hncomp::SArchiveMemoryOptions options;
options.zip = {hncomp::EZIP_AES256, "a-long-unique-password"};
hncomp::SBytes encrypted;
auto writer = hncomp::IArchive::createInMemory(8 * 1024 * 1024, options,
    [&](auto bytes) { encrypted.assign(bytes.begin(), bytes.end()); });
writer->addFile("secret.txt", hncomp::SBytes{'h','i'});
writer->finish();
auto decrypted = hncomp::IArchive::openFromMemory(encrypted, 256 * 1024 * 1024,
    "a-long-unique-password");
```

- 생성은 WinZip AE-2, 읽기는 AE-1/AE-2, AES-128/192/256을 지원합니다. 암호화는 기본적으로
  꺼져 있습니다. 생성 시 비어 있지 않은 비밀번호가 필요하며 최대 4096바이트를 허용합니다.
  비밀번호는 바이트열 그대로 사용하고 Unicode 정규화는 하지 않습니다. 디렉터리는 암호화하지 않습니다.
- 파일 경로는 `SArchiveOpenOptions::zip`에 비밀번호와 생성 방식을 지정합니다. 메모리 열기는
  세 번째 인자, 비동기 메모리 열기는 dispatcher 뒤의 인자로 비밀번호를 전달합니다.
- AES 키와 인증 키는 libcertpp PBKDF2-HMAC-SHA1으로 유도합니다. ZIP 규격 때문에 반복 수는
  1000으로 고정되므로 약한 비밀번호의 오프라인 추측을 막아주지 못합니다. 파일명·크기 등
  메타데이터도 숨기거나 인증하지 않습니다. 구형 ZipCrypto/PKWARE strong encryption은 미지원입니다.
- 인증 태그를 먼저 검증하고 나서 복호화된 압축 데이터를 해제기로 전달합니다. 잘못된 비밀번호나
  태그 변조는 `EERROR_AUTHENTICATION`이며 sink로 평문을 전달하지 않습니다. 열려 있는 파일은
  작업 중 외부에서 변경하면 안 됩니다. 손상된 압축 스트림·AE-1 CRC 실패 시에는 기존과 같이
  이미 전달된 sink 데이터가 잠정적일 수 있습니다.
- libcertpp에는 공개 raw-AES/CTR API가 없으므로 공개 unpadded CBC 변환기에
  `counter XOR 이전 암호 블록`을 넣어 `AES(counter)`를 얻습니다. 카운터는 WinZip 규격의
  little-endian이며, 자체 AES 구현이나 비공개 libcertpp 헤더를 사용하지 않습니다.
- `SArchiveMemoryOptions::xz` 또는 `SArchiveOpenOptions::xz`에 `SXzOptions`를 지정합니다.
  `filters`에는 인코딩 순서로 최대 3개, 마지막 LZMA2는 자동으로 붙습니다. 지원 prefilter는
  Delta(거리 1~256), PowerPC, ARM, ARM-Thumb, SPARC, ARM64입니다. BCJ `startOffset`은
  Thumb 2바이트, 나머지 4바이트 정렬이어야 합니다. x86·IA64·RISC-V는 명시적으로 거부합니다.
  `check`는 NONE/CRC32/CRC64/SHA256이며 기본 CRC32입니다. 체크는 원본 데이터 기준이고
  암호학적 인증은 아닙니다. 디코더는 청크 사이 필터 상태와 최대 3바이트 tail만 유지합니다.

GNU/Clang Debug 빌드는 upstream의 `-O0` BMI2 immediate 제약을 피하도록 libcertpp의
공식 `CERTPP_DISABLE_HWACCEL_SIMD` 옵션을 기본 ON으로 선택합니다. 별도 옵션인 SHA/AES
가속은 유지합니다. Release 기본값은 OFF이며 보안 메모리 잠금과 OS 난수 실패 정책은 바꾸지 않습니다.

### 비동기·리소스 제한·보안 경계

파일 열기, 메모리 열기, 파일 추가, 소스 파일 추가, 파일 읽기(collect/sink), 완료에 각각
`createFromFileAsync`, `openFromMemoryAsync`, `addFileAsync`, `addFileFromFileAsync`,
`readFileAsync`, `finishAsync`를 제공합니다. 호출 시 입력과 인스턴스 수명을 확보하고
호출자가 소유한 `asyncpp::dispatcher`로 작업을 넘깁니다. dispatcher 및 sink의 외부 참조 대상은
작업이 끝날 때까지 살아 있어야 합니다. sink/완료 콜백은 작업 worker에서 실행됩니다.

파일 열기의 `SArchiveOpenOptions::maximumExpandedBytes` 기본값은 64 GiB입니다.
`openFromMemory(span, maximumExpandedBytes)`의 기본값은 256 MiB입니다. ZIP은 선언된
파일 크기의 합, TAR 계열은 헤더/padding을 포함한 해제 스트림 크기, XZ는 실제 해제 크기를
기준으로 제한합니다. collect 버전 `readFile(name, maximumSize)`에는 별도로 기본 64 MiB의
결과 할당 한도가 있습니다. 더 큰 파일은 sink 버전으로 소비하십시오.
사전은 최대 64 MiB, 항목 수는 최대 백만 개, 개별 이름은 4096바이트, 누적 이름은 64 MiB,
TAR extended header는 1 MiB로 제한합니다. 파일 범위·정수 overflow·중복 이름·잘못된 체크섬·
종료/인덱스 불일치와 절대 경로/`..`/드라이브/역슬래시 경로를 거부합니다.

지원하지 않는 기능은 조용히 무시하거나 다른 알고리즘으로 대체하지 않습니다.
ZIP의 AES 이외 암호화·분할·STORE/DEFLATE 이외의 실제 압축 method·비 UTF-8 legacy codepage,
TAR link/device/sparse, 위 목록 이외의 XZ 필터는 `EERROR_UNSUPPORTED`입니다.
자동 디스크 추출 API는 제공하지 않습니다. TAR는 payload checksum이 없고, ZIP/XZ checksum도
인증이나 전자서명이 아닙니다. sink에 나온 바이트는 전체 읽기가 성공할 때까지 잠정 데이터입니다.

외부 구현을 링크하지 않는 것은 그 구현의 특정 코드 취약점에 직접 종속되지 않는다는 뜻이지,
동일한 취약점 유형이나 자체 구현의 새 결함이 없다는 보장은 아닙니다. 이 구현은 보안 감사를
완료한 제품이 아닙니다. 신뢰할 수 없는 대량 입력에는 크기 제한과 별도의 실행 시간/프로세스
격리를 함께 적용해야 합니다.
암호화는 예외적으로 libcertpp를 링크하므로 해당 구현의 보안 수정과 취약점 영향을 함께
관리해야 합니다. 자체 압축 구현이라는 이유만으로 보안 우위를 보장하지 않습니다.

[`examples/archive.cpp`](examples/archive.cpp)는 메모리 ZIP과 비동기 추가/완료 예제입니다.
`hncomp_archive_example` target으로 빌드합니다. doctest는 모드·수명·콜백·손상 입력과 mapped
창 경계를, Linux에서는 4 GiB를 넘는 sparse ZIP64 central-directory 오프셋도 검증합니다.
`HNCOMP_REFERENCE_TESTS=ON`의 `hncomp.archive.reference`는 테스트 전용 Python zipfile/tarfile/
lzma/gzip과 양방향으로 ZIP64·PAX·여러 LZMA2 압축 수준·연결된 XZ를 교차 검증합니다.
테스트용 OpenSSL·7-Zip·xz가 있으면 `hncomp.archive.crypto.reference`도 등록해
AE-1/AE-2와 STORE/DEFLATE 암호화를 양방향 확인하고 6종 XZ 필터와 모든 check를 교차 검증합니다.
이 실행 파일들은 런타임 의존성이 아닙니다. doctest는 공개 KAT, 인증 전 출력 방지,
비동기 암호화 읽기, 페이지 범위/복사 독립성도 검사합니다.

## 규격과 출처

압축·해제 로직은 직접 구현했습니다. Brotli의 필수 정적 사전/변환/context 상수만 RFC의
규범 데이터에서 추출했고 규격에 명시된 CRC와 길이를 검증했습니다. 외부 구현 소스는
가져오지 않았습니다. BCJ 비트 변환은 공식 XZ 필터를 참조했으며, 출처·라이선스를
[`NOTICE`](NOTICE)에 보존합니다. 암호화 primitive는 libcertpp를 사용합니다.

- [DEFLATE RFC 1951](https://www.rfc-editor.org/rfc/rfc1951.html),
  [zlib RFC 1950](https://www.rfc-editor.org/rfc/rfc1950.html),
  [gzip RFC 1952](https://www.rfc-editor.org/rfc/rfc1952.html)
- [LZMA alone format](https://github.com/tukaani-project/xz/blob/master/doc/lzma-file-format.txt)
- [XZ format](https://tukaani.org/xz/xz-file-format.txt),
  [WinZip AES AE-1/AE-2](https://www.winzip.com/en/support/aes-encryption/),
  [ZIP APPNOTE](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT),
  [TAR format](https://www.gnu.org/software/tar/manual/html_node/Standard.html)
- [Zstandard format](https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md)
- [Brotli RFC 7932](https://www.rfc-editor.org/rfc/rfc7932.html)
- [Snappy raw format](https://github.com/google/snappy/blob/main/format_description.txt),
  [Snappy framing](https://github.com/google/snappy/blob/main/framing_format.txt)
- [LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md),
  [LZ4 frame format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md)
