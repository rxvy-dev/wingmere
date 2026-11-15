/* WINGMERE - flight dynamics.  MIT licence.
 *
 * A six-degree-of-freedom rigid body on the WGS84 ellipsoid: lift with a real stall, induced and
 * parasitic drag, flaps and gear, prop or jet thrust that fades with speed and altitude,
 * stability derivatives for pitch/roll/yaw, and sprung, braked, steerable landing gear on the
 * streamed terrain.  Coefficients come from each aircraft.cfg. */
#include "wm.h"

static float rho_at(double h) { return (float)(1.225 * exp(-fmax(h, -500.0) / 8500.0)); }

/* pitching moment at zero elevator, chosen so the aircraft trims itself at cruise speed */
static float cm0_of(const AircraftDef *a) {
    float q = 0.5f * rho_at(1500) * a->v_cruise * a->v_cruise;
    float cl = a->mass * 9.81f / (q * a->wing_area);
    float alpha = (cl - a->cl0) / a->cla;
    return -a->cm_alpha * alpha;
}

void flight_auto_trim(Flight *f) {
    const AircraftDef *a = f->def;
    float Vs = vlen(vsub(f->vel, f->wind));
    if (Vs < 10) { f->trim = 0; return; }
    float q = 0.5f * rho_at(f->alt) * Vs * Vs;
    float cl = a->mass * 9.81f / (q * a->wing_area) * fmaxf(vdot(f->up, q_rot(f->rot, V(0, 1, 0))), 0.3f);
    float alpha = (cl - a->cl0 - a->flap_cl * f->flaps) / a->cla;
    float de = -(cm0_of(a) + a->cm_alpha * alpha) / a->cm_de;
    f->trim = clampf(-de / 0.35f, -0.8f, 0.8f);
}

static float lowest_gear(const AircraftDef *a, quat rot, v3 up) {
    float lo = 1e9f;
    for (int i = 0; i < 3; i++) lo = fminf(lo, vdot(q_rot(rot, a->gear[i]), up));
    return lo;
}

void flight_reset(Flight *f, const AircraftDef *a, double lat, double lon, double alt, float hdg, float speed, bool onGround) {
    memset(f, 0, sizeof *f);
    f->def = a;
    v3 e, n, u;
    enu_basis(lat, lon, &e, &n, &u);
    v3 fwd = vadd(vmul(n, cosf(hdg * DEG)), vmul(e, sinf(hdg * DEG)));
    float pitch = 0;
    if (onGround) {
        /* sit on all three wheels: taildraggers rest nose-high */
        v3 m = vmul(vadd(a->gear[0], a->gear[1]), 0.5f), t = a->gear[2];
        pitch = atan2f(t.y - m.y, a->tricycle ? t.z - m.z : m.z - t.z) * (a->tricycle ? 1.0f : 1.0f);
        if (a->tricycle) pitch = -pitch;
    } else pitch = 2 * DEG;
    v3 fp = vadd(vmul(fwd, cosf(pitch)), vmul(u, sinf(pitch))), up = vsub(vmul(u, cosf(pitch)), vmul(fwd, sinf(pitch)));
    f->rot = q_look(fp, up);
    if (onGround) {
        float gh = terrain_height(lat, lon, NULL, NULL);
        alt = gh - lowest_gear(a, f->rot, u) + 0.03;
    }
    f->pos = geo_to_ecef(lat, lon, alt);
    f->vel = vmul(fwd, speed);
    f->gearDown = f->gearCmd = (onGround || !a->retract || speed < a->v_rotate * 1.7f) ? 1.0f : 0.0f;
    if (!onGround && a->flaps && speed < a->v_rotate * 1.7f) { f->flapsSet = (a->flaps + 1) / 2; f->flaps = (float)f->flapsSet / a->flaps; }
    f->lat = lat; f->lon = lon; f->alt = alt; f->up = u; f->east = e; f->north = n;
    f->onGround = onGround;
    for (int i = 0; i < 3; i++) f->contact[i] = onGround;
    if (!onGround) flight_trim(f, 0);
    f->brake = onGround ? 1 : 0;
}

