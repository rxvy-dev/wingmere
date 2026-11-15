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


def _pbr_set(folder, prefix, names, metal=0.3, rough=0.5):
    out = {}
    for n in names:
        base = T(folder, 'textures', f'{prefix}{n}_')
        m = dict(metal=metal, rough=rough)
        if os.path.exists(base + 'BaseColor.png'): m['color'] = base + 'BaseColor.png'
        else: m['rgba'] = (0.08, 0.08, 0.09, 1)
        if os.path.exists(base + 'Normal.png'): m['normal'] = base + 'Normal.png'
        if os.path.exists(base + 'Roughness.png'): m['roughmap'] = base + 'Roughness.png'
        out[n] = m
    return out

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
        synth_gear=dict(nose=0.14, main=0.55, track=2.2, height=0.9, rn=0.5, rm=0.62),
        props=['airscrew01a_LOD0', 'airscrew01b_LOD0', 'airscrew01c_LOD0', 'airscrew01d_LOD0'], gear=[], tex_max=1024),
    'f35': dict(
        raw='f35.raw', length=15.67, yaw=0,
        mats={'default': dict(color=T('f35/textures/texture_diffuse.png'), normal=T('f35/textures/texture_normal.png'),
                              roughmap=T('f35/textures/texture_roughness.png'), metal=0.35, rough=0.5)},
        synth_gear=dict(nose=0.24, main=0.6, track=2.05, height=1.0, rn=0.33, rm=0.42),
        nozzle=dict(x=(0.0, 0.75), depth=0.35),
        pilot_from=dict(z=(0.2, 0.34), drop=0.36),
        props=[], gear=[], tex_max=2048),
    'f14': dict(
        raw='f14.raw', length=19.1, yaw=math.pi,
        mats={'f14.2': dict(color=T('f14/textures/F_14_Tomcat_Body.png'), rough=0.45, metal=0.15),
              'f_gear.0': dict(color=T('f14/textures/Gear_1.png'), rough=0.5, metal=0.4),
              'f_gear.2': dict(color=T('f14/textures/Gear_1.png'), rough=0.5, metal=0.4),
              'f_gear.3': dict(color=T('f14/textures/Gear_1.png'), rough=0.5, metal=0.4),
              'extra1.1': dict(color=T('f14/textures/AIM-54C_D.png'), rough=0.5, metal=0.1),
              'extra1.2': dict(color=T('f14/textures/AIM-9M_D.png'), rough=0.5, metal=0.1),
              'lights.0': dict(color=T('f14/textures/Lights_T.png'), rough=0.3),
              'misc_b.0': dict(rgba=(0.35, 0.36, 0.38, 1), rough=0.5, metal=0.3),
              'misc_b.1': dict(rgba=(0.35, 0.36, 0.38, 1), rough=0.5, metal=0.3),
              'glass': dict(rgba=(0.05, 0.06, 0.07, 0.3), glass=True, rough=0.05),
              'glass.001': dict(rgba=(0.05, 0.06, 0.07, 0.3), glass=True, rough=0.05),
              'glass.002': dict(rgba=(0.05, 0.06, 0.07, 0.3), glass=True, rough=0.05),
              'canopy.3': dict(rgba=(0.05, 0.06, 0.07, 0.3), glass=True, rough=0.05),
              '*cockpit': dict(rgba=(0.09, 0.09, 0.1, 1), rough=0.6)},
        drop=['static_prop', 'static_prop2', 'moving_prop', 'moving_prop2'],
        nozzle=dict(nodes=['engines'], x=(0.5, 2.3), pairs=True, depth=0.25),
        props=[], gear=['f_gear', 'm_gear', 'm_gear_arm', 'tire_l', 'tire_r'], tex_max=2048),
    'f117': dict(
        raw='f117.raw', length=20.1, yaw=math.pi,
        mats=_pbr_set('f117', 'full_', [f'Meshpart{i}Mtl' for i in range(1, 49)], metal=0.25, rough=0.6),
        pilot_from=dict(nodes=['Meshpart22', 'Meshpart23'], drop=0.3),
        props=[], gear=[f'Meshpart{i}' for i in list(range(6, 13)) + list(range(26, 33)) + list(range(39, 46))], tex_max=1024),
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
    'f35': dict(name='F-35C Lightning II', kind='Stealth multirole fighter, carrier variant', mass=22000, wing_area=62.1, chord=5.0,
                cl0=0.05, cla=3.6, alpha_stall=28, clmax=1.45, cd0=0.023, k=0.12, thrust=125000, afterburner=191000, engine='jet', prop_v=0,
                flaps=2, flap_cl=0.35, flap_cd=0.03, retract=1, gear_cd=0.025, cm_alpha=-0.45, cm_de=-1.0, cm_q=-9,
                cl_da=0.10, cl_p=-0.35, cl_beta=-0.05, cn_beta=0.10, cn_dr=-0.06, cn_r=-0.12, ixx=35000, iyy=180000, izz=200000,
                v_rotate=70, v_cruise=250, vne=360, cam_dist=24, sound='jet'),
    'f14': dict(name='F-14 Tomcat', kind='Swing-wing naval interceptor, two crew', mass=24000, wing_area=52.5, chord=3.0,
                cl0=0.1, cla=4.5, alpha_stall=22, clmax=1.6, cd0=0.024, k=0.09, thrust=142000, afterburner=250000, engine='jet', prop_v=0,
                flaps=2, flap_cl=0.6, flap_cd=0.05, retract=1, gear_cd=0.025, cm_alpha=-0.6, cm_de=-1.1, cm_q=-10,
                cl_da=0.09, cl_p=-0.40, cl_beta=-0.07, cn_beta=0.11, cn_dr=-0.07, cn_r=-0.13, ixx=100000, iyy=300000, izz=380000,
                v_rotate=75, v_cruise=250, vne=370, cam_dist=27, sound='jet'),
    'f117': dict(name='F-117 Nighthawk', kind='Stealth attack aircraft', mass=18000, wing_area=84.8, chord=6.0,
                 cl0=0.0, cla=2.8, alpha_stall=20, clmax=1.15, cd0=0.03, k=0.18, thrust=96000, engine='jet', prop_v=0,
                 flaps=0, flap_cl=0, flap_cd=0, retract=1, gear_cd=0.025, cm_alpha=-0.5, cm_de=-0.9, cm_q=-10,
                 cl_da=0.08, cl_p=-0.35, cl_beta=-0.06, cn_beta=0.08, cn_dr=-0.05, cn_r=-0.12, ixx=60000, iyy=200000, izz=250000,
                 v_rotate=82, v_cruise=230, vne=290, cam_dist=27, sound='jet')
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
    elif kind == 'roughmap':
        im = im.convert('L')
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

