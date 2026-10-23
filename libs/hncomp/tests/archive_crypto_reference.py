"""Test-only OpenSSL/7-Zip/liblzma interop; none are hncomp runtime dependencies."""
import hashlib
import io
import lzma
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


def oracle(container, operation, corpus, *args):
    result = subprocess.run([sys.argv[1], str(container), operation, *map(str, args)],
                            input="\n".join(data.hex() for data in corpus) + "\n",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(corpus):
        raise RuntimeError("oracle response count mismatch: " + result.stderr)
    return [line if line.startswith("ERROR ") else bytes.fromhex(line) for line in lines]


def openssl_ctr(key, data):
    # Independent ECB encryptions of WinZip's little-endian counters.
    blocks = b"".join(i.to_bytes(16, "little") for i in range(1, (len(data)+15)//16+1))
    stream = subprocess.run(["openssl", "enc", "-aes-"+str(len(key)*8)+"-ecb", "-K", key.hex(), "-nopad"],
                            input=blocks, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
    return bytes(a ^ b for a, b in zip(data, stream))


def aes_fixture(plain, strength, ae_version, stored):
    import hmac
    import zlib
    key_size = (16, 24, 32)[strength-1]
    salt = bytes(range(key_size//2))
    keys = hashlib.pbkdf2_hmac("sha1", b"test-password", salt, 1000, 2*key_size+2)
    compressor = zlib.compressobj(wbits=-15)
    raw = plain if stored else compressor.compress(plain)+compressor.flush()
    cipher = openssl_ctr(keys[:key_size], raw)
    payload = salt + keys[-2:] + cipher + hmac.new(keys[key_size:2*key_size], cipher, "sha1").digest()[:10]
    name = b"data.bin"
    extra = struct.pack("<HHH2sBH", 0x9901, 7, ae_version, b"AE", strength, 0 if stored else 8)
    crc = zlib.crc32(plain) if ae_version == 1 else 0
    local = struct.pack("<IHHHHHIIIHH", 0x04034b50, 51, 0x801, 99, 0, 33, crc, len(payload), len(plain), len(name), len(extra))+name+extra
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 51, 51, 0x801, 99, 0, 33, crc, len(payload), len(plain), len(name), len(extra), 0, 0, 0, 0, 0)+name+extra
    return local+payload+central+struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(central), len(local)+len(payload), 0)


def main():
    rng = random.Random(408)
    corpus = [b"", b"hello encrypted ZIP", rng.randbytes(65549), b"repeat content "*10000]
    if not all(shutil.which(tool) for tool in ("openssl", "7z", "xz")):
        raise RuntimeError("crypto reference tests require test-only openssl, 7z and xz executables")
    for strength in (1, 2, 3):
        fixtures = [aes_fixture(plain, strength, version, stored) for plain in corpus for version in (1, 2) for stored in (False, True)]
        expected = [plain for plain in corpus for _ in range(4)]
        if oracle(0, "archive-aes-decode", fixtures) != expected:
            raise RuntimeError(f"OpenSSL WinZip AES strength {strength} -> hncomp mismatch")
        ours = oracle(0, "archive-aes-encode", corpus, strength)
        with tempfile.TemporaryDirectory(prefix="hncomp-aes-reference-") as folder:
            root = pathlib.Path(folder)
            for index, (plain, packed) in enumerate(zip(corpus, ours)):
                if isinstance(packed, str):
                    raise RuntimeError(packed)
                archive = root / f"ours-{index}.zip"
                archive.write_bytes(packed)
                result = subprocess.run(["7z", "x", "-so", "-ptest-password", str(archive), "docs/data.bin"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                if result.stdout != plain:
                    raise RuntimeError("hncomp AES -> 7-Zip mismatch")
            source = root / "data.bin"
            source.write_bytes(corpus[-1])
            archive = root / "reference.zip"
            subprocess.run(["7z", "a", "-tzip", "-mem=AES"+str((128,192,256)[strength-1]), "-ptest-password", str(archive), str(source)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            if oracle(0, "archive-aes-decode", [archive.read_bytes()]) != [corpus[-1]]:
                raise RuntimeError("7-Zip AES -> hncomp mismatch")
        print(f"PASS WinZip AES-{(128,192,256)[strength-1]}: AE-1/AE-2 STORE/DEFLATE, OpenSSL and 7-Zip", flush=True)
    # Force conversion hits and short tails, plus cross-LZMA2-chunk state.
    binary = (bytes.fromhex("ffffffeb00f000f8000000484000000094000094fffffff0") + rng.randbytes(21))*3100 + b"tail!"
    filters = [(3, lzma.FILTER_DELTA), (5, lzma.FILTER_POWERPC), (7, lzma.FILTER_ARM),
               (8, lzma.FILTER_ARMTHUMB), (9, lzma.FILTER_SPARC), (10, 10)]
    for identifier, python_id in filters:
        for check in (0, 1, 4, 10):
            parameter = 256 if identifier==3 else 4096
            ours = oracle(2, "archive-filter-encode", [binary], identifier, check, parameter)[0]
            if isinstance(ours, str):
                raise RuntimeError(ours)
            if lzma.decompress(ours) != binary:
                raise RuntimeError(f"XZ filter {identifier} check {check} hncomp -> liblzma mismatch")
            spec = {"id": python_id}
            spec["dist" if identifier==3 else "start_offset"] = parameter
            if identifier == 10:
                # Python's filter-options whitelist predates ARM64 even when its linked
                # liblzma can decode it. Exercise the official xz encoder instead.
                reference = subprocess.run(["xz", "-c", "--check="+{0:"none", 1:"crc32", 4:"crc64", 10:"sha256"}[check],
                                            "--arm64=start="+str(parameter), "--lzma2"], input=binary,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
            else:
                reference = lzma.compress(binary, check=check, filters=[spec, {"id": lzma.FILTER_LZMA2}])
            if oracle(2, "archive-decode", [reference]) != [binary]:
                raise RuntimeError(f"XZ filter {identifier} check {check} liblzma -> hncomp mismatch")
        print(f"PASS XZ filter {identifier}: NONE/CRC32/CRC64/SHA256 in both directions", flush=True)


if __name__ == "__main__":
    main()
