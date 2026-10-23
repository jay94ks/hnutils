"""Optional independent compatibility checks; never linked into hncomp."""

import bz2
import ctypes as c
import ctypes.util
import json
import lzma
import random
import subprocess
import sys
import zlib


def oracle(algorithm, direction, corpus, wire=0):
    result = subprocess.run(
        [sys.argv[1], str(algorithm), direction, str(wire)],
        input="\n".join(data.hex() for data in corpus) + "\n",
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=120)
    lines = result.stdout.splitlines()
    if len(lines) != len(corpus):
        raise RuntimeError(f"oracle response count: {len(lines)} != {len(corpus)}: {result.stderr}")
    return [line if line.startswith("ERROR ") else bytes.fromhex(line) for line in lines]


def library(name):
    path = ctypes.util.find_library(name)
    if path is None:
        raise RuntimeError(f"required verification library not found: {name}")
    return c.CDLL(path)


def native_zstd(level=3):
    lib = library("zstd")
    lib.ZSTD_compressBound.argtypes = [c.c_size_t]
    lib.ZSTD_compressBound.restype = c.c_size_t
    lib.ZSTD_compress.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t, c.c_int]
    lib.ZSTD_compress.restype = c.c_size_t
    lib.ZSTD_decompress.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t]
    lib.ZSTD_decompress.restype = c.c_size_t
    lib.ZSTD_isError.argtypes = [c.c_size_t]
    lib.ZSTD_isError.restype = c.c_uint

    def encode(data):
        output = c.create_string_buffer(lib.ZSTD_compressBound(len(data)))
        size = lib.ZSTD_compress(output, len(output), data, len(data), level)
        if lib.ZSTD_isError(size):
            raise RuntimeError("native zstd compression failed")
        return output.raw[:size]

    def decode(data):
        output = c.create_string_buffer(2 * 1024 * 1024)
        size = lib.ZSTD_decompress(output, len(output), data, len(data))
        if lib.ZSTD_isError(size):
            raise RuntimeError("native zstd decompression failed")
        return output.raw[:size]
    return encode, decode


def native_brotli(quality=5):
    enc, dec = library("brotlienc"), library("brotlidec")
    enc.BrotliEncoderCompress.argtypes = [c.c_int, c.c_int, c.c_int, c.c_size_t, c.c_void_p, c.POINTER(c.c_size_t), c.c_void_p]
    enc.BrotliEncoderCompress.restype = c.c_int
    dec.BrotliDecoderDecompress.argtypes = [c.c_size_t, c.c_void_p, c.POINTER(c.c_size_t), c.c_void_p]
    dec.BrotliDecoderDecompress.restype = c.c_int

    def encode(data):
        output = c.create_string_buffer(2 * len(data) + 2048)
        size = c.c_size_t(len(output))
        if not enc.BrotliEncoderCompress(quality, 22, 0, len(data), data, c.byref(size), output):
            raise RuntimeError("native brotli compression failed")
        return output.raw[:size.value]

    def decode(data):
        output = c.create_string_buffer(2 * 1024 * 1024)
        size = c.c_size_t(len(output))
        if dec.BrotliDecoderDecompress(len(data), data, c.byref(size), output) != 1:
            raise RuntimeError("native brotli decompression failed")
        return output.raw[:size.value]
    return encode, decode


def native_snappy():
    lib = library("snappy")
    for name in ("snappy_compress", "snappy_uncompress"):
        getattr(lib, name).argtypes = [c.c_void_p, c.c_size_t, c.c_void_p, c.POINTER(c.c_size_t)]
        getattr(lib, name).restype = c.c_int

    def transform(data, encode):
        output = c.create_string_buffer(max(2 * len(data) + 1024, 2 * 1024 * 1024))
        size = c.c_size_t(len(output))
        status = (lib.snappy_compress if encode else lib.snappy_uncompress)(data, len(data), output, c.byref(size))
        if status:
            raise RuntimeError(f"native snappy failure: {status}")
        return output.raw[:size.value]
    return lambda data: transform(data, True), lambda data: transform(data, False)


def native_lz4():
    lib = library("lz4")
    lib.LZ4_compress_default.argtypes = [c.c_void_p, c.c_void_p, c.c_int, c.c_int]
    lib.LZ4_decompress_safe.argtypes = [c.c_void_p, c.c_void_p, c.c_int, c.c_int]

    def transform(data, encode):
        output = c.create_string_buffer(2 * 1024 * 1024)
        size = (lib.LZ4_compress_default if encode else lib.LZ4_decompress_safe)(data, output, len(data), len(output))
        if size < 0 or (encode and size == 0):
            raise RuntimeError(f"native lz4 failure: {size}")
        return output.raw[:size]
    return lambda data: transform(data, True), lambda data: transform(data, False)


