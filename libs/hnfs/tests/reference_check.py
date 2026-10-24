"""GNU cpio is an independent test oracle, never a runtime dependency."""
import pathlib
import random
import subprocess
import sys
import tempfile


def oracle(format_id, operation, corpus):
    result = subprocess.run([sys.argv[1], str(format_id), operation],
                            input="\n".join(data.hex() for data in corpus)+"\n", text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=40)
    lines = result.stdout.splitlines()
    if len(lines) != len(corpus):
        raise RuntimeError("unexpected oracle response count: " + result.stderr)
    return [line if line.startswith("ERROR ") else bytes.fromhex(line) for line in lines]


def main():
    rng = random.Random(7706)
    corpus = [b"", b"hello CPIO", bytes(range(256)), rng.randbytes(131079)]
    profiles = [(12, "bin"), (13, "bin-big-endian"), (14, "odc"), (15, "newc"), (16, "crc")]
    with tempfile.TemporaryDirectory(prefix="hnfs-reference-") as folder:
        root = pathlib.Path(folder)
        (root / "docs").mkdir()
        for format_id, name in profiles:
            packed = oracle(format_id, "encode", corpus)
            for plain, archive in zip(corpus, packed):
                if isinstance(archive, str):
                    raise RuntimeError(archive)
                result = subprocess.run([sys.argv[2], "-i", "--quiet", "--to-stdout", "docs/data.bin"],
                                        input=archive, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                if result.stdout != plain:
                    raise RuntimeError(f"hnfs {name} -> GNU cpio mismatch")
            if name == "bin-big-endian":
                print("PASS big-endian binary CPIO -> GNU cpio", flush=True)
                continue
            archives = []
            for plain in corpus:
                (root / "docs/data.bin").write_bytes(plain)
                result = subprocess.run([sys.argv[2], "-o", "--quiet", "--reproducible", "-H", name],
                                        input=b"docs\ndocs/data.bin\n", cwd=root,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                archives.append(result.stdout)
            if oracle(format_id, "decode", archives) != corpus:
                raise RuntimeError(f"GNU cpio {name} -> hnfs mismatch")
            print(f"PASS {name}: {len(corpus)} vectors in both directions", flush=True)


if __name__ == "__main__":
    main()
