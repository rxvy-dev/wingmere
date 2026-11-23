/* WINGMERE - the planet.  MIT licence.
 *
 * The whole Earth is a quadtree of Web Mercator tiles.  Every frame we walk it from zoom 2,
 * splitting tiles that are close to the camera, culling ones that are behind the horizon or off
 * screen.  Each visible tile becomes a 33x33 mesh in true WGS84 (so the horizon is curved),
 * lifted by Terrarium elevation data and draped with satellite imagery.  While better data is
 * still downloading, a tile borrows the closest coarser ancestor, so there are no holes. */
#include "wm.h"

#define G 33                     /* vertices per tile edge */
#define NV (G * G + 4 * G)       /* grid + skirt */
#define CAP_ELEV 768
#define CAP_IMG 4096
#define CAP_MESH 2048
#define MAX_DRAW 3000

/* ------------------------------------------------------------------ small hash map: key -> slot */
typedef struct { uint64_t *k; int *v; int cap; } Map;
static void map_init(Map *m, int cap) { m->cap = cap; m->k = calloc(cap, 8); m->v = calloc(cap, sizeof(int)); }
static uint32_t mh(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; return (uint32_t)k; }
static int map_get(Map *m, uint64_t k) {
    k++;
    for (uint32_t i = mh(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->k[i]) return -1;
        if (m->k[i] == k) return m->v[i];
    }
}
static void map_put(Map *m, uint64_t k, int v) {
    k++;
    uint32_t i = mh(k) & (m->cap - 1);
    while (m->k[i] && m->k[i] != k) i = (i + 1) & (m->cap - 1);
    m->k[i] = k; m->v[i] = v;
}
static void map_del(Map *m, uint64_t k) {
    k++;
    uint32_t i = mh(k) & (m->cap - 1);
    while (m->k[i] && m->k[i] != k) i = (i + 1) & (m->cap - 1);
    if (!m->k[i]) return;
    m->k[i] = 0;
    for (uint32_t j = (i + 1) & (m->cap - 1); m->k[j]; j = (j + 1) & (m->cap - 1)) { /* backward-shift delete */
        uint32_t home = mh(m->k[j]) & (m->cap - 1);
        bool move = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
        if (move) { m->k[i] = m->k[j]; m->v[i] = m->v[j]; m->k[j] = 0; i = j; }
    }
}
static uint64_t tkey(int z, int x, int y) { return ((uint64_t)z << 52) | ((uint64_t)x << 26) | (uint64_t)y; }

/* ------------------------------------------------------------------ caches */
typedef struct { uint64_t key; int z, x, y, status, last; float *h; float hmin, hmax; GLuint tex; } ElevTile;
typedef struct { uint64_t key; int z, x, y, status, last; GLuint tex; } ImgTile;
typedef struct { uint64_t key; int z, x, y, last, srcZ, maskZ, used; GLuint vao, vbo; d3 center; float hmin, hmax; } MeshTile;
static ElevTile elev[CAP_ELEV]; static Map elevMap; static int nElev;
static ImgTile img[CAP_IMG]; static Map imgMap; static int nImg, imgCap;
static MeshTile mesh[CAP_MESH]; static Map meshMap; static int nMesh;
static GLuint ibo; static int nIdx;
static int frameNo, builds, uploads;
static int elevMax, imgMax, maxZ;
static float anisotropy = 1;

typedef struct { int z, x, y; float dist; } Node;
static Node draw[MAX_DRAW]; static int nDraw;
static d3 camPos; static float frustum[6][4], splitK;

/* ------------------------------------------------------------------ elevation */
static ElevTile *elev_get(int z, int x, int y) { int s = map_get(&elevMap, tkey(z, x, y)); return s < 0 ? NULL : &elev[s]; }
static int evict_slot(void *arr, size_t stride, int count, int (*lastOf)(void *), void (*freeIt)(void *)) {
    int best = -1, bl = 1 << 30;
    for (int i = 0; i < count; i++) { int l = lastOf((char *)arr + stride * i); if (l < bl && l < frameNo - 2) { bl = l; best = i; } }
    if (best >= 0) freeIt((char *)arr + stride * best);
    return best;
}
static int elev_last(void *p) { return ((ElevTile *)p)->last; }
static void elev_free(void *p) { ElevTile *e = p; map_del(&elevMap, e->key); free(e->h); if (e->tex) glDeleteTextures(1, &e->tex); memset(e, 0, sizeof *e); e->last = -1000000; }
static ElevTile *elev_new(int z, int x, int y) {
    int s = nElev < CAP_ELEV ? nElev++ : evict_slot(elev, sizeof(ElevTile), CAP_ELEV, elev_last, elev_free);
    if (s < 0) return NULL;
    ElevTile *e = &elev[s];
    memset(e, 0, sizeof *e);
    e->key = tkey(z, x, y); e->z = z; e->x = x; e->y = y; e->last = frameNo;
    map_put(&elevMap, e->key, s);
    return e;
}
/* best loaded elevation tile covering (z,x,y), or NULL; reports its zoom */
static ElevTile *elev_best(int z, int x, int y, int *outZ) {
    for (int ez = z < elevMax ? z : elevMax; ez >= 0; ez--) {
        int d = z - ez;
        ElevTile *e = elev_get(ez, x >> d, y >> d);
        if (e && e->status == TILE_OK) { e->last = frameNo; *outZ = ez; return e; }
    }
    *outZ = -1;
    return NULL;
}
static float elev_sample(const ElevTile *e, double X, double Y) { /* X,Y: Mercator coordinates at the tile's own zoom */
    double px = (X - e->x) * 256 - 0.5, py = (Y - e->y) * 256 - 0.5;
    px = px < 0 ? 0 : (px > 255 ? 255 : px); py = py < 0 ? 0 : (py > 255 ? 255 : py);
    int ix = (int)px, iy = (int)py, ix1 = ix < 255 ? ix + 1 : 255, iy1 = iy < 255 ? iy + 1 : 255;
    float tx = (float)(px - ix), ty = (float)(py - iy);
    const float *h = e->h;
    return lerpf(lerpf(h[iy * 256 + ix], h[iy * 256 + ix1], tx), lerpf(h[iy1 * 256 + ix], h[iy1 * 256 + ix1], tx), ty);
}