def cylinder(c0, c1, r, n=14):
    """triangles of a capped cylinder between points c0 and c1 (8 floats per vertex)"""
    c0, c1 = np.array(c0, np.float32), np.array(c1, np.float32)
    ax = c1 - c0; ax /= np.linalg.norm(ax)
    t = np.cross(ax, [0, 1, 0] if abs(ax[1]) < 0.9 else [1, 0, 0]); t /= np.linalg.norm(t)
    b = np.cross(ax, t)
    out = []
    for i in range(n):
        a0, a1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
        d0, d1 = t * math.cos(a0) + b * math.sin(a0), t * math.cos(a1) + b * math.sin(a1)
        p = [c0 + d0 * r, c1 + d0 * r, c1 + d1 * r, c0 + d1 * r]
        for tri in ((0, 1, 2), (0, 2, 3)):
            for k in tri:
                nrm = d0 if k in (0, 1) else d1
                out.append([*p[k], *nrm, i / n, 0])
        for cap, sgn in ((c0, -1), (c1, 1)):
            for q in (cap, cap + d0 * r, cap + d1 * r):
                out.append([*q, *(ax * sgn), 0, 0])
    return np.array(out, np.float32)

def gear_mesh(x, ground, z, r, top):
    """a strut from the belly down to a wheel touching the ground"""
    w = r * 0.55
    wheel = cylinder((x - w, ground + r, z), (x + w, ground + r, z), r, 18)
    strut = cylinder((x, ground + r, z), (x, top, z), r * 0.18, 10)
    return np.concatenate([wheel, strut])

def fstr(s, n=48):
    b = s.encode('utf-8')[:n - 1]
    return b + b'\0' * (n - len(b))

