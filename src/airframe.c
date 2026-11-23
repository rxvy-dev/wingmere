/* WINGMERE - airframes for live traffic, built from each type's real dimensions, in its airline's colours.
 * MIT licence.
 *
 * data/types.txt gives length, span, fuselage width, engine layout, wing and tail arrangement for ~150 ICAO
 * types; from those this file lofts a fuselage, lays out swept wings and tail surfaces, hangs engines or
 * propellers, adds winglets and landing gear.  data/liveries.txt gives each airline's colour scheme (colours
 * only: no logos or titles), painted onto a texture with cabin windows spaced for that type's length.
 *
 * Model space matches the hangar models: +z forward, +y up, +x to the left, origin at the centre of gravity. */
#include "wm.h"

/* ------------------------------------------------------------------ tables */
typedef struct {
    char icao[6], name[40], eng[4];
    float len, span, diam;
    bool high, ttail, hump, ddeck; int winglet;
    GLuint vao, vbo, ibo, gvao, gvbo, gibo; int ni, gni; float ground;   /* ground: model y of the wheels' bottom */
} AType;
typedef struct { char code[8], alias[8]; float body[3], belly[3], cheat[3], tail[3], tail2[3], tail3[3], engine[3], nose[3]; float beltline, nosePart; int style; bool hasCheat, rear, hasNose; } Livery;
static AType types[256]; static int ntypes;
static Livery livs[160]; static int nlivs;

static void hexcol(const char *h, float *c) {
    unsigned v = (unsigned)strtoul(h, NULL, 16);
    for (int k = 0; k < 3; k++) { float x = ((v >> (16 - 8 * k)) & 255) / 255.0f; c[k] = x; }
}
void airframe_init(const char *datadir) {
    char path[600], line[400];
    snprintf(path, sizeof path, "%s/types.txt", datadir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f) && ntypes < 256) {
            if (line[0] == '#' || line[0] == '\n') continue;
            AType *t = &types[ntypes]; memset(t, 0, sizeof *t);
            char wing[4], tail[4]; int n = 0;
            if (sscanf(line, "%5s %f %f %f %3s %3s %3s %d %n", t->icao, &t->len, &t->span, &t->diam, t->eng, wing, tail, &t->winglet, &n) < 8) continue;
            char *nm = line + n; nm[strcspn(nm, "\n")] = 0;
            size_t L = strlen(nm);
            if (L > 2 && nm[L - 2] == ' ' && (nm[L - 1] == 'h' || nm[L - 1] == 'd')) { t->hump = nm[L - 1] == 'h'; t->ddeck = nm[L - 1] == 'd'; nm[L - 2] = 0; }
            while (*nm == ' ') nm++;
            for (char *e = nm + strlen(nm); e > nm && e[-1] == ' '; ) *--e = 0;
            snprintf(t->name, sizeof t->name, "%s", nm);
            t->high = wing[0] == 'H'; t->ttail = tail[0] == 'T';
            ntypes++;
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/liveries.txt", datadir);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f) && nlivs < 160) {
            if (line[0] == '#' || line[0] == '\n') continue;
            Livery *l = &livs[nlivs]; memset(l, 0, sizeof *l);
            char b[12][12];
            int n = sscanf(line, "%7s %11s %11s %11s %11s %11s %11s %11s %11s %11s %11s %11s %11s", l->code, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
            if (n >= 2 && b[0][0] == '=') { snprintf(l->alias, sizeof l->alias, "%s", b[0] + 1); nlivs++; continue; }
            if (n < 13) continue;
            hexcol(b[0], l->body); hexcol(b[1], l->belly); l->beltline = (float)atof(b[2]);
            l->hasCheat = b[3][0] != '-'; if (l->hasCheat) hexcol(b[3], l->cheat);
            hexcol(b[4], l->tail); hexcol(b[5], l->tail2); hexcol(b[6], l->tail3); l->style = atoi(b[7]);
            hexcol(b[8], l->engine); l->rear = atoi(b[9]) != 0;
            l->hasNose = b[10][0] != '-'; if (l->hasNose) hexcol(b[10], l->nose); l->nosePart = (float)atof(b[11]);
            nlivs++;
        }
        fclose(f);
    }
    fprintf(stderr, "wingmere: %d airframe types, %d airline colour schemes\n", ntypes, nlivs);
}
int airframe_find(const char *icao) {
    if (!icao || !icao[0]) return -1;
    for (int i = 0; i < ntypes; i++) if (!strcasecmp(types[i].icao, icao)) return i;
    return -1;
}
const char *airframe_name(int t) { return t >= 0 && t < ntypes ? types[t].name : ""; }
float airframe_length(int t) { return t >= 0 && t < ntypes ? types[t].len : 30; }
float airframe_span(int t) { return t >= 0 && t < ntypes ? types[t].span : 30; }
static int livery_index(const char *code) {
    for (int depth = 0; depth < 3; depth++) {
        int k = -1;
        for (int i = 0; i < nlivs; i++) if (!strcasecmp(livs[i].code, code)) { k = i; break; }
        if (k < 0) return -1;
        if (!livs[k].alias[0]) return k;
        code = livs[k].alias;
    }
    return -1;
}
/* the airline is the three letters at the front of an airline callsign (UAL123, BAW9) */
int airframe_livery(const char *callsign, bool mil) {
    if (mil) { int m = livery_index("MIL"); if (m >= 0) return m; }
    if (callsign && strlen(callsign) >= 4 && isalpha((unsigned char)callsign[0]) && isalpha((unsigned char)callsign[1]) && isalpha((unsigned char)callsign[2]) && isdigit((unsigned char)callsign[3])) {
        char c[4] = {(char)toupper((unsigned char)callsign[0]), (char)toupper((unsigned char)callsign[1]), (char)toupper((unsigned char)callsign[2]), 0};
        int k = livery_index(c);
        if (k >= 0) return k;
    }
    return livery_index("DEFAULT");
}