/* ------------------------------------------------------------------ imagery */
static ImgTile *img_get(int z, int x, int y) { int s = map_get(&imgMap, tkey(z, x, y)); return s < 0 ? NULL : &img[s]; }
static int img_last(void *p) { return ((ImgTile *)p)->last; }
static void img_free(void *p) { ImgTile *t = p; map_del(&imgMap, t->key); if (t->tex) glDeleteTextures(1, &t->tex); memset(t, 0, sizeof *t); t->last = -1000000; }
static ImgTile *img_new(int z, int x, int y) {
    int s = nImg < imgCap ? nImg++ : evict_slot(img, sizeof(ImgTile), imgCap, img_last, img_free);
    if (s < 0) return NULL;
    ImgTile *t = &img[s];
    memset(t, 0, sizeof *t);
    t->key = tkey(z, x, y); t->z = z; t->x = x; t->y = y; t->last = frameNo;
    map_put(&imgMap, t->key, s);
    return t;
}
static ImgTile *img_best(int z, int x, int y, int *outZ) {
    for (int iz = z < imgMax ? z : imgMax; iz >= 0; iz--) {
        int d = z - iz;
        ImgTile *t = img_get(iz, x >> d, y >> d);
        if (t && t->status == TILE_OK) { t->last = frameNo; *outZ = iz; return t; }
    }
    *outZ = -1;
    return NULL;
}

/* ------------------------------------------------------------------ OpenStreetMap vector data */
#define CAP_VEC 1024
#define VEC_MAX 14
#define VEC_MIN 4
typedef struct {
    uint64_t key; int z, x, y, status, last, born; GLuint tex; uint8_t *water;
    VecTile *raw;                         /* buildings and taxiway lines, waiting for good elevation data */
    GLuint bvao, bvbo, tvao, tvbo; int bn, tn; d3 center; d3 *lights; int nlights; bool built;
    ParkSpot *park; int npark;
} VecC;
static VecC vec[CAP_VEC]; static Map vecMap; static int nVec;
static VecC *vec_get(int z, int x, int y) { int s = map_get(&vecMap, tkey(z, x, y)); return s < 0 ? NULL : &vec[s]; }
static int vec_last(void *p) { return ((VecC *)p)->last; }
static void vec_free(void *p) {
    VecC *v = p; map_del(&vecMap, v->key);
    if (v->tex) glDeleteTextures(1, &v->tex);
    if (v->bvao) { glDeleteVertexArrays(1, &v->bvao); glDeleteBuffers(1, &v->bvbo); }
    if (v->tvao) { glDeleteVertexArrays(1, &v->tvao); glDeleteBuffers(1, &v->tvbo); }
    free(v->water); vectile_free(v->raw); free(v->lights); free(v->park);
    memset(v, 0, sizeof *v); v->last = -1000000;
}
static VecC *vec_new(int z, int x, int y) {
    int s = nVec < CAP_VEC ? nVec++ : evict_slot(vec, sizeof(VecC), CAP_VEC, vec_last, vec_free);
    if (s < 0) return NULL;
    VecC *v = &vec[s];
    memset(v, 0, sizeof *v);
    v->key = tkey(z, x, y); v->z = z; v->x = x; v->y = y; v->last = frameNo; v->born = frameNo;
    map_put(&vecMap, v->key, s);
    return v;
}
static VecC *vec_best(int z, int x, int y, int *outZ) {
    for (int vz = z < VEC_MAX ? z : VEC_MAX; vz >= VEC_MIN; vz--) {
        int d = z - vz;
        VecC *v = vec_get(vz, x >> d, y >> d);
        if (v && v->status == TILE_OK && v->tex) { v->last = frameNo; *outZ = vz; return v; }
    }
    *outZ = -1;
    return NULL;
}
/* water fraction 0..1 at Mercator X,Y (given at the vector tile's own zoom) */
static float vec_water(const VecC *v, double X, double Y) {
    int n = 128;
    double px = (X - v->x) * n - 0.5, py = (Y - v->y) * n - 0.5;
    px = px < 0 ? 0 : (px > n - 1 ? n - 1 : px); py = py < 0 ? 0 : (py > n - 1 ? n - 1 : py);
    int ix = (int)px, iy = (int)py, ix1 = ix < n - 1 ? ix + 1 : ix, iy1 = iy < n - 1 ? iy + 1 : iy;
    float tx = (float)(px - ix), ty = (float)(py - iy);
    const uint8_t *w = v->water;
    return lerpf(lerpf(w[iy * n + ix], w[iy * n + ix1], tx), lerpf(w[iy1 * n + ix], w[iy1 * n + ix1], tx), ty) / 255.0f;
}
static void local_to_geo(const VecC *v, float tileM, float lx, float ly, double *lat, double *lon) {
    double fx = 0.5 + lx / tileM, fy = 0.5 - ly / tileM;
    *lon = merc_lon(v->x + fx, v->z); *lat = merc_lat(v->y + fy, v->z);
}
static GLuint geo_vao(const float *v, int n, GLuint *vbo) {
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, vbo); glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 10 * n, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 40, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 40, (void *)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 40, (void *)24);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 40, (void *)32);
    return vao;
}
float terrain_height(double lat, double lon, bool *water, bool *runway);
/* buildings and taxiway paint, placed on the ground once the elevation there is known */
static void vec_build(VecC *v) {
    VecTile *r = v->raw;
    v->built = true;
    if (!r) return;
    if (v->bvao) { glDeleteVertexArrays(1, &v->bvao); glDeleteBuffers(1, &v->bvbo); v->bvao = 0; v->bn = 0; }
    if (v->tvao) { glDeleteVertexArrays(1, &v->tvao); glDeleteBuffers(1, &v->tvbo); v->tvao = 0; v->tn = 0; }
    free(v->lights); v->lights = NULL; v->nlights = 0;
    free(v->park); v->park = NULL; v->npark = 0;
    double latc = merc_lat(v->y + 0.5, v->z), lonc = merc_lon(v->x + 0.5, v->z);
    float tileM = (float)(2 * M_PI * EARTH_R * cos(latc * M_PI / 180) / (1 << v->z));
    v3 E, N, U; enu_basis(latc, lonc, &E, &N, &U);
    float h0 = terrain_height(latc, lonc, NULL, NULL);
    v->center = geo_to_ecef(latc, lonc, h0);
    if (r->nb && r->nbv) {
        float *out = malloc(sizeof(float) * 10 * r->nbv);
        for (int b = 0; b < r->nb; b++) {
            float *c = r->bc + b * 4; double la, lo;
            local_to_geo(v, tileM, c[0], c[1], &la, &lo);
            bool wet = false;
            float base = terrain_height(la, lo, &wet, NULL) - h0;
            int s = (int)c[2], cnt = (int)c[3];
            for (int k = s; k < s + cnt && k < r->nbv; k++) {
                float *i = r->bv + k * 10, *o = out + k * 10;
                v3 p = vadd(vadd(vmul(E, i[0]), vmul(N, i[1])), vmul(U, i[2] + base));
                v3 nn = vadd(vadd(vmul(E, i[3]), vmul(N, i[4])), vmul(U, i[5]));
                o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = nn.x; o[4] = nn.y; o[5] = nn.z; o[6] = i[6]; o[7] = i[7]; o[8] = i[8]; o[9] = i[9];
            }
        }
        v->bvao = geo_vao(out, r->nbv, &v->bvbo); v->bn = r->nbv;
        free(out);
    }
    if (r->nl > 1) {
        float *out = malloc(sizeof(float) * 10 * 6 * r->nl);
        v->lights = malloc(sizeof(d3) * (r->nl / 3 + 1));
        int n = 0, k = 0;
        float ph[2] = {0, 0}; v3 pp = {0, 0, 0}; bool have = false;
        for (int i = 0; i < r->nl; i++) {
            float lx = r->lights[i * 2], ly = r->lights[i * 2 + 1];
            if (lx > 1e8f) { have = false; continue; }
            double la, lo; local_to_geo(v, tileM, lx, ly, &la, &lo);
            float hz = terrain_height(la, lo, NULL, NULL) - h0 + 0.16f;
            v3 p = vadd(vadd(vmul(E, lx), vmul(N, ly)), vmul(U, hz));
            if (k % 3 == 0) v->lights[v->nlights++] = dadd(v->center, f2d(vadd(p, vmul(U, 0.15f))));
            k++;
            if (have) {
                v3 d = vsub(p, pp);
                float L = vlen(d);
                if (L > 0.5f && L < 20) {
                    v3 side = vmul(vnorm(vcross(d, U)), 0.17f);
                    v3 q[6] = {vsub(pp, side), vadd(pp, side), vadd(p, side), vsub(pp, side), vadd(p, side), vsub(p, side)};
                    for (int j = 0; j < 6; j++) {
                        float *o = out + n * 10;
                        o[0] = q[j].x; o[1] = q[j].y; o[2] = q[j].z; o[3] = U.x; o[4] = U.y; o[5] = U.z; o[6] = 0; o[7] = 0; o[8] = 2; o[9] = 0;
                        n++;
                    }
                }
            }
            pp = p; have = true; (void)ph;
        }
        if (n) { v->tvao = geo_vao(out, n, &v->tvbo); v->tn = n; }
        free(out);
    }
    if (r->npark) {
        v->park = malloc(sizeof(ParkSpot) * r->npark);
        for (int i = 0; i < r->npark; i++) {
            ParkSpot *p = &v->park[v->npark++];
            local_to_geo(v, tileM, r->park[i * 3], r->park[i * 3 + 1], &p->lat, &p->lon);
            p->ground = geo_to_ecef(p->lat, p->lon, terrain_height(p->lat, p->lon, NULL, NULL));
            p->heading = r->park[i * 3 + 2] / DEG;
            p->seed = (uint32_t)(v->x * 73856093u ^ v->y * 19349663u ^ (uint32_t)i * 83492791u);
        }
    }
    vectile_free(v->raw); v->raw = NULL;
}

