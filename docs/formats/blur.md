# Blur (PC, 2010) — file formats

Status legend: ✅ implemented & verified · 🟡 partially understood · ❓ unknown

Verified against the PC release: all 7 `cache/*.pak` files, 6,070 packed files,
0 decompression failures; 3,387/3,388 files of `root.pak` byte-identical to a
QuickBMS dump (the remaining one, `headlight.raw`, is mis-extracted by QuickBMS).

Reference implementation: [`tools/research/blur_pak.py`](../../tools/research/blur_pak.py).
C++: [`src/games/blur/PakArchive.cpp`](../../src/games/blur/PakArchive.cpp).

## Install layout

| Path | Contents |
|---|---|
| `cache/*.pak` | 2KAP packages (almost all assets) |
| `levels/<name>/Level.level`, `.levelrender`, `.levelphysics` | chunk files (loose) |
| `levels/<name>/*.levelstream` | `ARCH` streaming archives (loose, ~4.7 GB) ❓ |
| `UI/movies/*.bik` | Bink videos |
| `blur.bms` | a QuickBMS script shipped/added by modders (format hints, partly outdated) |

## `.pak` — "2KAP" package ✅

All integers little-endian.

### Header

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | char[4] | `2KAP` (`PAK2` = Xbox 360 big-endian variant with XMemCompress, unsupported) |
| 0x04 | u32 | version: 1 (`shaders-speed.pak`) or 2 |
| 0x08 | u32 | alignment, 0x800 — data offsets are stored in these units |
| 0x0C | u32 | header size? (0x20 in v2, 0x28 in v1) 🟡 |
| 0x10 | u16 | v2 flags: `0x0004` boot/root/lut, `0x0102` gamedata, `0x0001` streaming 🟡 |
| 0x12 | u16 | path separator `\` |
| 0x1C | u64 | FILETIME of the build |
| 0x24 | u32 | v2: file table offset − alignment · v1: flags (u16) + separator |
| 0x28 | u32 | v2: aligned size of the file table 🟡 |
| 0x38 | u32 | name table size; **if 0**, read it as u32 at `alignment` instead |

### Name table

Starts right after the size field (0x3C, or `alignment + 4`). XOR-obfuscated
with the repeating 14-byte key `VXo40j3@$%\%`x` (bytes
`56 58 6F 34 30 6A 33 40 24 25 5C 25 60 78`). Decoded, it is a list of
NUL-terminated paths such as `\models\cars\a.model`; offset 0 is the empty string.

### File table

At the next multiple of `alignment` after the name table:

| Type | Meaning |
|---|---|
| u32 | entry count (files **and** directory records) |
| u32 | table size in bytes → `stride = tableBytes / count` (32 / 40 / 64) |
| u32 | unknown (total size?) 🟡 |

Entry (`stride` bytes):

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | name offset into the name table |
| 4 | u32 | parent directory's name offset (`0xFFFFFFFF` for root) |
| 8 | u32 | uncompressed size |
| 12 | u64 | FILETIME |
| 20 | i64 | data offset in alignment units; **−1 = directory record** |
| 28 | u32 | v2: hash — not CRC-32 of the data or of the stored bytes 🟡 |
| 32 | u32 | v2: stored size |
| 36 | u32 | stride 64 only: flags/zero 🟡 |
| 40 | u8[20] | stride 64 only: digest — not SHA-1 of the data 🟡 |
| stride−4 | u32 | name hash; the table is *not* sorted by it 🟡 |

### Data encodings

Chosen per entry; detection order used by the reader:

1. **v1**: `u32 zsize` followed by a zlib stream.
2. **Stored**: `storedSize == size` (e.g. every file in `root_streaming.pak`).
3. **zlib** (RFC 1950): first two bytes form a valid zlib header (`gamedata*.pak`).
4. **Chunked raw deflate** (`boot`, `root`, `lut_rampak`):
   ```
   u32 chunkSize (0x10000)
   u32 zsize0, deflate[zsize0]
   u32 zsize1, deflate[zsize1]
   ...                       until `size` bytes are produced
   ```
   Each chunk inflates to `min(chunkSize, remaining)`. **Gotcha:** a chunk whose
   `zsize` equals its output size can still be compressed (seen in
   `skies/hfd_hemisunset18.texture`), so always try to inflate first and only
   copy verbatim if inflation fails.

## Chunk files ✅ (container) / 🟡 (contents)

Used by `.texture`, `.model`, `.font`, `.particlemesh`, `.level`,
`.levelrender`, `.levelphysics`.

```
char name[]   NUL-terminated, padded with NULs to a multiple of 4
u32  size     whole chunk including this header
u16  count    child count for containers
u16  kind     'C' (0x43) container · 'A' (0x41) data
...  payload  children (containers) or raw data
```

Some `'A'` chunks (e.g. `CPRender`) still contain child chunks; the parser treats
a data chunk as nested when its payload parses exactly as a chunk sequence.
Root names seen: `Texture`, `  CPModel`, `Font`, `PtlData`, `  CPLevel`,
`  CPRender`, `  CPPhysic`.

## `.texture` ✅

Chunk file: `Texture { Header, Data }`.
`Data` payload:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | width |
| 4 | u32 | height |
| 8 | u32 | depth / layers |
| 12 | u32 | mip count |
| 16 | u32 | format: FourCC (`DXT1`/`DXT3`/`DXT5`) or D3DFORMAT number (21 = A8R8G8B8, 28 = A8) |
| 20 | u32 | unknown (hash?) 🟡 |
| 24 | u32 | unknown (3 in all samples) 🟡 |
| 28 | … | surface data, mip 0 first |

Observed mix: DXT1 75%, DXT5 22%, A8R8G8B8, DXT3, A8.

## Other files

| Ext | Magic / notes | Status |
|---|---|---|
| `.model` | chunk file `  CPModel { Header, MdlDat, Render }` + trailing data | 🟡 structure only |
| `.cycle` / `.skeleton` | headerless binary (floats / bone indices) | ❓ |
| `.baf` | `BANK` audio bank | ❓ |
| `.dat` (audio) | `JBOA` | ❓ |
| `.bpr` | `2RZB` compressed UI resource | ❓ |
| `.ipf` | UI layout with named layers | ❓ |
| `.pf` | bitmap font: name + source path | ❓ |
| `.fxb` | compiled shader effects (`shaders-speed.pak`) | ❓ |
| `.loc`, `.bin`, `.xt`, `.aid` | gamedata tables | ❓ |
| `.levelstream` | `ARCH` container | ❓ next target |
| `.bik` | Bink video | external codec |