def main():
    randomizer = random.Random(12345)
    corpus = [b"", b"a", b"hello world", bytes(range(256)), b"A" * 10000,
              b"abcde012345" * 5000, b"the compression dictionary contains common words and sentences. " * 200,
              randomizer.randbytes(32769), randomizer.randbytes(131100)]
    corpus += [bytes(randomizer.randrange(8) for _ in range(70000)),
               (randomizer.randbytes(521) + b" testing context dictionary pattern ") * 300,
               b"A" * 64000 + b"B"]
    codecs = [(2, "lzma", lambda data: lzma.compress(data, format=lzma.FORMAT_ALONE),
               lambda data: lzma.decompress(data, format=lzma.FORMAT_ALONE), 0),
              (3, "zlib", zlib.compress, zlib.decompress, 2),
              (3, "raw deflate", lambda data: zlib.compress(data, wbits=-15), lambda data: zlib.decompress(data, wbits=-15), 1),
              (3, "gzip", lambda data: zlib.compress(data, wbits=31), lambda data: zlib.decompress(data, wbits=31), 3),
              (4, "bzip2", bz2.compress, bz2.decompress, 0)]
    for algorithm, name, factory, wire in [(5, "zstd", native_zstd, 0), (6, "brotli", native_brotli, 0),
                                            (7, "snappy raw", native_snappy, 1), (8, "lz4 raw", native_lz4, 1)]:
        encode, decode = factory()
        codecs.append((algorithm, name, encode, decode, wire))
    for level in (1, 19):
        encode, decode = native_zstd(level)
        codecs.append((5, f"zstd level {level}", encode, decode, 0))
    for quality in (0, 1, 9, 11):
        encode, decode = native_brotli(quality)
        codecs.append((6, f"brotli quality {quality}", encode, decode, 0))
    failed = False
    for algorithm, name, encode, decode, wire in codecs:
        encoded = oracle(algorithm, "encode", corpus, wire)
        for index, (plain, compressed) in enumerate(zip(corpus, encoded)):
            try:
                if isinstance(compressed, str) or decode(compressed) != plain:
                    raise RuntimeError(str(compressed)[:120])
            except Exception as error:
                failed = True
                print(f"FAIL {name} ours->reference case {index}: {error}", flush=True)
        reference = [encode(plain) for plain in corpus]
        decoded = oracle(algorithm, "decode", reference, wire)
        failures = [(index, value[:120]) for index, (plain, value) in enumerate(zip(corpus, decoded)) if plain != value]
        if failures:
            failed = True
            print(f"FAIL {name} reference->ours: {failures}", flush=True)
        else:
            print(f"PASS {name} reference->ours ({len(corpus)} vectors)", flush=True)
        if not any(isinstance(value, str) for value in encoded):
            print(f"CHECKED {name} ours->reference ({len(corpus)} vectors)", flush=True)
    packets = [b"", b"SSH-like packet payload" * 10, b"SSH-like packet payload" * 100,
               randomizer.randbytes(32768), b"later packet preserving dictionary" * 10, b""]
    encoded = oracle(3, "stream-encode", packets, 2)
    decoder = zlib.decompressobj()
    if any(isinstance(data, str) or decoder.decompress(data) != plain for plain, data in zip(packets, encoded)):
        failed = True
        print("FAIL persistent zlib PARTIAL flush ours->reference", flush=True)
    else:
        print("PASS persistent zlib PARTIAL flush ours->reference", flush=True)
    encoder = zlib.compressobj()
    reference = [encoder.compress(plain) + encoder.flush(zlib.Z_PARTIAL_FLUSH) for plain in packets]
    if oracle(3, "stream-decode", reference, 2) != packets:
        failed = True
        print("FAIL persistent zlib PARTIAL flush reference->ours", flush=True)
    else:
        print("PASS persistent zlib PARTIAL flush reference->ours", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--export-fixtures":
        plain = b"standard compatibility dictionary compression entropy repeat. " * 30
        encoders = [(2, 0, "LZMA1 alone", lambda data: lzma.compress(data, format=lzma.FORMAT_ALONE)),
                    (3, 2, "zlib dynamic Huffman", zlib.compress),
                    (3, 1, "raw dynamic DEFLATE", lambda data: zlib.compress(data, wbits=-15)),
                    (3, 3, "gzip", lambda data: zlib.compress(data, wbits=31)),
                    (4, 0, "bzip2 BWT MTF Huffman", bz2.compress),
                    (5, 0, "zstd FSE", native_zstd()[0]),
                    (6, 0, "Brotli contexts and dictionary", native_brotli(11)[0]),
                    (7, 1, "Snappy raw", native_snappy()[0]),
                    (8, 1, "LZ4 raw", native_lz4()[0])]
        fixtures = [dict(algorithm=a, format=f, name=n, plain=plain.hex(), encoded=encoder(plain).hex()) for a, f, n, encoder in encoders]
        rng = random.Random(94)
        extra = bytes(rng.randrange(8) for _ in range(1000))
        fixtures.append(dict(algorithm=5, format=0, name="zstd compressed Huffman weights", plain=extra.hex(), encoded=native_zstd(19)[0](extra).hex()))
        print(json.dumps(fixtures))
        sys.exit(0)
    sys.exit(main())