/* ------------------------------------------------------------------ meshes */
static int mesh_last(void *p) { return ((MeshTile *)p)->last; }
static void mesh_free(void *p) {
    MeshTile *m = p;
    map_del(&meshMap, m->key);
    if (m->vao) { glDeleteVertexArrays(1, &m->vao); glDeleteBuffers(1, &m->vbo); }
    memset(m, 0, sizeof *m); m->last = -1000000;
}
static void build_indices(void) {
    uint16_t *ix = malloc(sizeof(uint16_t) * ((G - 1) * (G - 1) * 6 + 4 * (G - 1) * 6)), *p = ix;
    for (int j = 0; j < G - 1; j++)
        for (int i = 0; i < G - 1; i++) {
            uint16_t a = j * G + i, b = a + 1, c = a + G, d = c + 1;
            *p++ = a; *p++ = c; *p++ = b; *p++ = b; *p++ = c; *p++ = d;
        }
    /* skirts: each border vertex has a dropped copy after the grid */
    for (int s = 0; s < 4; s++)
        for (int k = 0; k < G - 1; k++) {
            int a, b;
            if (s == 0) { a = k; b = k + 1; }                              /* north edge */
            else if (s == 1) { a = k * G + G - 1; b = (k + 1) * G + G - 1; } /* east edge */
            else if (s == 2) { a = (G - 1) * G + k; b = a + 1; }           /* south edge */
            else { a = k * G; b = (k + 1) * G; }                          /* west edge */
            uint16_t sa = G * G + s * G + k, sb = sa + 1;
            *p++ = a; *p++ = sa; *p++ = b; *p++ = b; *p++ = sa; *p++ = sb;
        }
    nIdx = (int)(p - ix);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16_t) * nIdx, ix, GL_STATIC_DRAW);
    free(ix);
}

