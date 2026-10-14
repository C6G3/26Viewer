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
| `levels/<name>/Level.levelstream` | `ARCH` streamed render data (loose, ~4.7 GB) — see [Levels](#levels--levelrender--levelstream-) ✅ |
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
              (.levelrender: a fixed 8-byte field with junk after the NUL,
              "LS\0ID\0\0\0" — "LS" written over "NO_ID")
u32  size     whole chunk including this header
u16  count    child count for containers
u16  kind     'C' (0x43) container · 'A' (0x41) data
...  payload  children (containers) or raw data
```

Some `'A'` chunks (e.g. `CPRender`) still contain child chunks; the parser treats
a data chunk as nested when its payload parses exactly as a chunk sequence.
Root names seen: `Texture`, `  CPModel`, `Font`, `PtlData`, `  CPLevel`,
`  CPRender`, `  CPPhysic`.

The `  CP…` roots have a 20-byte header (4 extra bytes) and store a size 4 bytes
short, which would cut off their last child (`Coll` in models, `Routes` in level
physics). The child count is right, so the parser widens the root by 4 bytes when
its children fall short of it.

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

### Vertex elements, materials and textures

- **Vertex elements.** Each vertex buffer record is preceded by its element list:
  `RA00`, u32 stride, `RA00`, u32 n, then n × (`RA00`, u32 type, u32 0, u32 usage,
  u32 usage index). Usages are Direct3D's (0 position, 3 normal, 5 texcoord, 6/7
  tangent/binormal — those sit in a second stream); type codes are the engine's own:

  | Type | Bytes | Meaning |
  |---|---|---|
  | 0x06 | 12 | float3 (positions of world objects) |
  | 0x07 | 16 | float4 |
  | 0x08 | 4 | half2 (texture coordinates) |
  | 0x09 | 8 | half4 (car positions, texcoords; tangents) |
  | 0x0A | 4 | 4 bytes (indices) |
  | 0x0B | 4 | 4 bytes mapped to −1…1 (normals; also damage offsets as texcoords 2/3) |

  A shared list of vertex declarations (same element records, with stream and offset)
  sits at the start of the Scene stream too. Checked over all 1,745 models.
- **Materials.** `RA00`, u32 count, then per material an empty `RA00` and `RA00`, u32
  length, the effect name (`veh_Paint_a_b_c.fx`, `veh_Glass_c_h.fx`…). A draw group's
  material number (in the `RA01` record before its draws) indexes this list. The letters
  after the family name are feature switches.
- **Render list** (`RenderingData::RenderListNode_Common`, after the scene nodes):
  `RA00`, u32 count, then per entry `RA00` + effect name; `RA00`, u32 n, n × (`RA00` +
  input name, u32 type, u32 count); u32 dataSize, u32 align; `RA00`, u32 n, u16
  offsets[n]; u32 material, u32 0. Entry 0 is `InstanceReset` (no inputs), entry 2 an
  unnamed empty one; each material has a main entry and a lighting (`SH`) entry. Types
  are Blur.exe's `MaterialElementType`: 0 S8, 1 U8, 2 S16, 3 U16, 4 S32, 5 U32, 6 F32,
  7 TEXTURE_INDEX, 8 MATERIAL_INDEX, 9 DATA_INDEX, 10 TEXTURE_PTR.
- **Material values** follow the draw records: `RA00`, u32 0, u32 count, u32 bytes
  (= the sum of the entries' dataSize), then count × (u32 render-list entry, that
  entry's values in declared order: 1/2/4 bytes per value by type; a DATA_INDEX is
  `RA00` + two u32 — the animated data it reads, e.g. the vehicle's lighting or a
  wheel's). A draw group names its value sets after its first stream record
  (`RA00`, u32 set, u32 0): pass 1 is the material's main entry. Decoded for 1,725 of
  1,744 models, every byte accounted for. ✅
- **Which texture a TEXTURE_INDEX means** ✅ (from Blur.exe: `FUN_006cd820` builds the
  table, `FUN_006ed6d0` binds `table[index]` when a material is drawn): after the
  material values, the scene stores three (package, index) vectors in a row — each
  `RA00`, u32 count, count × (s32 package, u32 index) — followed by another `RA00`
  record; the third vector has one pair per texture index. Package −1 means the
  model's keyed table (`index`-th keyed texture); any other package is a streamed
  package, numbered in file order, each holding streamed textures in file order (in
  practice one each). Sorting the distinct (package, index) pairs therefore lines them
  up with the streamed textures in file order. Found and consistent in all 1,519
  models whose values decode; 82 cars and 240 keyed-only models are not in stored
  order (e.g. the Audi S3: index 1 = (5, 0) = its 5th streamed texture,
  `audis3_scratch`). A file can hold several scenes, each with its own table; the one
  after the decoded value table is used.
- **Textures** are stored in the model — every model holds all of its own (checked
  over all models: none uses a texture it doesn't store). Each texture's own record is
  one texture, even when names repeat (a car may hold several `DefaultColour`s) or are
  empty. The non-streamed ones form a keyed table after the material list (`RA00`,
  `RA00`, u32 2, u32 count, then per texture: name, u32 hash, `RA00`, `RA00`, u32 2,
  name, the record); streamed ones sit in groups later (`RA02`, `RA00`, u32 2, name,
  the record). Full ones: a
  length-prefixed name, an `RA01` header of 12 words ending in a hash and the byte size,
  then the `.texture` Data layout (width, height, depth, mips, format, 0, 3) and pixels.
  Streamed ones: the name and an `RA01` header of 13 words ending in hash, 1, 28 for
  the smallest size (16×16, whole mip chain), then each larger size as `RA00`, 2, 1, 28,
  the Data layout and a blob with that size's top level. The Audi S3 holds 15
  (interior/engine sheet `audis3_1`, `audis3_2`, `audis3_alpha`, scratches, tyres,
  brake discs, broken glass…).
- **Texture IDs.** The u32 after a keyed texture's name is the same for that texture in
  every model that carries it (`paintdent` is `0x2222D9F7` in all 40 cars checked), so the
  game can share one copy. It is not a CRC-32, FNV or other common hash of the name (with
  or without folders / extension, any case) nor of the pixels — most likely the exporter's
  ID for the source file. A texture added to a model would only need an unused value.
- `Coll` (after `Render`): the model's collision — see [Collision](#collision--physics-streams-).
- `MtaDbg` (last chunk): the exporter's build info and error log.

### What a model holds, and what is read

Over all 1,744 models (1,695 `CPModel`, 49 older `Model` roots): `Render/Render/Scene` is
99.4 % of the bytes (1.0 GB) and is read for vertex and index buffers, draws, streams,
materials and their values, the texture table and the textures. Still unread there: the
second vertex stream (tangents / binormals), the damage offsets in texcoords 2/3, and the
animated data a DATA_INDEX points at (vehicle lighting, wheel spin). Elsewhere: `Coll`
(4.8 MB) ✅, `MdlDat/Element` (1.2 MB) ✅, `StrTab` ✅, `Models` (0.4 MB, damage bones:
placement read, links not), `MdlDat/Header` (40 bytes: u32 1, u32 ?, u32 elements, u32 0,
f32 bounds[6]), `Constr` (always empty), and both `Header` chunks (4 bytes, always 0).

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

### Skinning ✅

- Skinned meshes (characters, crowds) carry two extra texcoord elements: usage 5 index 3,
  type 0x0A = four bone slots per vertex (u8), and usage 5 index 4, type 0x0B = their four
  weights (u8 / 255).
- A slot is a number into the draw group's **bone palette**: the `g_mPalette` input of the
  material entry, a DATA_INDEX found in a *later pass's* value set (each pass is 32 bytes
  after the group record: value set at group end + 28 + 32·pass). The palette's numbers
  index a second render list (entry 0 `Element`, then `AnimatedParam` entries whose single
  input is named after a bone), which gives each slot its bone name. Every skinned part of
  the player model names bones of `animations/igm_driver_ml.skeleton`.
- Rigid models (vehicles, props) aren't skinned: each part follows the bone named like its
  `MdlDat` element (or the nearest ancestor that has one). A model without a `.skeleton`
  is posed by its element hierarchy, which is how construction machines' cycles play.

## `.skeleton` ✅

Little-endian, version 7 (version 6 has one extra byte before the bone count):

```
u32 version, u32 bones
s16 parent[bones]                 -1 = root; parents come before children
bones x (f32 quaternion xyzw, f32 position[3], f32 scale[3])   rest pose, relative to the parent
bones x f32[12]                   inverse bind matrix, 3x4 row vectors (model -> bone space)
14 bytes (u32 4, zeros), u32 count, count x (u32 length, name)
```

Row-vector convention: `world = local x parentWorld`, and `inverseBind x world` is the
identity at rest (checked on every skeleton). 18+ skeletons in `root.pak`; the drivers'
`igm_driver_ml` has 32 bones (`hips` 0.873 m up).

## `.cycle` — animation ✅

Version 15; version 14's header is 24 bytes shorter (offsets below: `h` = 0x58, or 0x40).
Every one of the 805 cycles in `root.pak` decodes to its last byte. Reader:
`src/games/blur/Animation.cpp`.

```
u32 version, u32 0, two transforms (10 floats each), ...
h+4: u32 frames   h+8: f32 last frame   h+12: f32 frames per second
h+16: u32 bones, one byte per bone, u32 count, count length-prefixed bone names
u16 constant count, u16 animated count, u16 listSize[9], u16 channel ids (all lists)
u32 size of the constant values, the constant values
the animated keys (if any channel is animated)
```

A channel id is `bone x 64 + kind`: 0 rotation, 1 position, 2 scale (3, 9 and 10 are other
values the viewer ignores). The nine lists:

| List | Holds | Bytes per value |
|---|---|---|
| 0 | constant vectors | 12 (3 floats) |
| 1 | constant numbers | 4 |
| 2 | constant rotations | 6 (48-bit) |
| 3 | animated vectors | 12 |
| 4 | animated rotations | 4 (32-bit) |
| 5 | animated numbers | 4 |
| 6 | animated rotations | 6 (48-bit) |
| 7 | animated vectors | 6 (u16 x 3 within a range) |
| 8 | animated vectors | 3 (u8 x 3 within a range) |

Constant values (size = 12·k0 + 4·k1 + 6·k2 + 24·(k7 + k8)): first each list-7/8 channel's
range (f32 minimum[3], f32 size[3]), then lists 0, 1 and 2.

**Rotations** keep the quaternion's three smallest components ("smallest three"): bit 0 set
= the largest component is negative, bits 1–2 = which component it is, then from bit 3 the
other three in order, each mapping 0…max to −1/√2…+1/√2; the largest is rebuilt from
unit length. 48-bit = the three u16 as one number (first u16 highest), 15 bits per
component; 32-bit = a u32 with 9, 10 and 10 bits. Checked against the skeletons' rest
rotations of still prop parts (0.012° error) and on spinning rotors (exactly 72° per key).

**Animated keys** come in blocks of up to 255 frames:

```
u32 blocks + 1, u32 ?, u32 0
blocks x (u16 frames, u16 ?, u16 last frame, u16 0)
u16 0, u16 ?
per block (starting at the previous block's last frame):
  every animated channel's value at the block's first frame (lists 3..8 in order)
  u16 keyTotal[6]                    one per animated list
  per channel: u8 count, count x u8 frame (relative to the block's start)
  the key values, channel by channel
  last block only: every channel's value at the last frame
```

## Levels — `.levelrender` + `.levelstream` ✅

A track's visual world. `Level.levelrender` is a chunk file (`  CPRender` ›
`Header`, `Scene`, `LS` light samples, `Lights`, `PVSData`, `CubePt2`);
`Level.levelstream` holds the bulk, in 2 KiB-aligned `ARCH` sections the game
streams in as the camera moves. Opening either file shows the whole level in
the 3D viewer (it reads the other one from the same folder). Reference reader:
`tools/research/blur_level.py`; C++: `src/games/blur/Level.cpp`.

**ARCH serialisation.** Every value is `"RA"` + u16 version, then its fields; a
vector is a u32 count and the items. A polymorphic object is tag byte 3, u32
length + class name, then the object. A blob (buffer data) is `RA` 0, u32
size, u32 16, then zero padding up to a 16-byte boundary counted from the
section's `ARCH` start. The field orders follow Blur.exe's readers
(`RenderingData::*`, 0x6D5000–0x6F9000).

**Blocks.** Level data is split into numbered blocks, each stored at ten levels:

```
"RA" 0, "RA" 0, u32 2, u32 level
level 0: vertex buffers (tag 2 + VB), index buffers (tag 2 + IB),
         objects (tag 3 …), u32 n + textures
level 1-9: the larger texture mips (often empty)
```

A `ResourceIndex` {u32 block, u32 index} names the index-th vertex buffer
(index buffer, texture — whichever list holds the reference) of a block.

**Table of contents.** The `Scene` chunk is itself an `ARCH` stream: block 0
(the root CullNode "Static", whose sub-nodes are the scene nodes) and, at its
end, one entry per section of the stream (509 for Brighton):

```
RA 1, bounds, RA + n × (RA, u32 block, u32 level),
3 × (RA + u32 list), u64 offset, u64 size, u64 used
```

The blocks of a section sit back to back in the listed order; the reader finds
each by its header. A few levels name blocks no section holds (a handful of
meshes; `usa_la_downtown_multiplayer` 157); those meshes aren't drawn.

**Scene nodes** are blocks whose objects are a `CullNode` (name, sub-nodes,
bounds, visibility bits) and zero, one or two `RenderListNode_Common`s
(0x6EA950, version 4):

| Field | Meaning |
|---|---|
| CullNode body | bounds, PVS bits, group, portal bits |
| material list | material types: effect (`default_LN_NB_NL.fx`…) and its elements (name, type, count) |
| static geometry | drawn as it is — already in world space (roads, buildings) |
| draws | RA, u32 geometry, packed instance ref, streams, u32 |
| instanced draws | (empty in the shipped levels) |
| geometry | the meshes the draws place |
| value sets | per set: u32 type, then its elements' values; an `Instance` set holds `g_mWorld` |
| … | six-word list, 3 bounds lists, 3 LOD lists, u32 list, bit pairs, extra material lists |
| 3 ResourceIndex lists | index buffers, vertex buffers, textures (the slots geometry uses) |
| resource groups | RA + n × (RA, u32, 3 × (RA + n × (RA, ResourceIndex, u32))) |

Value kinds: 0/1 one byte, 2/3 two, 4–7 and 10 four, 8/9 a packed reference
(`RA`, two u32 — texture element type 7, `g_tAlbedo`, indexes the node's
texture list). `g_mWorld` is 16 floats in row-vector order (p' = p·M,
translation in the last row).

**Geometry** (0x6E5310): `RA` version; u32, u32, u32 primitive (1 = strip),
u32 index-buffer slot, u32 flags; a packed ref (and a second one from version 1);
`RA` + 24 bytes of render states (u32 × 3, four switch bytes — byte 0 alpha
blending: puddles, race-day lines and banners, flares; byte 3 alpha test:
foliage, fences, trees — then u32 × 2; roads and most buildings are opaque); `RA` + ranges (`RA`, u32 start, count,
min index, vertex count); `RA` + streams (`RA`, u32 vertex-buffer slot, u32,
u32 byte offset, u32, packed material value-set ref).

**Buffers.** Vertex buffer: `RA` 1; `RA` u32 stride; `RA` + elements (`RA`, u32
type, 0, usage, usage index); `RA` + u16 offsets; u32 count; blob. Types 6
float3, 7 float4, 8 half2, 9 half4, 0xB ubyte4n; usages 0 position, 3 normal,
5 texcoord, 9 a colour (ubyte4n: its alpha fades blended decals — the
race-day lines — out at their edges, and blends a road's two layers). Index
buffer: `RA` 1, u32 count, u32 0, blob of u16.

**Textures.** After a data block's index buffers: `RA` + u32 object count (0),
then u32 n textures, each `RA` 2, `RA` 0, u32 2, name (`roadtarmac01_diff`),
`RA` 1 + ten words, u32 hash, and its level-0 copy:

```
u32 present; if 1: u32 28, u32 width, u32 height, u32 depth, u32 mip count,
FourCC (DXT1 / DXT5), i32 -1, u32 3, blob
```

Levels 1–9 of the block hold the larger sizes the same way (header, u32 n,
per texture `RA` 2, `RA` 0, u32 2, then the copy above, often absent). Level
0 holds the small sizes as a whole mip chain (16×16 down to 1×1); each higher
level holds **one** size (32×32 at level 2, … 512×512 at level 6), so the
full chain is the largest copy followed by the smaller ones, level by level.
The header's mip count is that of the whole texture. Brighton: 3,602
textures, all DXT1/DXT5, up to 2048 pixels.

The render file keeps a few textures of its own (Brighton 45: the
environment cube map, light flares, race barriers, parked cars): in the
`Scene` chunk, `RA` 0, `RA` 0, u32 2, u32 n, then per texture its name,
u32 hash, `RA` 2, `RA` 0, u32 2, name, `RA` 1 + ten words, u32 hash, u32 size
and (size bytes) u32 width, height, layers (6: a cube map), mip count,
FourCC, -1, u32, and the whole chain. Texture references to block 0xFFFFFFFF
name these, by position (`g_tEnvironment` is 0, the cube map).

**Materials → textures.** A geometry's first stream names a value set (kind
0: `values[index]`); its texture elements (kind 7: `g_tAlbedo`, `g_tMaterial`,
`g_tLightmap`, `g_tSpecular`, `g_tNormal`…) hold an index into the render
list's texture `ResourceIndex` list, which names the block and the texture
in it. 97% of colour references are the level's own; the viewer shows each
part's colour texture (`g_tAlbedo`, `g_tColour`, `g_tTexture`, `g_tNeon`) at
up to 1024 pixels, kept compressed (Russian Hill, the largest: 3,041
textures, 434 MB).

**Lightmaps.** A lightmapped geometry has a second stream (its vertex-buffer
slot names no buffer) whose value set holds only `g_tLightmap`; the lightmap
coordinates are the second half of the vertex's four-part texture coordinate
(half4 `TEXCOORD0`.zw, plus `g_vUVOffset` for instances). Lightmaps are DXT5
atlases (`lightmap_main_008-a`, 512×512): rgb a tint (mostly sky blue, lamps
orange), alpha the brightness. The effects' simple pass (`MaterialLOD`, read
from `road_ln_ng.fxb` with D3DCompiler's disassembler) draws

```
light = (lightmap.rgb * lightmap.a)^2
detail = lerp(albedo, detailAlbedo, detailAlbedo.a * vertex.a)
colour = sqrt(light * detail * g_vLuminance.x)
```

and the frame is then tone-mapped (automatic exposure: `g_vLuminance` is 1
until the HDR pass sets it). The viewer draws lightmapped parts with this
formula (the lightmap is their only light), a fixed exposure of 3 standing in
for `g_vLuminance` and the automatic exposure; the game's tone mapping and
colour grading aren't reproduced, so the scene keeps the lightmaps' blue cast.

Brighton: 91 scene nodes, 13,851 draws, 4.7 million triangles, 1,859 textures
drawn, ~1,700 lightmapped parts. Not drawn yet: normal, specular and material
maps, the detail layers, the ocean (its normal maps are animated by the game).

## Collision — physics streams ✅

Models keep their collision in a `Coll` chunk; levels in `.levelphysics`
(`  CPPhysic` › `Header`, `Main` = track and walls, `Routes` › one `Route` per
route layout — a race uses one, so their barriers overlap). Same stream in both,
read by the game's loader (0x9520A0; shapes 0x8F7E00); every stream in the game
(1,632) parses to its end. Reference reader: `tools/research/blur_physics.py`.

```
"Head", u32 version (10; a few props 3, 6 or 7)
objects until u32 type == 0x0FFFFFFF:
  u32 type, base, type fields, then f32 position[3], f32 rotation[9], f32 inertia[9]
base: f32, f32, u32, u32 material (v>=2), u32 flags, u32, f32, u32 handle,
      f32 centre[3], u32, u32 (v>=7)
```

Flags: the static world uses 0x15f, 0x17f or 0x5d; 0 marks a movable object
(a fence post, a barrier panel; the first float is small, likely its mass).
Some levels hold movable pieces whose placement is the world's origin:
Hackney's 59 panels + 58 posts, the size of its 57 `Constructionbarrier2_`
actors, all stacked at (0, 0.5, 0) while its ground lies 290 m lower. The game
places them while it runs (from data elsewhere), so the viewer lists them under
"not placed (at the origin)", hidden. Placed movable pieces (Sausalito's 113
wooden posts, Brighton's post) stand where they belong.

**Checked against the render data** (both exported as `.obj`; each collision
triangle's distance to the nearest visible surface measured): in Brighton, Hackney, Barcelona Gracia and Sausalito the ground
(tarmac, sand, water…) and placed copies lie within 1 mm of the visible
surfaces (median 0.000 m), hulls, posts and boxes within 0.05–0.3 m. Route
barriers are vertical walls about 200 m tall (−100…+100 m) along the track
edge (0.3–0.9 m from visible geometry), invisible in the game.

| Type | Shape | Fields |
|---|---|---|
| 0, 11 | triangle mesh (11: a template placed by type 10) | see below |
| 17 | triangle mesh + settings | f32, mesh, 8 words (v>=5) |
| 2 | box | f32 half extents[3] |
| 3 | convex hull | u32 nv, u32 nt, f32 points[nv][3], u32 corners[nt][3] (points around the centre) |
| 4 | sphere | f32 radius (v>=4) |
| 5 | capsule | f32 height, f32 radius, along y (v>=4) |
| 6 | cylinder | f32 height, f32 radius, along y (v>=4) |
| 15 | cone | f32 radius, f32 height, along y (v>=4) |
| 7 | body | u32 count, u32 (v>=6), count × (u32 type, fields, f32, f32 rotation[9], f32 position[3], f32[9]) |
| 10 | placed copy | u32 handle of the template (v>=4) |

Shape kinds come from the game's volume formulas (π r² h, π r² h / 3…). A car part's
body has handle `0xC00000nn`, the render tag of its element (`e_wheel_backleft`,
`e_door_left`…); the car itself has `m_CollisionMesh`, `E_ObjectCollider` and
`E_CarCollider` hulls. Props are mostly bodies of boxes and cylinders (a chair:
two boxes and four legs) plus hulls for their broken pieces (`S_…`).

**Triangle mesh:** u32 vertexCount, u32 n, u32 bytes (in memory), u32 nodeCount,
f32 bounds[6], f32 vertices[vertexCount][3], u32[n]; a box tree of nodeCount
28-byte nodes (u16 box[6], u32 triangles, u32 group offset in memory, u32 first
vertex, u32 vertex count); then each leaf's triangles in group-offset order, in
groups of a 15-byte header (u16 surface material at +6, u8 count at +14) and
count × 11 bytes (u8 corners[3] relative to the leaf's first vertex, u32 edge
flags, u32); from v7 on u32 n, n × 12 bytes.

**Surface materials** (Blur.exe's table at 0x416AD0): 0 Default, 1 Tarmac,
2 Concrete, 3 Rock, 4 Dirt, 5 Mud, 6 Grass, 7 Gravel, 8 Sand, 9 WetSand, 10 Water,
11 Brick, 12 Vegetation, 13 Wood, 14 Metal, 15 Plastic, 16 Glass, 17 Rubber,
18 Upholstery, 19 Cardboard, 20 Flesh, 21 TrainTracks, 22 WaterDeep,
23 ConcreteFenceChain, 24 ConcreteFencePlastic, 25 MetalHollow, 26 MetalArmco,
27 MetalSheet, 28 MetalFenceChain, 29 MetalFenceSolid, 30 MetalFenceSlatted,
31 MetalSolidPost, 32 DirtLightRough, 33 DirtHeavyRough (34–36 unnamed).

## `.pf` — fonts ✅

Vector fonts in `ui/data/pf/` (5: Myers Sans Bold Slant — the game's menu font —
Swiss 721 Roman / Heavy / Hebrew, Bizarre Digital), exported from TrueType outlines.
Reader: `src/games/blur/FontFile.cpp`; all five read to their last byte.

```
0x000  name (NUL-terminated); the rest of the 256-byte header is the exporter's
       memory (source folder, "FONT - <name>", 0xFD / 0xCD fill)
0x100  u32 glyphs, per glyph:
         u16 character (Unicode), f32 left space, f32 width, f32 right space
         u32 contours, per contour: u32 points, points x (f32 x, f32 y, u8 kind)
then   u32 pairs, pairs x (u16 left, u16 right, f32 kerning)
```

Point kinds: 0 on the outline (a straight line leads to it), 2 the control point of a
quadratic curve to the next point, 3 the first of two control points of a cubic curve
(the next point is the second, the one after it the end). x runs from 0 at the glyph's
left edge; y points up in a box ~480 high, the baseline (bottom of the capitals) at
~90. Advance = left space + width + right space, plus the pair's kerning. Contours
are filled with the non-zero rule (holes run the other way).

## `.bpr` — UI texture packs ("2RZB") ✅

The menus' and HUD's textures (`ui/data/ui_common.bpr` 63, `ui_frontend.bpr` 226 — menu
art, car pictures, manufacturer logos —, `ui_game.bpr` 6, `ui_font_eur.bpr` the font
atlas). From Blur.exe's loader (0xD531E0, version 3 at 0xD53660). Reader:
`src/games/blur/UiPack.cpp` (a texture dictionary: listed, read, replaced in place).

```
"2RZB", u8 version (3), u8 flags (low 4 bits = 1: data compressed; no game pack is),
u16 table size, u32 records size, u32 data size, u32 0
20                   records, chained: u16 size (to the next; 0 on the last),
                     u16 name offset, type at +8 (u16). Type 1, a texture (28 bytes):
                     u32 data offset, u16 1, u16 ?, u32 data size, u32 0,
                     u16 format, u16 mip levels, u16 width, u16 height
20 + records size    names, NUL-terminated
20 + table size      pixel data (offsets are relative to here)
```

Formats: 1 DXT1, 5 DXT5, 6 A8R8G8B8, 7 L8 (masks such as glows and corners). DXT mip
chains stop at the last level 4 pixels across, so a texture can hold fewer levels than
its record says (the byte size tells). The loader also knows record types 2–4 (and
10/0x14/0x1E in version 2); no game pack uses them.

## `.ipf` — vector UI layouts ✅

Icon sheets, HUD parts, the countdown, and each track's minimap (`ui/data/ipf/`, 75; the
minimaps in `minimap/<track>_ui.ipf` / `_game.ipf`). From Blur.exe's loader (0x773A60;
groups 0x7737E0 / 0x7736A0; shapes 0x772B20). Little-endian and packed (no alignment).
Reader: `src/games/blur/UiLayout.cpp`; 71 read to their last byte — the other 4
(`controller`, `icon_sheet`, `shapes`: big-endian console files; `shop_icons_main`:
version 0x000F0004) the PC loader rejects too.

```
u32 version (0x0010xxxx), u32 layers, layers x (u32 3, group)
group:  u32 ?, string name, u32 children, children x (u32 type: 3 group, 1 shape)
shape:  u32 id, string name,
        u32 fill (0 none, 1 colour: f32 r g b, 2 gradient: 5 x u32 — unused by the game's files)
        f32 outline r g b, f32 outline width (pixels), u8 alpha, u8 ?
        f32[3] bounds min, f32[3] bounds max, f32[2], f32[2]
        u32 fill lists,    per list: u32 n, n x (f32 x, f32 y, f32 gradient u v, f32, f32)
        u32 index lists,   per list (one per fill list): u32 n, n x u16
        u32 outline lists, per list: u32 n, n x (f32 x, f32 y, f32 dx, f32 dy, f32[4])
string: u32 length, characters
```

Each child of a top-level layer is an item with its own frame: x runs 0..1 across it, y
down as far as the item reaches (an icon of a sheet, a route of a minimap, a frame of the
countdown). Fill lists are fans over the outline — drawn with the even-odd rule (stencil)
their overlaps cancel, leaving holes and hollows. Outlines are triangle strips: each point
sits at (x, y) pushed sideways by (dx, dy) pixels, half the outline width.

## `ui/data/ui_font_eur.dat` — font atlas table ✅

Where each character of the UI's bitmap fonts sits in their atlas textures (in
`ui_font_eur.bpr`). Shown as text by `src/games/blur/UiFontTable.cpp`; reads to its last byte.

```
i32 -1, u32 fonts, per font:
  char name[128] ("E_Swis712"), u32 0, char texture[128] ("e_swis712")
  u32 atlas width, u32 atlas height, u32 slots (295), u32 line height, u32 0, u32 ?, f32 1, f32 1
  slots x (u32 character — 0: unused, f32 u0 v0 u1 v1, i16 x offset, i16 width, i16 advance, u16 0)
  u32 pairs, pairs x (u16 left, u16 right, i8 kerning, u8 unused)
```

## Data tables — "Flask" packs (`gamedata/xt/*.bin`, `levels/*/actorpack.xt`) ✅

Everything the game is tuned with: car handling (`racingphysics.bin`), vehicles and
their damage, lights and wheels (`amax.bin`), paint jobs (`vehicles.bin`), the
career (`singleplayer.bin`, `finalracebook.bin`), multiplayer, pickups, UI
screens, effects, crowds… Both `gamedata.pak` and `gamedata_boot.pak` hold them.
Each pack describes its own types, so all of them read the same way. From
Blur.exe: header check `0x584700`, loader `0x585660`, record reader `0x584CD0`,
unscrambling `0x5871B0`. (The schema builder `0x1F04BB0` lives in the protected
`.8thSin` section; its calls are `push return; push target; ret` — it was traced
with Ghidra's emulator.)

```
0x00 "KSLF", u32 version (1, or 2: names and texts scrambled — almost all are 2)
0x08 eight sections, each u32 offset, u32 count:
  types   x 12: u16 name, u16 fields, u16 bases, u16 kind, u16 value type, u16 size
  bases   x 4:  u16 type, u16 offset           (each type's bases, in type order)
  fields  x 8:  u16 name, u16 type, u16 offset, u8 kind, u8 array
  names:        `count` bytes, 0-terminated names
  records x 8:  u32 id (sorted), u16 type, u16 block (0xFFFF: defined in another table)
  blocks  x 16: u32 record, u32 layouts, u32 object bytes, u32 text bytes
  layouts x 6:  u16 type, u16 count, u16 kind (0 objects, 1 pointers, 2 record refs)
  data:         per block: its objects, then its texts
```

* Type kinds: 0 a value, 1 an enum, 2 flags, 3 a struct. Value types: 1 bool,
  2 i8, 4 i32, 5 i64, 6 u8, 7 u16, 8 u32, 9 u64, 10 f32, 11 f64, 12 text (`sz8`),
  14 `LocId`. Values take 4 bytes (64-bit ones 8).
* Field kinds: 0 value, 1 enum (u32 item index), 2 flags (bit n = item n),
  3 struct inline, 4 pointer to an object of the same block (u16 layout, u16
  element), 5 another record (u32 index into the records), 6 an enum's item.
  An array field is u16 layout, u16 first element, u32 count.
* A block's objects: its layouts one after another, each starting on a multiple
  of 4; element size = the type's size (kind 0) or 4 (kinds 1, 2). Layout 0 is
  the record's own object. A text value is an offset into the block's texts.
* Record ids are shared by all tables: `finalracebook.bin` points at the
  `AmaxVehicle` records of `amax.bin`, which point at `RP_CarDynamics` in
  `racingphysics.bin` (`dynamics`).
* Scrambling (version 2), for the names and for each block's texts:
  `key = 0xCC; for i < n-1: c = key - (in[i] ^ 0xB3); out[i] = c; key += c - i`;
  the last byte becomes 0.
* Editing (`FlaskTable.cpp`, `ListEditor`): values are written in place. A list
  that gets longer gets copies of its last item inserted after it (an empty one
  gets blank items), each with its own copies of the item's lists and of the
  objects the text writes out inside it (the first pointer to an object, in
  text order, that no list covers); references to later elements of that layout
  shift up. A list that gets shorter loses items from its end; pointers to them
  become null, and it's refused if something outside the removed items points at
  what they hold. The sections are then rewritten in their order, 4-byte
  aligned, with the offsets and counts updated. One list per block per round,
  re-decompiling between rounds, until every length matches; then the values.

## `gamedata/xt/loc/*.loc` — text tables ("OLTX") ✅

```
0x00 "OLTX", u32 version 1, u32 16, u32 (first language table - 16)
0x10 u32 12, u32 languages (5: en fr de it es), u32 (key table - 16)
0x1C languages x {char code[16], u32 table offset, u32 table size}
tables (keys, then each language): u32 count, u32 8,
  count x {u32 id, u32 string offset from the table start}, then 0-terminated
  strings: 8-bit key names ("VEH_FULL_ASTRA_LEGEND"), UTF-16 texts
```

The ids are the `LocId` values data tables store.

## Other files

| Ext | Magic / notes | Status |
|---|---|---|
| `.fxb` | compiled shader effects (`shaders-speed.pak`) | ❓ |
| `.aid` | AI route data (`gamedata/levels/<level>/<level>_<n>.aid`: named routes such as `MainLoop`, `RacingCorner` points) | ❓ |
| `gamedata/events/*.csv` | plain CSV number tables, one per race size and difficulty (columns not mapped yet) | text |
| `.bik` | Bink video | external codec |
