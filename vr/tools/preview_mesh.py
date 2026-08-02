#!/usr/bin/env python3
"""Software-render chassis.mvm to a PNG, to check the mesh without a headset.

Not part of the build — a sanity check for gen_vr_chassis.py. Painter's
algorithm, per-pixel UV, one directional light, so a wrong winding, an inverted
normal or a mismapped texture is obvious at a glance.

  python3 vr/tools/preview_mesh.py [yaw_deg] [pitch_deg] [out.png]
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MESH = os.path.join(ROOT, "vr", "assets", "chassis.mvm")
TEX  = os.path.join(ROOT, "vr", "assets", "chassis.jpg")

RES = 900


def load():
    with open(MESH, "rb") as f:
        assert f.read(4) == b"MVM1"
        nv, ni = struct.unpack("<II", f.read(8))
        v = np.frombuffer(f.read(nv * 32), dtype="<f4").reshape(nv, 8)
        idx = np.frombuffer(f.read(ni * 2), dtype="<u2").reshape(-1, 3)
    return v.astype(np.float64), idx.astype(np.int32)


def main():
    yaw = math.radians(float(sys.argv[1]) if len(sys.argv) > 1 else 28.0)
    pitch = math.radians(float(sys.argv[2]) if len(sys.argv) > 2 else 22.0)
    out = sys.argv[3] if len(sys.argv) > 3 else "/tmp/chassis_mesh.png"

    v, idx = load()
    tex = np.asarray(Image.open(TEX).convert("RGB"), dtype=np.float64) / 255.0
    th, tw = tex.shape[:2]

    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    R = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ np.array(
        [[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    R = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]]) @ np.array(
        [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])

    pos = v[:, 0:3] @ R.T
    nrm = v[:, 3:6] @ R.T
    uv = v[:, 6:8]

    dist = 0.16
    f = 2.2
    z = pos[:, 2] + dist
    sx = (pos[:, 0] * f / z) * RES * 0.5 + RES * 0.5
    sy_ = RES * 0.5 - (pos[:, 1] * f / z) * RES * 0.5

    img = np.zeros((RES, RES, 3))
    img[:, :] = (0.10, 0.11, 0.13)
    zbuf = np.full((RES, RES), 1e9)

    L = np.array([0.35, 0.55, 0.76])
    L /= np.linalg.norm(L)

    for tri in idx:
        p = np.stack([sx[tri], sy_[tri]], axis=1)
        # backface cull (screen space, y down => flip the sign)
        a = (p[1, 0] - p[0, 0]) * (p[2, 1] - p[0, 1]) - (p[1, 1] - p[0, 1]) * (p[2, 0] - p[0, 0])
        if a >= 0:
            continue
        x0 = max(int(np.floor(p[:, 0].min())), 0)
        x1 = min(int(np.ceil(p[:, 0].max())) + 1, RES)
        y0 = max(int(np.floor(p[:, 1].min())), 0)
        y1 = min(int(np.ceil(p[:, 1].max())) + 1, RES)
        if x1 <= x0 or y1 <= y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        d = a
        w0 = ((p[1, 0] - xs) * (p[2, 1] - ys) - (p[1, 1] - ys) * (p[2, 0] - xs)) / d
        w1 = ((p[2, 0] - xs) * (p[0, 1] - ys) - (p[2, 1] - ys) * (p[0, 0] - xs)) / d
        w2 = 1.0 - w0 - w1
        m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not m.any():
            continue
        zz = w0 * z[tri[0]] + w1 * z[tri[1]] + w2 * z[tri[2]]
        sub = zbuf[y0:y1, x0:x1]
        m &= zz < sub
        if not m.any():
            continue
        uu = w0 * uv[tri[0], 0] + w1 * uv[tri[1], 0] + w2 * uv[tri[2], 0]
        vv = w0 * uv[tri[0], 1] + w1 * uv[tri[1], 1] + w2 * uv[tri[2], 1]
        tx = np.clip((uu * tw).astype(int), 0, tw - 1)
        ty = np.clip((vv * th).astype(int), 0, th - 1)
        col = tex[ty, tx]
        nn = (w0[..., None] * nrm[tri[0]] + w1[..., None] * nrm[tri[1]]
              + w2[..., None] * nrm[tri[2]])
        nn /= np.linalg.norm(nn, axis=2, keepdims=True) + 1e-9
        lam = np.clip(nn @ L, 0, 1)
        shade = 0.35 + 0.65 * lam
        col = col * shade[..., None]
        dst = img[y0:y1, x0:x1]
        dst[m] = col[m]
        sub[m] = zz[m]

    Image.fromarray((np.clip(img, 0, 1) * 255).astype("uint8")).save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