/* vertex: pos(3) normal(3) uv(2) height(1) */
static void build_mesh(MeshTile *m, int z, int x, int y) {
    static float vb[NV * 9];
    static d3 P[G * G];
    static float H[G * G];
    int sz; ElevTile *e = elev_best(z, x, y, &sz);
    m->srcZ = sz;
    int runways[48], nr = 0;
    int vz; VecC *vm = vec_best(z, x, y, &vz);
    if (vm && vz < 7) vm = NULL;
    m->maskZ = vm ? vz : -1;
    double clat = merc_lat(y + 0.5, z), clon = merc_lon(x + 0.5, z);
    double tileM = 2 * M_PI * EARTH_R * cos(clat * M_PI / 180) / (1 << z);
    if (z >= 11) nr = runways_near(clat, clon, tileM * 0.75 + 5000, runways, 48);
    float hmin = 1e9f, hmax = -1e9f;
    for (int j = 0; j < G; j++)
        for (int i = 0; i < G; i++) {
            double X = x + (double)i / (G - 1), Y = y + (double)j / (G - 1);
            float h = 0;
            if (e) { double sc = 1.0 / (double)(1 << (z - sz)); h = elev_sample(e, X * sc, Y * sc); }
            if (vm) {                                          /* real coastlines: the sea is flat, land below sea level stays */
                double sc = 1.0 / (double)(1 << (z - vz));
                if (h < 0 && vec_water(vm, X * sc, Y * sc) > 0.2f) h = 0;
            } else if (h < 0) h = 0;                           /* no map data: anything below zero is sea */
            double lat = merc_lat(Y, z), lon = merc_lon(X, z);
            if (nr) h = runway_flatten_list(runways, nr, lat, lon, h, NULL);
            H[j * G + i] = h;
            hmin = fminf(hmin, h); hmax = fmaxf(hmax, h);
            P[j * G + i] = geo_to_ecef(lat, lon, h);
        }
    m->center = P[(G / 2) * G + G / 2];
    m->hmin = hmin; m->hmax = hmax;
    v3 eU, eN, up; enu_basis(clat, clon, &eU, &eN, &up);
    for (int j = 0; j < G; j++)
        for (int i = 0; i < G; i++) {
            d3 px1 = P[j * G + (i < G - 1 ? i + 1 : i)], px0 = P[j * G + (i > 0 ? i - 1 : i)];
            d3 py1 = P[(j < G - 1 ? j + 1 : j) * G + i], py0 = P[(j > 0 ? j - 1 : j) * G + i];
            v3 n = vnorm(vcross(d2f(dsub(px1, px0)), d2f(dsub(py0, py1))));
            if (vdot(n, up) < 0) n = vmul(n, -1);
            float *v = &vb[(j * G + i) * 9];
            v3 p = d2f(dsub(P[j * G + i], m->center));
            v[0] = p.x; v[1] = p.y; v[2] = p.z; v[3] = n.x; v[4] = n.y; v[5] = n.z;
            v[6] = (float)i / (G - 1); v[7] = (float)j / (G - 1); v[8] = H[j * G + i];
        }
    /* skirts hang below the border to hide cracks between neighbours of different detail */
    float skirt = (float)fmin(fmax(tileM * 0.03, 8), 3000);
    for (int s = 0; s < 4; s++)
        for (int k = 0; k < G; k++) {
            int src = s == 0 ? k : (s == 1 ? k * G + G - 1 : (s == 2 ? (G - 1) * G + k : k * G));
            float *d = &vb[(G * G + s * G + k) * 9], *o = &vb[src * 9];
            memcpy(d, o, sizeof(float) * 9);
            v3 p = vsub(V(o[0], o[1], o[2]), vmul(up, skirt));
            d[0] = p.x; d[1] = p.y; d[2] = p.z; d[8] = o[8] - skirt;
        }
    if (!m->vao) {
        glGenVertexArrays(1, &m->vao); glBindVertexArray(m->vao);
        glGenBuffers(1, &m->vbo); glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof vb, vb, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void *)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 36, (void *)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 36, (void *)24);
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 36, (void *)32);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof vb, vb);
    }
}
static MeshTile *mesh_for(int z, int x, int y, bool allowBuild) {
    int s = map_get(&meshMap, tkey(z, x, y));
    MeshTile *m = s >= 0 ? &mesh[s] : NULL;
    int best; ElevTile *e = elev_best(z, x, y, &best);
    (void)e;
    if (m) {
        m->last = frameNo;
        int vzz; VecC *vv = vec_best(z, x, y, &vzz); int mz = vv && vzz >= 7 ? vzz : -1; (void)vv;
        if ((m->srcZ != best || m->maskZ != mz) && allowBuild && builds < 60) { builds++; build_mesh(m, z, x, y); }
        return m;
    }
    if (!allowBuild || builds >= 40) return NULL;
    s = nMesh < CAP_MESH ? nMesh++ : evict_slot(mesh, sizeof(MeshTile), CAP_MESH, mesh_last, mesh_free);
    if (s < 0) return NULL;
    m = &mesh[s];
    memset(m, 0, sizeof *m);
    m->key = tkey(z, x, y); m->z = z; m->x = x; m->y = y; m->last = frameNo;
    map_put(&meshMap, m->key, s);
    builds++;
    build_mesh(m, z, x, y);
    return m;
}

/* ------------------------------------------------------------------ incoming tiles */
static void receive(void) {
    TileData t[32];
    int n;
    while (uploads < 40 && (n = tiles_poll(t, 32 < 40 - uploads ? 32 : 40 - uploads)) > 0) {
        for (int i = 0; i < n; i++) {
            TileData *d = &t[i];
            if (d->layer == LAYER_ELEV) {
                ElevTile *e = elev_get(d->z, d->x, d->y);
                if (!e) e = elev_new(d->z, d->x, d->y);
                if (e) {
                    e->status = d->status;
                    if (d->status == TILE_OK && d->w == 256 && d->h == 256) {
                        free(e->h); e->h = d->height; d->height = NULL;
                        e->hmin = 1e9f; e->hmax = -1e9f;
                        for (int k = 0; k < 256 * 256; k += 7) { e->hmin = fminf(e->hmin, e->h[k]); e->hmax = fmaxf(e->hmax, e->h[k]); }
                        /* the heights also go to the GPU, so the shader can light every pixel with the real slope */
                        if (!e->tex) glGenTextures(1, &e->tex);
                        glBindTexture(GL_TEXTURE_2D, e->tex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 256, 256, 0, GL_RED, GL_FLOAT, e->h);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    } else if (d->status == TILE_OK) e->status = TILE_FAILED;
                    if (e->status == TILE_FAILED) e->last = frameNo + 300; /* retry in a few seconds */
                }
            } else if (d->layer == LAYER_VEC) {
                VecC *v = vec_get(d->z, d->x, d->y);
                if (!v) v = vec_new(d->z, d->x, d->y);
                if (v) {
                    v->status = d->status;
                    if (d->status == TILE_OK && d->vec) {
                        VecTile *t = d->vec;
                        if (!v->tex) glGenTextures(1, &v->tex);
                        glBindTexture(GL_TEXTURE_2D, v->tex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->n, t->n, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->mask);
                        glGenerateMipmap(GL_TEXTURE_2D);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        free(v->water); v->water = t->water; t->water = NULL;
                        free(t->mask); t->mask = NULL;
                        if (t->nb || t->nl || t->npark) { vectile_free(v->raw); v->raw = t; v->built = false; v->born = frameNo; }
                        else vectile_free(t);
                        d->vec = NULL;
                        uploads++;
                    } else if (d->status == TILE_FAILED) v->last = frameNo + 600;
                    vectile_free(d->vec); d->vec = NULL;
                }
            } else {
                ImgTile *it = img_get(d->z, d->x, d->y);
                if (!it) it = img_new(d->z, d->x, d->y);
                if (it) {
                    it->status = d->status;
                    if (d->status == TILE_OK) {
                        if (!it->tex) glGenTextures(1, &it->tex);
                        glBindTexture(GL_TEXTURE_2D, it->tex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, d->w, d->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d->rgba);
                        glGenerateMipmap(GL_TEXTURE_2D);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        if (anisotropy > 1) glTexParameterf(GL_TEXTURE_2D, 0x84FE, anisotropy);
                        uploads++;
                    } else if (d->status == TILE_FAILED) it->last = frameNo + 300;
                }
            }
            free(d->rgba); free(d->height);
        }
    }
}