static float sound_speed(double alt) { return fmaxf(295.0f, 340.3f - 0.0039f * (float)fmax(alt, 0)); }
/* turbofan thrust falls away with Mach number (ram drag) and with thinner air */
static float jet_lapse(float mach) { mach = fminf(mach, 1.2f); return 1 - 0.45f * mach + 0.12f * mach * mach; }
static float full_thrust(const AircraftDef *a, float V, float rho) {
    if (a->jet) return a->thrust * powf(rho / 1.225f, 0.75f) * jet_lapse(V / 330);
    return a->thrust * clampf(1 - V / a->prop_v * 0.8f, 0.15f, 1) * powf(rho / 1.225f, 0.7f);
}
float flight_stall_speed(const AircraftDef *a, float flaps) {
    return sqrtf(2 * a->mass * 9.80665f / (1.225f * a->wing_area * (a->clmax + a->flap_cl * flaps * 0.85f)));
}
void flight_trim(Flight *f, float gamma_deg) {
    const AircraftDef *a = f->def;
    v3 air = vsub(f->vel, f->wind);
    float Vs = vlen(air);
    if (Vs < 5) return;
    v3 u = f->up, F0 = q_rot(f->rot, V(0, 0, 1));
    v3 h = vnorm(vsub(F0, vmul(u, vdot(F0, u))));
    float gam = gamma_deg * DEG, rho = rho_at(f->alt);
    float q = 0.5f * rho * Vs * Vs, W = a->mass * 9.80665f;
    float cl = W * cosf(gam) / (q * a->wing_area);
    float alpha = clampf((cl - a->cl0 - a->flap_cl * f->flaps) / a->cla, -0.1f, a->alpha_stall * 0.9f);
    v3 vdir = vadd(vmul(h, cosf(gam)), vmul(u, sinf(gam)));
    float pitch = gam + alpha;
    v3 fp = vadd(vmul(h, cosf(pitch)), vmul(u, sinf(pitch))), upb = vsub(vmul(u, cosf(pitch)), vmul(h, sinf(pitch)));
    f->rot = q_look(fp, upb);
    f->vel = vadd(vmul(vdir, Vs), f->wind);
    f->omega = V(0, 0, 0);
    flight_auto_trim(f);
    float D = q * a->wing_area * (a->cd0 + a->k * cl * cl + a->flap_cd * f->flaps + a->gear_cd * f->gearDown);
    float T = full_thrust(a, Vs, rho), need = D + W * sinf(gam);
    f->throttle = clampf(a->jet ? (need / T - 0.04f) / 0.96f : need / T, 0, 1);
}

static void crash(Flight *f, const char *why) {
    if (f->crashed) return;
    f->crashed = true;
    snprintf(f->crashWhy, sizeof f->crashWhy, "%s", why);
    f->vel = V(0, 0, 0); f->omega = V(0, 0, 0);
    audio_event(2);
}

