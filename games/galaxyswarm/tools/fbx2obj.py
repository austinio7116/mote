#!/usr/bin/env python3
"""Minimal binary FBX -> OBJ converter (geometry + UVs, per-object transforms).

Parses the Kaydara FBX binary node tree, extracts Geometry nodes
(Vertices / PolygonVertexIndex / LayerElementUV), applies each mesh's
Model transform (Lcl Translation/Rotation/Scaling + geometric transforms),
and writes a single merged OBJ.
"""
import sys, struct, zlib, math

def read_node(buf, pos, ver):
    if ver >= 7500:
        end, nprops, plen = struct.unpack_from('<QQQ', buf, pos); pos += 24
        nlen = struct.unpack_from('<B', buf, pos)[0]; pos += 1
    else:
        end, nprops, plen = struct.unpack_from('<III', buf, pos); pos += 12
        nlen = struct.unpack_from('<B', buf, pos)[0]; pos += 1
    if end == 0:
        return None, pos
    name = buf[pos:pos+nlen].decode('ascii', 'replace'); pos += nlen
    props = []
    for _ in range(nprops):
        t = chr(buf[pos]); pos += 1
        if t == 'Y': v = struct.unpack_from('<h', buf, pos)[0]; pos += 2
        elif t == 'C': v = bool(buf[pos]); pos += 1
        elif t == 'I': v = struct.unpack_from('<i', buf, pos)[0]; pos += 4
        elif t == 'F': v = struct.unpack_from('<f', buf, pos)[0]; pos += 4
        elif t == 'D': v = struct.unpack_from('<d', buf, pos)[0]; pos += 8
        elif t == 'L': v = struct.unpack_from('<q', buf, pos)[0]; pos += 8
        elif t in 'fdliby':
            n, enc, clen = struct.unpack_from('<III', buf, pos); pos += 12
            raw = buf[pos:pos+clen]; pos += clen
            if enc: raw = zlib.decompress(raw)
            fmt = {'f':'f','d':'d','l':'q','i':'i','b':'b','y':'h'}[t]
            v = list(struct.unpack('<%d%s' % (n, fmt), raw))
        elif t == 'S':
            n = struct.unpack_from('<I', buf, pos)[0]; pos += 4
            v = buf[pos:pos+n].decode('utf-8', 'replace'); pos += n
        elif t == 'R':
            n = struct.unpack_from('<I', buf, pos)[0]; pos += 4
            v = buf[pos:pos+n]; pos += n
        else:
            raise ValueError('unknown prop type %r' % t)
        props.append(v)
    children = []
    while pos < end:
        child, pos = read_node(buf, pos, ver)
        if child is None:
            break
        children.append(child)
    return (name, props, children), max(pos, end)

def parse(path):
    buf = open(path, 'rb').read()
    assert buf[:20] == b'Kaydara FBX Binary  '
    ver = struct.unpack_from('<I', buf, 23)[0]
    pos = 27
    roots = []
    while pos < len(buf):
        node, pos = read_node(buf, pos, ver)
        if node is None:
            break
        roots.append(node)
    return roots

def find_all(nodes, name):
    return [n for n in nodes if n[0] == name]

def child(node, name):
    for c in node[2]:
        if c[0] == name:
            return c
    return None

def prop70(model, key, default):
    p = child(model, 'Properties70')
    if p:
        for c in p[2]:
            if c[1] and c[1][0] == key:
                return c[1][4:7]
    return default

def rot_matrix(rx, ry, rz):
    rx, ry, rz = (math.radians(a) for a in (rx, ry, rz))
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    # FBX default rotation order XYZ: R = Rz*Ry*Rx
    return [
        [cy*cz, sx*sy*cz - cx*sz, cx*sy*cz + sx*sz],
        [cy*sz, sx*sy*sz + cx*cz, cx*sy*sz - sx*cz],
        [-sy,   sx*cy,            cx*cy],
    ]

def apply_m(m, s, t, p):
    x = p[0]*s[0], p[1]*s[1], p[2]*s[2]
    return tuple(m[i][0]*x[0] + m[i][1]*x[1] + m[i][2]*x[2] + t[i] for i in range(3))

def arrprop(node):
    """Array payload: FBX 7.x = one typed-array prop; 6100 = flat scalar props."""
    if node is None: return None
    if len(node[1]) == 1 and isinstance(node[1][0], list):
        return node[1][0]
    return list(node[1])