/* ------------------------------------------------------------------ quadtree walk */
static bool sphere_visible(v3 c, float r) {
    for (int i = 0; i < 6; i++) if (frustum[i][0] * c.x + frustum[i][1] * c.y + frustum[i][2] * c.z + frustum[i][3] < -r) return false;
    return true;
}
static void want(int z, int x, int y, float dist, float size) {
    float prio = -dist / (size + 1) - 0.35f * z;
    int ez = z < elevMax ? z : elevMax, iz = z < imgMax ? z : imgMax;
    int d = z - ez;
    ElevTile *e = elev_get(ez, x >> d, y >> d);
    if (!e) { e = elev_new(ez, x >> d, y >> d); }
    if (e) { e->last = frameNo > e->last ? frameNo : e->last; if (e->status == 0) tiles_request(LAYER_ELEV, ez, x >> d, y >> d, prio + 0.5f); }
    d = z - iz;
    ImgTile *t = img_get(iz, x >> d, y >> d);
    if (!t) t = img_new(iz, x >> d, y >> d);
    if (t) { t->last = frameNo > t->last ? frameNo : t->last; if (t->status == 0) tiles_request(LAYER_IMG, iz, x >> d, y >> d, prio); }
    if (z >= VEC_MIN && !S.offline) {
        int vz = z < VEC_MAX ? z : VEC_MAX; d = z - vz;
        VecC *v = vec_get(vz, x >> d, y >> d);
        if (!v) v = vec_new(vz, x >> d, y >> d);
        if (v) { v->last = frameNo > v->last ? frameNo : v->last; if (v->status == 0) tiles_request(LAYER_VEC, vz, x >> d, y >> d, prio + 0.2f); }
        /* buildings show from further away than the terrain reaches zoom 14 */
        if (S.buildings && z >= 12 && z < VEC_MAX && dist < 9000) {
            int k = VEC_MAX - z;
            for (int j = 0; j < (1 << k); j++)
                for (int i = 0; i < (1 << k); i++) {
                    int bx = (x << k) + i, by = (y << k) + j;
                    VecC *c = vec_get(VEC_MAX, bx, by);
                    if (!c) c = vec_new(VEC_MAX, bx, by);
                    if (c) { c->last = frameNo; if (c->status == 0) tiles_request(LAYER_VEC, VEC_MAX, bx, by, prio - 1.0f); }
                }
        }
    }
}
static void visit(int z, int x, int y) {
    double lat0 = merc_lat(y, z), lat1 = merc_lat(y + 1, z), lon0 = merc_lon(x, z), lon1 = merc_lon(x + 1, z);
    double latc = merc_lat(y + 0.5, z), lonc = merc_lon(x + 0.5, z);
    float hmin = 0, hmax = z < 8 ? 6000 : 4500;
    int ez; ElevTile *e = elev_best(z, x, y, &ez);
    if (e && ez >= z - 3) { hmin = fmaxf(e->hmin, 0) - 50; hmax = e->hmax + 50; }
    d3 c = geo_to_ecef(latc, lonc, (hmin + hmax) * 0.5);
    double r = 0;
    double la[3] = {lat0, latc, lat1}, lo[3] = {lon0, lonc, lon1};
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
            for (int k = 0; k < 2; k++) { double dd = dlen(dsub(geo_to_ecef(la[a], lo[b], k ? hmax : hmin), c)); if (dd > r) r = dd; }
    d3 rel = dsub(c, camPos);
    double dist = dlen(rel) - r;
    if (dist < 0) dist = 0;
    /* horizon: hidden if the whole bounding sphere sits below the camera's horizon */
    double cr = dlen(camPos), R = EARTH_R - 500;
    if (z >= 3 && cr > R) {
        double horizon = acos(fmin(R / cr, 1.0)) + acos(fmin(R / (R + hmax + 500), 1.0));
        double ang = acos(fmax(-1.0, fmin(1.0, ddot(dnorm(camPos), dnorm(c)))));
        if (ang - r / R > horizon) return;
    }
    if (!sphere_visible(d2f(rel), (float)r)) return;
    double size = 2 * M_PI * EARTH_R * cos(latc * M_PI / 180) / (1 << z);
    if (z < maxZ && dist < size * splitK) {
        for (int k = 0; k < 4; k++) visit(z + 1, x * 2 + (k & 1), y * 2 + (k >> 1));
        return;
    }
    if (nDraw < MAX_DRAW) { draw[nDraw].z = z; draw[nDraw].x = x; draw[nDraw].y = y; draw[nDraw].dist = (float)dist; nDraw++; }
    want(z, x, y, (float)dist, (float)size);
}
/* prefetch: ask for the ground where you will be in half a minute, so it is sharp when you get there */
static d3 aheadPos; static bool aheadOn;
void terrain_lookahead(d3 p, bool on) { aheadPos = p; aheadOn = on; }
static void prefetch(int z, int x, int y) {
    double latc = merc_lat(y + 0.5, z), lonc = merc_lon(x + 0.5, z);
    double size = 2 * M_PI * EARTH_R * cos(latc * M_PI / 180) / (1 << z);
    d3 c = geo_to_ecef(latc, lonc, 0);
    double dist = dlen(dsub(c, aheadPos)) - size * 0.75;
    if (dist < 0) dist = 0;
    if (dist > size * 6 && z > 4) return;
    if (z < maxZ - 1 && dist < size * splitK * 0.55) {
        for (int k = 0; k < 4; k++) prefetch(z + 1, x * 2 + (k & 1), y * 2 + (k >> 1));
        return;
    }
    want(z, x, y, (float)dist + 3000, (float)size);     /* behind what is on screen now */
}
static int by_dist(const void *a, const void *b) { float d = ((const Node *)a)->dist - ((const Node *)b)->dist; return d < 0 ? -1 : d > 0; }

