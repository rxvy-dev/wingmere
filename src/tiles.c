/* WINGMERE - background tile downloads, disk cache and decoding.  MIT licence.
 *
 * Two layers stream in Web Mercator tiles: satellite imagery and Terrarium-encoded elevation.
 * Worker threads take the most urgent request, look in the disk cache, otherwise download it
 * with libcurl, save it, decode it, and hand it to the main thread.  When there is no network
 * the same requests are answered with procedural terrain, so the sim still flies. */
#include "wm.h"
#include <curl/curl.h>
#include <zlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"

#define MAXREQ 4096
#define MAXDONE 256
typedef struct { uint64_t key; int layer, z, x, y, state, stamp; float prio; } Req; /* state: 0 free, 1 queued, 2 working */
static Req req[MAXREQ];
static TileData done[MAXDONE];
static int ndone, frame, nthreads, busy, netOk, netFail, consecutiveFail;
static bool quitting, netDown;
static Uint32 netDownSince;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t threads[16];

static uint64_t key_of(int layer, int z, int x, int y) { return ((uint64_t)layer << 60) | ((uint64_t)z << 52) | ((uint64_t)x << 26) | (uint64_t)y; }

/* ------------------------------------------------------------------ procedural fallback terrain */
static float hash2(int x, int y, int s) {
    uint32_t h = (uint32_t)x * 0x27d4eb2du ^ (uint32_t)y * 0x165667b1u ^ (uint32_t)s * 0x9e3779b9u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return (h >> 8) * (1.0f / 16777216.0f);
}
static float vnoise(double x, double y, int s) {
    double fx = floor(x), fy = floor(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(x - fx), ty = (float)(y - fy);
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
    return lerpf(lerpf(hash2(ix, iy, s), hash2(ix + 1, iy, s), tx), lerpf(hash2(ix, iy + 1, s), hash2(ix + 1, iy + 1, s), tx), ty);
}
/* height in metres at a lat/lon: rolling land with mountain ranges and seas */
static float proc_height(double lat, double lon) {
    double x = lon * 1.7, y = lat * 1.7;
    float a = 0, amp = 1, sum = 0;
    for (int o = 0; o < 9; o++) { a += amp * vnoise(x, y, o); sum += amp; amp *= 0.5f; x *= 2.03; y *= 2.03; }
    a /= sum;
    float ridge = 1 - fabsf(vnoise(lon * 4.1, lat * 4.1, 77) * 2 - 1);
    float h = (a - 0.42f) * 5200 + ridge * ridge * 900 * smooth(0.45f, 0.65f, a);
    return h < 0 ? h * 0.2f : h;
}
static void proc_tile(TileData *t) {
    int N = 256;
    t->w = t->h = N;
    if (t->layer == LAYER_ELEV) t->height = malloc(sizeof(float) * N * N);
    else t->rgba = malloc(4 * N * N);
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            double lon = merc_lon(t->x + (i + 0.5) / N, t->z), lat = merc_lat(t->y + (j + 0.5) / N, t->z);
            float h = proc_height(lat, lon);
            if (t->layer == LAYER_ELEV) { t->height[j * N + i] = h; continue; }
            float n = vnoise(lon * 600, lat * 600, 5) * 0.6f + vnoise(lon * 4000, lat * 4000, 6) * 0.4f;
            float r, g, b;
            if (h <= 0) { r = 0.05f; g = 0.12f; b = 0.20f; }
            else if (h > 2600) { r = g = b = 0.85f; }
            else {
                float dry = vnoise(lon * 30, lat * 30, 9);
                r = lerpf(0.20f, 0.42f, dry); g = lerpf(0.30f, 0.36f, dry); b = lerpf(0.14f, 0.24f, dry);
                float rock = smooth(1400, 2400, h);
                r = lerpf(r, 0.45f, rock); g = lerpf(g, 0.42f, rock); b = lerpf(b, 0.40f, rock);
            }
            r *= 0.75f + 0.5f * n; g *= 0.75f + 0.5f * n; b *= 0.75f + 0.5f * n;
            unsigned char *p = &t->rgba[(j * N + i) * 4];
            p[0] = (unsigned char)clampf(r * 255, 0, 255); p[1] = (unsigned char)clampf(g * 255, 0, 255); p[2] = (unsigned char)clampf(b * 255, 0, 255); p[3] = 255;
        }
    t->status = TILE_OK;
}

