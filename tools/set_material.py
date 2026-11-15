#!/usr/bin/env python3
"""Change a material's metalness/roughness inside a model.wmdl without rebuilding it.

    tools/set_material.py data/aircraft/corsair/model.wmdl            # list materials
    tools/set_material.py data/aircraft/corsair/model.wmdl Cylinder.002 metal=0.05 rough=0.45
"""
import struct
import sys

MAT_SIZE = 48 + 16 + 12 + 144


def materials(b):
    n = struct.unpack_from('<I', b, 20)[0]
    off = 24
    for _ in range(n):
        name = b[off:off + 48].split(b'\0')[0].decode()
        metal, rough, flags = struct.unpack_from('<ffI', b, off + 64)
        yield off, name, metal, rough, flags
        off += MAT_SIZE


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    b = bytearray(open(path, 'rb').read())
    if b[:4] != b'WMDL':
        print('not a WMDL file')
        return 1
    if len(sys.argv) == 2:
        for _, name, metal, rough, flags in materials(b):
            print(f'{name:32s} metal={metal:.2f} rough={rough:.2f}{"  glass" if flags & 1 else ""}')
        return 0
    target, changes = sys.argv[2], dict(kv.split('=') for kv in sys.argv[3:])
    hit = False
    for off, name, metal, rough, flags in materials(b):
        if name == target or target == '*':
            if flags & 1 and target == '*':
                continue
            metal = float(changes.get('metal', metal))
            rough = float(changes.get('rough', rough))
            struct.pack_into('<ff', b, off + 64, metal, rough)
            hit = True
    if not hit:
        print('no such material:', target)
        return 1
    open(path, 'wb').write(b)
    return 0


if __name__ == '__main__':
    sys.exit(main())