void terrain_init(void) {
    map_init(&elevMap, 4096); map_init(&imgMap, 8192); map_init(&meshMap, 8192); map_init(&vecMap, 4096);
    elevMax = S.elevation_max_zoom; imgMax = S.imagery_max_zoom;
    maxZ = imgMax + 1 > 17 ? 17 : imgMax + 1;
    if (maxZ < elevMax) maxZ = elevMax;
    imgCap = S.texture_budget > 256 ? (S.texture_budget < CAP_IMG ? S.texture_budget : CAP_IMG) : 256;
    glGetFloatv(0x84FF, &anisotropy); /* GL_MAX_TEXTURE_MAX_ANISOTROPY */
    if (glGetError() != GL_NO_ERROR || anisotropy < 1) anisotropy = 1;
    if (anisotropy > 16) anisotropy = 16;
    build_indices();
    /* the whole world at zoom 0-2 is cheap and gives every later tile something to borrow */
    for (int z = 0; z <= 2; z++)
        for (int y = 0; y < (1 << z); y++)
            for (int x = 0; x < (1 << z); x++) { tiles_request(LAYER_IMG, z, x, y, 100.0f - z); tiles_request(LAYER_ELEV, z, x, y, 100.5f - z); }
}

void terrain_update(d3 cam, const float fr[6][4], float tanHalfFov, int screenH) {
    frameNo++; builds = 0; uploads = 0;
    camPos = cam;
    memcpy(frustum, fr, sizeof frustum);
    splitK = 1.9f * S.detail * (screenH / 1080.0f) * (0.58f / tanHalfFov);
    if (splitK < 0.6f) splitK = 0.6f;
    receive();
    for (int z = 0; z <= 2; z++)
        for (int y = 0; y < (1 << z); y++)
            for (int x = 0; x < (1 << z); x++) want(z, x, y, 1e7f, 1e7f);
    nDraw = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) visit(2, x, y);
    if (aheadOn) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) prefetch(2, x, y);
    qsort(draw, nDraw, sizeof(Node), by_dist);
    for (int i = 0; i < CAP_IMG; i++) if (img[i].status == 0 && img[i].key && img[i].last < frameNo - 120) img_free(&img[i]);
    for (int i = 0; i < CAP_ELEV; i++) if (elev[i].status == 0 && elev[i].key && elev[i].last < frameNo - 120) elev_free(&elev[i]);
    for (int i = 0; i < CAP_IMG; i++) if (img[i].status == TILE_FAILED && img[i].last < frameNo) { img[i].status = 0; }
    for (int i = 0; i < CAP_ELEV; i++) if (elev[i].status == TILE_FAILED && elev[i].last < frameNo) { elev[i].status = 0; }
    for (int i = 0; i < CAP_VEC; i++) {
        VecC *v = &vec[i];
        if (!v->key) continue;
        if (v->status == 0 && v->last < frameNo - 120) vec_free(v);
        else if (v->status == TILE_FAILED && v->last < frameNo) v->status = 0;
    }
    /* put new buildings on the ground once the best elevation is in (or after waiting a while) */
    int nb = 0;
    for (int i = 0; i < CAP_VEC && nb < 2; i++) {
        VecC *v = &vec[i];
        if (!v->key || v->built || !v->raw) continue;
        int ez; double latc = merc_lat(v->y + 0.5, v->z), lonc = merc_lon(v->x + 0.5, v->z);
        elev_best(elevMax, (int)merc_x(lonc, elevMax), (int)merc_y(latc, elevMax), &ez);
        if (ez >= elevMax || frameNo - v->born > 600) { vec_build(v); nb++; }
    }
}

static void caps_draw(GLuint prog, d3 cam);
void terrain_draw(GLuint prog, d3 cam) {
    GLint uOff = glGetUniformLocation(prog, "uOffset"), uST = glGetUniformLocation(prog, "uImgST"), uHas = glGetUniformLocation(prog, "uHasImg");
    GLint uMST = glGetUniformLocation(prog, "uMaskST"), uHM = glGetUniformLocation(prog, "uHasMask");
    GLint uEST = glGetUniformLocation(prog, "uElevST"), uHE = glGetUniformLocation(prog, "uHasElev"), uETM = glGetUniformLocation(prog, "uElevTexelM");
    glUniform1i(glGetUniformLocation(prog, "uElev"), 9);
    glUniform1i(glGetUniformLocation(prog, "uMask"), 7);
    glActiveTexture(GL_TEXTURE2);   /* units 0, 1 and 3 hold the sky, shadow and detail textures */
    for (int i = 0; i < nDraw; i++) {
        Node *n = &draw[i];
        MeshTile *m = mesh_for(n->z, n->x, n->y, true);
        if (!m) {
            /* not built yet: draw the parent tile's mesh portion is not possible, so try the parent outright */
            continue;
        }
        int iz; ImgTile *t = img_best(n->z, n->x, n->y, &iz);
        if (t) {
            int d = n->z - iz; float s = 1.0f / (float)(1 << d);
            glUniform4f(uST, s, s, (float)(n->x - ((n->x >> d) << d)) * s, (float)(n->y - ((n->y >> d) << d)) * s);
            glBindTexture(GL_TEXTURE_2D, t->tex);
            glUniform1f(uHas, 1);
        } else glUniform1f(uHas, 0);
        int vz; VecC *vm = vec_best(n->z, n->x, n->y, &vz);
        if (vm) {
            int d = n->z - vz; float s = 1.0f / (float)(1 << d);
            glUniform4f(uMST, s, s, (float)(n->x - ((n->x >> d) << d)) * s, (float)(n->y - ((n->y >> d) << d)) * s);
            glUniform1f(uHM, 1);
            glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, vm->tex); glActiveTexture(GL_TEXTURE2);
        } else glUniform1f(uHM, 0);
        int ez; ElevTile *et = elev_best(n->z, n->x, n->y, &ez);
        if (et && et->tex) {
            int d = n->z - ez; float s = 1.0f / (float)(1 << d);
            glUniform4f(uEST, s, s, (float)(n->x - ((n->x >> d) << d)) * s, (float)(n->y - ((n->y >> d) << d)) * s);
            double lat = merc_lat(et->y + 0.5, ez);
            glUniform1f(uETM, (float)(2 * M_PI * EARTH_R * cos(lat * M_PI / 180) / (1 << ez) / 256.0));
            glUniform1f(uHE, 1);
            glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, et->tex); glActiveTexture(GL_TEXTURE2);
        } else glUniform1f(uHE, 0);
        v3 off = d2f(dsub(m->center, cam));
        glUniform3f(uOff, off.x, off.y, off.z);
        glBindVertexArray(m->vao);
        glDrawElements(GL_TRIANGLES, nIdx, GL_UNSIGNED_SHORT, 0);
    }
    caps_draw(prog, cam);
}