/* ------------------------------------------------------------------ disk cache + network */
static void mkdirs(const char *path) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static void make_url(char *out, size_t n, const char *tmpl, int z, int x, int y) {
    char *o = out, *end = out + n - 12;
    for (const char *s = tmpl; *s && o < end; s++) {
        if (!strncmp(s, "{z}", 3)) { o += sprintf(o, "%d", z); s += 2; }
        else if (!strncmp(s, "{x}", 3)) { o += sprintf(o, "%d", x); s += 2; }
        else if (!strncmp(s, "{y}", 3)) { o += sprintf(o, "%d", y); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
typedef struct { unsigned char *p; size_t n, cap; } Buf;
static size_t on_data(void *ptr, size_t sz, size_t nm, void *ud) {
    Buf *b = ud; size_t n = sz * nm;
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, ptr, n); b->n += n;
    return n;
}
static bool read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    b->p = malloc(n > 0 ? n : 1); b->n = fread(b->p, 1, n, f); b->cap = n;
    fclose(f);
    return b->n > 0;
}
static volatile int vecState;   /* 0 resolving the tile URL, 1 ready, -1 unavailable */
static void decode(TileData *t, Buf *b) {
    if (t->layer == LAYER_VEC) {
        unsigned char *p = b->p; size_t n = b->n; unsigned char *tmp = NULL;
        if (n > 2 && p[0] == 0x1f && p[1] == 0x8b) {                 /* gzip inside the file */
            z_stream zs = {0};
            if (inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK) {
                size_t cap = n * 6 + 4096; tmp = malloc(cap);
                zs.next_in = p; zs.avail_in = (uInt)n;
                int r;
                do {
                    if (zs.total_out >= cap) { cap *= 2; tmp = realloc(tmp, cap); }
                    zs.next_out = tmp + zs.total_out; zs.avail_out = (uInt)(cap - zs.total_out);
                    r = inflate(&zs, Z_NO_FLUSH);
                } while (r == Z_OK);
                n = zs.total_out; p = tmp;
                inflateEnd(&zs);
                if (r != Z_STREAM_END) n = 0;
            }
        }
        t->vec = n ? mvt_decode(p, n, t->z, t->x, t->y) : NULL;
        t->status = t->vec ? TILE_OK : TILE_MISSING;
        free(tmp);
        return;
    }
    int w, h, comp;
    unsigned char *px = stbi_load_from_memory(b->p, (int)b->n, &w, &h, &comp, 4);
    if (!px) { t->status = TILE_FAILED; return; }
    t->w = w; t->h = h; t->status = TILE_OK;
    if (t->layer == LAYER_IMG) { t->rgba = px; return; }
    t->height = malloc(sizeof(float) * w * h);
    for (int i = 0; i < w * h; i++) {   /* Terrarium: height = R*256 + G + B/256 - 32768 */
        float v = px[i * 4] * 256.0f + px[i * 4 + 1] + px[i * 4 + 2] / 256.0f - 32768.0f;
        t->height[i] = v < -11000 ? 0 : v;
    }
    stbi_image_free(px);
}
static void fetch(TileData *t, CURL *c) {
    if (t->layer == LAYER_VEC) {
        if (S.offline || vecState < 0) { t->status = TILE_MISSING; return; }
        if (vecState == 0) { t->status = TILE_FAILED; return; }       /* try again once the URL is known */
    }
    const char *tmpl = t->layer == LAYER_ELEV ? S.elevation_url : t->layer == LAYER_VEC ? S.vector_url : S.imagery_url;
    bool procedural = t->layer != LAYER_VEC && (S.offline || !tmpl[0] || !strcmp(tmpl, "procedural"));
    pthread_mutex_lock(&mu); bool down = netDown; pthread_mutex_unlock(&mu);
    char path[1024], miss[1040], dir[1024];
    uint32_t uh = 2166136261u;                    /* each imagery source gets its own cache folder */
    for (const char *c = tmpl; *c; c++) uh = (uh ^ (uint8_t)*c) * 16777619u;
    if (t->layer == LAYER_ELEV) snprintf(dir, sizeof dir, "%s/elevation/%d/%d", S.cache_dir, t->z, t->x);
    else if (t->layer == LAYER_VEC) snprintf(dir, sizeof dir, "%s/vector/%d/%d", S.cache_dir, t->z, t->x);
    else snprintf(dir, sizeof dir, "%s/imagery-%08x/%d/%d", S.cache_dir, uh, t->z, t->x);
    snprintf(path, sizeof path, "%s/%d.tile", dir, t->y);
    snprintf(miss, sizeof miss, "%s/%d.none", dir, t->y);
    Buf b = {0};
    if (!procedural && read_file(path, &b)) { decode(t, &b); free(b.p); if (t->status == TILE_OK) return; }
    if (!procedural && access(miss, F_OK) == 0) { t->status = TILE_MISSING; return; }
    if (t->layer == LAYER_VEC && (down || !c)) { t->status = TILE_FAILED; return; }
    if (procedural || down || !c) { proc_tile(t); return; }
    char url[1024];
    make_url(url, sizeof url, tmpl, t->z, t->x, t->y);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    long code = 0;
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    pthread_mutex_lock(&mu);
    if (rc == CURLE_OK && (code == 200 || code == 404 || code == 204 || code == 0)) { netOk++; consecutiveFail = 0; }
    else { netFail++; if (++consecutiveFail >= 12 && netOk == 0) { netDown = true; netDownSince = SDL_GetTicks(); fprintf(stderr, "wingmere: tile servers unreachable (%s) - using procedural terrain\n", curl_easy_strerror(rc)); } }
    pthread_mutex_unlock(&mu);
    if (rc == CURLE_OK && (code == 200 || code == 0) && b.n > 0) {
        decode(t, &b);
        if (t->status == TILE_OK) {
            mkdirs(dir);
            char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.%lu", path, (unsigned long)pthread_self());
            FILE *f = fopen(tmp, "wb");
            if (f) { fwrite(b.p, 1, b.n, f); fclose(f); rename(tmp, path); }
        }
    } else if (rc == CURLE_OK && (code == 404 || code == 204)) {
        mkdirs(dir);
        FILE *f = fopen(miss, "wb"); if (f) fclose(f);
        t->status = TILE_MISSING;
    } else t->status = TILE_FAILED;
    free(b.p);
}

static void *worker(void *arg) {
    (void)arg;
    CURL *c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(c, CURLOPT_USERAGENT, S.user_agent);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    }
    for (;;) {
        pthread_mutex_lock(&mu);
        int pick = -1;
        while (!quitting) {
            float best = -1e30f;
            if (ndone < MAXDONE)
                for (int i = 0; i < MAXREQ; i++) {
                    if (req[i].state != 1) continue;
                    if (frame - req[i].stamp > 90) { req[i].state = 0; continue; } /* nobody wants it any more */
                    if (req[i].prio > best) { best = req[i].prio; pick = i; }
                }
            if (pick >= 0) break;
            pthread_cond_wait(&cv, &mu);
        }
        if (quitting) { pthread_mutex_unlock(&mu); break; }
        req[pick].state = 2; busy++;
        TileData t = {req[pick].layer, req[pick].z, req[pick].x, req[pick].y, 0, 0, 0, NULL, NULL, NULL};
        pthread_mutex_unlock(&mu);
        fetch(&t, c);
        pthread_mutex_lock(&mu);
        busy--;
        req[pick].state = 0;
        if (ndone < MAXDONE) done[ndone++] = t; else { free(t.rgba); free(t.height); vectile_free(t.vec); }
        pthread_mutex_unlock(&mu);
    }
    if (c) curl_easy_cleanup(c);
    return NULL;
}

