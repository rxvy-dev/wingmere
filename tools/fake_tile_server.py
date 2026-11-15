#!/usr/bin/env python3
"""A local tile server for testing WINGMERE's streaming without the internet.

It invents a landscape (hills, a coastline, a grey 'city' patch) and serves it in the same
formats as the real servers: Terrarium-encoded PNG elevation and JPEG imagery.

    python3 tools/fake_tile_server.py 8765
    # then in ~/.config/wingmere/wingmere.cfg:
    imagery_url = http://127.0.0.1:8765/img/{z}/{y}/{x}.jpg
    elevation_url = http://127.0.0.1:8765/elev/{z}/{x}/{y}.png
    vector_tilejson = http://127.0.0.1:8765/planet
"""
import io
import math
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import json
import random

import numpy as np
from PIL import Image

try:
    import mapbox_vector_tile
    from shapely.geometry import LineString, Polygon, box, mapping
    from shapely.ops import transform
except ImportError:                     # vector tiles are optional: pip install mapbox-vector-tile
    mapbox_vector_tile = None


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
    k = 8.0
    e = np.zeros_like(lat)
    amp, f = 1.0, 1.0
    for o in range(5):
        e += amp * vnoise(lon * k * f, lat * k * f, o)
        amp *= 0.5
        f *= 2.1
    coast = (lon + 122.32) * 30          # sea to the east of this meridian near San Francisco
    h = (e - 0.75) * 220 - coast * 10
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


def city_features():
    """an invented town west of the airport, a terminal, an apron and a taxiway at KSFO"""
    rnd = random.Random(4)
    b = []
    for i in range(40):
        for j in range(30):
            lon, lat = -122.43 + i * 0.0007, 37.63 + j * 0.0006
            if rnd.random() < 0.7:
                w, h = 0.00018 + rnd.random() * 0.0002, 0.00015 + rnd.random() * 0.00018
                b.append((Polygon([(lon, lat), (lon + w, lat), (lon + w, lat + h), (lon, lat + h)]), 8 + rnd.random() ** 3 * 140))
    b.append((Polygon([(-122.392, 37.6152), (-122.3885, 37.6162), (-122.3880, 37.6152), (-122.3915, 37.6143)]), 18))
    b.append((Polygon([(-122.3872, 37.6170), (-122.3870, 37.6170), (-122.3870, 37.6172), (-122.3872, 37.6172)]), 64))
    roads = [LineString([(-122.43 + i * 0.0035, 37.628), (-122.43 + i * 0.0035, 37.65)]) for i in range(9)]
    roads += [LineString([(-122.432, 37.628 + j * 0.003), (-122.40, 37.628 + j * 0.003)]) for j in range(8)]
    apron = Polygon([(-122.3935, 37.6160), (-122.3870, 37.6178), (-122.3860, 37.6162), (-122.3925, 37.6145)])
    taxi = [LineString([(-122.3575, 37.6155), (-122.3935, 37.6315)]), LineString([(-122.380, 37.6195), (-122.3885, 37.6168)])]
    water = Polygon([(-122.31, 37.4), (-121.9, 37.4), (-121.9, 37.9), (-122.31, 37.9)])
    return b, roads, apron, taxi, water


CITY = None


def vector_tile(z, x, y):
    global CITY
    if CITY is None:
        CITY = city_features()
    blds, roads, apron, taxi, water = CITY
    n = 1 << z

    def to_tile(lon, lat, *_):
        X = (np.asarray(lon) + 180) / 360 * n - x
        Y = (1 - np.log(np.tan(np.radians(lat)) + 1 / np.cos(np.radians(lat))) / math.pi) / 2 * n - y
        return X * 4096, Y * 4096
    clip = box(-64, -64, 4160, 4160)
    def feats(geoms, props):
        out = []
        for g, p in zip(geoms, props):
            t = transform(to_tile, g).intersection(clip)
            if not t.is_empty:
                out.append({'geometry': t.wkt, 'properties': p})
        return out
    layers = [{'name': 'water', 'features': feats([water], [{'class': 'ocean'}])}]
    if z >= 10:
        layers.append({'name': 'transportation', 'features': feats(roads, [{'class': 'primary' if i % 3 == 0 else 'minor'} for i in range(len(roads))])})
        layers.append({'name': 'aeroway', 'features': feats([apron] + taxi, [{'class': 'apron'}] + [{'class': 'taxiway'}] * len(taxi))})
    if z >= 13:
        layers.append({'name': 'building', 'features': feats([g for g, _ in blds], [{'render_height': h, 'render_min_height': 0} for _, h in blds])})
    return mapbox_vector_tile.encode(layers, default_options={'y_coord_down': True, 'quantize_bounds': None})


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            parts = self.path.strip('/').split('/')
            kind = parts[0]
            if kind == 'planet' and mapbox_vector_tile:
                body = json.dumps({'tiles': ['http://127.0.0.1:%d/vec/{z}/{x}/{y}.pbf' % self.server.server_port], 'maxzoom': 14}).encode()
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)
                return
            if kind == 'vec' and mapbox_vector_tile:
                z, x, y = [int(p.split('.')[0]) for p in parts[1:4]]
                data = vector_tile(z, x, y)
                self.send_response(200); self.send_header('Content-Type', 'application/x-protobuf'); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
                return
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
