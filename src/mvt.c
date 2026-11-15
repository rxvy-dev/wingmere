/* WINGMERE - OpenStreetMap vector tiles (Mapbox Vector Tile format, OpenMapTiles schema).  MIT licence.
 *
 * Runs on the download threads.  A tile becomes:
 *   - a mask texture: R water, G pavement (aprons, taxiways), B distance to a taxiway centreline,
 *     A roads (they glow at night)
 *   - a small water bitmap the physics and the terrain mesh use to tell sea from low land
 *   - extruded 3D buildings (zoom 14 tiles), in metres around the tile centre
 *   - taxiway centreline light positions */
#include "wm.h"

/* ------------------------------------------------------------------ protobuf */
typedef struct { const uint8_t *p, *e; } PB;
static uint64_t varint(PB *b) {
    uint64_t v = 0; int s = 0;
    while (b->p < b->e) { uint8_t c = *b->p++; v |= (uint64_t)(c & 127) << s; if (!(c & 128)) break; s += 7; if (s > 63) break; }
    return v;
}
static int key(PB *b, int *wt) { uint64_t k = varint(b); *wt = (int)(k & 7); return (int)(k >> 3); }
static PB sub(PB *b) { uint64_t n = varint(b); PB r = {b->p, b->p + n}; if (r.e > b->e) r.e = b->e; b->p = r.e; return r; }
static void skip(PB *b, int wt) {
    if (wt == 0) varint(b);
    else if (wt == 1) b->p += 8;
    else if (wt == 2) { uint64_t n = varint(b); b->p += n; }
    else if (wt == 5) b->p += 4;
    else b->p = b->e;
    if (b->p > b->e) b->p = b->e;
}
static int64_t zz(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

/* layer values: numbers as doubles, strings kept as pointers into the buffer */
typedef struct { double num; const char *str; int slen; int isStr; } Val;
typedef struct { const char *s; int n; } Str;
static bool str_is(Str a, const char *b) { return a.n == (int)strlen(b) && !memcmp(a.s, b, a.n); }
static bool val_is(const Val *v, const char *b) { return v && v->isStr && v->slen == (int)strlen(b) && !memcmp(v->str, b, v->slen); }

/* ------------------------------------------------------------------ geometry */
typedef struct { float x, y; } P2;
typedef struct { P2 *p; int n, cap; int *ring; int nring, capRing; } Geom;   /* ring[i] = start index; rings end at the next start */
static void gpush(Geom *g, float x, float y) { if (g->n == g->cap) { g->cap = g->cap ? g->cap * 2 : 256; g->p = realloc(g->p, sizeof(P2) * g->cap); } g->p[g->n].x = x; g->p[g->n].y = y; g->n++; }
static void gring(Geom *g) { if (g->nring == g->capRing) { g->capRing = g->capRing ? g->capRing * 2 : 32; g->ring = realloc(g->ring, sizeof(int) * g->capRing); } g->ring[g->nring++] = g->n; }
static void decode_geom(PB geom, Geom *g, float scale) {
    g->n = 0; g->nring = 0;
    int64_t cx = 0, cy = 0;
    while (geom.p < geom.e) {
        uint32_t ci = (uint32_t)varint(&geom);
        int id = ci & 7, cnt = ci >> 3;
        if (id == 1 || id == 2) {
            for (int i = 0; i < cnt && geom.p < geom.e; i++) {
                cx += zz(varint(&geom)); cy += zz(varint(&geom));
                if (id == 1) gring(g);
                gpush(g, cx * scale, cy * scale);
            }
        } else if (id == 7) {               /* close path: repeat the first point of the ring */
            if (g->nring) { P2 f = g->p[g->ring[g->nring - 1]]; gpush(g, f.x, f.y); }
        } else break;
    }
}
static int ring_end(const Geom *g, int r) { return r + 1 < g->nring ? g->ring[r + 1] : g->n; }

/* ------------------------------------------------------------------ rasterising */
typedef struct { int n; uint8_t *ch[4]; } Raster;     /* separate planes, n*n each */
/* even-odd polygon fill with 2x2 supersampling; coordinates in pixels */
static void fill_poly(Raster *R, int c, const Geom *g, float strength) {
    int n = R->n;
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < g->n; i++) { ymin = fminf(ymin, g->p[i].y); ymax = fmaxf(ymax, g->p[i].y); }
    int y0 = (int)floorf(ymin) < 0 ? 0 : (int)floorf(ymin), y1 = (int)ceilf(ymax) > n ? n : (int)ceilf(ymax);
    static __thread float xs[8192];
    static __thread float cov[4096];
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < n; x++) cov[x] = 0;
        bool any = false;
        for (int sy = 0; sy < 2; sy++) {
            float fy = y + 0.25f + 0.5f * sy;
            int nx = 0;
            for (int r = 0; r < g->nring; r++) {
                int s = g->ring[r], e = ring_end(g, r);
                for (int i = s; i + 1 < e && nx < 8190; i++) {
                    P2 a = g->p[i], b = g->p[i + 1];
                    if ((a.y <= fy) != (b.y <= fy)) xs[nx++] = a.x + (fy - a.y) / (b.y - a.y) * (b.x - a.x);
                }
            }
            if (nx < 2) continue;
            for (int i = 1; i < nx; i++) { float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
            for (int i = 0; i + 1 < nx; i += 2) {
                for (int sx = 0; sx < 2; sx++) {
                    float off = 0.25f + 0.5f * sx;
                    int xa = (int)ceilf(xs[i] - off), xb = (int)floorf(xs[i + 1] - off);
                    if (xa < 0) xa = 0;
                    if (xb > n - 1) xb = n - 1;
                    for (int x = xa; x <= xb; x++) cov[x] += 0.25f;
                    if (xa <= xb) any = true;
                }
            }
        }
        if (!any) continue;
        uint8_t *row = R->ch[c] + y * n;
        for (int x = 0; x < n; x++) if (cov[x] > 0) { int v = row[x] + (int)(cov[x] * strength * 255); row[x] = v > 255 ? 255 : (uint8_t)v; }
    }
}
/* thick polyline: coverage into channel c, and optionally a distance field (metres*10) into channel d */
static void stroke(Raster *R, int c, const Geom *g, float halfPx, float strength, int dch, float mPerPx) {
    int n = R->n;
    float reach = halfPx + 1;
    if (dch >= 0) reach = fmaxf(reach, 25.5f / mPerPx);
    for (int r = 0; r < g->nring; r++) {
        int s = g->ring[r], e = ring_end(g, r);
        for (int i = s; i + 1 < e; i++) {
            P2 a = g->p[i], b = g->p[i + 1];
            float bx0 = fminf(a.x, b.x) - reach, bx1 = fmaxf(a.x, b.x) + reach, by0 = fminf(a.y, b.y) - reach, by1 = fmaxf(a.y, b.y) + reach;
            int x0 = bx0 < 0 ? 0 : (int)bx0, x1 = bx1 > n - 1 ? n - 1 : (int)bx1, y0 = by0 < 0 ? 0 : (int)by0, y1 = by1 > n - 1 ? n - 1 : (int)by1;
            float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    float px = x + 0.5f - a.x, py = y + 0.5f - a.y;
                    float t = L2 > 1e-6f ? clampf((px * dx + py * dy) / L2, 0, 1) : 0;
                    float ex = px - t * dx, ey = py - t * dy, d = sqrtf(ex * ex + ey * ey);
                    float cv = clampf(halfPx - d + 0.5f, 0, 1) * strength;
                    if (cv > 0) { uint8_t *q = R->ch[c] + y * n + x; int v = (int)(cv * 255); if (v > *q) *q = (uint8_t)v; }
                    if (dch >= 0) { float dm = d * mPerPx * 10; uint8_t *q = R->ch[dch] + y * n + x; if (dm < *q) *q = (uint8_t)dm; }
                }
        }
    }
}