static void substep(Flight *f, float h) {
    const AircraftDef *a = f->def;
    double lat, lon, alt;
    ecef_to_geo(f->pos, &lat, &lon, &alt);
    v3 e, n, u;
    enu_basis(lat, lon, &e, &n, &u);
    v3 F = q_rot(f->rot, V(0, 0, 1)), Up = q_rot(f->rot, V(0, 1, 0)), Rt = vcross(F, Up);
    v3 vel = vsub(f->vel, f->wind);              /* air-relative velocity drives the aerodynamics */
    float Vv = vlen(vel), rho = rho_at(alt), qd = 0.5f * rho * Vv * Vv, S = a->wing_area;
    float uf = vdot(vel, F), wu = vdot(vel, Up), sr = vdot(vel, Rt);
    float alpha = Vv > 3 ? atan2f(-wu, uf) : 0;
    float beta = Vv > 3 ? asinf(clampf(sr / Vv, -1, 1)) : 0;
    float fl = f->flaps;
    /* ---- lift with stall */
    float as = a->alpha_stall - fl * 1.5f * DEG, clmaxF = a->clmax + a->flap_cl * fl * 0.85f;
    float clLin = a->cl0 + a->cla * alpha + a->flap_cl * fl, CL, aa = fabsf(alpha);
    if (aa <= as) CL = clampf(clLin, -clmaxF, clmaxF);
    else {
        float atS = clampf(a->cl0 + a->cla * as * signf(alpha) + a->flap_cl * fl, -clmaxF, clmaxF);
        CL = lerpf(atS, 1.05f * sinf(2 * alpha), smooth(as, as + 12 * DEG, aa));
    }
    f->stall = aa > as && Vv > 8 && !f->onGround;
    float mach = Vv / sound_speed(alt), mcrit = a->jet ? 0.74f : 0.64f;
    float wave = mach > mcrit ? 20 * powf(mach - mcrit, 4) + 0.6f * powf(fmaxf(mach - mcrit - 0.06f, 0), 2) : 0;   /* transonic drag rise */
    float CD = a->cd0 + wave + a->k * CL * CL + a->flap_cd * fl + a->gear_cd * f->gearDown + (aa > as ? 1.0f * sinf(aa) * sinf(aa) : 0) + 0.5f * fabsf(sinf(beta));
    v3 vhat = Vv > 0.1f ? vmul(vel, 1 / Vv) : F;
    v3 Ld = Vv > 0.1f ? vnorm(vcross(Rt, vhat)) : Up;
    v3 force = vadd(vmul(Ld, CL * qd * S), vmul(vhat, -CD * qd * S));
    force = vadd(force, vmul(Rt, -0.2f * beta * qd * S));
    /* ---- thrust */
    float thr = f->throttle, T;
    if (a->jet) T = a->thrust * (0.04f + 0.96f * thr) * powf(rho / 1.225f, 0.75f) * jet_lapse(mach);
    else T = a->thrust * thr * clampf(1 - Vv / a->prop_v * 0.8f, 0.15f, 1) * powf(rho / 1.225f, 0.7f);
    force = vadd(force, vmul(F, T));
    force = vadd(force, vmul(u, -9.80665f * a->mass));
    /* ---- moments from the stability derivatives */
    float Vm = fmaxf(Vv, 1), qc = qd;
    if (!a->jet) qc += 0.5f * rho * (28 * thr) * (28 * thr) * 0.5f;      /* prop wash keeps the tail alive at low speed */
    float p = vdot(f->omega, F), q = vdot(f->omega, Rt), r = -vdot(f->omega, Up);
    float de = -clampf(f->elev + f->trim, -1.2f, 1.2f) * 0.35f;
    float M = qc * S * a->chord * (cm0_of(a) * smooth(5, 20, Vv) + a->cm_alpha * alpha * smooth(5, 20, Vv) + a->cm_de * de) + qd * S * a->chord * a->cm_q * (q * a->chord / (2 * Vm));
    float L = qc * S * a->span * (a->cl_da * f->ail * 0.3f) + qd * S * a->span * (a->cl_p * (p * a->span / (2 * Vm)) + a->cl_beta * beta);
    float N = qc * S * a->span * (a->cn_dr * (-f->rud * 0.4f)) + qd * S * a->span * (a->cn_beta * beta + a->cn_r * (r * a->span / (2 * Vm)));
    v3 angAcc = vadd(vadd(vmul(Rt, M / a->iyy), vmul(F, L / a->ixx)), vmul(Up, -N / a->izz));
    /* ---- landing gear and ground contact */
    v3 torque = V(0, 0, 0);
    bool ground = false;
    int wheels = 0;
    float crashVs = a->jet ? 4.0f : 4.6f;
    float cl = cosf((float)(lat * M_PI / 180));
    bool gearOut = !a->retract || f->gearDown > 0.95f;
    for (int i = 0; i < 3 && gearOut; i++) {
        v3 rr = q_rot(f->rot, a->gear[i]);
        double wlat = lat + vdot(rr, n) / 111320.0, wlon = lon + vdot(rr, e) / (111320.0 * fmax(cl, 0.01));
        double walt = alt + vdot(rr, u);
        bool water = false, rw = false;
        float gh = terrain_height(wlat, wlon, &water, &rw);
        float depth = (float)(gh - walt);
        if (depth <= 0) { f->contact[i] = false; continue; }
        v3 vp = vadd(f->vel, vcross(f->omega, rr));   /* wheels care about ground speed */
        float vn = vdot(vp, u);
        if (water && vlen(f->vel) > 3) { crash(f, "DITCHED IN THE WATER"); return; }
        if (!f->contact[i]) {
            if (vn < -crashVs) { crash(f, "HARD LANDING - GEAR COLLAPSED"); return; }
            if (i < 2 && !f->contact[0] && !f->contact[1]) { f->lastTouchdownVs = vn; f->touchdowns++; audio_event(1); }
        }
        f->contact[i] = true;
        float share = a->mass / 3, k = a->mass * 9.81f / (3 * 0.12f), c = 2 * 0.75f * sqrtf(k * share);
        float Fn = fmaxf(k * depth - c * vn, 0);
        v3 wf = vnorm(vsub(F, vmul(u, vdot(F, u)))), side = vcross(wf, u);
        if (i == 2) {
            float steer = f->rud * (a->tricycle ? 55 : 25) * DEG * clampf(1 - Vv / 35, 0.12f, 1);
            v3 w2 = vadd(vmul(wf, cosf(steer)), vmul(side, sinf(steer)));
            side = vcross(w2, u); wf = w2;
        }
        float vf = vdot(vp, wf), vs = vdot(vp, side);
        float mu = 0.02f + (i < 2 ? f->brake * 0.65f : 0);
        float fr = -signf(vf) * fminf(mu * Fn, fabsf(vf) * share / h);
        float fs = -signf(vs) * fminf(0.85f * Fn, fabsf(vs) * share / h);
        v3 fw = vadd(vmul(u, Fn), vadd(vmul(wf, fr), vmul(side, fs)));
        force = vadd(force, fw);
        torque = vadd(torque, vcross(rr, fw));
        ground = true; wheels++;
    }
    /* anything else touching the ground is a crash */
    v3 body[6] = {V(0, 0, a->bmax.z), V(0, 0, a->bmin.z * 0.95f), V(a->span * 0.48f, 0, 0), V(-a->span * 0.48f, 0, 0),
                  V(0, a->gear[0].y + a->height * (gearOut ? 0.08f : 0.1f), 0), V(0, a->bmax.y * 0.9f, a->bmin.z * 0.9f)};
    for (int i = 0; i < 6; i++) {
        v3 rr = q_rot(f->rot, body[i]);
        double blat = lat + vdot(rr, n) / 111320.0, blon = lon + vdot(rr, e) / (111320.0 * fmax(cl, 0.01));
        float gh = terrain_height(blat, blon, NULL, NULL);
        if (alt + vdot(rr, u) < gh - (gearOut && i == 4 ? 0.15f : 0.0f)) {
            crash(f, i == 4 && !gearOut ? "GEAR-UP LANDING" : (i >= 2 && i <= 3 ? "WINGTIP STRIKE" : (i == 0 ? "NOSE STRIKE" : (i == 1 ? "TAIL STRIKE" : "CRASHED"))));
            return;
        }
    }
    f->onGround = ground; f->wheelsDown = wheels;
    angAcc = vadd(angAcc, vadd(vadd(vmul(F, vdot(torque, F) / a->ixx), vmul(Rt, vdot(torque, Rt) / a->iyy)), vmul(Up, vdot(torque, Up) / a->izz)));
    /* ---- integrate */
    v3 acc = vmul(force, 1 / a->mass);
    f->gload = vdot(vadd(acc, vmul(u, 9.80665f)), Up) / 9.80665f;
    f->vel = vadd(f->vel, vmul(acc, h));
    f->pos = dfmad(f->pos, f->vel, h);
    f->omega = vadd(f->omega, vmul(angAcc, h));
    if (ground) f->omega = vmul(f->omega, 1 - 1.5f * h);
    float w = vlen(f->omega);
    if (w > 1e-7f) f->rot = q_norm(q_mul(q_axis(vmul(f->omega, 1 / w), w * h), f->rot));
    f->aoa = alpha / DEG; f->slip = beta / DEG;
    f->lat = lat; f->lon = lon; f->alt = alt; f->up = u; f->east = e; f->north = n;
}

