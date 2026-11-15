"""WINGMERE - point-splat preview of a built aircraft (model.wmdl + textures + gear points) (development tool).
usage: preview_wmdl.py data/aircraft/<id> out.png"""
import sys, os, struct
import numpy as np
from PIL import Image

def load_wmdl(d):
    b = open(os.path.join(d, 'model.wmdl'), 'rb').read()
    assert b[:4] == b'WMDL'
    off = 20
    nmat, = struct.unpack_from('<I', b, off); off += 4
    mats = []
    for _ in range(nmat):
        name = b[off:off + 48].split(b'\0')[0].decode(); off += 48
        rgba = struct.unpack_from('<4f', b, off); off += 16
        metal, rough, flags = struct.unpack_from('<2fI', b, off); off += 12
        tex = [b[off + 48 * i:off + 48 * (i + 1)].split(b'\0')[0].decode() for i in range(3)]; off += 144
        mats.append(dict(name=name, rgba=rgba, tex=tex))
    npart, = struct.unpack_from('<I', b, off); off += 4
    parts = []
    for _ in range(npart):
        name = b[off:off + 48].split(b'\0')[0].decode(); off += 48
        kind, = struct.unpack_from('<I', b, off); off += 4
        piv = struct.unpack_from('<3f', b, off); off += 24
        mat, nv, ni = struct.unpack_from('<3I', b, off); off += 12
        v = np.frombuffer(b, '<f4', nv * 8, off).reshape(nv, 8); off += nv * 32
        idx = np.frombuffer(b, '<u4', ni, off); off += ni * 4
        parts.append(dict(name=name, kind=kind, pivot=piv, mat=mat, v=v, idx=idx))
    return mats, parts

def gear_points(d):
    pts = []
    for line in open(os.path.join(d, 'aircraft.cfg')):
        if line[:4] == 'gear' and line[4].isdigit():
            pts.append([float(x) for x in line.split('=')[1].split()])
    return np.array(pts)

def main():
    d = sys.argv[1]
    mats, parts = load_wmdl(d)
    P, C = [], []
    texcache = {}
    for p in parts:
        tri = p['v'][p['idx']]
        cen = tri.reshape(-1, 3, 8).mean(1)
        allv = np.concatenate([p['v'], cen])
        m = mats[p['mat']]
        if m['tex'][0]:
            if m['tex'][0] not in texcache: texcache[m['tex'][0]] = np.asarray(Image.open(os.path.join(d, m['tex'][0])).convert('RGB')).astype(np.float32) / 255
            im = texcache[m['tex'][0]]; h, w = im.shape[:2]
            c = im[np.clip((1 - allv[:, 7] % 1) * h, 0, h - 1).astype(int), np.clip((allv[:, 6] % 1) * w, 0, w - 1).astype(int)]
        else: c = np.tile(np.array(m['rgba'][:3]), (len(allv), 1))
        c = c * (0.55 + 0.45 * np.clip(allv[:, 4:5] * 0.7 + 0.3, 0, 1))
        P.append(allv[:, :3]); C.append(c)
    P, C = np.concatenate(P), np.concatenate(C)
    g = gear_points(d)
    views = []
    for axes, flipd in [((2, 1, 0), True), ((0, 2, 1), False), ((0, 1, 2), True)]:
        a, bb, dd = axes
        size = 400
        pts = np.concatenate([P, g])
        mn, mx = pts[:, [a, bb]].min(0), pts[:, [a, bb]].max(0)
        s = (size - 30) / max(mx - mn)
        def proj(q): return ((q[:, [a, bb]] - mn) * s + 15).astype(int)
        img = np.full((size, size, 3), 0.1, np.float32)
        order = np.argsort(P[:, dd] * (-1 if flipd else 1))
        xy = proj(P)[order]
        img[size - 1 - xy[:, 1].clip(0, size - 1), xy[:, 0].clip(0, size - 1)] = C[order]
        for q in proj(g):
            img[max(0, size - 1 - q[1] - 4):size - q[1] + 4, max(0, q[0] - 4):q[0] + 5] = (1, 0, 0)
        # origin (centre of gravity) in green
        o = proj(np.zeros((1, 3)))[0]
        img[max(0, size - 1 - o[1] - 3):size - o[1] + 3, max(0, o[0] - 3):o[0] + 4] = (0, 1, 0)
        views.append(img)
    Image.fromarray((np.concatenate(views, 1) * 255).astype(np.uint8)).save(sys.argv[2])

main()
