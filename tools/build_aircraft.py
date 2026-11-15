"""WINGMERE - turns the raw FBX dumps (tools/fbx2raw) + textures into the game's aircraft folders.

    python3 tools/build_aircraft.py [--report]

For each aircraft: rescale to real-world size, put the nose on +Z and Y up, move the origin to the
centre of gravity, find propellers / fans / landing gear, de-duplicate vertices, downscale textures,
convert glossiness and bump maps, and write data/aircraft/<id>/model.wmdl plus its textures.

.wmdl (little endian):
  "WMDL" u32 version=1, f32 length, span, height
  u32 nmat, per material: char name[48], f32 rgba[4], f32 metal, f32 rough, u32 flags (1 = glass),
                          char color_tex[48], char normal_tex[48], char rough_tex[48]
  u32 npart, per part:    char name[48], u32 kind (0 static, 1 propeller/fan, 2 landing gear),
                          f32 pivot[3], f32 axis[3], u32 material, u32 nvert, u32 nidx,
                          nvert * (f32 pos[3], f32 normal[3], f32 uv[2]), nidx * u32
"""
import os, sys, struct, math
import numpy as np
from PIL import Image, ImageFilter
sys.path.insert(0, os.path.dirname(__file__))
from rawlib import load_raw

ROOT = os.path.join(os.path.dirname(__file__), '..')
SRC = os.path.join(ROOT, 'assets_src')
RAW = os.path.join(ROOT, 'raw')
OUT = os.path.join(ROOT, 'data', 'aircraft')
REPORT = '--report' in sys.argv

def T(*p): return os.path.join(SRC, *p)

