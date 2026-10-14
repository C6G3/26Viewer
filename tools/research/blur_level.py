#!/usr/bin/env python3
"""Reference reader for Blur's level render data (.levelrender + .levelstream).

    python -I tools/research/blur_level.py F:/Games/BLUR/levels/uk_brighton [out.obj]

Both files hold "ARCH" streams of serialised objects. Every value is written as
"RA" + u16 version + its fields (little-endian); a vector is u32 count + items.
The field order below follows Blur.exe's binary readers (RenderingData::*,
0x6E0000-0x6F9000; the text writers next to them carry the field names).

Blocks. Level data is split into numbered *blocks*, each stored at ten detail
levels. A block (one level of it) starts
    "RA" 0, "RA" 0, u32 2, u32 level, then four lists:
vertex buffers (tag 2 + VB), index buffers (tag 2 + IB), objects (tag 3: u32
length + class name, then the object) and textures. Level 0 holds everything
but the larger texture mips, which levels 1-9 add. ResourceIndex {u32 Block,
u32 Index} names the Index-th vertex buffer (or index buffer, or texture:
whichever list the reference comes from) of a block.

  * .levelrender  the Scene chunk holds block 0 (the root CullNode "Static",
    whose SubNodes are the scene nodes) and ends with the stream's table of
    contents: per 2 KiB-aligned section of the stream, its offset and size
    and the (block, level) pairs it holds, in order.
  * .levelstream  the sections. Scene-node blocks hold a CullNode and zero,
    one or two RenderListNode_Commons; the others hold buffers and textures.

A render list draws its `static` geometry as it is (already in world space)
and its `geometry` through `draws`: each draw names a geometry and an
"Instance" value set (in `values`) whose g_mWorld places it. A geometry's
index-buffer slot indexes `ib`, its first stream's slot indexes `vb`.
"""

import mmap
import re
import struct
import sys

# ---- primitive reader ----------------------------------------------------------------

class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def ra(self):
        if self.d[self.p:self.p + 2] != b'RA':
            raise ValueError('expected RA at 0x%x: %s' % (self.p, bytes(self.d[self.p:self.p + 8]).hex(' ')))
        v = struct.unpack_from('<H', self.d, self.p + 2)[0]
        self.p += 4
        return v

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v

    def f32(self, n):
        v = struct.unpack_from('<%df' % n, self.d, self.p); self.p += 4 * n; return v

    def raw(self, n):
        v = bytes(self.d[self.p:self.p + n]); self.p += n; return v

    def str(self):
        return self.raw(self.u32()).decode('latin-1')

    def res(self):
        return (self.u32(), self.u32())

    def vec(self, item):
        return [item() for _ in range(self.u32())]

# ---- RenderingData types --------------------------------------------------------------

def node(r):                                   # RenderingData::Node
    r.ra()
    name = r.str()
    r.ra(); sub = r.vec(r.res)
    return {'name': name, 'sub': sub, 'parent': r.res(), 'location': r.res()}

def bounds(r):                                 # AabbMin, AabbMax, DistanceMin, DistanceMax
    r.ra(); return r.f32(8)

def bitvector(r):
    r.ra(); bits = r.u32()
    r.ra(); data = r.raw(r.u32())
    return bits, data

def cull_body(r, ver):                         # RenderingData::CullNode
    n = node(r)
    n['bounds'] = bounds(r)
    n['pvs'] = bitvector(r)
    n['group'] = r.u32() if ver >= 1 else 0xFFFFFFFF
    if ver >= 2:
        n['portals'] = bitvector(r)
    return n

def material_element(r):                       # name, type, count
    r.ra(); return (r.str(), r.u32(), r.u32())

def material_type(r):                          # Effect, MaterialElements, DataSize, MaxAlign, Offsets, ?, ListIndex
    r.ra()
    t = {'effect': r.str()}
    r.ra(); t['elements'] = r.vec(lambda: material_element(r))
    t['size'], t['align'] = r.u32(), r.u32()
    r.ra(); t['offsets'] = r.vec(r.u16)
    t['a'], t['b'] = r.u32(), r.u32()
    return t

def value_sets(r, types):
    """u32 sets, u32 bytes, then per set u32 type and that type's values."""
    n, _ = r.u32(), r.u32()
    sets = []
    for _ in range(n):
        t = r.u32()
        vals = []
        for name, kind, count in types[t]['elements']:
            for _ in range(count):
                if kind in (0, 1): vals.append(r.u8())
                elif kind in (2, 3): vals.append(r.u16())
                elif kind in (4, 5, 6, 7, 10): vals.append(r.raw(4))
                elif kind in (8, 9): vals.append(('tex',) + packed(r))
                else: raise ValueError('value kind %d' % kind)
        sets.append((t, vals))
    return sets

def material_list(r):                          # types, then value sets
    r.ra()
    r.ra(); types = r.vec(lambda: material_type(r))
    return {'types': types, 'sets': value_sets(r, types)}

def packed(r):                                 # RA, u32 index, u32 kind (kept as index | kind << 28)
    r.ra(); return (r.u32(), r.u32())

def range_(r):                                 # RA, u32 start, u32 count, u32 min index, u32 vertices
    r.ra(); return tuple(r.u32() for _ in range(4))

def stream(r):                                 # RA, u32 ?, u32 ?, u32 byte offset, u32 ?, packed buffer
    r.ra()
    v = (r.u32(), r.u32(), r.u32(), r.u32())
    return v + (packed(r),)

