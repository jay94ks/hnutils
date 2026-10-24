"""Test-only independent generators/readers; none are hnfs runtime dependencies."""
import json
import pathlib
import subprocess
import sys
import tempfile
import shutil

oracle = pathlib.Path(sys.argv[1]).resolve()

def run(*args, **kwargs):
    result = subprocess.run([str(x) for x in args], capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{args}: {result.stderr.decode(errors='replace')} {result.stdout.decode(errors='replace')}")
    return result.stdout

def rejects(*args):
    result = subprocess.run([str(x) for x in args], capture_output=True, timeout=10)
    assert result.returncode != 0, args
    assert result.stderr, args

with tempfile.TemporaryDirectory(prefix="hnfs-disk-reference-") as folder:
    root = pathlib.Path(folder)
    source = root / "source"
    source.mkdir()
    (source / "docs").mkdir()
    payload = bytes((i * 37) % 256 for i in range(131079))
    (source / "docs" / "data.bin").write_bytes(payload)
    (source / "한글.txt").write_bytes(b"Unicode content")
    for mode, flags in [("iso", []), ("joliet", ["-J"]), ("udf", ["-udf", "-J"])]:
        image = root / (mode + ".iso")
        run("genisoimage", "-quiet", *flags, "-o", image, source)
        path = "DOCS/DATA.BIN" if mode == "iso" else "docs/data.bin"
        assert bytes.fromhex(run(oracle, "tree", image, 0, path).decode()) == payload, mode
        if mode == "udf":
            assert bytes.fromhex(run(oracle, "tree", image, 18, "docs/data.bin").decode()) == payload
        if mode != "iso":
            assert bytes.fromhex(run(oracle, "tree", image, 0, "한글.txt").decode()) == b"Unicode content"
        if mode == "udf":
            corrupt = root / "bad-udf.iso"
            contents = bytearray(image.read_bytes())
            for sector in range(16, min(512, len(contents) // 2048)):
                at = sector * 2048
                if int.from_bytes(contents[at:at+2], "little") == 5 and int.from_bytes(contents[at+12:at+16], "little") == sector:
                    contents[at+188] ^= 1
                    break
            else:
                raise AssertionError("no independent UDF partition descriptor")
            corrupt.write_bytes(contents)
            rejects(oracle, "tree", corrupt, 0, "docs/data.bin")
    for scheme in ["mbr", "gpt"]:
        image = root / (scheme + ".raw")
        run(oracle, "write-partitions", image, scheme)
        table = json.loads(run("sfdisk", "--json", image))["partitiontable"]
        assert table["partitions"][0]["start"] == 2048
        assert table["partitions"][0]["size"] == 4096
        if scheme == "gpt":
            run("sgdisk", "-v", image)
        else:
            assert table["partitions"][-1]["start"] == 8193
        assert run(oracle, "partitions", image, 512).decode().splitlines()[0].split()[:3] == ["1", "2048", "4096"]
        external = root / (scheme + "-external.raw")
        with external.open("wb") as stream:
            stream.truncate(32 * 1024 * 1024)
        run("sfdisk", external, input=f"label: {'gpt' if scheme == 'gpt' else 'dos'}\nstart=2048,size=4096\n".encode())
        assert run(oracle, "partitions", external, 512).decode().splitlines()[0].split()[:3] == ["1", "2048", "4096"]
    raw = root / "disk.raw"
    with raw.open("wb") as stream:
        stream.truncate(32 * 1024 * 1024)
        stream.seek(777)
        stream.write(payload)
        stream.seek(12 * 1024 * 1024 + 51)
        stream.write(b"tail data")
    for fmt, qemu_format, options in [("vhd", "vpc", "subformat=dynamic,force_size=on"), ("vhdx", "vhdx", "subformat=dynamic,block_size=2097152")]:
        image, compact = root / (fmt + ".img"), root / (fmt + "-compact.img")
        run("qemu-img", "convert", "-f", "raw", "-O", qemu_format, "-o", options, raw, image)
        assert bytes.fromhex(run(oracle, "virtual", image, 777, len(payload)).decode()) == payload
        assert bytes.fromhex(run(oracle, "virtual", image, 4*1024*1024, 4096).decode()) == bytes(4096)
        run(oracle, "compact", image, compact)
        run("qemu-img", "compare", "-f", qemu_format, "-F", "raw", compact, raw)
        assert bytes.fromhex(run(oracle, "virtual", compact, 777, len(payload)).decode()) == payload
        fixed = root / (fmt + "-fixed.img")
        fixed_options = "subformat=fixed,force_size=on" if fmt == "vhd" else "subformat=fixed,block_size=2097152"
        run("qemu-img", "convert", "-f", "raw", "-O", qemu_format, "-o", fixed_options, raw, fixed)
        assert bytes.fromhex(run(oracle, "virtual", fixed, 777, len(payload)).decode()) == payload
        rejects(oracle, "compact", fixed, root / (fmt + "-not-created.img"))
        corrupt = root / (fmt + "-corrupt.img")
        shutil.copyfile(image, corrupt)
        with corrupt.open("r+b") as stream:
            if fmt == "vhd":
                stream.seek(-512+64, 2)
                value = stream.read(1)
                stream.seek(-1, 1)
                stream.write(bytes([value[0] ^ 1]))
            else:
                # One corrupt header is recoverable. Neither header valid must be rejected.
                stream.seek(65536)
                stream.write(b"BAD!")
        if fmt == "vhdx":
            assert bytes.fromhex(run(oracle, "virtual", corrupt, 777, len(payload)).decode()) == payload
            with corrupt.open("r+b") as stream:
                stream.seek(131072)
                stream.write(b"BAD!")
        rejects(oracle, "virtual", corrupt, 0, 1)
        # Allocate zero blocks, then require compaction to actually reduce the image length.
        dense = root / (fmt + "-dense.img")
        run("qemu-img", "convert", "-f", "raw", "-O", qemu_format, "-S", "0", "-o", options, raw, dense)
        packed = root / (fmt + "-packed.img")
        result = run(oracle, "compact", dense, packed).decode().split()
        assert int(result[2]) < int(result[1]), result
        run("qemu-img", "compare", "-f", qemu_format, "-F", "raw", packed, raw)
        print(fmt, "compaction", result)
    large = root / "large.vhdx"
    run("qemu-img", "create", "-f", "vhdx", "-o", "block_size=33554432", large, "5T")
    assert bytes.fromhex(run(oracle, "virtual", large, 5*1024**4-4096, 4096).decode()) == bytes(4096)
print("ISO/Joliet/UDF, MBR/EBR/GPT and VHD/VHDX independent interoperability passed")