def build(acid, cfg):
    parts = load_raw(os.path.join(RAW, cfg['raw']))
    parts = [p for p in parts if p['name'] not in cfg.get('drop', []) and len(p['v']) >= 12]
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
    if 'synth_gear' in cfg:
        g = cfg['synth_gear']
        L = mx[2] - mn[2]
        ground = mn[1] - g['height']
        cx0 = (mx[0] + mn[0]) / 2
        for gi, (gx, gz, r) in enumerate([(cx0, mx[2] - g['nose'] * L, g['rn']), (cx0 - g['track'], mx[2] - g['main'] * L, g['rm']), (cx0 + g['track'], mx[2] - g['main'] * L, g['rm'])]):
            parts.append(dict(name=f'gear{gi}', mat='synth_gear', pivot=np.zeros(3, np.float32), v=gear_mesh(gx, ground, gz, r, mn[1] + 0.2)))
        allp = np.concatenate([p['v'][:, :3] for p in parts]); mn, mx = allp.min(0), allp.max(0)
        cfg['gear'] = cfg['gear'] + ['gear0', 'gear1', 'gear2']
        cfg['mats']['synth_gear'] = dict(rgba=(0.28, 0.28, 0.3, 1), rough=0.45, metal=0.6)
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
    # engine nozzles: the aft-most ring of vertices near the given lateral band
    if 'nozzle' in cfg:
        nz = cfg['nozzle']
        src = [p for p in parts if not nz.get('nodes') or p['name'] in nz['nodes']]
        pts = np.concatenate([p['v'][:, :3] for p in src])
        noz = []
        for side in ((-1, 1) if nz.get('pairs') else (0,)):
            ax = np.abs(pts[:, 0])
            sel = pts[(ax >= nz['x'][0]) & (ax <= nz['x'][1]) & ((pts[:, 0] * side >= 0) if side else True) & (pts[:, 1] > nz.get('ymin', -99)) & (pts[:, 1] < nz.get('ymax', 99))]
            zmin = sel[:, 2].min()
            ring = sel[sel[:, 2] < zmin + nz.get('depth', 0.3)]
            c = ring.mean(0)
            r = float(np.percentile(np.linalg.norm(ring[:, :2] - c[:2], axis=1), 90))
            noz.append((c.round(3).tolist(), round(r, 3)))
        report['nozzles'] = noz
    if 'pilot_from' in cfg:
        pf = cfg['pilot_from']
        if 'nodes' in pf:
            pts = np.concatenate([p['v'][:, :3] for p in parts if p['name'] in pf['nodes']])
        else:                                   # a band of the fuselage, as fractions of length from the nose
            z0, z1 = mx[2] - pf['z'][1] * length, mx[2] - pf['z'][0] * length
            pts = allp[(allp[:, 2] > z0) & (allp[:, 2] < z1) & (np.abs(allp[:, 0] - 0) < 0.35)]
        cmin, cmax = pts.min(0), pts.max(0)
        top = cmax[1] - pf.get('drop', 0.32)
        seats = pf.get('seats', 1)
        zs = [(cmin[2] + cmax[2]) / 2] if seats == 1 else [cmin[2] + (cmax[2] - cmin[2]) * f for f in (0.72, 0.3)]
        cfg['pilots'] = [[0.0, float(top), float(z) + pf.get('dz', 0.0)] for z in zs]
        report['pilots'] = cfg['pilots']
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
    # merge everything that is drawn the same way (same material, not a propeller) to cut draw calls
    merged = {}
    for op in out_parts:
        if op['kind'] == 1:
            merged[id(op)] = op
            continue
        k = (op['mat'], op['kind'])
        if k in merged:
            merged[k]['v'] = np.concatenate([merged[k]['v'], op['v']])
        else:
            merged[k] = dict(op, name=op['name'] if op['kind'] == 0 else 'gear')
    out_parts = list(merged.values())
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
        mc = cfg['mats'].get(mname)
        if mc is None:
            mc = next((v for k, v in cfg['mats'].items() if k.startswith('*') and mname.startswith(k[1:])), {})
        safe = ''.join(ch if ch.isalnum() else '_' for ch in mname).strip('_').lower()
        tex = {'color': '', 'normal': '', 'rough': ''}
        if 'color' in mc: tex['color'] = fix_texture(mc['color'], cfg['tex_max'], 'color', os.path.join(d, safe + '_color'))
        if 'normal' in mc: tex['normal'] = fix_texture(mc['normal'], cfg['tex_max'], 'normal', os.path.join(d, safe + '_normal'))
        if 'bump' in mc: tex['normal'] = fix_texture(mc['bump'], cfg['tex_max'], 'bump', os.path.join(d, safe + '_normal'))
        if 'gloss' in mc: tex['rough'] = fix_texture(mc['gloss'], cfg['tex_max'], 'gloss', os.path.join(d, safe + '_rough'))
        if 'spec' in mc: tex['rough'] = fix_texture(mc['spec'], cfg['tex_max'], 'spec', os.path.join(d, safe + '_rough'))
        if 'roughmap' in mc: tex['rough'] = fix_texture(mc['roughmap'], cfg['tex_max'], 'roughmap', os.path.join(d, safe + '_rough'))
        rgba = mc.get('rgba', (1, 1, 1, 1) if 'color' in mc else (0.6, 0.6, 0.6, 1))
        mats.append(dict(name=mname, rgba=rgba, metal=mc.get('metal', 0.0), rough=mc.get('rough', 0.55), flags=1 if mc.get('glass') else 0, tex=tex))
    # 5b. some models carry zero-length normals: give those vertices their face normal
    for op in out_parts:
        v = op['v']; tri = v.reshape(-1, 3, 8)
        fn = np.cross(tri[:, 1, :3] - tri[:, 0, :3], tri[:, 2, :3] - tri[:, 0, :3])
        fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
        bad = np.linalg.norm(tri[:, :, 3:6], axis=2) < 0.5
        tri[:, :, 3:6] = np.where(bad[..., None], fn[:, None, :], tri[:, :, 3:6])
        op['v'] = tri.reshape(-1, 8)
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
        for i, (c, r) in enumerate(report.get('nozzles', [])): f.write(f'nozzle{i} = {c[0]:.3f} {c[1]:.3f} {c[2]:.3f} {r:.3f}\n')
        for i, pp in enumerate(cfg.get('pilots', [])): f.write(f'pilot{i} = {pp[0]:.3f} {pp[1]:.3f} {pp[2]:.3f}\n')
    size = sum(os.path.getsize(os.path.join(d, x)) for x in os.listdir(d))
    print(f'{acid}: {len(out_parts)} parts, {len(mats)} materials, {report["length"]:.1f} m long, {report["span"]:.1f} m span, {size/1e6:.1f} MB')

only = [a for a in sys.argv[1:] if not a.startswith('--')]
for k, c in AIRCRAFT.items():
    if not only or k in only:
        build(k, c)