def geometry(r):                               # 6e5310: one draw call's geometry
    ver = r.ra()
    g = {'a': r.u32(), 'b': r.u32(), 'prim': r.u32(), 'c': r.u32(), 'd': r.u32()}
    g['indices'] = packed(r)
    g['decl'] = packed(r) if ver else None
    r.ra()                                     # 6d7b00: render states
    g['states'] = (r.u32(), r.u32(), r.u32(), r.raw(4), r.u32(), r.u32())
    r.ra(); g['ranges'] = r.vec(lambda: range_(r))
    r.ra(); g['streams'] = r.vec(lambda: stream(r))
    return g

def geometry_list(r):                          # 6e77a0 / 6e6c30
    r.ra(); return r.vec(lambda: geometry(r))

def draw(r):                                   # 6e6d80 / 6e4900
    r.ra()
    d = {'material': r.u32(), 'geometry': packed(r)}
    r.ra(); d['streams'] = r.vec(lambda: stream(r))
    d['values'] = r.u32()
    return d

def draw_list(r):                              # 6e8060 / 6e7ad0
    r.ra(); return r.vec(lambda: draw(r))

def instanced(r):                              # 6e8990: a draw plus its instance stream
    r.ra(); d = draw(r)
    d['instance'] = stream(r)
    return d

def instanced_list(r):
    r.ra(); return r.vec(lambda: instanced(r))

def six(r):                                    # 6dc8c0 / 6dc780
    r.ra(); return [r.u32() for _ in range(6)]

def byte_vec(r):
    r.ra(); return r.raw(r.u32())

def lod_list(r):                               # 6e7830 / 6e6130
    ver = r.ra()
    a = r.u32()
    u16s = None
    if ver:
        r.ra(); u16s = r.vec(r.u16)
    return {'a': a, 'u16': u16s, 'b': r.u32(), 'bytes': byte_vec(r)}

def bit_pair(r):                               # 6e84e0
    r.ra()
    out = []
    for _ in range(2):
        r.ra(); bits = r.u32()
        out.append((bits, byte_vec(r)))
    return out

def entry3(r):                                 # 6dc9b0 / 6dc950: ResourceIndex, u32
    r.ra(); return (r.res(), r.u32())

def resource_group(r):                         # 6e7cf0 / 6e6410
    r.ra()
    k = r.u32()
    lists = []
    for _ in range(3):
        r.ra(); lists.append(r.vec(lambda: entry3(r)))
    return (k, lists)

def render_list(r, ver):                       # RenderingData::RenderListNode_Common (v4)
    cv = r.ra()
    n = cull_body(r, cv)
    n['materials'] = material_list(r)
    n['static'] = geometry_list(r)            # world-space, drawn as it is
    n['draws'] = draw_list(r)
    n['instanced'] = instanced_list(r)
    n['geometry'] = geometry_list(r)          # what the draws place
    n['values'] = value_sets(r, n['materials']['types'])  # what the draws' instance refs name
    r.ra(); n['six'] = r.vec(lambda: six(r))
    for k in ('bounds_a', 'bounds_b', 'bounds_c'):
        r.ra(); n[k] = r.vec(lambda: bounds(r))
    for k in ('lod_a', 'lod_b', 'lod_c'):
        n[k] = lod_list(r)
    r.ra(); n['u32s'] = r.vec(r.u32)
    r.ra(); n['bitpairs'] = r.vec(lambda: bit_pair(r))
    if ver == 3:
        n['extra'] = [material_list(r), material_list(r)]
    elif ver > 3:
        r.ra(); n['extra'] = r.vec(lambda: material_list(r))
    for k in ('ib', 'vb', 'textures'):          # ResourceIndex lists
        r.ra(); n[k] = r.vec(r.res)
    r.ra(); n['groups'] = r.vec(lambda: resource_group(r))
    return n

# ---- blocks ------------------------------------------------------------------------------

BLOCK = re.compile(rb'RA\x00\x00RA\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00RA\x00\x00')


def node_block(d, p):
    """A scene-node block at p: CullNode + RenderListNode_Commons. Returns (objects, end)."""
    r = R(d, p)
    r.ra(); r.ra(); r.u32(); r.u32()
    r.ra(); assert r.u32() == 0
    r.ra(); assert r.u32() == 0
    r.ra()
    objs = []
    for _ in range(r.u32()):
        tag = r.u8()
        if tag != 3:
            raise ValueError('object tag %d at 0x%x' % (tag, r.p - 1))
        cls = r.raw(r.u32()).decode()
        if cls == 'RenderingData::CullNode':
            o = cull_body(r, r.ra())
        elif cls == 'RenderingData::RenderListNode_Common':
            r.ra()                             # the pointer wrapper
            o = render_list(r, r.ra())
        else:
            raise ValueError('class ' + cls)
        o['class'] = cls
        objs.append(o)
    return objs, r.p


def main():
    folder = sys.argv[1].rstrip('/\\')
    f = open(folder + '/Level.levelstream', 'rb')
    d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    starts = [m.start() for m in BLOCK.finditer(d)]
    nodes = []
    for p in starts:
        if d[p + 45:p + 45 + 23] == b'RenderingData::CullNode':
            objs, end = node_block(d, p)
            nodes.append((p, objs, end))
    print('%d blocks, %d scene nodes' % (len(starts), len(nodes)))
    for p, objs, end in nodes[:3]:
        for rl in objs[1:]:
            print(hex(p), hex(end), rl['location'], len(rl['static']), 'static', len(rl['geometry']), 'geometry',
                  len(rl['draws']), 'draws', len(rl['ib']), 'IBs', len(rl['vb']), 'VBs', len(rl['textures']), 'textures')


if __name__ == '__main__':
    main()
