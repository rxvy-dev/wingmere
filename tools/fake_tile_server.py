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
import io, time
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
    # a second terminal with its apron just north of the 10L threshold
    b.append((Polygon([(-122.3990, 37.6345), (-122.3920, 37.6345), (-122.3920, 37.6352), (-122.3990, 37.6352)]), 16))
    apron2 = Polygon([(-122.3995, 37.6324), (-122.3915, 37.6324), (-122.3915, 37.6345), (-122.3995, 37.6345)])
    taxi = [LineString([(-122.3575, 37.6155), (-122.3935, 37.6315)]), LineString([(-122.380, 37.6195), (-122.3885, 37.6168)])]
    water = Polygon([(-122.31, 37.4), (-121.9, 37.4), (-121.9, 37.9), (-122.31, 37.9)])
    return b, roads, [apron, apron2], taxi, water


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
        layers.append({'name': 'aeroway', 'features': feats(apron + taxi, [{'class': 'apron'}] * len(apron) + [{'class': 'taxiway'}] * len(taxi))})
    if z >= 13:
        layers.append({'name': 'building', 'features': feats([g for g, _ in blds], [{'render_height': h, 'render_min_height': 0} for _, h in blds])})
    return mapbox_vector_tile.encode(layers, default_options={'y_coord_down': True, 'quantize_bounds': None})



T0 = time.time()
PORT = 8765
# (hex, callsign, reg, type, east m, north m, alt ft or 'ground', kt, track, ft/min) relative to where the game asks
def _at(fwd, right, hdg=118.0):
    """east, north metres from the anchor: fwd along the runway heading, right of it"""
    f = (math.sin(math.radians(hdg)), math.cos(math.radians(hdg)))
    r = (math.sin(math.radians(hdg + 90)), math.cos(math.radians(hdg + 90)))
    return fwd * f[0] + right * r[0], fwd * f[1] + right * r[1]
# (hex, callsign, reg, type, east m, north m, alt ft or 'ground', kt, track, ft/min) relative to where the game asks
FAKE_TRAFFIC = [
    ('a1b2c3', 'UAL1234 ', 'N12345', 'B738', *_at(140, 95), 'ground', 0, 28, 0),
    ('a1b2c4', 'BAW285  ', 'G-XLEA', 'A388', *_at(260, 150), 'ground', 0, 28, 0),
    ('a1b2c5', 'ASA331  ', 'N623VA', 'A320', *_at(380, 95), 'ground', 0, 28, 0),
    ('a1b2cd', 'UAE225  ', 'A6-EGO', 'B77W', *_at(500, 150), 'ground', 0, 28, 0),
    ('a1b2ce', 'DAL412  ', 'N841MH', 'A21N', *_at(620, 95), 'ground', 0, 28, 0),
    ('a1b2c9', 'QXE2014 ', 'N401QX', 'DH8D', *_at(720, 80), 'ground', 0, 28, 0),
    ('a1b2c6', 'SKW5521 ', 'N601SK', 'CRJ9', *_at(800, 80), 'ground', 0, 28, 0),
    ('a1b2cf', 'KLM605  ', 'PH-BHA', 'B789', *_at(940, 150), 'ground', 0, 28, 0),
    ('a1b2c7', 'N172SP  ', 'N172SP', 'C172', *_at(90, -70), 'ground', 0, 298, 0),
    ('a1b2d0', 'UAL837  ', 'N26966', 'B789', -2000, 1500, 14000, 300, 285, 1500),
    ('a1b2c8', 'VIPER11 ', '', 'F16', -4000, -2500, 9000, 420, 45, 0),
    ('a1b2ca', 'LIFE1   ', 'N911LF', 'EC35', 500, 3500, 900, 110, 150, 0),
    ('a1b2cb', 'DAL88   ', 'N801DN', 'B77W', -6000, 6000, 11000, 300, 135, 1800),
    ('a1b2cc', '', 'N5512', '', 9000, -4000, 3500, 120, 0, 0),
]
ANCHOR = []
def fake_traffic(lat, lon):
    if not ANCHOR: ANCHOR.append((lat, lon))   # the traffic stays where it first appeared
    lat, lon = ANCHOR[0]
    t = time.time() - T0
    ac = []
    for hx, cs, reg, ty, e, n, alt, kt, trk, vr in FAKE_TRAFFIC:
        v = kt * 0.514444
        e += math.sin(math.radians(trk)) * v * t
        n += math.cos(math.radians(trk)) * v * t
        a = {'hex': hx, 'type': 'adsb_icao', 'flight': cs, 'r': reg, 't': ty,
             'lat': lat + n / 111320.0, 'lon': lon + e / (111320.0 * math.cos(math.radians(lat))),
             'gs': kt, 'track': trk, 'baro_rate': vr, 'category': 'A3', 'seen': 0.4}
        a['alt_baro'] = alt if alt == 'ground' else round(alt + vr * t / 60.0)
        if not cs: del a['flight']
        if not ty: del a['t']
        ac.append(a)
    return {'ac': ac, 'msg': 'No error', 'now': int(time.time() * 1000), 'total': len(ac)}


