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

then `count` entries of `stride` bytes, **sorted by name hash** (the game
binary-searches them):

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | **name hash** (see below) ✅ |
| 4 | u32 | name offset into the name table |
| 8 | u32 | parent directory's name offset (`0xFFFFFFFF` for root) |
| 12 | u32 | uncompressed size |
| 16 | u64 | FILETIME |
| 24 | i64 | data offset in alignment units; **−1 = directory record** |
| 32 | u32 | v2: **content hash** (see below) ✅ |
| 36 | u32 | v2: stored size |
| 40 | u32 | stride 64 only: flags/zero 🟡 |
| 44 | u8[20] | stride 64 only: **SHA-1 of the stored (compressed) bytes** ✅ |

(Earlier versions of this document started each entry 4 bytes too late, which
made the name hash look like the *last* field of the previous entry.)

### Hashes ✅

Both were recovered from `Blur.exe` (PC, 32-bit) and verified against every
entry of every shipped archive (6,775 name hashes; 4,815 content hashes, 2,297
of them for files over 32 KiB). "`~crc32`" below means standard CRC-32 (zlib,
polynomial `0xEDB88320`) **without** the final inversion, i.e.
`zlib.crc32(x) ^ 0xFFFFFFFF`.

- **Name hash** = `~crc32(path)` over the in-archive path as stored in the name
  table: lower case, backslashes, leading `\` (e.g. `\models\cars\a.model`;
  directories too, e.g. `\ui`). Code: name lookup at `0x00CB6720` — normalises
  the path with `0x00403970` (A–Z → a–z, `/` → `\`), hashes it with the table
  built by `0x00CB6460`, then binary-searches the entries (`0x00CB6620`).
- **Content hash** = sum (mod 2³²) of `~crc32(block)` over the uncompressed
  data split into **32 KiB blocks**. A file of ≤ 32 KiB is a single block, so
  its hash is plain `~crc32(data)`. Code: the archive self-check at
  `0x00CB7340` (reads each file in `0x8000`-byte pieces, adds the per-piece
  CRCs, compares the sum with entry+0x20).

Python: `name_hash()` / `content_hash()` in `tools/research/blur_pak.py`
(`verify` checks both). C++: `PakArchive::nameHash()` / `contentHash()`.

`gamedata.pak` / `gamedata_boot.pak` (the stride-64 archives) ship with
128-byte `.sig` files, most likely RSA-1024 signatures over the table.

### Writing (replace) ✅ (format) / 🟡 (game acceptance)

`PakArchive::replace` keeps every other byte intact:

1. Encode the new data the way the original entry was stored (stored / zlib /
   64 KiB chunked raw deflate — every chunk deflated / v1 size-prefixed zlib).
2. Append it at the next `alignment` boundary at the end of the file (the old
   data stays, unused).
3. Patch only that entry: size, FILETIME, offset, stored size, content hash,
   and for stride-64 archives the SHA-1 digest. The name — and therefore the
   name hash and the table order — stays the same.

The record is patched last, so an interrupted write leaves the archive
pointing at the old data. Both hashes are written exactly as the game
computes them; whether the game accepts modified **signed** archives
(`gamedata*.pak`) has **not been tested in-game yet**.

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

Observed mix: DXT1 75%, DXT5 22%, A8R8G8B8, DXT3, A8. Every texture is one layer
(`depth` 1) and stores either a full mip chain down to 1×1 or a single level; the
`Header` chunk payload is 8 bytes (`0`, then `0` or `1`).

**Writing** (`TextureDecoder::rebuild`): the chunks are rewritten in order, `Header`
copied and `Data` replaced (new size, mip count and format; the unknown value and
the `3` kept). All 321 textures in the game's archives rebuild byte for byte from
their own pixels (`tests/test_blur_media.cpp`).

## `audio/dat/*.dat` — "JBOA" audio entities ✅ (structure) / 🟡 (field meanings)

365 files in `root.pak`. Each is a node graph for Bizarre's **Mercury** audio engine:
inputs from the game (`CollisionForce`, car position…), maths components (curves,
rolloffs, arrays, products, timers, scripts), sounds that name a `.baf` bank and its
waves, filters and mixers, wired together by links. Decompiled to text by
`src/games/blur/AudioEntity.cpp` (preview, Export → `.txt`, `26viewer-cli decompile`);
Python reference: `tools/research/blur_aobj.py`.

```
0x00 "JBOA", u16 version 11, u16 2
0x08 u32 string pool offset        0x0c u32 entity name (pool offset)
0x10 u32 component count           0x14 u32 update rate
0x18 components back to back:      u32 type, u32 name (pool offset), payload
```

Recovered from Blur.exe: the header check is at `0x562810`; the component factory
(`0x5643f8`) switches on the type (≤ 0xDA, byte table `0x5658d4` → jump table
`0x565714`), constructs the class, then calls its `load` (vtable slot 1), which calls
the shared reader `0x5744f0` (name + FNV-1a hash) and returns the next record. That
gives every type's name (`mov eax, name; ret` getters) and size: most are fixed, the
rest are a header plus u16-counted lists (curve points 12 B, array/sum/product items
8 B, sound-table waves 4 B, mixer channel blocks 20/36/68/132 B). All 365 files parse
exactly up to their string pool.

Payload words (layouts inferred from every file, checked against the links they make):

| Kind | Bytes | Meaning |
|---|---|---|
| link | 4 | u16 component (0xFFFF = none), u16 output port — audio links sometimes leave junk in the port half |
| parameter | 8 | link + f32 constant (used when the link is 0xFFFF) |
| component | 4 | u32 component index (ActionIn → its Script, Array → its index input…) |
| string | 4 | pool offset (sounds: bank name, wave names) |

Field *names* aren't in the game files or the exe, so the decompiler numbers them by
kind (`param 1`, `input 2`…).

## `.baf` — "BANK" audio bank ✅

Little-endian. Decoded by `src/games/blur/BafBank.cpp`; all 319 banks (5,199 sounds,
~320 minutes) decode with frame counts matching their headers.

Header: `"BANK"`, u32 0x34, u32 4, u32 sound count, u16 0, char name[32] at 0x12.
Then one `"WAVE"` record per sound: `"WAVE"`, u32 record size, u32 type, char name[32] at +12.

| Type | Size | Fields (offsets from the record start) |
|---|---|---|
| 7 | 76 | +44 data offset, +48 data size, +52 f32 volume, +64 sample rate, +68 frames, +72 u16 loop flag, +74 u8 bits, +75 u8 channels — PlayStation-style ADPCM |
| 3 | 80 | 16-bit PCM; +60 rate, +64 bits, +68 channels (all silent "sync" tracks) |

ADPCM (decoder in Blur.exe at 0x540C80, tables at 0xE858F0 / 0xE85918): 33-byte frames,
header byte `filter << 4 | shift`, then 32 bytes = 64 samples, low nibble first.
`sample = nibble · 4096 · 2^-shift + h1·c1 + h2·c2` with float history and the filters
{0,0}, {0.9375,0}, {1.796875,−0.8125}, {1.53125,−0.859375}, {1.90625,−0.9375}.
Stereo alternates channels frame by frame.

Sound data follows the records, in record order: `"DATA"`, u32 (size + 8), then the
bytes the record's +44 offset points at. Banks inside `root_streaming.pak` pad with
zeros so that each `"DATA"` starts 8 bytes before a 0x800 boundary, and pad the whole
file to 0x800. **Writing** (`BafFormat::rebuild`) re-encodes replaced sounds (type 7:
ADPCM — each frame tries the five filters with shifts around the best fit, simulating
the float-history decoder; type 3 stays PCM), patches rate / frames / channels and the
offset / size fields, and lays the data out the same way. All 193 banks in the game's
archives rebuild byte for byte when nothing is replaced.

## `.model` geometry 🟡

Chunk file `CPModel { Header, MdlDat { Header, StrTab, … }, Models, Element, Constr, Render { Header, Scene } }`.
`StrTab` holds element names (`e_exterior`, `e_wheels`, `e_door_left`, …); `Scene` is an
`ARCH` stream: a serialized object graph where every record starts with `"RA"` + u16 version.
`src/games/blur/ModelDecoder.cpp` reads only the geometry records:

| Record | Layout |
|---|---|
| vertex buffer | `RA00`, u32 n, u16 element offsets[n], u32 vertex count, blob |
| index buffer | `RA01`, u32 index count, u32 0, blob of u16 |
| blob | `RA00`, u32 byte size, u32 16, zero padding to the first 16-byte boundary (counted from `"ARCH"`) at least 5 bytes past the align field, data |
| draw group | `RA00`, u32 n (LOD slots, 4 on cars), then n draws |
| draw | `RA00`, u32 start index, u32 index count, u32 min index, u32 vertex count. Unused LOD slots are empty draws (count 0, min index 0x7FFFFFFF): the part isn't drawn at that level |
| streams | right after a group (and its empty slots): `RA00`, u32 count (1–4), then `RA00`, u32 buffer number, u32 ?, u32 byte offset, u32 ? — byte offset / stride is the group's first vertex |

- Draws tile each index buffer exactly; indices are triangle strips stitched with
  degenerate triangles, or plain lists.
- Buffer numbers are *not* file order: each number's buffer is solved from constraints
  (all groups using it must fit; one buffer per number). Shadow-hull groups carry all-zero
  streams, so a buffer that fits a group exactly wins over the mapped one.
- Vertex element 0 is the position: 3 floats (12/16 bytes) or 4 halves (8 bytes).
  Buffers with only a position are shadow hulls (`veh_Shadow.fx`).
- 1,539 of 1,543 models decode; the other 4 contain no geometry.
- Positions are left-handed (Direct3D: x right, y up, z forward), so viewers mirror x.

### Names, hierarchy and placement (`MdlDat`)

- `StrTab`: u32 count, u32 offsets[count], u32 ?, then NUL-terminated strings.
- `Element`: 104-byte records — u32 0, f32 rotation[9], f32 position[3], f32 bounds[6],
  u32 name (string index), u32 index, i32 parent, i32 first child, i32 next sibling, u32 ?,
  u32 render tag (`0xC0000000 | k`, or −1 for nodes without geometry such as damage bones).
  Element 0 is `DefaultElement`; e.g. `e_exterior` › `e_door_left` › `e_mirror_left`.
- Transforms are absolute (children repeat their parent's placement), applied as
  `p' = p · R + position` (Direct3D row vectors). Wheel parts (tyres, discs, rims) are
  modelled at the origin and placed at their hub this way.
- Each draw group is preceded (0x54 bytes before its first draw) by
  `"RA" 01 00, u32 material, u32 ?, u32 ?, u32 ?, u32 element` — the element's low 16 bits
  name the part (high bits are flags). Matches 14,983 of 14,994 groups across all models.
- `Models` (100-byte records: rotation, position, bounds, links) holds the damage-bone set.
- Not yet: materials (`veh_Paint…fx`, textures), UVs, normals.

## Other files

| Ext | Magic / notes | Status |
|---|---|---|
| `.cycle` / `.skeleton` | headerless binary (floats / bone indices) | ❓ |
| `.dat` (audio) | `JBOA` | ❓ |
| `.bpr` | `2RZB` compressed UI resource | ❓ |
| `.ipf` | UI layout with named layers | ❓ |
| `.pf` | bitmap font: name + source path | ❓ |
| `.fxb` | compiled shader effects (`shaders-speed.pak`) | ❓ |
| `.loc`, `.bin`, `.xt`, `.aid` | gamedata tables | ❓ |
| `.levelstream` | `ARCH` container | ❓ next target |
| `.bik` | Bink video | external codec |