static float height_merc(double lat, double lon, bool *water, bool *runway) {
    double X = merc_x(lon, elevMax), Y = merc_y(lat, elevMax);
    int x = (int)X, y = (int)Y;
    float h = 0;
    bool have = false;
    for (int ez = elevMax; ez >= 0; ez--) {
        int d = elevMax - ez;
        ElevTile *e = elev_get(ez, x >> d, y >> d);
        if (e && e->status == TILE_OK) { double sc = 1.0 / (double)(1 << d); h = elev_sample(e, X * sc, Y * sc); have = true; break; }
    }
    (void)have;
    int vz; VecC *vm = vec_best(VEC_MAX, (int)merc_x(lon, VEC_MAX), (int)merc_y(lat, VEC_MAX), &vz);
    float wet = -1;
    if (vm && vz >= 7) wet = vec_water(vm, merc_x(lon, vz), merc_y(lat, vz));
    if (wet >= 0) { if (h < 0 && wet > 0.2f) h = 0; }
    else if (h < 0) h = 0;
    bool on = false;
    h = runway_flatten(lat, lon, h, &on);
    if (runway) *runway = on;
    if (water) *water = !on && (wet >= 0 ? wet > 0.5f : h <= 0.05f);
    return h;
}
/* Beyond 85.05 degrees the Web Mercator tiles stop.  The rest of the way to each pole is filled in:
 * flat sea ice to the north (you can land on it), and the Antarctic plateau rising to ~2900 m in the south. */
#define MERC_EDGE 85.0511
static float polar_height(double lat, double lon) {
    double a = fabs(lat);
    float edge = height_merc(lat > 0 ? MERC_EDGE - 0.02 : -(MERC_EDGE - 0.02), lon, NULL, NULL);
    float cap = lat > 0 ? 0.0f : 2900.0f;
    float k = (float)smooth(MERC_EDGE, MERC_EDGE + 2.0, a);
    float h = edge + (cap - edge) * k;
    if (lat < 0) h += 120.0f * k * (float)(sin(lon * 0.11) * cos(a * 1.7));   /* a little relief on the ice sheet */
    return h;
}
float terrain_height(double lat, double lon, bool *water, bool *runway) {
    if (fabs(lat) <= MERC_EDGE) return height_merc(lat, lon, water, runway);
    if (water) *water = false;
    if (runway) *runway = false;
    return polar_height(lat, lon);
}
/* the two polar caps: one mesh each, rebuilt now and then as the tiles at their edges sharpen */
enum { CAP_R = 64, CAP_A = 256 };
static struct { GLuint vao, vbo, ibo; int nidx; d3 center; int built; } capm[2];
static void cap_build(int k) {
    double sgn = k == 0 ? 1 : -1;
    d3 c = geo_to_ecef(90 * sgn, 0, polar_height(90 * sgn, 0));
    int nv = (CAP_R + 1) * (CAP_A + 1);
    float *v = malloc(sizeof(float) * 9 * nv);
    for (int r = 0; r <= CAP_R; r++)
        for (int a = 0; a <= CAP_A; a++) {
            double t = (double)r / CAP_R, lat = sgn * (90 - (90 - (MERC_EDGE - 0.01)) * t), lon = -180 + 360.0 * a / CAP_A;
            float h = polar_height(lat, lon);
            d3 p = geo_to_ecef(lat, lon, h);
            v3 e, n, u; enu_basis(lat, lon, &e, &n, &u);
            v3 rel = d2f(dsub(p, c));
            float *o = &v[((size_t)r * (CAP_A + 1) + a) * 9];
            o[0] = rel.x; o[1] = rel.y; o[2] = rel.z; o[3] = u.x; o[4] = u.y; o[5] = u.z; o[6] = (float)t; o[7] = (float)a / CAP_A; o[8] = h;
        }
    int ni = CAP_R * CAP_A * 6;
    uint32_t *ix = malloc(sizeof(uint32_t) * ni), *q = ix;
    for (int r = 0; r < CAP_R; r++)
        for (int a = 0; a < CAP_A; a++) {
            uint32_t i0 = r * (CAP_A + 1) + a, i1 = i0 + 1, i2 = i0 + CAP_A + 1, i3 = i2 + 1;
            if (k == 0) { *q++ = i0; *q++ = i2; *q++ = i1; *q++ = i1; *q++ = i2; *q++ = i3; }
            else { *q++ = i0; *q++ = i1; *q++ = i2; *q++ = i1; *q++ = i3; *q++ = i2; }
        }
    if (!capm[k].vao) {
        glGenVertexArrays(1, &capm[k].vao); glGenBuffers(1, &capm[k].vbo); glGenBuffers(1, &capm[k].ibo);
        glBindVertexArray(capm[k].vao); glBindBuffer(GL_ARRAY_BUFFER, capm[k].vbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void *)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 36, (void *)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 36, (void *)24);
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 36, (void *)32);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, capm[k].ibo);
    }
    glBindVertexArray(capm[k].vao);
    glBindBuffer(GL_ARRAY_BUFFER, capm[k].vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * nv, v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, capm[k].ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint32_t) * ni, ix, GL_STATIC_DRAW);
    glBindVertexArray(0);
    capm[k].nidx = ni; capm[k].center = c; capm[k].built = frameNo;
    free(v); free(ix);
}
static void caps_draw(GLuint prog, d3 cam) {
    double lat, lon, h; ecef_to_geo(cam, &lat, &lon, &h);
    GLint uIce = glGetUniformLocation(prog, "uIce");
    for (int k = 0; k < 2; k++) {
        double near = k == 0 ? lat : -lat;
        if (near < 62 && h < 400000) continue;   /* only when the cap could be in view */
        if (!capm[k].vao || (frameNo - capm[k].built > 300 && near > 70)) cap_build(k);
        v3 off = d2f(dsub(capm[k].center, cam));
        glUniform3f(glGetUniformLocation(prog, "uOffset"), off.x, off.y, off.z);
        glUniform4f(glGetUniformLocation(prog, "uImgST"), 1, 1, 0, 0);
        glUniform1f(glGetUniformLocation(prog, "uHasImg"), 0); glUniform1f(glGetUniformLocation(prog, "uHasMask"), 0); glUniform1f(glGetUniformLocation(prog, "uHasElev"), 0);
        glUniform1f(uIce, k == 0 ? 1.0f : 2.0f);
        glBindVertexArray(capm[k].vao);
        glDrawElements(GL_TRIANGLES, capm[k].nidx, GL_UNSIGNED_INT, 0);
    }
    glUniform1f(uIce, 0);
}

