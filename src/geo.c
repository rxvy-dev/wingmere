/* WINGMERE - matrices, WGS84 geodesy and Web Mercator tile maths.  MIT licence. */
#include "wm.h"

/* ------------------------------------------------------------------ matrices */
m4 m4_ident(void) { m4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; return r; }
m4 m4_mul(m4 a, m4 b) {
    m4 r;
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + i] * b.m[c * 4 + k];
            r.m[c * 4 + i] = s;
        }
    return r;
}
m4 m4_persp(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    m4 r = {{f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0}};
    return r;
}
m4 m4_ortho(float l, float r, float b, float t, float n, float f) {
    m4 o = {{2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0, -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1}};
    return o;
}
m4 m4_look(v3 eye, v3 f, v3 up) {
    f = vnorm(f);
    v3 s = vnorm(vcross(f, up)), u = vcross(s, f);
    m4 r = {{s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -vdot(s, eye), -vdot(u, eye), vdot(f, eye), 1}};
    return r;
}
m4 m4_trs(v3 p, quat q, float s) {
    v3 x = vmul(q_rot(q, V(1, 0, 0)), s), y = vmul(q_rot(q, V(0, 1, 0)), s), z = vmul(q_rot(q, V(0, 0, 1)), s);
    m4 r = {{x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, p.x, p.y, p.z, 1}};
    return r;
}
v3 m4_point(m4 a, v3 p) {
    return V(a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12], a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
             a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]);
}
m4 m4_inv(m4 a) {
    float *m = a.m, inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    m4 r;
    det = fabsf(det) > 1e-30f ? 1.0f / det : 0;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] * det;
    return r;
}
quat q_look(v3 f, v3 up) {
    /* build the rotation matrix whose columns are right(-X), up(+Y), forward(+Z), then convert */
    f = vnorm(f);
    v3 x = vnorm(vcross(up, f)), y = vcross(f, x);
    float m00 = x.x, m11 = y.y, m22 = f.z, tr = m00 + m11 + m22;
    quat q;
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q.w = 0.25f * s; q.x = (y.z - f.y) / s; q.y = (f.x - x.z) / s; q.z = (x.y - y.x) / s; }
    else if (m00 > m11 && m00 > m22) { float s = sqrtf(1 + m00 - m11 - m22) * 2; q.w = (y.z - f.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (f.x + x.z) / s; }
    else if (m11 > m22) { float s = sqrtf(1 + m11 - m00 - m22) * 2; q.w = (f.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (f.y + y.z) / s; }
    else { float s = sqrtf(1 + m22 - m00 - m11) * 2; q.w = (x.y - y.x) / s; q.x = (f.x + x.z) / s; q.y = (f.y + y.z) / s; q.z = 0.25f * s; }
    return q_norm(q);
}

/* ------------------------------------------------------------------ WGS84 */
static const double WA = 6378137.0, WF = 1.0 / 298.257223563;
#define WE2 (WF * (2 - WF))
#define RAD (M_PI / 180.0)

d3 geo_to_ecef(double lat, double lon, double h) {
    double la = lat * RAD, lo = lon * RAD, s = sin(la), c = cos(la);
    double n = WA / sqrt(1 - WE2 * s * s);
    return D((n + h) * c * cos(lo), (n + h) * c * sin(lo), (n * (1 - WE2) + h) * s);
}
void ecef_to_geo(d3 p, double *lat, double *lon, double *h) {
    /* Bowring's method with one refinement: millimetre accurate at flight altitudes */
    double b = WA * (1 - WF), ep2 = (WA * WA - b * b) / (b * b);
    double r = sqrt(p.x * p.x + p.y * p.y);
    double th = atan2(p.z * WA, r * b);
    double la = atan2(p.z + ep2 * b * pow(sin(th), 3), r - WE2 * WA * pow(cos(th), 3));
    for (int i = 0; i < 2; i++) {
        double n = WA / sqrt(1 - WE2 * sin(la) * sin(la));
        double hh = r / cos(la) - n;
        la = atan2(p.z, r * (1 - WE2 * n / (n + hh)));
    }
    double n = WA / sqrt(1 - WE2 * sin(la) * sin(la));
    *lat = la / RAD;
    *lon = atan2(p.y, p.x) / RAD;
    *h = fabs(cos(la)) > 1e-9 ? r / cos(la) - n : fabs(p.z) - b;
}
void enu_basis(double lat, double lon, v3 *e, v3 *n, v3 *u) {
    double la = lat * RAD, lo = lon * RAD;
    *e = V((float)-sin(lo), (float)cos(lo), 0);
    *n = V((float)(-sin(la) * cos(lo)), (float)(-sin(la) * sin(lo)), (float)cos(la));
    *u = V((float)(cos(la) * cos(lo)), (float)(cos(la) * sin(lo)), (float)sin(la));
}
double geo_dist(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * RAD, p2 = lat2 * RAD, dp = p2 - p1, dl = (lon2 - lon1) * RAD;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * EARTH_R * atan2(sqrt(a), sqrt(1 - a));
}
double geo_bearing(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * RAD, p2 = lat2 * RAD, dl = (lon2 - lon1) * RAD;
    double b = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / RAD;
    return fmod(b + 360, 360);
}
void geo_offset(double lat, double lon, double brg, double dist, double *olat, double *olon) {
    double p1 = lat * RAD, l1 = lon * RAD, b = brg * RAD, d = dist / EARTH_R;
    double p2 = asin(sin(p1) * cos(d) + cos(p1) * sin(d) * cos(b));
    double l2 = l1 + atan2(sin(b) * sin(d) * cos(p1), cos(d) - sin(p1) * sin(p2));
    *olat = p2 / RAD; *olon = fmod(l2 / RAD + 540, 360) - 180;
}

/* ------------------------------------------------------------------ Web Mercator (the slippy-map tile grid) */
double merc_x(double lon, int z) { return (lon + 180.0) / 360.0 * (double)(1 << z); }
double merc_y(double lat, int z) {
    lat = lat > 85.05112878 ? 85.05112878 : (lat < -85.05112878 ? -85.05112878 : lat);
    double la = lat * RAD;
    return (1.0 - log(tan(la) + 1.0 / cos(la)) / M_PI) / 2.0 * (double)(1 << z);
}
double merc_lon(double x, int z) { return x / (double)(1 << z) * 360.0 - 180.0; }
double merc_lat(double y, int z) { double n = M_PI - 2.0 * M_PI * y / (double)(1 << z); return atan(sinh(n)) / RAD; }
