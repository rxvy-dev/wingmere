#!/usr/bin/env python3
"""A local tile server for testing WINGMERE's streaming without the internet.

It invents a landscape (hills, a coastline, a grey 'city' patch) and serves it in the same
formats as the real servers: Terrarium-encoded PNG elevation and JPEG imagery.

    python3 tools/fake_tile_server.py 8765
    # then in ~/.config/wingmere/wingmere.cfg:
    imagery_url = http://127.0.0.1:8765/img/{z}/{y}/{x}.jpg
    elevation_url = http://127.0.0.1:8765/elev/{z}/{x}/{y}.png
"""
import io
import math
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from PIL import Image


def lonlat(z, x, y, n=256):
    i = (np.arange(n) + 0.5) / n
    X, Y = np.meshgrid(x + i, y + i)
    lon = X / (1 << z) * 360 - 180
    lat = np.degrees(np.arctan(np.sinh(math.pi * (1 - 2 * Y / (1 << z)))))
    return lat, lon


def vnoise(x, y, seed):
    xi, yi = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    tx, ty = x - xi, y - yi
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)

    def h(a, b):
        v = (a * 374761393 + b * 668265263 + seed * 1442695041) & 0xFFFFFFFF
        v = ((v ^ (v >> 13)) * 1274126177) & 0xFFFFFFFF
        return (v & 0xFFFF) / 65535.0
    return (h(xi, yi) * (1 - tx) + h(xi + 1, yi) * tx) * (1 - ty) + (h(xi, yi + 1) * (1 - tx) + h(xi + 1, yi + 1) * tx) * ty


def height(lat, lon):
    k = 25.0
    e = np.zeros_like(lat)
    amp, f = 1.0, 1.0
    for o in range(7):
        e += amp * vnoise(lon * k * f, lat * k * f, o)
        amp *= 0.5
        f *= 2.1
    coast = (lon + 122.32) * 30          # sea to the east of this meridian near San Francisco
    h = (e - 0.75) * 1400 - coast * 120
    return np.clip(h, -50, 4000)


def imagery(lat, lon, h):
    rng = vnoise(lon * 900, lat * 900, 99)
    green = np.stack([0.22 + 0.1 * rng, 0.30 + 0.08 * rng, 0.16 + 0.05 * rng], -1)
    rock = np.stack([0.45 + 0.1 * rng, 0.42 + 0.1 * rng, 0.38 + 0.1 * rng], -1)
    t = np.clip(h / 900, 0, 1)[..., None]
    col = green * (1 - t) + rock * t
    city = ((lat - 37.70) ** 2 + (lon + 122.45) ** 2 < 0.004)[..., None] & (h > 0)[..., None]
    grey = 0.45 + 0.12 * vnoise(lon * 4000, lat * 4000, 7)[..., None]
    col = np.where(city, np.repeat(grey, 3, -1), col)
    sea = (h <= 0)[..., None]
    col = np.where(sea, np.array([0.05, 0.12, 0.18]), col)
    return (np.clip(col, 0, 1) * 255).astype(np.uint8)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            parts = self.path.strip('/').split('/')
            kind = parts[0]
            nums = [int(p.split('.')[0]) for p in parts[1:4]]
            if kind == 'img':
                z, y, x = nums
            else:
                z, x, y = nums
            lat, lon = lonlat(z, x, y)
            h = height(lat, lon)
            buf = io.BytesIO()
            if kind == 'elev':
                v = h + 32768
                r = np.floor(v / 256)
                g = np.floor(v - r * 256)
                b = np.floor((v - np.floor(v)) * 256)
                Image.fromarray(np.stack([r, g, b], -1).astype(np.uint8)).save(buf, 'PNG')
                ctype = 'image/png'
            else:
                Image.fromarray(imagery(lat, lon, h)).save(buf, 'JPEG', quality=85)
                ctype = 'image/jpeg'
            data = buf.getvalue()
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):
        pass


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
