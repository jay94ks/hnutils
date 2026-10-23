"""Test-only ZIP/TAR/XZ interop through Python's independent standard library."""
import gzip
import io
import lzma
import random
import subprocess
import sys
import tarfile
import zipfile


def oracle(container, operation, corpus):
    result = subprocess.run([sys.argv[1], str(container), operation],
                            input="\n".join(data.hex() for data in corpus) + "\n",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=150, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(corpus):
        raise RuntimeError("unexpected archive oracle response count: " + result.stderr)
    return [line if line.startswith("ERROR ") else bytes.fromhex(line) for line in lines]


def tar_encode(data, long_name=False):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.PAX_FORMAT) as archive:
        name = "docs/" + ("long-" * 50 if long_name else "data.bin")
        info = tarfile.TarInfo(name)
        info.size = len(data)
        archive.addfile(info, io.BytesIO(data))
    return output.getvalue()


def tar_decode(data):
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
        member = next(value for value in archive.getmembers() if value.isfile())
        return archive.extractfile(member).read()


def zip_encode(data, stored=False):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo("docs/\u00e9.bin")
        info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
        with archive.open(info, "w", force_zip64=True) as file:
            file.write(data)
    return output.getvalue()


def zip_decode(data):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        assert archive.testzip() is None
        return archive.read(next(info for info in archive.infolist() if not info.is_dir()))


def main():
    rng = random.Random(281)
    corpus = [b"", b"hello archive", bytes(range(256)), b"compress this pattern " * 10000,
              rng.randbytes(131100), bytes(rng.randrange(16) for _ in range(700000)),
              (rng.randbytes(100003) + b"dictionary and probabilities retained " * 50) * 25]
    profiles = [(0, "ZIP64 deflate", zip_encode, zip_decode),
                (1, "TAR PAX", lambda d: tar_encode(d, True), tar_decode),
                (2, "XZ CRC64", lzma.compress, lzma.decompress),
                (3, "TAR XZ", lambda d: lzma.compress(tar_encode(d)), lambda d: tar_decode(lzma.decompress(d))),
                (4, "TAR gzip", lambda d: gzip.compress(tar_encode(d)), lambda d: tar_decode(gzip.decompress(d)))]
    for container, name, encode, decode in profiles:
        ours = oracle(container, "archive-encode", corpus)
        for index, (plain, packed) in enumerate(zip(corpus, ours)):
            if isinstance(packed, str) or decode(packed) != plain:
                raise RuntimeError(f"{name} ours->reference case {index}: {str(packed)[:120]}")
        reference = [encode(plain) for plain in corpus]
        decoded = oracle(container, "archive-decode", reference)
        for index, (plain, output) in enumerate(zip(corpus, decoded)):
            if plain != output:
                raise RuntimeError(f"{name} reference->ours case {index}: {str(output)[:120]}")
        print(f"PASS {name}: {len(corpus)} vectors in both directions", flush=True)
    for preset in (0, 9):
        reference = [lzma.compress(plain, preset=preset) for plain in corpus]
        if oracle(2, "archive-decode", reference) != corpus:
            raise RuntimeError(f"XZ preset {preset} mismatch")
        print(f"PASS XZ preset {preset} retained state and dictionary", flush=True)
    if oracle(0, "archive-decode", [zip_encode(value, True) for value in corpus]) != corpus:
        raise RuntimeError("stored ZIP mismatch")
    joined = lzma.compress(b"first ") + bytes(8) + lzma.compress(b"second") + bytes(4)
    if oracle(2, "archive-decode", [joined]) != [b"first second"]:
        raise RuntimeError("concatenated XZ stream mismatch")
    print("PASS stored ZIP and concatenated/padded XZ", flush=True)


if __name__ == "__main__":
    main()
