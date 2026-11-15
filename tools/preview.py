"""WINGMERE - quick textured point-splat previews of raw model dumps (development tool).
usage: preview.py model.raw out.png [part=texture.png ...]"""
import sys
import numpy as np
from PIL import Image
from rawlib import load_raw

def sample(texpath, uv):
    im = np.asarray(Image.open(texpath).convert('RGB')).astype(np.float32) / 255
    h, w = im.shape[:2]
    x = np.clip((uv[:, 0] % 1) * w, 0, w - 1).astype(int)
    y = np.clip((1 - uv[:, 1] % 1) * h, 0, h - 1).astype(int)
    return im[y, x]

def render(pts, col, axes, flip, size=420):
    a, b, d = axes                       # screen x, screen y, depth
    p = pts[:, [a, b]].copy()
    if flip[0]: p[:, 0] *= -1
    if flip[1]: p[:, 1] *= -1
    depth = pts[:, d] * (-1 if flip[2] else 1)
    mn, mx = p.min(0), p.max(0)
    s = (size - 20) / max(mx - mn)
    xy = ((p - mn) * s + 10).astype(int)
    img = np.full((size, size, 3), 0.12, np.float32)
    order = np.argsort(depth)              # far first, near overwrites
    xy, c = xy[order], col[order]
    img[size - 1 - xy[:, 1].clip(0, size - 1), xy[:, 0].clip(0, size - 1)] = c
    return img

def main():
    parts = load_raw(sys.argv[1])
    texmap = dict(a.split('=', 1) for a in sys.argv[3:])
    pts, cols = [], []
    for p in parts:
        v = p['v']
        # splat vertices plus triangle centroids for density
        cen = v.reshape(-1, 3, 8).mean(1)
        allv = np.concatenate([v, cen])
        key = p['name'] if p['name'] in texmap else p['mat']
        if key in texmap: c = sample(texmap[key], allv[:, 6:8])
        else:
            shade = 0.35 + 0.5 * np.clip(allv[:, 4], 0, 1)
            c = np.stack([shade] * 3, 1) * np.array([0.8, 0.8, 0.9])
        pts.append(allv[:, :3]); cols.append(c)
    pts, cols = np.concatenate(pts), np.concatenate(cols)
    side = render(pts, cols, (2, 1, 0), (False, False, False))   # looking along -x: z right, y up
    top = render(pts, cols, (0, 2, 1), (False, False, False))    # looking down: x right, z up
    front = render(pts, cols, (0, 1, 2), (False, False, True))
    out = np.concatenate([side, top, front], 1)
    Image.fromarray((out * 255).astype(np.uint8)).save(sys.argv[2])

main()