AIRCRAFT = {
    'biplane': dict(
        raw='biplane.raw', length=7.6, yaw=0,
        mats={
            'PLANE OBJ_AVION': dict(color=T('yellow-plane-biplane/textures/PLANE_OBJ_AVION_Diffuse.png'), normal=T('yellow-plane-biplane/textures/PLANE_OBJ_AVION_Normal.png'), gloss=T('yellow-plane-biplane/textures/PLANE_OBJ_AVION_Glossiness.png')),
            'PLANE OBJ_Motor': dict(color=T('yellow-plane-biplane/textures/PLANE_OBJ_Motor_Diffuse.png'), normal=T('yellow-plane-biplane/textures/PLANE_OBJ_Motor_Normal.png'), gloss=T('yellow-plane-biplane/textures/PLANE_OBJ_Motor_Glossiness.png'), metal=0.8),
            'PLANE OBJ_Helice': dict(color=T('yellow-plane-biplane/textures/PLANE_OBJ_Helice_Diffuse.png'), gloss=T('yellow-plane-biplane/textures/PLANE_OBJ_Helice_Glossiness.png')),
            'PLANE OBJ_ASIENTOS_Y_RUEDAS': dict(color=T('yellow-plane-biplane/textures/PLANE_OBJ_ASIENTOS_Y_RUEDAS_Diffuse.png'), normal=T('yellow-plane-biplane/textures/PLANE_OBJ_ASIENTOS_Y_RUEDAS_Normal.png'), gloss=T('yellow-plane-biplane/textures/PLANE_OBJ_ASIENTOS_Y_RUEDAS_Glossiness.png')),
            'PLANE OBJ_Cristales': dict(rgba=(0.05, 0.06, 0.07, 0.35), glass=True, rough=0.05),
        },
        props=['Helice'], gear=[], tex_max=2048),
    'corsair': dict(
        raw='corsair.raw', length=10.2, yaw='auto', prop_part='Cube.001',
        mats={
            'Cylinder.001': dict(color=T('corsair-plane/textures/Plane.png'), rough=0.5, metal=0.05),
            'Cylinder.002': dict(color=T('corsair-plane/textures/Metal.png'), rough=0.42, metal=0.05),
            'Cylinder.003': dict(color=T('corsair-plane/textures/Glass.png'), glass=True, rough=0.05),
            'Cylinder.014': dict(color=T('corsair-plane/textures/Black Pre.png'), rough=0.42, metal=0.05),
            'Cube.049': dict(color=T('corsair-plane/textures/Yellow Pre.png'), rough=0.42, metal=0.05),
            'Cube.001': dict(color=T('corsair-plane/textures/Black Pre.png'), rough=0.42, metal=0.05),
        },
        props=['Cube.001', 'Cube.049'], gear=['Cylinder.014'], tex_max=1024),
    'b737': dict(
        raw='b737.raw', length=39.5, yaw=0,
        mats={
            'Body 737': dict(color=T('737-max-lion-air/textures/Body_738_Base_Color_Lion.png'), bump=T('737-max-lion-air/textures/Body_737_Bump.png'), rough=0.35, metal=0.1),
            'Wings 737.': dict(color=T('737-max-lion-air/textures/Wings_737._Base_Color_Lion.png'), bump=T('737-max-lion-air/textures/Wings_737._Bump.png'), rough=0.4, metal=0.1),
            'Egnine 737': dict(color=T('737-max-lion-air/textures/Egnine_737_Base_Color_Wings.png'), rough=0.35, metal=0.2),
            'Jendela': dict(rgba=(0.02, 0.025, 0.03, 1), rough=0.08),
            'White': dict(rgba=(0.62, 0.62, 0.62, 1), rough=0.4, metal=0.2),
            'Engine.001': dict(rgba=(0.33, 0.33, 0.35, 1), rough=0.35, metal=0.7),
            'Engine': dict(rgba=(0.07, 0.07, 0.08, 1), rough=0.5, metal=0.5),
            'Black': dict(rgba=(0.03, 0.03, 0.03, 1), rough=0.5),
            'Roda': dict(rgba=(0.05, 0.05, 0.05, 1), rough=0.8),
            'Landing Gear': dict(rgba=(0.45, 0.45, 0.47, 1), rough=0.4, metal=0.8),
            'Plane Egnine': dict(rgba=(0.45, 0.45, 0.47, 1), rough=0.4, metal=0.8),
        },
        props=['Plane.002'], split_props=True, gear=['Cylinder.029', 'Cylinder.030', 'Cylinder.031', 'Cylinder.032', 'Cube.033', 'Cube.041'], tex_max=2048),
    'transport': dict(
        raw='transport.raw', length=29.8, yaw=0,
        mats={
            'Main Body': dict(color=T('freefire-plane/textures/Main_Body.png'), normal=T('freefire-plane/textures/Main Body_NRM.png'), spec=T('freefire-plane/textures/Main Body_S.png'), metal=0.2),
            'Main Body 2': dict(color=T('freefire-plane/textures/Main_Body2.png'), normal=T('freefire-plane/textures/Main Body 2_NRM.png'), spec=T('freefire-plane/textures/Main Body 2_S.png'), metal=0.2),
        },
        props=['airscrew01a_LOD0', 'airscrew01b_LOD0', 'airscrew01c_LOD0', 'airscrew01d_LOD0'], gear=[], tex_max=1024),
}