/* ------------------------------------------------------------------ mesh building */
typedef struct { float *v; int nv, cv; uint32_t *ix; int ni, ci; } MB;
static int mb_v(MB *m, v3 p, v3 n, float u, float vv) {
    if (m->nv == m->cv) { m->cv = m->cv ? m->cv * 2 : 4096; m->v = realloc(m->v, sizeof(float) * 8 * m->cv); }
    float *o = &m->v[m->nv * 8];
    o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = n.x; o[4] = n.y; o[5] = n.z; o[6] = u; o[7] = vv;
    return m->nv++;
}
static void mb_t(MB *m, int a, int b, int c) {
    if (m->ni + 3 > m->ci) { m->ci = m->ci ? m->ci * 2 : 8192; m->ix = realloc(m->ix, sizeof(uint32_t) * m->ci); }
    m->ix[m->ni++] = a; m->ix[m->ni++] = b; m->ix[m->ni++] = c;
}
static void mb_q(MB *m, int a, int b, int c, int d) { mb_t(m, a, b, c); mb_t(m, a, c, d); }
/* smooth normals for everything added since (v0, i0) */
static void mb_smooth(MB *m, int v0, int i0) {
    for (int i = v0; i < m->nv; i++) { float *o = &m->v[i * 8]; o[3] = o[4] = o[5] = 0; }
    for (int i = i0; i < m->ni; i += 3) {
        float *a = &m->v[m->ix[i] * 8], *b = &m->v[m->ix[i + 1] * 8], *c = &m->v[m->ix[i + 2] * 8];
        v3 e1 = V(b[0] - a[0], b[1] - a[1], b[2] - a[2]), e2 = V(c[0] - a[0], c[1] - a[1], c[2] - a[2]), n = vcross(e1, e2);
        for (int k = 0; k < 3; k++) { float *o = &m->v[m->ix[i + k] * 8]; o[3] += n.x; o[4] += n.y; o[5] += n.z; }
    }
    for (int i = v0; i < m->nv; i++) { float *o = &m->v[i * 8]; v3 n = V(o[3], o[4], o[5]); float l = vlen(n); if (l > 1e-9f) { o[3] /= l; o[4] /= l; o[5] /= l; } }
}
static void mb_upload(MB *m, GLuint *vao, GLuint *vbo, GLuint *ibo, int *ni) {
    glGenVertexArrays(1, vao); glBindVertexArray(*vao);
    glGenBuffers(1, vbo); glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8 * m->nv, m->v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    glGenBuffers(1, ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint32_t) * m->ni, m->ix, GL_STATIC_DRAW);
    glBindVertexArray(0);
    *ni = m->ni;
    free(m->v); free(m->ix); memset(m, 0, sizeof *m);
}
/* texture atlas: fuselage | fin | engine | wing */
#define U_FUS(t) ((t) * 0.745f + 0.002f)
#define U_FIN(c) (0.752f + (c) * 0.12f)
#define U_ENG(a) (0.879f + (a) * 0.054f)
#define U_WING 0.97f
/* a flat-shaded box (gear legs, pylons, propeller blades) */
static void mb_box(MB *m, v3 c, v3 ax, v3 ay, v3 az, float u, float vv) {
    static const int F[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    v3 p[8];
    for (int i = 0; i < 8; i++) p[i] = vadd(vadd(vadd(c, vmul(ax, i & 1 ? 1 : -1)), vmul(ay, i & 2 ? 1 : -1)), vmul(az, i & 4 ? 1 : -1));
    for (int f = 0; f < 6; f++) {
        v3 n = vnorm(vcross(vsub(p[F[f][1]], p[F[f][0]]), vsub(p[F[f][2]], p[F[f][0]])));
        int a = mb_v(m, p[F[f][0]], n, u, vv), b = mb_v(m, p[F[f][1]], n, u, vv), cc = mb_v(m, p[F[f][2]], n, u, vv), d = mb_v(m, p[F[f][3]], n, u, vv);
        mb_q(m, a, b, cc, d);
    }
}
/* a body of revolution around an axis: profile = (distance along the axis, radius) pairs */
static void mb_lathe(MB *m, v3 o, v3 ax, v3 side, const float *prof, int np, int seg, float u0, float u1) {
    int v0 = m->nv, i0 = m->ni;
    v3 up = vcross(ax, side);
    float total = prof[(np - 1) * 2] - prof[0];
    for (int i = 0; i < np; i++)
        for (int s = 0; s <= seg; s++) {
            float a = 2 * PI * s / seg, r = prof[i * 2 + 1];
            v3 p = vadd(vadd(o, vmul(ax, prof[i * 2])), vadd(vmul(side, cosf(a) * r), vmul(up, sinf(a) * r)));
            mb_v(m, p, V(0, 1, 0), u0 + (u1 - u0) * (prof[i * 2] - prof[0]) / fmaxf(total, 1e-3f), (float)s / seg);
        }
    for (int i = 0; i < np - 1; i++)
        for (int s = 0; s < seg; s++) {
            int a = v0 + i * (seg + 1) + s, b = a + 1, c = a + seg + 1, d = c + 1;
            mb_q(m, a, c, d, b);
        }
    mb_smooth(m, v0, i0);
}
/* a lifting surface (wing, tailplane, fin, winglet): airfoil sections from root to tip */
static void mb_surface(MB *m, v3 rootLE, float cr, v3 tipLE, float ct, float tr, float tt, v3 thick, int region) {
    int v0 = m->nv, i0 = m->ni, NS = 5, NC = 9;
    v3 back = V(0, 0, -1);
    for (int s = 0; s <= NS; s++) {
        float k = (float)s / NS, ch = cr + (ct - cr) * k, th = tr + (tt - tr) * k;
        v3 le = vlerp(rootLE, tipLE, k);
        for (int side = 0; side < 2; side++)
            for (int c = 0; c <= NC; c++) {
                float x = (float)c / NC; x = x * x * (3 - 2 * x) * 0.6f + x * 0.4f;     /* bunch points at the nose and tail */
                float yt = 5 * th * (0.2969f * sqrtf(x) - 0.126f * x - 0.3516f * x * x + 0.2843f * x * x * x - 0.1036f * x * x * x * x);
                v3 p = vadd(vadd(le, vmul(back, x * ch)), vmul(thick, side ? -yt * ch : yt * ch));
                float u = region == 0 ? U_WING : region == 1 ? U_FIN(x) : U_FIN(0.5f);
                float vv = region == 1 ? 0.02f + 0.96f * k : region == 2 ? 0.55f : 0.5f;
                mb_v(m, p, V(0, 1, 0), u, vv);
            }
    }
    int row = 2 * (NC + 1);
    for (int s = 0; s < NS; s++)
        for (int side = 0; side < 2; side++)
            for (int c = 0; c < NC; c++) {
                int a = v0 + s * row + side * (NC + 1) + c, b = a + 1, cc = a + row, d = cc + 1;
                if (side) mb_q(m, a, b, d, cc); else mb_q(m, a, cc, d, b);
            }
    /* tip cap */
    int t0 = v0 + NS * row;
    for (int c = 0; c < NC; c++) mb_q(m, t0 + c, t0 + c + 1, t0 + NC + 1 + c + 1, t0 + NC + 1 + c);
    mb_smooth(m, v0, i0);
}

/* ------------------------------------------------------------------ the airframe */
typedef struct { float zNose, L, R, ry; bool light, rearEng; } Fus;
static void fus_at(const AType *T, const Fus *F, float t, float *rs, float *cy, float *top) {
    float tn = F->light ? 0.13f : 0.085f, ts = F->light ? 0.36f : (F->rearEng ? 0.64f : 0.70f);
    if (T->eng[0] == 'P' && T->eng[1] == '1') tn = 0.16f;
    if (t < tn) {
        float s = t / tn;
        *rs = sqrtf(fmaxf(1 - (1 - s) * (1 - s), 0)) * (T->eng[1] == '1' && T->eng[0] == 'P' ? 0.75f + 0.25f * s : 1) + (T->eng[1] == '1' && T->eng[0] == 'P' ? 0.2f * (1 - s) : 0);
        *cy = -0.2f * F->R * (1 - s) * (1 - s);
    } else if (t < ts) { *rs = 1; *cy = 0; }
    else {
        float s = (t - ts) / (1 - ts);
        *rs = 1 - (F->light ? 0.82f : 0.86f) * powf(s, 1.25f);
        *cy = (F->light ? 0.3f : 0.55f) * F->R * powf(s, 1.4f);
    }
    float hump = 0;
    if (T->hump) hump = smooth(0.04f, 0.11f, t) * (1 - smooth(0.28f, 0.42f, t)) * 0.36f;
    *top = 1 + hump;
}
static float fus_z(const Fus *F, float t) { return F->zNose - t * F->L; }

static void build(AType *T) {
    MB m = {0}, g = {0};
    Fus F;
    F.L = T->len; F.R = T->diam * 0.5f; F.ry = F.R * (T->ddeck ? 1.2f : 1.0f);
    F.light = T->len < 15; F.rearEng = T->eng[0] == 'R';
    F.zNose = F.L * (F.light ? 0.31f : (F.rearEng ? 0.55f : 0.43f));
    float b = T->span, bs = b * 0.5f, R = F.R;
    bool prop = T->eng[0] == 'P', wide = T->diam > 4.5f;
    /* ---- fuselage */
    {
        static const float ts[] = {0, .004f, .01f, .02f, .032f, .046f, .062f, .08f, .1f, .13f, .17f, .22f, .28f, .34f, .4f, .46f, .52f, .58f, .64f, .68f, .72f, .76f, .8f, .84f, .88f, .91f, .94f, .965f, .985f, 1};
        int NT = sizeof ts / sizeof ts[0], SEG = 40, v0 = m.nv;
        for (int i = 0; i < NT; i++) {
            float t = ts[i], rs, cy, top; fus_at(T, &F, t, &rs, &cy, &top);
            if (i == 0) rs = fmaxf(rs, 0.02f);
            if (i == NT - 1) rs = fmaxf(rs, 0.1f);
            for (int s = 0; s <= SEG; s++) {
                float th = 2 * PI * s / SEG, cs = cosf(th), sn = sinf(th);
                float ryy = F.ry * rs * (cs < 0 ? top : 1);
                v3 p = V(R * rs * sn, cy - ryy * cs, fus_z(&F, t));
                v3 n = vnorm(V(sn / R, -cs / F.ry, 0));
                mb_v(&m, p, n, U_FUS(t), (float)s / SEG);
            }
        }
        int i0 = m.ni;
        for (int i = 0; i < NT - 1; i++)
            for (int s = 0; s < SEG; s++) { int a = v0 + i * (SEG + 1) + s; mb_q(&m, a, a + 1, a + SEG + 2, a + SEG + 1); }
        mb_smooth(&m, v0, i0);
        /* tail end cap */
        int last = v0 + (NT - 1) * (SEG + 1);
        float rs, cy, top; fus_at(T, &F, 1, &rs, &cy, &top);
        int c = mb_v(&m, V(0, cy, fus_z(&F, 1) - 0.05f), V(0, 0, -1), U_FUS(0.999f), 0.5f);
        for (int s = 0; s < SEG; s++) mb_t(&m, last + s, c, last + s + 1);
    }
    /* ---- wing */
    float lam = F.light ? 0.65f : (prop ? 0.55f : 0.28f), AR = F.light ? 7.4f : (prop ? 12.0f : (wide ? 9.6f : 9.3f));
    float cr = 2 * b / (AR * (1 + lam)), ct = cr * lam;
    float sweep = (F.light || prop) ? 2.5f : (F.rearEng && T->len < 35 ? 28 : (wide ? 33 : 27));
    float dih = T->high ? 0.5f : (F.light ? 5.0f : 5.5f);
    float xr = R * 0.82f, yw = T->high ? F.ry * 0.82f : -F.ry * 0.55f;
    float tanS = tanf(sweep * DEG), yM = (bs - xr) * (1 + 2 * lam) / (3 * (1 + lam)), mac = 2.0f / 3 * cr * (1 + lam + lam * lam) / (1 + lam);
    float zr = tanS * yM + 0.25f * mac;
    #define WING_Z(x) (zr - tanS * ((x) - xr))
    #define WING_Y(x) (yw + ((x) - xr) * tanf(dih * DEG))
    for (int sd = -1; sd <= 1; sd += 2) {
        v3 rootLE = V(sd * xr, yw, zr), tipLE = V(sd * bs, WING_Y(bs), WING_Z(bs));
        mb_surface(&m, rootLE, cr, tipLE, ct, 0.135f, 0.10f, V(0, 1, 0), 0);
        if (T->winglet == 1 || T->winglet == 2) {
            float h = b * (T->winglet == 2 ? 0.065f : 0.05f);
            v3 wl = vadd(tipLE, V(0, 0, -ct * 0.15f)), wt = vadd(wl, V(sd * h * 0.22f, h, -h * 0.75f));
            mb_surface(&m, wl, ct * 0.85f, wt, ct * 0.3f, 0.09f, 0.08f, V(sd * 1.0f, 0, 0), 2);
        } else if (T->winglet == 3) {
            /* raked tip: a sharply swept extension */
            v3 rt = vadd(tipLE, V(sd * b * 0.035f, b * 0.004f, -b * 0.05f));
            mb_surface(&m, tipLE, ct, rt, ct * 0.25f, 0.10f, 0.07f, V(0, 1, 0), 0);
        }
    }
    /* ---- tail */
    float hv = b * (T->ttail ? 0.165f : (F.light ? 0.13f : (wide ? 0.145f : 0.16f)));
    float cv = cr * (F.light ? 1.0f : (prop ? 1.1f : 0.95f)), ctv = cv * (T->ttail ? 0.72f : 0.38f);
    float sv = F.light ? 25 : (prop ? 30 : 38);
    float rsF, cyF, topF; fus_at(T, &F, 0.86f, &rsF, &cyF, &topF);
    float finY = cyF + F.ry * rsF * 0.92f, finZ = fus_z(&F, 0.985f) + cv;
    v3 finRoot = V(0, finY, finZ), finTip = V(0, finY + hv, finZ - tanf(sv * DEG) * hv);
    mb_surface(&m, finRoot, cv, finTip, ctv, 0.11f, 0.10f, V(1, 0, 0), 1);
    float bh = b * (F.light ? 0.34f : (prop ? 0.33f : 0.35f)) * 0.5f, crh = cr * (F.light ? 0.62f : 0.48f), cth = crh * (F.light ? 0.7f : 0.38f), sh = (F.light || prop) ? 6 : sweep + 4;
    v3 stabRoot;
    if (T->ttail) stabRoot = vadd(finTip, V(0, -0.05f * hv, ctv * 0.35f));
    else { float rs, cy, top; fus_at(T, &F, 0.9f, &rs, &cy, &top); stabRoot = V(0, cy + F.ry * rs * 0.15f, fus_z(&F, 0.975f) + crh); }
    for (int sd = -1; sd <= 1; sd += 2) {
        v3 tip = vadd(stabRoot, V(sd * bh, bh * tanf(5 * DEG), -tanf(sh * DEG) * bh));
        mb_surface(&m, vadd(stabRoot, V(sd * R * 0.12f, 0, 0)), crh, tip, cth, 0.11f, 0.09f, V(0, 1, 0), T->ttail ? 0 : 0);
    }
    /* ---- engines */
    float engBottom = 0;
    int ne = T->eng[1] - '0';
    if (T->eng[0] == 'W' || T->eng[0] == 'T') {
        int nw = T->eng[0] == 'T' ? 2 : ne;
        float dn = clampf(b * (nw == 4 ? (wide ? 0.042f : 0.05f) : (wide ? 0.055f : 0.055f)), 1.0f, 3.9f), ln = dn * (wide ? 1.85f : 2.15f);
        if (T->high) dn *= 0.85f;
        for (int k = 0; k < nw; k++) {
            float eta = nw == 4 ? (k < 2 ? 0.36f : 0.66f) : 0.34f;
            if (!strcmp(T->icao, "A388")) eta = k < 2 ? 0.33f : 0.62f;
            float sd = k % 2 ? 1 : -1, x = sd * (xr + (bs - xr) * eta), ax = fabsf(x);
            float zl = WING_Z(ax), wy = WING_Y(ax);
            float ey = T->high ? wy - 0.55f * dn : wy - 0.62f * dn - 0.12f, ez = zl + ln * 0.62f;
            float prof[] = {0, 0.43f * dn, 0.03f * ln, 0.5f * dn, 0.4f * ln, 0.5f * dn, 0.78f * ln, 0.42f * dn, 0.95f * ln, 0.3f * dn, 1.0f * ln, 0.22f * dn, 1.12f * ln, 0.04f * dn};
            mb_lathe(&m, V(x, ey, ez), V(0, 0, -1), V(1, 0, 0), prof, 7, 24, U_ENG(0.08f), U_ENG(1));
            float face[] = {0.035f * ln, 0.0f, 0.036f * ln, 0.43f * dn};
            mb_lathe(&m, V(x, ey, ez), V(0, 0, -1), V(1, 0, 0), face, 2, 24, U_ENG(0.0f), U_ENG(0.02f));
            /* pylon */
            mb_box(&m, V(x, (ey + 0.45f * dn + wy) * 0.5f, zl - ln * 0.05f), V(0.06f * dn, 0, 0), V(0, fmaxf((wy - ey - 0.45f * dn) * 0.5f, 0.1f), 0), V(0, 0, ln * 0.42f), U_WING, 0.5f);
            engBottom = fminf(engBottom, ey - 0.5f * dn);
        }
    }
    if (T->eng[0] == 'R' || T->eng[0] == 'T') {
        float dn = clampf(b * (T->eng[0] == 'T' ? 0.052f : (T->len < 25 ? 0.062f : 0.057f)), 0.6f, 3.0f), ln = dn * 2.2f;
        float te = T->eng[0] == 'T' ? 0.86f : 0.79f, rs, cy, top; fus_at(T, &F, te, &rs, &cy, &top);
        float ez = fus_z(&F, te) + ln * 0.5f;
        int nr = T->eng[0] == 'T' ? 0 : (ne >= 2 ? 2 : 0);
        for (int k = 0; k < nr; k++) {
            float sd = k ? 1 : -1, x = sd * (F.R * rs + 0.62f * dn), ey = cy + F.ry * rs * 0.32f;
            float prof[] = {0, 0.43f * dn, 0.04f * ln, 0.5f * dn, 0.6f * ln, 0.48f * dn, 0.92f * ln, 0.36f * dn, 1.0f * ln, 0.28f * dn, 1.1f * ln, 0.05f * dn};
            mb_lathe(&m, V(x, ey, ez), V(0, 0, -1), V(1, 0, 0), prof, 6, 20, U_ENG(0.08f), U_ENG(1));
            float face[] = {0.04f * ln, 0.0f, 0.041f * ln, 0.43f * dn};
            mb_lathe(&m, V(x, ey, ez), V(0, 0, -1), V(1, 0, 0), face, 2, 20, U_ENG(0.0f), U_ENG(0.02f));
            mb_box(&m, V(sd * (F.R * rs + 0.1f * dn) * 0.75f + sd * 0.1f, ey, ez - ln * 0.45f), V(0.55f * dn, 0, 0), V(0, 0.07f * dn, 0), V(0, 0, ln * 0.28f), U_WING, 0.5f);
        }
        if (ne == 3 || T->eng[0] == 'T') {
            /* the middle engine, on top of the rear fuselage / in the base of the fin */
            float ey = cy + F.ry * rs + 0.42f * dn;
            float prof[] = {0, 0.43f * dn, 0.04f * ln, 0.5f * dn, 0.7f * ln, 0.5f * dn, 1.0f * ln, 0.32f * dn, 1.08f * ln, 0.05f * dn};
            mb_lathe(&m, V(0, ey, ez), V(0, 0, -1), V(1, 0, 0), prof, 5, 20, U_ENG(0.08f), U_ENG(1));
            float face[] = {0.04f * ln, 0.0f, 0.041f * ln, 0.43f * dn};
            mb_lathe(&m, V(0, ey, ez), V(0, 0, -1), V(1, 0, 0), face, 2, 20, U_ENG(0.0f), U_ENG(0.02f));
        }
    }
    if (prop) {
        int np = ne;
        float dp = b * (np == 1 ? 0.17f : 0.14f);
        int blades = np == 1 ? (T->len < 10 ? 2 : 3) : (np == 4 ? 4 : 6);
        for (int k = 0; k < np; k++) {
            v3 hub; float dn;
            if (np == 1) { float rs, cy, top; fus_at(T, &F, 0, &rs, &cy, &top); hub = V(0, cy, F.zNose + 0.05f); dn = R; }
            else {
                float eta = np == 4 ? (k < 2 ? 0.27f : 0.55f) : 0.31f, sd = k % 2 ? 1 : -1, x = sd * (xr + (bs - xr) * eta), ax = fabsf(x);
                dn = b * (F.light ? 0.06f : 0.046f);
                float ln = T->len * (F.light ? 0.2f : 0.3f), zl = WING_Z(ax), wy = WING_Y(ax);
                float ey = T->high ? wy - 0.12f * dn : wy + 0.05f * dn, ez = zl + ln * 0.42f;
                float prof[] = {0, 0.25f * dn, 0.06f * ln, 0.45f * dn, 0.25f * ln, 0.5f * dn, 0.75f * ln, 0.45f * dn, 1.0f * ln, 0.15f * dn};
                mb_lathe(&m, V(x, ey, ez), V(0, 0, -1), V(1, 0, 0), prof, 5, 18, U_ENG(0.1f), U_ENG(1));
                hub = V(x, ey, ez + 0.02f);
            }
            float sp[] = {-0.35f * dn, 0.0f, -0.15f * dn, 0.22f * dn, 0.02f, 0.25f * dn};
            mb_lathe(&m, hub, V(0, 0, -1), V(1, 0, 0), sp, 3, 14, U_ENG(0.0f), U_ENG(0.02f));
            for (int bl = 0; bl < blades; bl++) {
                float a = 2 * PI * bl / blades + 0.3f;
                v3 dir = V(cosf(a), sinf(a), 0), wid = V(-sinf(a), cosf(a), 0);
                mb_box(&m, vadd(hub, vmul(dir, dp * 0.27f)), vmul(dir, dp * 0.23f), vmul(wid, dp * 0.035f), V(0, 0, dp * 0.01f), U_ENG(0.01f), 0.5f);
            }
        }
    }
    /* ---- landing gear (its own mesh: drawn only when it is down) */
    float rw = clampf(0.16f + T->len * 0.0075f, 0.22f, 0.66f);
    float yg = -F.ry - (F.light ? 0.5f : (T->high ? 0.42f * F.ry : 0.55f * F.ry));
    if (engBottom < 0) yg = fminf(yg, engBottom - 0.5f);
    if (F.light) yg = fminf(yg, -F.ry - 0.45f);
    float xm = F.light ? 0.115f * b : (T->high ? F.R * 0.95f : fmaxf(0.1f * b, F.R * 1.05f));
    if (wide) xm = fmaxf(0.088f * b, F.R * 1.0f);
    float zm = -0.03f * T->len - (F.light ? 0.1f : 0.5f), zn = F.zNose - (F.light ? 0.1f : 0.125f) * T->len;
    bool bogie = wide || T->len > 50;
    float topY = T->high ? -F.ry * 0.55f : (F.light && T->high ? -F.ry * 0.7f : -F.ry * 0.62f);
    for (int sd = -1; sd <= 1; sd += 2) {
        float x = sd * xm, cyw = yg + rw;
        float topLeg = T->high || F.light ? fminf(topY, -F.ry * 0.5f) : WING_Y(fabsf(x)) - 0.1f;
        mb_box(&g, V(x, (cyw + topLeg) * 0.5f, zm), V(rw * 0.16f, 0, 0), V(0, fmaxf((topLeg - cyw) * 0.5f, 0.1f), 0), V(0, 0, rw * 0.16f), U_WING, 0.5f);
        int nwheels = bogie ? 4 : (F.light ? 1 : 2);
        for (int wI = 0; wI < nwheels; wI++) {
            float wx = x + (nwheels >= 2 ? (wI % 2 ? 1 : -1) * rw * 0.62f : 0), wz = zm + (nwheels == 4 ? (wI < 2 ? 1 : -1) * rw * 1.2f : 0);
            float tp[] = {-rw * 0.28f, 0.0f, -rw * 0.27f, rw * 0.85f, -rw * 0.2f, rw, rw * 0.2f, rw, rw * 0.27f, rw * 0.85f, rw * 0.28f, 0.0f};
            mb_lathe(&g, V(wx, cyw, wz), V(1, 0, 0), V(0, 0, 1), tp, 6, 16, U_ENG(0.0f), U_ENG(0.02f));
        }
    }
    {
        float rn = rw * 0.75f, cyn = yg + rn, topN = -F.ry * 0.7f;
        mb_box(&g, V(0, (cyn + topN) * 0.5f, zn), V(rn * 0.15f, 0, 0), V(0, fmaxf((topN - cyn) * 0.5f, 0.1f), 0), V(0, 0, rn * 0.15f), U_WING, 0.5f);
        int nn = F.light ? 1 : 2;
        for (int wI = 0; wI < nn; wI++) {
            float wx = nn == 2 ? (wI ? 1 : -1) * rn * 0.55f : 0;
            float tp[] = {-rn * 0.25f, 0.0f, -rn * 0.24f, rn * 0.85f, -rn * 0.18f, rn, rn * 0.18f, rn, rn * 0.24f, rn * 0.85f, rn * 0.25f, 0.0f};
            mb_lathe(&g, V(wx, cyn, zn), V(1, 0, 0), V(0, 0, 1), tp, 6, 14, U_ENG(0.0f), U_ENG(0.02f));
        }
    }
    T->ground = yg;
    mb_upload(&m, &T->vao, &T->vbo, &T->ibo, &T->ni);
    mb_upload(&g, &T->gvao, &T->gvbo, &T->gibo, &T->gni);
}
float airframe_ground(int t) {
    if (t < 0 || t >= ntypes) return 0;
    if (!types[t].vao) build(&types[t]);
    return -types[t].ground;
}

/* ------------------------------------------------------------------ painting the livery */
static void put(unsigned char *px, int W, int x, int y, const float *c) {
    unsigned char *o = &px[(y * W + x) * 4];
    for (int k = 0; k < 3; k++) o[k] = (unsigned char)(clampf(c[k], 0, 1) * 255 + 0.5f);
    o[3] = 255;
}
static void mixc(float *o, const float *a, const float *b, float k) { for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * k; }
static GLuint paint(const AType *T, const Livery *l) {
    enum { W = 1024, H = 256 };
    unsigned char *px = malloc(W * H * 4);
    static const float DARK[3] = {0.05f, 0.06f, 0.08f}, WINGC[3] = {0.74f, 0.76f, 0.78f}, GREY[3] = {0.55f, 0.57f, 0.6f};
    float L = T->len, circ = PI * T->diam;
    bool light = T->len < 15;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (x + 0.5f) / W, v = (y + 0.5f) / H, c[3];
            if (u < 0.75f) {
                /* fuselage: t along the length, h = height around (-1 bottom .. 1 top) */
                float t = clampf((u - 0.002f) / 0.745f, 0, 1), h = -cosf(2 * PI * v);
                memcpy(c, l->body, sizeof c);
                if (h < l->beltline) memcpy(c, l->belly, sizeof c);
                if (l->hasCheat && fabsf(h - (l->beltline > -0.9f ? l->beltline + 0.06f : -0.1f)) < 0.035f) memcpy(c, l->cheat, sizeof c);
                if (l->rear && t > 0.8f && h > l->beltline + (1 - (t - 0.8f) / 0.2f) * 1.2f) memcpy(c, l->tail, sizeof c);
                if (l->hasNose && t < l->nosePart && h < 0.35f - t * 2) memcpy(c, l->nose, sizeof c);
                /* cabin windows (both sides), spaced for this type's length, and the cockpit */
                float side = fabsf(h - 0.3f) * circ * 0.5f / 2;     /* rough metres from the window line */
                if (!light) {
                    float pos = t * L, pitch = 0.53f;
                    if (t > 0.13f && t < (T->eng[0] == 'R' ? 0.74f : 0.82f) && side < 0.18f && fmodf(pos, pitch) < 0.27f && fabsf(fmodf(v * 2, 1) - 0.5f) > 0.2f) memcpy(c, DARK, sizeof c);
                    if (T->ddeck && t > 0.08f && t < 0.78f && fabsf(h - 0.75f) * circ * 0.25f < 0.16f && fmodf(pos, pitch) < 0.27f) memcpy(c, DARK, sizeof c);
                    if (T->hump && t > 0.07f && t < 0.3f && fabsf(h - 0.85f) * circ * 0.25f < 0.14f && fmodf(pos, pitch) < 0.27f) memcpy(c, DARK, sizeof c);
                    if (t > 0.028f && t < 0.06f && h > 0.25f && h < 0.55f) memcpy(c, DARK, sizeof c);
                } else if (t > 0.14f && t < 0.33f && h > 0.25f && h < 0.75f) memcpy(c, DARK, sizeof c);   /* light aircraft: big cabin windows */
                /* a little panel shading so it does not look like plastic */
                float shade = 0.97f + 0.03f * (fmodf(t * L / 1.6f, 1) < 0.03f ? -1 : 0);
                for (int k = 0; k < 3; k++) c[k] *= shade;
            } else if (u < 0.875f) {
                /* fin: chordwise fraction cu, height fraction fv */
                float cu = clampf((u - 0.752f) / 0.12f, 0, 1), fv = v;
                memcpy(c, l->tail, sizeof c);
                if (l->style == 1 && fv < 0.25f) memcpy(c, l->tail2, sizeof c);
                if (l->style == 2 && fv > 0.25f + cu * 0.5f) memcpy(c, l->tail2, sizeof c);
                if (l->style == 3) { if (fv > 0.3f && fv < 0.45f) memcpy(c, l->tail2, sizeof c); else if (fv > 0.52f && fv < 0.67f) memcpy(c, l->tail3, sizeof c); }
                if (fv > 0.5f && fv < 0.6f && u < 0.752f) memcpy(c, l->tail, sizeof c);
            } else if (u < 0.9375f) {
                /* engines: fan face dark, then the nacelle colour, a lighter lip */
                float a = clampf((u - 0.879f) / 0.054f, 0, 1);
                if (a < 0.05f) memcpy(c, DARK, sizeof c);
                else if (a < 0.12f) mixc(c, GREY, l->engine, 0.3f);
                else memcpy(c, l->engine, sizeof c);
                if (a > 0.85f) mixc(c, l->engine, GREY, 0.7f);
            } else memcpy(c, WINGC, sizeof c);
            put(px, W, x, y, c);
        }
    GLuint t; glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 4);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    free(px);
    return t;
}
/* painted textures, one per (type, airline), most recently used kept */
static struct { int type, liv; GLuint tex; unsigned used; } texc[40];
static unsigned useClock;
static GLuint livery_tex(int type, int liv) {
    int free_ = -1, old = 0;
    for (int i = 0; i < 40; i++) {
        if (texc[i].tex && texc[i].type == type && texc[i].liv == liv) { texc[i].used = ++useClock; return texc[i].tex; }
        if (!texc[i].tex && free_ < 0) free_ = i;
        if (texc[i].used < texc[old].used) old = i;
    }
    int k = free_ >= 0 ? free_ : old;
    if (texc[k].tex) glDeleteTextures(1, &texc[k].tex);
    static Livery plain;
    const Livery *l = liv >= 0 ? &livs[liv] : &plain;
    if (liv < 0) { for (int i = 0; i < 3; i++) plain.body[i] = plain.belly[i] = plain.tail[i] = plain.tail2[i] = plain.tail3[i] = plain.engine[i] = 0.9f; plain.beltline = -1; }
    texc[k].tex = paint(&types[type], l); texc[k].type = type; texc[k].liv = liv; texc[k].used = ++useClock;
    return texc[k].tex;
}

void airframe_draw(GLuint prog, int type, int liv, m4 model, float gearDown) {
    if (type < 0 || type >= ntypes) return;
    AType *T = &types[type];
    if (!T->vao) build(T);
    GLuint tex = livery_tex(type, liv);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, model.m);
    glUniform4f(glGetUniformLocation(prog, "uColor"), 1, 1, 1, 1);
    glUniform3f(glGetUniformLocation(prog, "uMatParams"), 0.05f, 0.32f, 0);
    glUniform3f(glGetUniformLocation(prog, "uHasTex"), 1, 0, 0);
    glUniform1f(glGetUniformLocation(prog, "uPaintMask"), 0);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(T->vao);
    glDrawElements(GL_TRIANGLES, T->ni, GL_UNSIGNED_INT, 0);
    if (gearDown > 0.5f) { glBindVertexArray(T->gvao); glDrawElements(GL_TRIANGLES, T->gni, GL_UNSIGNED_INT, 0); }
    glBindVertexArray(0);
}