/* ------------------------------------------------------------------ buildings */
typedef struct { float *v; int n, cap; float *c; int nb, capB; } BBuf;
static void bv(BBuf *b, float x, float y, float z, float nx, float ny, float nz, float u, float w, float kind, float hash) {
    if (b->n + 10 > b->cap) { b->cap = b->cap ? b->cap * 2 : 8192; b->v = realloc(b->v, sizeof(float) * b->cap); }
    float *o = b->v + b->n;
    o[0] = x; o[1] = y; o[2] = z; o[3] = nx; o[4] = ny; o[5] = nz; o[6] = u; o[7] = w; o[8] = kind; o[9] = hash;
    b->n += 10;
}
static float area2(const P2 *p, int n) { float a = 0; for (int i = 0; i < n; i++) { P2 u = p[i], w = p[(i + 1) % n]; a += u.x * w.y - w.x * u.y; } return a; }
static bool in_tri(P2 p, P2 a, P2 b, P2 c) {
    float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y), d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y), d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}
/* ear clipping of a counter-clockwise ring into roof triangles */
static void roof(BBuf *b, const P2 *p, int n, float h, float hash) {
    if (n < 3 || n > 400) return;
    int idx[400], m = n;
    for (int i = 0; i < n; i++) idx[i] = i;
    int guard = 0;
    while (m > 3 && guard++ < 4000) {
        bool cut = false;
        for (int i = 0; i < m; i++) {
            P2 a = p[idx[(i + m - 1) % m]], c0 = p[idx[i]], c = p[idx[(i + 1) % m]];
            if ((c0.x - a.x) * (c.y - a.y) - (c0.y - a.y) * (c.x - a.x) <= 0) continue;   /* reflex */
            bool ok = true;
            for (int k = 0; k < m && ok; k++) {
                int q = idx[k];
                if (q == idx[(i + m - 1) % m] || q == idx[i] || q == idx[(i + 1) % m]) continue;
                if (in_tri(p[q], a, c0, c)) ok = false;
            }
            if (!ok) continue;
            bv(b, a.x, a.y, h, 0, 0, 1, a.x, a.y, 1, hash); bv(b, c0.x, c0.y, h, 0, 0, 1, c0.x, c0.y, 1, hash); bv(b, c.x, c.y, h, 0, 0, 1, c.x, c.y, 1, hash);
            for (int k = i; k + 1 < m; k++) idx[k] = idx[k + 1];
            m--; cut = true;
            break;
        }
        if (!cut) break;
    }
    if (m == 3) { P2 a = p[idx[0]], c0 = p[idx[1]], c = p[idx[2]]; bv(b, a.x, a.y, h, 0, 0, 1, a.x, a.y, 1, hash); bv(b, c0.x, c0.y, h, 0, 0, 1, c0.x, c0.y, 1, hash); bv(b, c.x, c.y, h, 0, 0, 1, c.x, c.y, 1, hash); }
}
static void building(BBuf *b, const Geom *g, float tileM, float h, float hmin) {
    if (h <= hmin) h = hmin + 3;
    float bottom = hmin > 0.5f ? hmin : -4;          /* sink ground-level walls so slopes never show a gap */
    float cx = 0, cy = 0; int cn = 0;
    int start = b->n / 10;
    float hash = 0;
    for (int r = 0; r < g->nring; r++) {
        int s = g->ring[r], e = ring_end(g, r), n = e - s;
        if (n > 1 && g->p[s].x == g->p[e - 1].x && g->p[s].y == g->p[e - 1].y) n--;
        if (n < 3 || n > 400) continue;
        P2 pts[400];
        for (int i = 0; i < n; i++) { pts[i].x = (g->p[s + i].x - 0.5f) * tileM; pts[i].y = (0.5f - g->p[s + i].y) * tileM; }
        float A = area2(pts, n);
        bool outer = A > 0;                          /* after flipping y, outer rings run counter-clockwise */
        if (r == 0 && !outer) { for (int i = 0; i < n / 2; i++) { P2 t = pts[i]; pts[i] = pts[n - 1 - i]; pts[n - 1 - i] = t; } outer = true; A = -A; }
        if (outer) { for (int i = 0; i < n; i++) { cx += pts[i].x; cy += pts[i].y; } cn += n; hash = fmodf(fabsf(pts[0].x * 12.9898f + pts[0].y * 78.233f), 1.0f); }
        float u = 0;
        for (int i = 0; i < n; i++) {
            P2 a = pts[i], c = pts[(i + 1) % n];
            float dx = c.x - a.x, dy = c.y - a.y, L = sqrtf(dx * dx + dy * dy);
            if (L < 0.05f) continue;
            float nx = dy / L, ny = -dx / L;          /* outward for a counter-clockwise ring */
            if (!outer) { nx = -nx; ny = -ny; }
            bv(b, a.x, a.y, bottom, nx, ny, 0, u, bottom, 0, hash); bv(b, c.x, c.y, bottom, nx, ny, 0, u + L, bottom, 0, hash); bv(b, c.x, c.y, h, nx, ny, 0, u + L, h, 0, hash);
            bv(b, a.x, a.y, bottom, nx, ny, 0, u, bottom, 0, hash); bv(b, c.x, c.y, h, nx, ny, 0, u + L, h, 0, hash); bv(b, a.x, a.y, h, nx, ny, 0, u, h, 0, hash);
            u += L;
        }
        if (outer) roof(b, pts, n, h, hash);
    }
    int count = b->n / 10 - start;
    if (!count || !cn) return;
    if (b->nb * 4 + 4 > b->capB) { b->capB = b->capB ? b->capB * 2 : 1024; b->c = realloc(b->c, sizeof(float) * b->capB); }
    float *c = b->c + b->nb * 4;
    c[0] = cx / cn; c[1] = cy / cn; c[2] = (float)start; c[3] = (float)count;
    b->nb++;
}