# what adsbdb would say about each fake aircraft
FAKE_ROUTES = {
    'a1b2d0': ('United Airlines', ('KSFO', 'SFO', 'San Francisco International Airport', 'San Francisco', 37.619, -122.375), ('RJAA', 'NRT', 'Narita International Airport', 'Tokyo', 35.765, 140.386), 'Boeing', '787-9', 'B789', 'N26966', 'United Airlines'),
    'a1b2c3': ('United Airlines', ('KSFO', 'SFO', 'San Francisco International Airport', 'San Francisco', 37.619, -122.375), ('KJFK', 'JFK', 'John F Kennedy International Airport', 'New York', 40.640, -73.779), 'Boeing', '737-824', 'B738', 'N12345', 'United Airlines'),
    'a1b2c4': ('British Airways', ('EGLL', 'LHR', 'London Heathrow Airport', 'London', 51.470, -0.454), ('KSFO', 'SFO', 'San Francisco International Airport', 'San Francisco', 37.619, -122.375), 'Airbus', 'A380-841', 'A388', 'G-XLEA', 'British Airways'),
    'a1b2c5': ('Alaska Airlines', ('KSEA', 'SEA', 'Seattle Tacoma International Airport', 'Seattle', 47.449, -122.309), ('KSFO', 'SFO', 'San Francisco International Airport', 'San Francisco', 37.619, -122.375), 'Airbus', 'A320-214', 'A320', 'N623VA', 'Alaska Airlines'),
}
def fake_adsbdb(hexid):
    r = FAKE_ROUTES.get(hexid)
    if not r: return None
    airline, a, b, man, typ, icao, reg, owner = r
    def ap(x): return {'icao_code': x[0], 'iata_code': x[1], 'name': x[2], 'municipality': x[3], 'country_name': 'United States', 'latitude': x[4], 'longitude': x[5], 'elevation': 13}
    return {'response': {'aircraft': {'type': typ, 'icao_type': icao, 'manufacturer': man, 'mode_s': hexid.upper(), 'registration': reg,
            'registered_owner': owner, 'registered_owner_country_name': 'United States', 'url_photo': 'http://127.0.0.1:%d/photo/%s.jpg' % (PORT, hexid), 'url_photo_thumbnail': None},
            'flightroute': {'callsign': 'X', 'airline': {'name': airline, 'icao': 'XXX', 'country': 'United States'}, 'origin': ap(a), 'destination': ap(b)}}}