# Flight data: rough real-world figures; everything here ends up in data/aircraft/<id>/aircraft.cfg
FLIGHT = {
    'biplane': dict(name='Yellow Biplane', kind='Stearman-style trainer, 1930s', mass=1200, wing_area=27.6, chord=1.5,
                    cl0=0.25, cla=4.6, alpha_stall=15, clmax=1.45, cd0=0.045, k=0.075, thrust=4800, engine='prop', prop_v=70,
                    flaps=0, flap_cl=0, flap_cd=0, retract=0, gear_cd=0, cm_alpha=-0.9, cm_de=-1.3, cm_q=-12,
                    cl_da=0.20, cl_p=-0.5, cl_beta=-0.08, cn_beta=0.12, cn_dr=-0.10, cn_r=-0.15, ixx=1300, iyy=1700, izz=2700,
                    v_rotate=24, v_cruise=45, cam_dist=13, sound='radial'),
    'corsair': dict(name='F4U Corsair', kind='WWII carrier fighter', mass=5400, wing_area=29.2, chord=2.5,
                    cl0=0.2, cla=5.0, alpha_stall=15, clmax=1.5, cd0=0.025, k=0.055, thrust=21000, engine='prop', prop_v=190,
                    flaps=2, flap_cl=0.55, flap_cd=0.04, retract=1, gear_cd=0.02, cm_alpha=-0.8, cm_de=-1.4, cm_q=-15,
                    cl_da=0.16, cl_p=-0.45, cl_beta=-0.06, cn_beta=0.10, cn_dr=-0.08, cn_r=-0.12, ixx=18000, iyy=30000, izz=45000,
                    v_rotate=45, v_cruise=140, cam_dist=16, sound='radial'),
    'b737': dict(name='737 MAX 8', kind='Narrow-body airliner, Lion Air colours', mass=62000, wing_area=127, chord=4.2,
                 cl0=0.25, cla=5.5, alpha_stall=14, clmax=1.55, cd0=0.021, k=0.045, thrust=260000, engine='jet', prop_v=0,
                 flaps=4, flap_cl=1.05, flap_cd=0.07, retract=1, gear_cd=0.02, cm_alpha=-1.2, cm_de=-1.6, cm_q=-20,
                 cl_da=0.12, cl_p=-0.5, cl_beta=-0.10, cn_beta=0.12, cn_dr=-0.08, cn_r=-0.15, ixx=1.3e6, iyy=3.2e6, izz=4.4e6,
                 v_rotate=75, v_cruise=230, cam_dist=55, sound='jet'),
    'transport': dict(name='Airdrop Transport', kind='Four-engine turboprop lifter', mass=50000, wing_area=162, chord=4.2,
                      cl0=0.3, cla=5.3, alpha_stall=15, clmax=1.6, cd0=0.03, k=0.05, thrust=150000, engine='prop', prop_v=200,
                      flaps=3, flap_cl=0.9, flap_cd=0.06, retract=0, gear_cd=0.015, cm_alpha=-1.1, cm_de=-1.5, cm_q=-18,
                      cl_da=0.13, cl_p=-0.5, cl_beta=-0.09, cn_beta=0.12, cn_dr=-0.08, cn_r=-0.15, ixx=1.0e6, iyy=1.3e6, izz=2.2e6,
                      v_rotate=55, v_cruise=150, cam_dist=50, sound='turboprop'),
}

def rotate_y(p, ang):
    c, s = math.cos(ang), math.sin(ang)
    out = p.copy()
    out[..., 0] = c * p[..., 0] + s * p[..., 2]
    out[..., 2] = -s * p[..., 0] + c * p[..., 2]
    return out

def fix_texture(path, size, kind, dst):
    im = Image.open(path)
    if kind == 'color':
        im = im.convert('RGBA') if 'A' in im.getbands() else im.convert('RGB')
    elif kind == 'normal':
        im = im.convert('RGB')
    elif kind == 'gloss':
        im = Image.eval(im.convert('L'), lambda g: 255 - g)          # glossiness -> roughness
    elif kind == 'spec':
        im = Image.eval(im.convert('L'), lambda g: int(255 - 0.75 * g))
    elif kind == 'bump':                                              # height -> tangent-space normal map
        h = np.asarray(im.convert('L').filter(ImageFilter.GaussianBlur(1)), np.float32) / 255
        gx = np.roll(h, -1, 1) - np.roll(h, 1, 1); gy = np.roll(h, -1, 0) - np.roll(h, 1, 0)
        n = np.stack([-gx * 3, gy * 3, np.ones_like(h)], -1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        im = Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8))
    if max(im.size) > size:
        im = im.resize((size, size) if im.size[0] == im.size[1] else (size, int(size * im.size[1] / im.size[0])), Image.LANCZOS)
    if kind == 'color' and im.mode == 'RGBA' and np.asarray(im)[..., 3].min() > 250: im = im.convert('RGB')
    if im.mode == 'RGBA' or kind == 'normal': dst += '.png'; im.save(dst, optimize=True)
    else: dst += '.jpg'; im.save(dst, quality=90)
    return os.path.basename(dst)

def fstr(s, n=48):
    b = s.encode('utf-8')[:n - 1]
    return b + b'\0' * (n - len(b))