bool terrain_ready(double lat, double lon) {
    int z = elevMax - 3 > 0 ? elevMax - 3 : 0;
    double X = merc_x(lon, z), Y = merc_y(lat, z);
    for (int ez = elevMax; ez >= z; ez--) {
        int d = elevMax - ez;
        ElevTile *e = elev_get(ez, (int)merc_x(lon, elevMax) >> d, (int)merc_y(lat, elevMax) >> d);
        if (e && (e->status == TILE_OK || e->status == TILE_MISSING)) return true;
    }
    (void)X; (void)Y;
    return false;
}

void terrain_stats(int *drawn, int *meshes, int *textures, int *elevs) {
    int t = 0, e = 0;
    for (int i = 0; i < nImg; i++) t += img[i].status == TILE_OK;
    for (int i = 0; i < nElev; i++) e += elev[i].status == TILE_OK;
    *drawn = nDraw; *meshes = nMesh; *textures = t; *elevs = e;
}

/* 2D map: imagery tiles around a point, drawn in screen space with the HUD's map shader */
void terrain_draw_map(GLuint prog, double lat, double lon, int zoom, float W, float H) {
    static GLuint vao, vbo;
    if (!vao) {
        glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, NULL, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    }
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uRes"), W, H);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    double cx = merc_x(lon, zoom), cy = merc_y(lat, zoom);
    float tilePx = 256 * (H / 1080.0f) * 1.2f;
    int nx = (int)(W / tilePx / 2) + 2, ny = (int)(H / tilePx / 2) + 2, n = 1 << zoom;
    for (int dy = -ny; dy <= ny; dy++)
        for (int dx = -nx; dx <= nx; dx++) {
            int tx = (int)floor(cx) + dx, ty = (int)floor(cy) + dy;
            if (ty < 0 || ty >= n) continue;
            int wx = ((tx % n) + n) % n;
            float x0 = W * 0.5f + (float)((tx - cx) * tilePx), y0 = H * 0.5f + (float)((ty - cy) * tilePx), x1 = x0 + tilePx, y1 = y0 + tilePx;
            int iz; ImgTile *t = img_best(zoom, wx, ty, &iz);
            if (!t) { tiles_request(LAYER_IMG, zoom < imgMax ? zoom : imgMax, wx >> (zoom > imgMax ? zoom - imgMax : 0), ty >> (zoom > imgMax ? zoom - imgMax : 0), 50); continue; }
            if (iz < zoom) { int d = zoom - iz; ImgTile *want = img_get(zoom, wx, ty); if (!want) { want = img_new(zoom, wx, ty); if (want) tiles_request(LAYER_IMG, zoom, wx, ty, 40); } (void)d; }
            int d = zoom - iz; float s = 1.0f / (float)(1 << d);
            float u0 = (float)(wx - ((wx >> d) << d)) * s, v0 = (float)(ty - ((ty >> d) << d)) * s;
            float q[24] = {x0, y0, u0, v0, x1, y0, u0 + s, v0, x1, y1, u0 + s, v0 + s, x0, y0, u0, v0, x1, y1, u0 + s, v0 + s, x0, y1, u0, v0 + s};
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof q, q);
            glBindTexture(GL_TEXTURE_2D, t->tex);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
}

/* 3D buildings, taxiway paint and taxiway lights near the camera */
void terrain_draw_city(GLuint prog, d3 cam, float maxDist, int what) {
    GLint uOff = glGetUniformLocation(prog, "uOffset");
    for (int i = 0; i < CAP_VEC; i++) {
        VecC *v = &vec[i];
        if (!v->key || !v->built || (what == 0 ? !v->bvao : !v->tvao)) continue;
        v3 off = d2f(dsub(v->center, cam));
        float d = vlen(off);
        if (d > maxDist) continue;
        if (!sphere_visible(off, 2500)) continue;
        v->last = frameNo;
        glUniform3f(uOff, off.x, off.y, off.z);
        glBindVertexArray(what == 0 ? v->bvao : v->tvao);
        glDrawArrays(GL_TRIANGLES, 0, what == 0 ? v->bn : v->tn);
    }
}
void terrain_lights(d3 cam, float night) {
    if (night < 0.25f) return;
    for (int i = 0; i < CAP_VEC; i++) {
        VecC *v = &vec[i];
        if (!v->key || !v->built || !v->nlights) continue;
        if (dlen(dsub(v->center, cam)) > 9000) continue;
        for (int k = 0; k < v->nlights; k++) fx_light(v->lights[k], 0.22f, 0.15f, 1.0f, 0.35f, 2.2f * smooth(0.25f, 0.6f, night));
    }
}
int terrain_parked(d3 cam, float maxDist, ParkSpot *out, int max) {
    int n = 0;
    for (int i = 0; i < CAP_VEC && n < max; i++) {
        VecC *v = &vec[i];
        if (!v->key || !v->built || !v->npark || dlen(dsub(v->center, cam)) > maxDist + 2000) continue;
        for (int k = 0; k < v->npark && n < max; k++) if (dlen(dsub(v->park[k].ground, cam)) < maxDist) out[n++] = v->park[k];
    }
    return n;
}