/* ------------------------------------------------------------------ parking spots on aprons */
typedef struct { P2 *p; int n; int *ring; int nring; } Poly;
static bool inside_poly(const Poly *a, float x, float y) {
    bool in = false;
    for (int r = 0; r < a->nring; r++) {
        int s = a->ring[r], e = r + 1 < a->nring ? a->ring[r + 1] : a->n;
        for (int i = s; i + 1 < e; i++) {
            P2 u = a->p[i], w = a->p[i + 1];
            if ((u.y > y) != (w.y > y) && x < u.x + (y - u.y) / (w.y - u.y) * (w.x - u.x)) in = !in;
        }
    }
    return in;
}
static float edge_dist(const Poly *a, float x, float y, float *ex, float *ey) {
    float best = 1e9f;
    for (int r = 0; r < a->nring; r++) {
        int s = a->ring[r], e = r + 1 < a->nring ? a->ring[r + 1] : a->n;
        for (int i = s; i + 1 < e; i++) {
            P2 u = a->p[i], w = a->p[i + 1];
            float dx = w.x - u.x, dy = w.y - u.y, L2 = dx * dx + dy * dy;
            float t = L2 > 1e-6f ? clampf(((x - u.x) * dx + (y - u.y) * dy) / L2, 0, 1) : 0;
            float qx = u.x + t * dx, qy = u.y + t * dy, d = sqrtf((x - qx) * (x - qx) + (y - qy) * (y - qy));
            if (d < best) { best = d; *ex = qx; *ey = qy; }
        }
    }
    return best;
}
/* stands: points well inside an apron, a nose-length from its edge, off the taxiways, near buildings */
static int find_stands(const Poly *ap, int nap, const Raster *R, float mPerPx, float tileM, const float *bc, int nb, float *out, int max) {
    int n = R->n, cnt = 0;
    float step = 30 / mPerPx;
    for (int a = 0; a < nap && cnt < max; a++) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < ap[a].n; i++) { x0 = fminf(x0, ap[a].p[i].x); x1 = fmaxf(x1, ap[a].p[i].x); y0 = fminf(y0, ap[a].p[i].y); y1 = fmaxf(y1, ap[a].p[i].y); }
        x0 = fmaxf(x0, 0); y0 = fmaxf(y0, 0); x1 = fminf(x1, n - 1); y1 = fminf(y1, n - 1);
        for (float y = y0 + step * 0.5f; y < y1 && cnt < max; y += step)
            for (float x = x0 + step * 0.5f; x < x1 && cnt < max; x += step) {
                if (!inside_poly(&ap[a], x, y)) continue;
                float ex = x, ey = y, dm = edge_dist(&ap[a], x, y, &ex, &ey) * mPerPx;
                if (dm < 24 || dm > 46) continue;
                if (R->ch[2][(int)y * n + (int)x] < 245) continue;                  /* a taxiway runs through here */
                float lx = (x / n - 0.5f) * tileM, ly = (0.5f - y / n) * tileM;
                bool nearB = false;
                for (int b = 0; b < nb && !nearB; b++) { float dx = bc[b * 4] - lx, dy = bc[b * 4 + 1] - ly; if (dx * dx + dy * dy < 220 * 220) nearB = true; }
                if (!nearB) continue;
                bool crowded = false;
                for (int k = 0; k < cnt && !crowded; k++) { float dx = out[k * 3] - lx, dy = out[k * 3 + 1] - ly; if (dx * dx + dy * dy < 52 * 52) crowded = true; }
                if (crowded) continue;
                float hx = (ex / n - 0.5f) * tileM - lx, hy = (0.5f - ey / n) * tileM - ly;   /* nose towards the apron edge */
                out[cnt * 3] = lx; out[cnt * 3 + 1] = ly; out[cnt * 3 + 2] = atan2f(hx, hy);
                cnt++;
            }
    }
    return cnt;
}

