"""WINGMERE - helpers for the raw model dumps written by tools/fbx2raw (development tool)."""
import struct
import numpy as np


def load_raw(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'RAW1', path
    n, = struct.unpack_from('<I', data, 4)
    off = 8
    parts = []
    for _ in range(n):
        l, = struct.unpack_from('<I', data, off); off += 4
        name = data[off:off + l].decode('utf-8', 'replace'); off += l
        l, = struct.unpack_from('<I', data, off); off += 4
        mat = data[off:off + l].decode('utf-8', 'replace'); off += l
        piv = np.frombuffer(data, '<f4', 3, off).copy(); off += 12
        nv, = struct.unpack_from('<I', data, off); off += 4
        v = np.frombuffer(data, '<f4', nv * 8, off).reshape(nv, 8).copy(); off += nv * 32
        parts.append({'name': name, 'mat': mat, 'pivot': piv, 'v': v})
    return parts


def bounds(parts):
    allp = np.concatenate([p['v'][:, :3] for p in parts])
    return allp.min(0), allp.max(0)
