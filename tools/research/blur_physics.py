#!/usr/bin/env python3
"""Reference reader for Blur physics streams (collision shapes).

Found in two places, same layout:
  * a model's "Coll" chunk (cars, props): bodies made of shapes
  * a level's .levelphysics file: "Main" chunk (the track) and one stream per
    "Route" chunk (barriers for each route layout)

    python -I tools/research/blur_physics.py F:/Games/BLUR/levels/uk_brighton/Level.levelphysics
    python -I tools/research/blur_physics.py some.model

Stream:  "Head", u32 version (10), then objects until type 0x0FFFFFFF:
    u32 type, the object's fields (below), then its placement: f32 position[3],
    f32 rotation[9] (rows; p' = p * R + position), f32 inertia[9].

Every object starts with 13 common words ("base"):
    f32, f32, u32, u32 (version >= 2), u32 flags, u32, f32, u32 handle/tag,
    f32 offset[3] (centre of mass), u32, u32 (version >= 7)
Streams are version 10 (a few props 3, 6 or 7). Shape sizes and radii need version >= 4.
The handle is 0xC00000nn for a car part (the model element with that render
tag), 0x8000nnnn / 0xE00000nn / 0xA00000nn for shapes and instances.

Types (fields after the base):
    0, 11  triangle mesh: see read_mesh
    17     f32, triangle mesh, then (version >= 5) 8 more words
    2      box: f32 half extents[3]
    3      convex hull: u32 nv, u32 nt, f32 vertices[nv][3], u32 triangles[nt][3]
    4      sphere: f32 radius
    5, 6, 15  cylinder-like: f32 height (full, along y), f32 radius
    7      body: u32 count, u32 ?, then count shapes, each: u32 type, the
           shape's fields, f32 ?, f32 rotation[9], f32 position[3], f32 ?[9]
    10     instance: u32 handle of the shape it repeats (placed by its own
           placement words)
"""

import struct
import sys

END = 0x0FFFFFFF


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self, n=1):
        v = struct.unpack_from('<%df' % n, self.d, self.p)
        self.p += 4 * n
        return v if n > 1 else v[0]