/* OpenFreeMap publishes its tile URL (it changes with each planet build) in a TileJSON document */
static void *resolve_vector(void *arg) {
    (void)arg;
    if (S.vector_url[0]) { vecState = 1; return NULL; }
    if (S.offline || !S.vector_tilejson[0]) { vecState = -1; return NULL; }
    CURL *c = curl_easy_init();
    Buf b = {0};
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, S.vector_tilejson);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        curl_easy_setopt(c, CURLOPT_USERAGENT, S.user_agent);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK && b.n) {
            b.p = realloc(b.p, b.n + 1); b.p[b.n] = 0;
            char *t = strstr((char *)b.p, "\"tiles\"");
            char *q = t ? strchr(t, '[') : NULL;
            q = q ? strchr(q, '"') : NULL;
            char *e = q ? strchr(q + 1, '"') : NULL;
            if (e && e - q - 1 < (long)sizeof S.vector_url) {
                memcpy(S.vector_url, q + 1, e - q - 1); S.vector_url[e - q - 1] = 0;
                fprintf(stderr, "wingmere: map data from %s\n", S.vector_url);
                vecState = 1; free(b.p); return NULL;
            }
        }
    }
    free(b.p);
    fprintf(stderr, "wingmere: no OpenStreetMap vector tiles (TileJSON unreachable) - no buildings or coastlines this time\n");
    vecState = -1;
    return NULL;
}
void tiles_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_t rt; pthread_create(&rt, NULL, resolve_vector, NULL); pthread_detach(rt);
    mkdirs(S.cache_dir);
    nthreads = S.threads < 1 ? 1 : (S.threads > 16 ? 16 : S.threads);
    for (int i = 0; i < nthreads; i++) pthread_create(&threads[i], NULL, worker, NULL);
}
void tiles_request(int layer, int z, int x, int y, float priority) {
    uint64_t k = key_of(layer, z, x, y);
    pthread_mutex_lock(&mu);
    int freeSlot = -1;
    uint32_t h = (uint32_t)(k ^ (k >> 29) ^ (k >> 47)) * 2654435761u;
    for (int n = 0; n < 64; n++) {                    /* small probe window keeps this cheap */
        int i = (h + n) & (MAXREQ - 1);
        if (req[i].state && req[i].key == k) { req[i].stamp = frame; if (req[i].state == 1) req[i].prio = priority; pthread_mutex_unlock(&mu); return; }
        if (!req[i].state && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot >= 0) {
        Req *r = &req[freeSlot];
        r->key = k; r->layer = layer; r->z = z; r->x = x; r->y = y; r->state = 1; r->stamp = frame; r->prio = priority;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
}
int tiles_poll(TileData *out, int max) {
    pthread_mutex_lock(&mu);
    int n = ndone < max ? ndone : max;
    memcpy(out, done, sizeof(TileData) * n);
    memmove(done, done + n, sizeof(TileData) * (ndone - n));
    ndone -= n;
    if (n) pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return n;
}
void tiles_frame(void) {
    pthread_mutex_lock(&mu);
    frame++;
    if (netDown && SDL_GetTicks() - netDownSince > 60000) { netDown = false; consecutiveFail = 0; } /* try the network again */
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}
void tiles_stats(int *queued, int *b, int *ok, int *fail) {
    pthread_mutex_lock(&mu);
    int q = 0;
    for (int i = 0; i < MAXREQ; i++) q += req[i].state == 1;
    *queued = q; *b = busy; *ok = netOk; *fail = netDown ? -1 : netFail;
    pthread_mutex_unlock(&mu);
}
void tiles_shutdown(void) {
    pthread_mutex_lock(&mu); quitting = true; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);
    curl_global_cleanup();
}
