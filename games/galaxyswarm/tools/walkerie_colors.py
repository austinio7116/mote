#!/usr/bin/env python3
"""Reconstruct per-part colours for the decimated Walkerie boss.

The original Walkerie.obj has 602 usemtl groups whose material names match the
texture JPGs in Models/Walkerie/ (the .mtl itself was never committed). We
sample each material's average colour from its JPG, transfer colours to the
decimated mesh by nearest-original-face lookup, cluster to <=8 colours, and
rewrite assets/walkerie.obj + walkerie.mtl as a multi-material OBJ — which
obj2mesh bakes to a per-part-coloured MoteModel.
"""
import os, sys, glob
import numpy as np
from PIL import Image

ORIG = '/tmp/GalaxySwarm/Assets/Models/Walkerie.obj'
TEXDIR = '/tmp/GalaxySwarm/Assets/Models/Walkerie'
GAME = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'walkerie.obj')

def load_obj(path, with_mtl=False):
    vs, faces, mats = [], [], []
    cur = 'default'
    for line in open(path, errors='replace'):
        p = line.split()
        if not p: continue
        if p[0] == 'v': vs.append([float(x) for x in p[1:4]])
        elif p[0] == 'usemtl': cur = ' '.join(p[1:])
        elif p[0] == 'f':
            idx = []
            for w in p[1:]:
                i = int(w.split('/')[0])
                idx.append(i - 1 if i > 0 else len(vs) + i)
            for j in range(1, len(idx) - 1):
                faces.append((idx[0], idx[j], idx[j+1]))
                mats.append(cur)
    return np.array(vs, dtype=np.float32), faces, mats

def normalize(vs):
    c = (vs.min(0) + vs.max(0)) / 2
    h = (vs.max(0) - vs.min(0)).max() / 2 or 1
    return (vs - c) / h

# material name -> colour. The OBJ's material names are semantic (blue_light,
# glass, noir, metal ...) and the .mtl never shipped, so map them by meaning:
# grey-green military hull with coloured light clusters.
def mat_color(name):
    n = name.lower()
    if 'blue_light' in n:   return (70, 130, 255)
    if 'red_light' in n:    return (255, 60, 50)
    if 'yellow_light' in n: return (255, 210, 60)
    if 'white_light' in n or 'head_lights' in n: return (235, 240, 255)
    if 'glow' in n:         return (255, 150, 50)
    if '_light' in n:       return (170, 200, 230)
    if 'glass' in n:        return (40, 70, 90)
    if 'noir' in n:         return (28, 30, 34)
    if 'gun' in n:          return (55, 58, 62)
    if 'metal' in n or 'hyd' in n or 'holder' in n: return (110, 115, 122)
    if 'panel' in n or 'detail' in n: return (88, 96, 88)
    if 'head' in n:         return (105, 115, 100)
    if 'bump' in n:         return (78, 84, 78)
    # hull default: drab military green-grey
    return (96, 104, 92)

print('loading original (may take a moment)...')
ovs, ofaces, omats = load_obj(ORIG)
ovs = normalize(ovs)
ocent = np.array([(ovs[a] + ovs[b] + ovs[c]) / 3 for a, b, c in ofaces], dtype=np.float32)
mat_names = sorted(set(omats))
print(len(ofaces), 'orig faces,', len(mat_names), 'materials')
colors = {m: mat_color(m) for m in mat_names}
ocol = np.array([colors[m] for m in omats], dtype=np.float32)

dvs, dfaces, _ = load_obj(GAME)
dvs_n = normalize(dvs)
dcent = np.array([(dvs_n[a] + dvs_n[b] + dvs_n[c]) / 3 for a, b, c in dfaces], dtype=np.float32)

# nearest original face per decimated face (chunked brute force)
fcol = np.zeros((len(dfaces), 3), dtype=np.float32)
for i in range(0, len(dcent), 64):
    d = ((dcent[i:i+64, None, :] - ocent[None, :, :]) ** 2).sum(-1)
    fcol[i:i+64] = ocol[d.argmin(1)]

# brighten a touch (the source jpgs are dark) and cluster to <=8 colours
fcol = np.clip(fcol * 1.15, 0, 255)
from PIL import Image as I
pal_img = I.fromarray(fcol.reshape(1, -1, 3).astype(np.uint8)).quantize(colors=8, method=I.MEDIANCUT)
raw_pal = pal_img.getpalette()
labels = np.asarray(pal_img).reshape(-1)
npal = int(labels.max()) + 1
pal = np.array(raw_pal[:npal * 3], dtype=np.uint8).reshape(npal, 3)

mtl_path = GAME[:-4] + '.mtl'
with open(mtl_path, 'w') as m:
    for k in range(npal):
        m.write('newmtl part%d\nKd %.4f %.4f %.4f\n' % (k, pal[k][0]/255, pal[k][1]/255, pal[k][2]/255))
with open(GAME, 'w') as f:
    f.write('# decimated walkerie with reconstructed part colours (walkerie_colors.py)\n')
    f.write('mtllib walkerie.mtl\n')
    for v in dvs:
        f.write('v %.6f %.6f %.6f\n' % tuple(v))
    order = np.argsort(labels, kind='stable')
    cur = -1
    for fi in order:
        if labels[fi] != cur:
            cur = labels[fi]
            f.write('usemtl part%d\n' % cur)
        a, b, c = dfaces[fi]
        f.write('f %d %d %d\n' % (a + 1, b + 1, c + 1))
print('wrote', GAME, 'and', mtl_path, '- 8 colour parts:', [tuple(int(x) for x in p) for p in pal])