def fake_photo(hexid):
    # a simple invented "photo": sky gradient, ground, and a white airliner silhouette
    from PIL import Image, ImageDraw
    im = Image.new('RGB', (640, 400)); d = ImageDraw.Draw(im)
    for y in range(400): d.line([(0, y), (640, y)], fill=(90 + y // 6, 140 + y // 8, 200 + y // 10) if y < 300 else (70, 75, 70))
    d.polygon([(120, 210), (520, 195), (560, 205), (520, 222), (120, 228)], fill=(240, 240, 245))
    d.polygon([(300, 210), (230, 290), (265, 290), (370, 212)], fill=(225, 225, 230))
    d.polygon([(130, 212), (100, 150), (125, 150), (175, 210)], fill=(30, 60, 140))
    buf = io.BytesIO(); im.save(buf, 'JPEG', quality=85); return buf.getvalue()


def fake_world(n=6000):
    """an OpenSky-style /api/states/all: aircraft all over the planet, moving with time"""
    import random
    rnd = random.Random(42)
    t = time.time() - T0
    codes = ['UAL', 'DAL', 'AAL', 'BAW', 'DLH', 'AFR', 'KLM', 'UAE', 'QTR', 'SIA', 'CPA', 'ANA', 'JAL', 'QFA', 'RYR', 'EZY', 'SWA', 'ACA', 'THY', 'CCA']
    countries = ['United States', 'United Kingdom', 'Germany', 'France', 'Netherlands', 'United Arab Emirates', 'Japan', 'Australia', 'Canada', 'China']
    states = []
    for i in range(n):
        lat0 = rnd.uniform(-55, 70); lon0 = rnd.uniform(-180, 180)
        trk = rnd.uniform(0, 360); v = rnd.uniform(180, 260)   # m/s
        ground = rnd.random() < 0.08
        if ground: v = 0
        d = v * t
        lat = lat0 + d * math.cos(math.radians(trk)) / 111320.0
        lon = lon0 + d * math.sin(math.radians(trk)) / (111320.0 * max(math.cos(math.radians(lat0)), 0.2))
        lon = (lon + 180) % 360 - 180; lat = max(min(lat, 85), -85)
        cs = '%s%d  ' % (rnd.choice(codes), rnd.randint(1, 9999))
        alt = None if ground else rnd.uniform(9000, 12500)
        states.append(['%06x' % (0xb00000 + i), cs, rnd.choice(countries), int(time.time()), int(time.time()), lon, lat, alt, ground, v, trk, 0.0, None, alt, '%04d' % rnd.randint(1000, 7777), False, 0, rnd.choice([4, 4, 4, 6, 3])])
    return {'time': int(time.time()), 'states': states}

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            parts = self.path.strip('/').split('/')
            kind = parts[0]
            if kind == 'v2' and len(parts) > 2 and parts[1] == 'hex':
                ac = [a for a in fake_traffic(37.6, -122.37)['ac'] if a['hex'] == parts[2].lower()]
                body = json.dumps({'ac': ac, 'msg': 'No error', 'total': len(ac)}).encode()
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)
                return
            if kind == 'v0':
                r = fake_adsbdb(parts[2].split('?')[0].lower()) if len(parts) > 2 else None
                body = json.dumps(r if r else {'response': 'unknown aircraft'}).encode()
                self.send_response(200 if r else 404); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)
                return
            if kind == 'api' and len(parts) > 1 and parts[1] == 'states':
                body = json.dumps(fake_world()).encode()
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)
                return
            if kind == 'pub':   # a planespotters.net style /pub/photos/hex/{hex}
                hx = parts[-1].lower()
                ph = [{'id': '1', 'thumbnail_large': {'src': 'http://127.0.0.1:%d/photo/%s.jpg' % (PORT, hx)}, 'link': 'http://example.invalid/', 'photographer': 'Test Spotter'}] if hx in FAKE_ROUTES else []
                body = json.dumps({'photos': ph}).encode()
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)
                return
            if kind == 'photo':
                body = fake_photo(parts[1].split('.')[0])
                self.send_response(200); self.send_header('Content-Type', 'image/jpeg'); self.end_headers(); self.wfile.write(body)
                return
            if kind == 'v2':   # an adsb.lol style /v2/lat/{lat}/lon/{lon}/dist/{nm}
                q = dict(zip(parts[1::2], parts[2::2]))
                body = json.dumps(fake_traffic(float(q['lat']), float(q['lon']))).encode()
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)
                return
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
    PORT = port
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