/* ------------------------------------------------------------------ the tile */
VecTile *mvt_decode(const uint8_t *data, size_t len, int z, int x, int y) {
    VecTile *vt = calloc(1, sizeof *vt);
    int n = z >= 12 ? 512 : 256;
    vt->n = n;
    Raster R = {n, {calloc(n * n, 1), calloc(n * n, 1), malloc(n * n), calloc(n * n, 1)}};
    memset(R.ch[2], 255, n * n);
    double latc = merc_lat(y + 0.5, z);
    float tileM = (float)(2 * M_PI * EARTH_R * cos(latc * M_PI / 180) / (1 << z));
    float mPerPx = tileM / n;
    BBuf B = {0};
    Geom g = {0};
    float *lights = NULL; int nl = 0, capL = 0;
    Poly aprons[48]; int napr = 0;
    PB tile = {data, data + len};
    while (tile.p < tile.e) {
        int wt, f = key(&tile, &wt);
        if (f != 3 || wt != 2) { skip(&tile, wt); continue; }
        PB L = sub(&tile);
        /* first pass over the layer: name, keys, values, extent */
        Str name = {0}; Str keys[256]; int nk = 0; Val *vals = NULL; int nv = 0, capV = 0; uint32_t extent = 4096;
        PB scan = L;
        while (scan.p < scan.e) {
            int w2, f2 = key(&scan, &w2);
            if (f2 == 1 && w2 == 2) { PB s = sub(&scan); name.s = (const char *)s.p; name.n = (int)(s.e - s.p); }
            else if (f2 == 3 && w2 == 2) { PB s = sub(&scan); if (nk < 256) { keys[nk].s = (const char *)s.p; keys[nk].n = (int)(s.e - s.p); nk++; } }
            else if (f2 == 4 && w2 == 2) {
                PB s = sub(&scan); Val v = {0};
                while (s.p < s.e) {
                    int w3, f3 = key(&s, &w3);
                    if (f3 == 1 && w3 == 2) { PB q = sub(&s); v.isStr = 1; v.str = (const char *)q.p; v.slen = (int)(q.e - q.p); }
                    else if (f3 == 2 && w3 == 5) { float fv; memcpy(&fv, s.p, 4); s.p += 4; v.num = fv; }
                    else if (f3 == 3 && w3 == 1) { double dv; memcpy(&dv, s.p, 8); s.p += 8; v.num = dv; }
                    else if (f3 == 4 && w3 == 0) v.num = (double)(int64_t)varint(&s);
                    else if (f3 == 5 && w3 == 0) v.num = (double)varint(&s);
                    else if (f3 == 6 && w3 == 0) v.num = (double)zz(varint(&s));
                    else if (f3 == 7 && w3 == 0) v.num = (double)varint(&s);
                    else skip(&s, w3);
                }
                if (nv == capV) { capV = capV ? capV * 2 : 64; vals = realloc(vals, sizeof(Val) * capV); }
                vals[nv++] = v;
            } else if (f2 == 5 && w2 == 0) extent = (uint32_t)varint(&scan);
            else skip(&scan, w2);
        }
        int kind = str_is(name, "water") ? 1 : str_is(name, "aeroway") ? 2 : str_is(name, "transportation") ? 3 : str_is(name, "building") ? 4 : str_is(name, "waterway") ? 5 : 0;
        if (!kind || (kind == 4 && z < 14)) { free(vals); continue; }
        float scale = (float)n / (float)(extent ? extent : 4096), scaleT = 1.0f / (float)(extent ? extent : 4096);
        /* second pass: the features */
        while (L.p < L.e) {
            int w2, f2 = key(&L, &w2);
            if (f2 != 2 || w2 != 2) { skip(&L, w2); continue; }
            PB F = sub(&L);
            int type = 0; PB geom = {0}, tags = {0};
            while (F.p < F.e) {
                int w3, f3 = key(&F, &w3);
                if (f3 == 3 && w3 == 0) type = (int)varint(&F);
                else if (f3 == 4 && w3 == 2) geom = sub(&F);
                else if (f3 == 2 && w3 == 2) tags = sub(&F);
                else skip(&F, w3);
            }
            if (!geom.p) continue;
            const Val *cls = NULL, *brunnel = NULL, *rh = NULL, *rmh = NULL, *hide = NULL;
            while (tags.p < tags.e) {
                uint32_t ki = (uint32_t)varint(&tags), vi = (uint32_t)varint(&tags);
                if ((int)ki >= nk || (int)vi >= nv) continue;
                Str k = keys[ki];
                if (str_is(k, "class")) cls = &vals[vi];
                else if (str_is(k, "brunnel")) brunnel = &vals[vi];
                else if (str_is(k, "render_height")) rh = &vals[vi];
                else if (str_is(k, "render_min_height")) rmh = &vals[vi];
                else if (str_is(k, "hide_3d")) hide = &vals[vi];
            }
            bool tunnel = val_is(brunnel, "tunnel");
            if (kind == 4) {
                if (type != 3 || (hide && hide->num != 0 && !hide->isStr)) continue;
                decode_geom(geom, &g, scaleT);
                building(&B, &g, tileM, rh ? (float)rh->num : 8.0f, rmh ? (float)rmh->num : 0.0f);
                continue;
            }
            decode_geom(geom, &g, scale);
            if (kind == 1) { if (type == 3 && !tunnel) fill_poly(&R, 0, &g, 1); }
            else if (kind == 5) {
                if (type == 2 && !tunnel) {
                    float w = val_is(cls, "river") ? 22 : val_is(cls, "canal") ? 12 : 0;
                    if (w > 0) stroke(&R, 0, &g, w * 0.5f / mPerPx, 1, -1, mPerPx);
                }
            } else if (kind == 2) {
                if (type == 3 && (val_is(cls, "apron") || val_is(cls, "taxiway") || val_is(cls, "runway") || val_is(cls, "helipad"))) {
                    fill_poly(&R, 1, &g, 1);
                    if (z >= 14 && val_is(cls, "apron") && napr < 48 && g.n > 3) {
                        Poly *ap = &aprons[napr++];
                        ap->n = g.n; ap->nring = g.nring;
                        ap->p = malloc(sizeof(P2) * g.n); memcpy(ap->p, g.p, sizeof(P2) * g.n);
                        ap->ring = malloc(sizeof(int) * g.nring); memcpy(ap->ring, g.ring, sizeof(int) * g.nring);
                    }
                }
                else if (type == 2 && val_is(cls, "taxiway")) {
                    stroke(&R, 1, &g, 11.5f / mPerPx, 1, 2, mPerPx);
                    /* the centreline as points every 10 m (tile-local metres); a 1e9 x marks the end of a line */
                    for (int r = 0; r < g.nring; r++) {
                        int s = g.ring[r], e = ring_end(&g, r);
                        float carry = 0; bool open = false;
                        for (int i = s; i + 1 < e; i++) {
                            float ax = g.p[i].x / n, ay = g.p[i].y / n, bx2 = g.p[i + 1].x / n, by2 = g.p[i + 1].y / n;
                            float segM = sqrtf((bx2 - ax) * (bx2 - ax) + (by2 - ay) * (by2 - ay)) * tileM;
                            for (float d = carry; d < segM; d += 10) {
                                float t = d / fmaxf(segM, 1e-3f), fx = ax + (bx2 - ax) * t, fy = ay + (by2 - ay) * t;
                                if (fx < 0 || fx > 1 || fy < 0 || fy > 1) {      /* only the part inside this tile */
                                    if (open) { if (nl + 2 > capL) { capL = capL ? capL * 2 : 512; lights = realloc(lights, sizeof(float) * capL); } lights[nl++] = 1e9f; lights[nl++] = 0; }
                                    open = false; continue;
                                }
                                if (nl + 4 > capL) { capL = capL ? capL * 2 : 512; lights = realloc(lights, sizeof(float) * capL); }
                                lights[nl++] = (fx - 0.5f) * tileM; lights[nl++] = (0.5f - fy) * tileM;
                                open = true;
                            }
                            carry = fmodf(carry - segM, 10); if (carry < 0) carry += 10;
                        }
                        if (open) { if (nl + 2 > capL) { capL = capL ? capL * 2 : 512; lights = realloc(lights, sizeof(float) * capL); } lights[nl++] = 1e9f; lights[nl++] = 0; }
                    }
                } else if (type == 2 && val_is(cls, "runway")) stroke(&R, 1, &g, 22 / mPerPx, 1, -1, mPerPx);
            } else if (kind == 3 && type == 2 && !tunnel) {
                float w = val_is(cls, "motorway") ? 22 : val_is(cls, "trunk") ? 18 : val_is(cls, "primary") ? 14 : val_is(cls, "secondary") ? 11
                        : val_is(cls, "tertiary") ? 9 : val_is(cls, "minor") ? 7 : val_is(cls, "service") ? 4.5f : 0;
                if (w > 0) stroke(&R, 3, &g, w * 0.5f / mPerPx, val_is(cls, "motorway") || val_is(cls, "trunk") ? 1.0f : 0.75f, -1, mPerPx);
            }
        }
        free(vals);
    }
    free(g.p); free(g.ring);
    if (napr) {
        vt->park = malloc(sizeof(float) * 3 * 64);
        vt->npark = find_stands(aprons, napr, &R, mPerPx, tileM, B.c, B.nb, vt->park, 64);
        /* buildings on the edge of an apron are terminals and hangars: give them glass fronts */
        for (int bi = 0; bi < B.nb; bi++) {
            float *c = B.c + bi * 4, px = (c[0] / tileM + 0.5f) * n, py = (0.5f - c[1] / tileM) * n, ex, ey;
            bool term = false;
            for (int a2 = 0; a2 < napr && !term; a2++) if (edge_dist(&aprons[a2], px, py, &ex, &ey) * mPerPx < 140) term = true;
            if (term) for (int k = (int)c[2]; k < (int)(c[2] + c[3]); k++) B.v[k * 10 + 9] = 0.97f;
        }
        for (int a = 0; a < napr; a++) { free(aprons[a].p); free(aprons[a].ring); }
    }
    /* interleave into RGBA, and keep a 128x128 water bitmap for the physics */
    vt->mask = malloc((size_t)n * n * 4);
    for (int i = 0; i < n * n; i++) { vt->mask[i * 4] = R.ch[0][i]; vt->mask[i * 4 + 1] = R.ch[1][i]; vt->mask[i * 4 + 2] = R.ch[2][i]; vt->mask[i * 4 + 3] = R.ch[3][i]; }
    vt->nw = 128;
    vt->water = malloc(128 * 128);
    int st = n / 128;
    for (int yy = 0; yy < 128; yy++)
        for (int xx = 0; xx < 128; xx++) {
            int s = 0;
            for (int a = 0; a < st; a++) for (int b2 = 0; b2 < st; b2++) s += R.ch[0][(yy * st + a) * n + xx * st + b2];
            vt->water[yy * 128 + xx] = (uint8_t)(s / (st * st));
        }
    for (int c = 0; c < 4; c++) free(R.ch[c]);
    vt->bv = B.v; vt->nbv = B.n / 10; vt->bc = B.c; vt->nb = B.nb;
    vt->lights = lights; vt->nl = nl / 2;
    return vt;
}
void vectile_free(VecTile *v) {
    if (!v) return;
    free(v->mask); free(v->water); free(v->bv); free(v->bc); free(v->lights); free(v->park); free(v);
}