void flight_step(Flight *f, float dt) {
    if (f->crashed || dt <= 0) return;
    const AircraftDef *a = f->def;
    float flapTarget = a->flaps ? (float)f->flapsSet / a->flaps : 0;
    f->flaps += clampf(flapTarget - f->flaps, -dt / 5, dt / 5);
    if (a->retract) {
        if (f->onGround) f->gearCmd = 1;
        f->gearDown += clampf(f->gearCmd - f->gearDown, -dt / 5, dt / 5);
    } else f->gearDown = 1;
    int n = (int)ceilf(dt * 240);
    n = n < 1 ? 1 : (n > 24 ? 24 : n);
    for (int i = 0; i < n && !f->crashed; i++) substep(f, dt / n);
    /* readouts */
    v3 F = q_rot(f->rot, V(0, 0, 1)), Up = q_rot(f->rot, V(0, 1, 0)), Rt = vcross(F, Up);
    float Vv = vlen(vsub(f->vel, f->wind));
    f->ias = Vv * sqrtf(rho_at(f->alt) / 1.225f);
    f->vs = vdot(f->vel, f->up);
    f->gs = vlen(vsub(f->vel, vmul(f->up, f->vs)));
    f->heading = fmodf(atan2f(vdot(F, f->east), vdot(F, f->north)) / DEG + 360, 360);
    f->pitch = asinf(clampf(vdot(F, f->up), -1, 1)) / DEG;
    f->bank = atan2f(-vdot(Rt, f->up), vdot(Up, f->up)) / DEG;
    float gh = terrain_height(f->lat, f->lon, NULL, NULL);
    f->agl = (float)(f->alt - gh) + lowest_gear(a, f->rot, f->up);
    float target = a->jet ? 20 + 80 * f->throttle : 700 + 1900 * f->throttle + f->ias * 2;
    f->rpm += (target - f->rpm) * (1 - expf(-dt * (a->jet ? 0.6f : 3.0f)));
    f->propAngle = fmodf(f->propAngle + (a->jet ? f->rpm * 0.6f : f->rpm) / 60 * 2 * PI * dt * (a->jet ? 1 : 0.35f), 2 * PI * 64);
}