def prop60(model, key, default):
    p = child(model, 'Properties60')
    if p:
        for c in p[2]:
            if c[1] and c[1][0] == key and len(c[1]) >= 6:
                return c[1][3:6]
    return default

def convert(src, dst, scale=1.0):
    roots = parse(src)
    objects = None
    conns = []
    for r in roots:
        if r[0] == 'Objects': objects = r
        if r[0] == 'Connections':
            conns = [(c[1][1], c[1][2]) for c in r[2] if c[0] == 'C' and len(c[1]) >= 3]
    geoms = {}   # id -> node
    models = {}  # id -> node
    for n in objects[2]:
        if n[0] == 'Geometry': geoms[n[1][0]] = n
        elif n[0] == 'Model':
            models[n[1][0]] = n
            # FBX 6100: geometry lives directly on Mesh-type Model nodes
            if len(n[1]) > 1 and n[1][1] == 'Mesh' and child(n, 'Vertices'):
                geoms[n[1][0]] = n
    parent = dict(conns)  # child -> parent (first wins is fine for simple scenes)
    out_v, out_vt, out_f = [], [], []
    for gid, g in geoms.items():
        vn = child(g, 'Vertices'); pn = child(g, 'PolygonVertexIndex')
        if not vn or not pn: continue
        verts = arrprop(vn); poly = arrprop(pn)
        # transform: 7.x = connected Model node; 6100 = the Model node itself
        mid = gid if g[0] == 'Model' else parent.get(gid)
        T = (0.0, 0.0, 0.0); R = (0.0, 0.0, 0.0); S = (1.0, 1.0, 1.0)
        if mid in models:
            m = models[mid]
            getp = prop60 if child(m, 'Properties60') else prop70
            T = tuple(float(v) for v in getp(m, 'Lcl Translation', (0, 0, 0)))
            R = tuple(float(v) for v in getp(m, 'Lcl Rotation', (0, 0, 0)))
            S = tuple(float(v) for v in getp(m, 'Lcl Scaling', (1, 1, 1)))
        M = rot_matrix(*R)
        # UVs
        uvn = child(g, 'LayerElementUV')
        uvs, uvidx, uvmap = None, None, 'NoMappingInformation'
        if uvn:
            uvd = child(uvn, 'UV'); uvi = child(uvn, 'UVIndex')
            ref = child(uvn, 'MappingInformationType')
            uvmap = ref[1][0] if ref else ''
            uvs = arrprop(uvd)
            uvidx = arrprop(uvi)
        vbase = len(out_v); tbase = len(out_vt)
        for i in range(0, len(verts), 3):
            p = apply_m(M, S, T, (verts[i], verts[i+1], verts[i+2]))
            out_v.append((p[0]*scale, p[1]*scale, p[2]*scale))
        if uvs:
            for i in range(0, len(uvs), 2):
                out_vt.append((uvs[i], uvs[i+1]))
        # polygons: negative index marks end (value = ~idx)
        face, fuv = [], []
        for k, idx in enumerate(poly):
            last = idx < 0
            if last: idx = ~idx
            face.append(idx)
            if uvs is not None:
                if uvmap.startswith('ByPolygonVertex'):
                    fuv.append(uvidx[k] if uvidx else k)
                else:  # ByVertex/ByVertice
                    fuv.append(uvidx[idx] if uvidx else idx)
            if last:
                # fan triangulate
                for j in range(1, len(face) - 1):
                    tri = (face[0], face[j], face[j+1])
                    if fuv:
                        tuv = (fuv[0], fuv[j], fuv[j+1])
                        out_f.append(tuple(zip(tri, tuv)))
                    else:
                        out_f.append(tuple((v, None) for v in tri))
                face, fuv = [], []
    with open(dst, 'w') as f:
        f.write('# fbx2obj from %s\n' % src)
        for v in out_v:
            f.write('v %.6f %.6f %.6f\n' % v)
        for t in out_vt:
            f.write('vt %.6f %.6f\n' % t)
        for tri in out_f:
            if tri[0][1] is not None:
                f.write('f %d/%d %d/%d %d/%d\n' % (tri[0][0]+1, tri[0][1]+1, tri[1][0]+1, tri[1][1]+1, tri[2][0]+1, tri[2][1]+1))
            else:
                f.write('f %d %d %d\n' % (tri[0][0]+1, tri[1][0]+1, tri[2][0]+1))
    print('%s -> %s: %d verts, %d tris, %d uvs' % (src, dst, len(out_v), len(out_f), len(out_vt)))

if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.0)
