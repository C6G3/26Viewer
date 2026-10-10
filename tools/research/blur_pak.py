#!/usr/bin/env python3
"""Reference reader for Blur "2KAP" .pak archives.

This is the throw-away prototype used to reverse-engineer the format, kept as
a readable executable specification (see docs/formats/blur.md). The C++
implementation in src/games/blur/PakArchive.cpp follows it exactly.

    python -I tools/research/blur_pak.py list   F:/Games/BLUR/cache/root.pak
    python -I tools/research/blur_pak.py verify F:/Games/BLUR/cache/root.pak
"""

import struct
import sys
import zlib

KEY = b"VXo40j3@$%\\%`x"  # 14-byte XOR key for the name table


def parse(path):
    f = open(path, "rb")
    hdr = f.read(0x40)
    magic, version, align = struct.unpack_from("<4sII", hdr, 0)
    if magic != b"2KAP":
        raise ValueError("not a Blur PAK")
    names_off, names_size = 0x3C, struct.unpack_from("<I", hdr, 0x38)[0]
    if names_size == 0:  # size stored at the first alignment boundary instead
        f.seek(align)
        names_size = struct.unpack("<I", f.read(4))[0]
        names_off = align + 4
    f.seek(names_off)
    names = bytes(b ^ KEY[i % len(KEY)] for i, b in enumerate(f.read(names_size)))

    table = (names_off + names_size + align - 1) // align * align
    f.seek(table)
    count, table_bytes = struct.unpack("<II", f.read(8))
    stride = table_bytes // count  # 32 (v1), 40 or 64 (v2)
    entries = []
    for _ in range(count):
        raw = f.read(stride)
        name_hash, name_off, parent, size, filetime, offset = struct.unpack_from("<IIIIQq", raw, 0)
        content_hash, stored = struct.unpack_from("<II", raw, 32) if version >= 2 else (None, None)
        name = names[name_off:names.index(b"\0", name_off)].decode("latin1")
        if offset != -1:  # -1 marks directory records
            entries.append((name, size, offset * align, stored, name_hash, content_hash))
    return f, version, entries


def name_hash(path):
    """CRC-32 without the final inversion of the lower-case path, e.g. "\\ui\\a.texture"."""
    return zlib.crc32(path.lower().replace("/", "\\").encode("latin1")) ^ 0xFFFFFFFF


def content_hash(data):
    """32-bit sum of ~crc32 over each 32 KiB block of the uncompressed data."""
    return sum(zlib.crc32(data[i:i + 0x8000]) ^ 0xFFFFFFFF for i in range(0, len(data), 0x8000)) & 0xFFFFFFFF


def read(f, version, entry):
    name, size, pos, stored = entry[:4]
    f.seek(pos)
    if version < 2:
        z = struct.unpack("<I", f.read(4))[0]
        return zlib.decompress(f.read(z))
    raw = f.read(stored)
    if stored == size:
        return raw
    if raw[0] == 0x78 and (raw[0] << 8 | raw[1]) % 31 == 0:
        return zlib.decompress(raw)
    # Chunked raw deflate: u32 chunkSize, u32 zsize, data, then u32 zsize, data ...
    chunk, z = struct.unpack_from("<II", raw, 0)
    p, out = 8, bytearray()
    while True:
        want = min(chunk, size - len(out))
        block = raw[p:p + z]
        p += z
        try:
            out += zlib.decompress(block, -15)
        except zlib.error:
            if z != want:
                raise
            out += block  # genuinely stored chunk
        if len(out) >= size:
            return bytes(out)
        z = struct.unpack_from("<I", raw, p)[0]
        p += 4


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("list", "verify"):
        print(__doc__)
        return 2
    f, version, entries = parse(sys.argv[2])
    bad = 0
    for e in entries:
        if sys.argv[1] == "list":
            print(f"{e[1]:>10}  {e[0]}")
        else:
            data = read(f, version, e)
            hashes_ok = name_hash(e[0]) == e[4] and (e[5] is None or content_hash(data) == e[5])
            if len(data) != e[1] or not hashes_ok:
                bad += 1
                print("FAIL", e[0])
    print(f"{len(entries)} files, version {version}, {bad} failures", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