def read_base(r, version):
    start = r.p
    r.p += 8                            # f32, f32
    r.u32()
    if version >= 2:
        r.u32()
    flags = r.u32()
    r.u32()
    r.f32()
    handle = r.u32()
    offset = r.f32(3)
    r.u32()
    if version >= 7:
        r.u32()
    return {'flags': flags, 'handle': handle, 'offset': offset, 'base_words': (r.p - start) // 4}


def read_mesh(r, version):
    """u32 nv, u32 nk, u32 bytes (in memory), u32 nodes, f32 bounds[6],
    f32 vertices[nv][3], u32 ?[nk], tree nodes[nodes] (28 bytes: u16 box[6],
    u32 triangleCount, u32 groupOffset (in memory), u32 firstVertex,
    u32 vertexCount), then for each leaf (by groupOffset) its triangles in
    groups: 13 bytes (u16 ?, u16 ?, u16 ?, u16 surface, ...), u8 ?, u8 count,
    count x (u8 a, u8 b, u8 c, u32 edges, u32 ?) — corners are firstVertex + a/b/c.
    Version 7 on: u32 n, n x 12 bytes (not needed for display)."""
    nv, nk, _, nn = r.u32(), r.u32(), r.u32(), r.u32()
    r.f32(6)
    verts = struct.unpack_from('<%df' % (3 * nv), r.d, r.p)
    r.p += 12 * nv + 4 * nk
    leaves = []
    for k in range(nn):
        tc, off, fv, vc = struct.unpack_from('<4I', r.d, r.p + 28 * k + 12)
        if tc:
            leaves.append((off, tc, fv, vc))
    r.p += 28 * nn
    leaves.sort()
    tris = []
    for off, tc, fv, vc in leaves:
        left = tc
        while left > 0:
            surface = struct.unpack_from('<H', r.d, r.p + 6)[0]
            count = r.d[r.p + 14]
            if count == 0 or count > left:
                raise ValueError('bad triangle group at %#x' % r.p)
            r.p += 15
            for _ in range(count):
                a, b, c = r.d[r.p], r.d[r.p + 1], r.d[r.p + 2]
                if max(a, b, c) >= vc:
                    raise ValueError('corner outside its leaf at %#x' % r.p)
                tris.append((fv + a, fv + b, fv + c, surface))
                r.p += 11
            left -= count
    if version >= 7:
        n = r.u32()
        r.p += 12 * n
    return {'vertices': verts, 'triangles': tris}


def read_shape_fields(r, t, version):
    base = read_base(r, version)
    s = {'type': t, **base}
    if t in (0, 11):
        s.update(read_mesh(r, version))
    elif t == 17:                       # mesh with extra settings
        r.f32()
        s.update(read_mesh(r, version))
        if version >= 5:
            r.f32(8)
    elif t == 2:
        s['half'] = r.f32(3)
    elif t == 3:
        nv, nt = r.u32(), r.u32()
        s['vertices'] = struct.unpack_from('<%df' % (3 * nv), r.d, r.p)
        r.p += 12 * nv
        idx = struct.unpack_from('<%dI' % (3 * nt), r.d, r.p)
        r.p += 12 * nt
        s['triangles'] = [(idx[3 * i], idx[3 * i + 1], idx[3 * i + 2], 0) for i in range(nt)]
    elif t == 4:
        s['radius'] = r.f32() if version >= 4 else 0.0
    elif t in (5, 6, 15):
        s['height'], s['radius'] = r.f32(2) if version >= 4 else (0.0, 0.0)
    elif t == 7:
        count = r.u32()
        if version >= 6:
            r.u32()
        s['shapes'] = []
        for _ in range(count):
            st = r.u32()
            child = read_shape_fields(r, st, version)
            r.f32()
            child['rotation'] = r.f32(9)
            child['position'] = r.f32(3)
            r.f32(9)
            s['shapes'].append(child)
    elif t == 10:
        s['instance_of'] = r.u32() if version >= 4 else 0
    else:
        raise ValueError('unknown shape type %d at %#x' % (t, r.p))
    return s


def read_stream(data, pos):
    r = Reader(data, pos)
    if data[pos:pos + 4] != b'Head':
        raise ValueError('no Head')
    r.p += 4
    version = r.u32()
    objects = []
    while True:
        t = r.u32()
        if t == END:
            break
        o = read_shape_fields(r, t, version)
        o['position'] = r.f32(3)
        o['rotation'] = r.f32(9)
        r.f32(9)
        objects.append(o)
    return objects, r.p


def streams(data):
    """(name, start, end) of every physics stream in a chunk file."""
    out = []
    for name in (b'Coll\0\0\0\0', b'Main\0\0\0\0', b'Route\0\0\0'):
        i = data.find(name)
        while i >= 0:
            size = struct.unpack_from('<I', data, i + 8)[0]
            if data[i + 16:i + 20] == b'Head':
                out.append((name.rstrip(b'\0').decode(), i + 16, i + size))
            i = data.find(name, i + 1)
    return out


def summary(o, depth=0):
    pad = '  ' * depth
    t = o['type']
    extra = ''
    if 'vertices' in o:
        extra = ' %d vertices, %d triangles' % (len(o['vertices']) // 3, len(o['triangles']))
    if 'half' in o:
        extra = ' half %s' % ['%.3f' % x for x in o['half']]
    if 'radius' in o:
        extra += ' radius %.3f' % o['radius']
    if 'height' in o:
        extra += ' height %.3f' % o['height']
    if 'instance_of' in o:
        extra = ' of %#x' % o['instance_of']
    pos = o.get('position')
    print('%stype %d handle %#x%s%s' % (pad, t, o['handle'], extra, ' at %s' % ['%.2f' % x for x in pos] if pos else ''))
    for c in o.get('shapes', []):
        summary(c, depth + 1)


def main():
    for path in sys.argv[1:]:
        data = open(path, 'rb').read()
        for name, start, end in streams(data):
            objects, reached = read_stream(data, start)
            print('%s: %s stream, %d object(s), parsed to %#x of %#x' % (path, name, len(objects), reached, end))
            for o in objects:
                summary(o, 1)


if __name__ == '__main__':
    main()