def build(acid, cfg):
    parts = load_raw(os.path.join(RAW, cfg['raw']))
    allp = np.concatenate([p['v'][:, :3] for p in parts])
    # 1. orientation: nose on +Z
    yaw = cfg['yaw']
    if yaw == 'auto':
        # the fuselage runs from the top of the tail fin (the highest point) to the propeller hub
        hub = [p for p in parts if p['name'] == cfg['prop_part']][0]['v'][:, :3].mean(0)
        dist = np.linalg.norm((allp - hub)[:, [0, 2]], axis=1)
        far = allp[dist > 0.6 * dist.max()]                 # ignore the propeller blades themselves
        fin = far[np.argmax(far[:, 1])]
        nose = (hub - fin)[[0, 2]]
        yaw = -math.atan2(nose[0], nose[1])
    for p in parts:
        p['v'][:, 0:3] = rotate_y(p['v'][:, 0:3], yaw)
        p['v'][:, 3:6] = rotate_y(p['v'][:, 3:6], yaw)
    allp = np.concatenate([p['v'][:, :3] for p in parts])
    mn, mx = allp.min(0), allp.max(0)
    s = cfg['length'] / (mx[2] - mn[2])
    for p in parts: p['v'][:, :3] *= s
    allp *= s; mn *= s; mx *= s
    # 2. landing gear: the main wheels are the lowest points off the centreline; the third wheel is
    #    the lowest point on the centreline well ahead (tricycle) or well behind (taildragger)
    span, length, height = mx[0] - mn[0], mx[2] - mn[2], mx[1] - mn[1]
    cx = (mx[0] + mn[0]) / 2
    rel = allp - np.array([cx, 0, 0])
    band = rel[rel[:, 1] < mn[1] + 0.04 * height]
    side = band[np.abs(band[:, 0]) > 0.025 * span]
    L, Rr = side[side[:, 0] < 0], side[side[:, 0] > 0]
    mainL = np.array([L[:, 0].mean(), mn[1], L[:, 2].mean()]); mainR = np.array([Rr[:, 0].mean(), mn[1], Rr[:, 2].mean()])
    mz = (mainL[2] + mainR[2]) / 2
    centre = rel[np.abs(rel[:, 0]) < 0.03 * span]
    fwd = centre[centre[:, 2] > mz + 0.2 * length]; aft = centre[centre[:, 2] < mz - 0.25 * length]
    nose = fwd[np.argmin(fwd[:, 1])] if len(fwd) else None
    tail = aft[np.argmin(aft[:, 1])] if len(aft) else None
    tricycle = nose is not None and nose[1] < mn[1] + 0.12 * height
    third = nose if tricycle else tail
    # 3. origin at the centre of gravity: a little ahead of the mains (tricycle) or behind them (taildragger)
    cz = mz + (0.05 if tricycle else -0.07) * length
    cy = mn[1] + 0.42 * height
    off = np.array([cx, cy, cz], np.float32)
    for p in parts: p['v'][:, :3] -= off
    allp -= off; mn -= off; mx -= off
    gear = [mainL - off + np.array([cx, 0, 0]), mainR - off + np.array([cx, 0, 0]), np.array([0, third[1], third[2]]) + np.array([0, -cy, -cz])]
    report = {'length': float(length), 'span': float(span), 'height': float(height), 'tricycle': bool(tricycle), 'gear': [g.round(2).tolist() for g in gear]}
    # 4. split parts; propeller parts that span both sides become two parts
    out_parts = []
    for p in parts:
        kind = 1 if p['name'] in cfg['props'] else (2 if p['name'] in cfg['gear'] else 0)
        v = p['v']
        groups = [v]
        if kind == 1:
            cen = v.reshape(-1, 3, 8)[:, :, 0].mean(1)
            if cfg.get('split_props') and cen.min() < -0.5 and cen.max() > 0.5:
                tri = v.reshape(-1, 3, 8)
                groups = [tri[cen < 0].reshape(-1, 8), tri[cen >= 0].reshape(-1, 8)]
        for g in groups:
            pivot = g[:, :3].mean(0) if kind else np.zeros(3, np.float32)
            if kind == 2: pivot = np.array([g[:, 0].mean(), g[:, 1].max(), g[:, 2].mean()])
            out_parts.append(dict(name=p['name'], mat=p['name'] if p['name'] in cfg['mats'] else p['mat'], kind=kind, pivot=pivot, v=g))
    if REPORT:
        print(acid, report)
        for op in out_parts:
            if op['kind']: print('   part', op['name'], 'kind', op['kind'], 'pivot', np.round(op['pivot'], 2))
        return
    # 5. materials + textures
    d = os.path.join(OUT, acid); os.makedirs(d, exist_ok=True)
    matnames = sorted(set(op['mat'] for op in out_parts))
    mats = []
    for mname in matnames:
        mc = cfg['mats'].get(mname, {})
        safe = ''.join(ch if ch.isalnum() else '_' for ch in mname).strip('_').lower()
        tex = {'color': '', 'normal': '', 'rough': ''}
        if 'color' in mc: tex['color'] = fix_texture(mc['color'], cfg['tex_max'], 'color', os.path.join(d, safe + '_color'))
        if 'normal' in mc: tex['normal'] = fix_texture(mc['normal'], cfg['tex_max'], 'normal', os.path.join(d, safe + '_normal'))
        if 'bump' in mc: tex['normal'] = fix_texture(mc['bump'], cfg['tex_max'], 'bump', os.path.join(d, safe + '_normal'))
        if 'gloss' in mc: tex['rough'] = fix_texture(mc['gloss'], cfg['tex_max'], 'gloss', os.path.join(d, safe + '_rough'))
        if 'spec' in mc: tex['rough'] = fix_texture(mc['spec'], cfg['tex_max'], 'spec', os.path.join(d, safe + '_rough'))
        rgba = mc.get('rgba', (1, 1, 1, 1) if 'color' in mc else (0.6, 0.6, 0.6, 1))
        mats.append(dict(name=mname, rgba=rgba, metal=mc.get('metal', 0.0), rough=mc.get('rough', 0.55), flags=1 if mc.get('glass') else 0, tex=tex))
    # 6. write the model
    with open(os.path.join(d, 'model.wmdl'), 'wb') as f:
        f.write(b'WMDL' + struct.pack('<I3f', 1, report['length'], report['span'], report['height']))
        f.write(struct.pack('<I', len(mats)))
        for m in mats:
            f.write(fstr(m['name']) + struct.pack('<4f2fI', *m['rgba'], m['metal'], m['rough'], m['flags']))
            f.write(fstr(m['tex']['color']) + fstr(m['tex']['normal']) + fstr(m['tex']['rough']))
        f.write(struct.pack('<I', len(out_parts)))
        for op in out_parts:
            v = np.ascontiguousarray(op['v'].astype(np.float32))
            uniq, inv = np.unique(v.view(np.dtype((np.void, 32))), return_inverse=True)
            verts = uniq.view(np.float32).reshape(-1, 8)
            idx = inv.astype(np.uint32).ravel()
            f.write(fstr(op['name']) + struct.pack('<I3f3fIII', op['kind'], *op['pivot'], 0, 0, 1, matnames.index(op['mat']), len(verts), len(idx)))
            f.write(verts.tobytes()); f.write(idx.tobytes())
    fl = FLIGHT[acid]
    with open(os.path.join(d, 'aircraft.cfg'), 'w') as f:
        f.write('# WINGMERE aircraft definition. Units: metres, kilograms, newtons, metres/second, degrees.\n')
        f.write('# Generated by tools/build_aircraft.py; edit freely to tune how it flies.\n')
        for k2, v2 in fl.items(): f.write(f'{k2} = {v2}\n')
        f.write(f'span = {report["span"]:.2f}\nlength = {report["length"]:.2f}\nheight = {report["height"]:.2f}\n')
        f.write(f'tricycle = {int(report["tricycle"])}\n')
        for i, g in enumerate(report['gear']): f.write(f'gear{i} = {g[0]:.3f} {g[1]:.3f} {g[2]:.3f}\n')
    size = sum(os.path.getsize(os.path.join(d, x)) for x in os.listdir(d))
    print(f'{acid}: {len(out_parts)} parts, {len(mats)} materials, {report["length"]:.1f} m long, {report["span"]:.1f} m span, {size/1e6:.1f} MB')

for k, c in AIRCRAFT.items():
    build(k, c)
