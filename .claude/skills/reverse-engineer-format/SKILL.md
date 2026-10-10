---
name: reverse-engineer-format
description: Workflow for figuring out an unknown game file or archive format (hex dumps, Python prototype, validation over every file, documentation) before writing C++. Use when a game file's layout is unknown or a reader produces wrong data.
---

# Reverse-engineering a game format

Never write the C++ reader first. Prove the format in a throw-away Python
prototype, validate it on **every** file the game ships, then port.

## 1. Survey

```bash
# What's there, how much of each?
powershell: Get-ChildItem <install> -Recurse -File | Group-Object Extension | Sort Count -Desc
# Headers of every candidate file side by side
for f in <dir>/*.pak; do echo "== $f"; xxd -l 96 "$f"; done
```

Look for: magic bytes, version, alignment (0x800, 0x1000), sizes that match the
file size, offsets that point inside the file, little vs big endian, readable
strings (or obviously XOR'd text — repeating patterns like `56 04` where `00 5C` is expected).
Check the install for helper scripts (`*.bms` QuickBMS scripts are great hints
but are often outdated or wrong for some files).

## 2. Prototype in Python

- Put scripts in the session scratchpad first; promote the final one to `tools/research/<game>_<format>.py`.
- Run with `python -I` (isolated mode).
- Derive structure sizes from the data when possible (e.g. `stride = tableBytes / count`)
  rather than hard-coding — that's how 32/40/64-byte entry variants were found in Blur.

## 3. Validate exhaustively

- Decode **every** entry of **every** archive; count successes vs failures per archive.
- Tabulate unknown fields (`collections.Counter`) to spot flags vs hashes vs sizes.
- Test hypotheses for hash fields (CRC-32 of data, of stored bytes, of name…) and record the negative results too.
- If a reference extraction exists, byte-compare everything (`cmp`). When they differ,
  inspect both — the reference can be the wrong one (QuickBMS mis-extracts Blur's `headlight.raw`).
- Watch for "coincidence" bugs: e.g. a compressed chunk whose size equals its
  output size looks stored. Prefer try-decode-then-fallback over size heuristics.

## 4. Document

Write/extend `docs/formats/<game>.md`: offset tables, encodings, gotchas, and a
status marker per field (✅ verified · 🟡 partly · ❓ unknown). Note what was
verified and against how many files.

## 5. Port to C++

- Archive container → `ArchiveFormat` + `Archive` in `src/games/<game>/` (see the `add-game-module` skill).
- Image-like data → `ImageDecoder`; tagged/structured data → `StructureParser`.
- Parse with `BinaryReader`/`loadScalar` and bounds checks; return `fail(ErrorCode::…, "…")`, never throw.
- Add synthetic tests in `tests/` that build the format in memory (see `tests/test_blur_pak.cpp`)
  — they must pass without game files. Real-file tests go behind an env var (e.g. `BLUR_DIR`).
- Re-run `26viewer-cli verify <install>` and compare with the Python numbers.
