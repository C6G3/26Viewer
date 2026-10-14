#!/usr/bin/env python3
"""Lists and disassembles the shader programs inside a Blur effect (.fxb).

    python -I tools/research/blur_fxb.py shaders/veh_paint_a.fxb        # list programs and passes
    python -I tools/research/blur_fxb.py shaders/veh_paint_a.fxb 3      # disassemble program 3

An .fxb ("2BXFFXB" / "PCFX") holds an effect's techniques/passes (names such as
"Material", "MaterialNoDamage", "MaterialLOD") and, for each pass, a Direct3D 9
vertex shader (vs_3_0) and pixel shader (ps_3_0) as raw bytecode. Each program
starts with its version token (0xFFFE0300 vertex, 0xFFFF0300 pixel), carries a
constant table comment ("CTAB": parameter names, registers) and ends with
0x0000FFFF. Disassembly uses Windows' own d3dcompiler_47.dll (D3DDisassemble),
so this runs on Windows only. Extract effects first, e.g.

    26viewer-cli extract F:/Games/BLUR/cache/shaders-speed.pak out veh_paint
"""

import ctypes
import re
import struct
import sys

PASS_NAMES = re.compile(rb'(Material[A-Za-z]*|Shadow[A-Za-z]*|Depth[A-Za-z]*|ZPrepass)\x00')


def programs(data):
    """[(index, 'vs'/'ps', offset, bytes)] in file order."""
    out, p = [], 0
    while True:
        starts = [x for x in (data.find(b'\x00\x03\xff\xff', p), data.find(b'\x00\x03\xfe\xff', p)) if x >= 0]
        if not starts:
            return out
        i = min(starts)
        q = i + 4
        while q + 4 <= len(data):
            t = struct.unpack_from('<I', data, q)[0]
            if t == 0x0000FFFF:
                q += 4
                break
            q += 4 + (4 * (t >> 16) if t & 0xFFFF == 0xFFFE else 0)
        out.append((len(out), 'ps' if data[i + 2:i + 4] == b'\xff\xff' else 'vs', i, data[i:q]))
        p = q


def disassemble(code):
    dll = ctypes.WinDLL('d3dcompiler_47.dll')
    dll.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p,
                                   ctypes.POINTER(ctypes.c_void_p)]
    blob = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(code, len(code))
    hr = dll.D3DDisassemble(buf, len(code), 0, None, ctypes.byref(blob))
    if hr != 0:
        raise OSError('D3DDisassemble failed: %#x' % (hr & 0xFFFFFFFF))
    # ID3DBlob: vtable slots 3 GetBufferPointer, 4 GetBufferSize.
    vtable = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    pointer = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])(blob)
    size = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtable[4])(blob)
    return ctypes.string_at(pointer, size).decode('latin-1').replace('\x00', '')


def main():
    data = open(sys.argv[1], 'rb').read()
    progs = programs(data)
    if len(sys.argv) > 2:
        print(disassemble(progs[int(sys.argv[2])][3]))
        return
    for m in PASS_NAMES.finditer(data):
        print('%#07x  pass %s' % (m.start(), m.group(1).decode()))
    for index, kind, offset, code in progs:
        print('%#07x  program %d: %s, %d bytes' % (offset, index, kind, len(code)))


if __name__ == '__main__':
    main()
