/* WINGMERE - plane tracker: pick any real aircraft around you and see who it is, where it came from,
 * where it is going, how far along it is, and a photo of the actual airframe.  MIT licence.
 *
 * Live position and state come from the ADS-B feed (traffic.c).  Route, airline, owner and photo come
 * from adsbdb (api.adsbdb.com), looked up once per aircraft on a background thread. */
#include "wm.h"
#include <curl/curl.h>
#include <pthread.h>
#include "../third_party/stb_image.h"

TrackInfo g_track;
bool g_trackPanel, g_trackTyping;
char g_trackSearch[24];
static int scroll;

#define TRED 0.86f, 0.09f, 0.08f
#define TWHITE 0.94f, 0.93f, 0.91f
#define TGREY 0.62f, 0.62f, 0.64f
#define TAMBER 1.0f, 0.68f, 0.12f
#define TGREEN 0.35f, 0.9f, 0.45f

/* ------------------------------------------------------------------ JSON helpers (just enough for adsbdb) */
static const char *jws(const char *s) { while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++; return s; }
static const char *jskip(const char *s) {
    s = jws(s);
    if (*s == '"') { s++; while (*s && *s != '"') { if (*s == '\\' && s[1]) s++; s++; } return *s ? s + 1 : s; }
    if (*s == '{' || *s == '[') {
        int depth = 0;
        do {
            if (*s == '"') { s = jskip(s); continue; }
            if (*s == '{' || *s == '[') depth++;
            else if (*s == '}' || *s == ']') depth--;
            s++;
        } while (*s && depth > 0);
        return s;
    }
    while (*s && *s != ',' && *s != '}' && *s != ']') s++;
    return s;
}
/* copy a JSON string, folding accented Latin letters to plain ones (the HUD font is ASCII) */
static const char *jstr(const char *s, char *out, size_t n) {
    static const char *FOLD = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   /* U+00C0..U+00FF */
    s = jws(s);
    size_t k = 0;
    if (*s != '"') { out[0] = 0; if (!strncmp(s, "null", 4)) return s + 4; return jskip(s); }
    s++;
    while (*s && *s != '"') {
        unsigned char c = (unsigned char)*s;
        char o = 0;
        if (c == '\\' && s[1]) { s++; o = *s == 'n' ? ' ' : *s; if (*s == 'u') { s += 4; o = '?'; } }
        else if (c < 0x80) o = (char)c;
        else if ((c == 0xC3) && (unsigned char)s[1] >= 0x80) { o = FOLD[((unsigned char)s[1] & 0x3F) + 0]; s++; }
        else if ((c & 0xE0) == 0xC0) { o = '?'; s++; }
        else if ((c & 0xF0) == 0xE0) { o = '?'; s += 2; }
        else if ((c & 0xF8) == 0xF0) { o = '?'; s += 3; }
        if (o && k + 1 < n) out[k++] = o;
        if (*s) s++;
    }
    out[k] = 0;
    return *s ? s + 1 : s;
}
/* the value of "key" among the top-level members of the object at s ('{'), or NULL */
static const char *jget(const char *s, const char *key) {
    if (!s) return NULL;
    s = jws(s);
    if (*s != '{') return NULL;
    s++;
    while (*s) {
        s = jws(s);
        if (*s == ',') { s++; continue; }
        if (*s == '}' || !*s) return NULL;
        char k[48]; s = jstr(s, k, sizeof k);
        s = jws(s); if (*s == ':') s++;
        s = jws(s);
        if (!strcmp(k, key)) return s;
        s = jskip(s);
    }
    return NULL;
}
static void jgets(const char *obj, const char *key, char *out, size_t n) { const char *v = jget(obj, key); if (v) jstr(v, out, n); else out[0] = 0; }
static double jgetn(const char *obj, const char *key) { const char *v = jget(obj, key); return v && *v != 'n' ? strtod(v, NULL) : 0; }
static bool jairport(const char *v, RouteAirport *a) {
    memset(a, 0, sizeof *a);
    if (!v || *v != '{') return false;
    jgets(v, "icao_code", a->icao, sizeof a->icao); jgets(v, "iata_code", a->iata, sizeof a->iata);
    jgets(v, "name", a->name, sizeof a->name); jgets(v, "municipality", a->city, sizeof a->city);
    jgets(v, "country_name", a->country, sizeof a->country);
    a->lat = jgetn(v, "latitude"); a->lon = jgetn(v, "longitude"); a->elev = (float)jgetn(v, "elevation") * FT;
    return a->icao[0] || a->iata[0];
}

/* ------------------------------------------------------------------ the lookup thread */
typedef struct { char *p; size_t n, cap; } XB;
static size_t xdata(void *ptr, size_t sz, size_t nm, void *ud) {
    XB *b = ud; size_t n = sz * nm;
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, ptr, n); b->n += n; b->p[b->n] = 0;
    return n;
}
static pthread_mutex_t xmu = PTHREAD_MUTEX_INITIALIZER;
static char wantHex[8], wantCall[12]; static bool wantNew;
static TrackInfo result; static unsigned char *resultPx; static bool resultNew;

static bool fetch(CURL *c, const char *url, XB *b, long *code) {
    b->n = 0; if (b->p) b->p[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    *code = 0;
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, code);
    return rc == CURLE_OK && b->p;
}
static void *xworker(void *arg) {
    (void)arg;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, xdata);
    curl_easy_setopt(c, CURLOPT_USERAGENT, S.user_agent);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    XB b = {0};
    for (;;) {
        pthread_mutex_lock(&xmu);
        bool go = wantNew; char hex[8], call[12];
        memcpy(hex, wantHex, sizeof hex); memcpy(call, wantCall, sizeof call); wantNew = false;
        pthread_mutex_unlock(&xmu);
        if (!go) { SDL_Delay(100); continue; }
        TrackInfo r; memset(&r, 0, sizeof r);
        snprintf(r.hex, sizeof r.hex, "%s", hex); snprintf(r.callsign, sizeof r.callsign, "%s", call);
        r.state = 3;
        char url[700], *o = url;
        for (const char *q = S.tracker_info_url; *q && o < url + sizeof url - 16; q++) {
            if (!strncmp(q, "{hex}", 5)) { o += sprintf(o, "%s", hex); q += 4; }
            else if (!strncmp(q, "{callsign}", 10)) { o += sprintf(o, "%s", call[0] ? call : "-"); q += 9; }
            else *o++ = *q;
        }
        *o = 0;
        long code; char photo[400] = "";
        if (fetch(c, url, &b, &code) && code == 200) {
            const char *resp = jget(b.p, "response");
            const char *ac = jget(resp, "aircraft"), *fr = jget(resp, "flightroute");
            if (ac && *ac == '{') {
                jgets(ac, "type", r.type, sizeof r.type); jgets(ac, "manufacturer", r.manufacturer, sizeof r.manufacturer);
                jgets(ac, "icao_type", r.icaoType, sizeof r.icaoType); jgets(ac, "registration", r.reg, sizeof r.reg);
                jgets(ac, "registered_owner", r.owner, sizeof r.owner); jgets(ac, "registered_owner_country_name", r.ownerCountry, sizeof r.ownerCountry);
                jgets(ac, "url_photo", photo, sizeof photo);
                if (!photo[0]) jgets(ac, "url_photo_thumbnail", photo, sizeof photo);
                r.state = 2;
            }
            if (fr && *fr == '{') {
                const char *al = jget(fr, "airline");
                if (al && *al == '{') { jgets(al, "name", r.airline, sizeof r.airline); jgets(al, "country", r.airlineCountry, sizeof r.airlineCountry); }
                r.hasRoute = jairport(jget(fr, "origin"), &r.from) && jairport(jget(fr, "destination"), &r.to);
                r.state = 2;
            }
        }
        /* a photo of this very airframe: Planespotters first (with the photographer's name), else adsbdb's */
        char by[64] = "";
        if (S.photo_url[0]) {
            char pu[600], *q2 = pu;
            for (const char *q = S.photo_url; *q && q2 < pu + sizeof pu - 12; q++) {
                if (!strncmp(q, "{hex}", 5)) { q2 += sprintf(q2, "%s", hex); q += 4; } else *q2++ = *q;
            }
            *q2 = 0;
            if (fetch(c, pu, &b, &code) && code == 200) {
                const char *ph = jget(b.p, "photos");
                if (ph && *ph == '[') {
                    const char *first = jws(ph + 1);
                    if (*first == '{') {
                        const char *tl = jget(first, "thumbnail_large");
                        char src[400] = "", who[48] = "";
                        if (tl) jgets(tl, "src", src, sizeof src);
                        if (!src[0]) { const char *th = jget(first, "thumbnail"); if (th) jgets(th, "src", src, sizeof src); }
                        jgets(first, "photographer", who, sizeof who);
                        if (src[0]) {
                            snprintf(photo, sizeof photo, "%s", src);
                            const char *hs = strstr(pu, "://"); hs = hs ? hs + 3 : pu;
                            const char *site = strstr(hs, "planespotters") ? "PLANESPOTTERS.NET" : "";
                            snprintf(by, sizeof by, "%.40s%s%s", who[0] ? who : "", who[0] && site[0] ? " / " : "", site);
                        }
                    }
                }
            }
        }
        unsigned char *px = NULL;
        if (photo[0] && fetch(c, photo, &b, &code) && code == 200 && b.n > 100) {
            int w, h, comp;
            unsigned char *img = stbi_load_from_memory((unsigned char *)b.p, (int)b.n, &w, &h, &comp, 4);
            if (img && w > 0 && h > 0) {
                int f = 1; while (w / f > 720) f++;           /* shrink big photos by whole steps */
                int ow = w / f, oh = h / f;
                px = malloc((size_t)ow * oh * 4);
                for (int y = 0; y < oh; y++)
                    for (int x = 0; x < ow; x++)
                        for (int k = 0; k < 4; k++) {
                            int sum = 0;
                            for (int j = 0; j < f; j++) for (int i = 0; i < f; i++) sum += img[(((size_t)(y * f + j) * w) + x * f + i) * 4 + k];
                            px[((size_t)y * ow + x) * 4 + k] = (unsigned char)(sum / (f * f));
                        }
                r.pw = ow; r.ph = oh;
                const char *hs = strstr(photo, "://"); hs = hs ? hs + 3 : photo;
                if (by[0]) { snprintf(r.photoBy, sizeof r.photoBy, "%s", by); hs = NULL; }
                if (hs) {
                size_t hl = strcspn(hs, "/:"); if (hl >= sizeof r.photoBy) hl = sizeof r.photoBy - 1;
                memcpy(r.photoBy, hs, hl); r.photoBy[hl] = 0;
                if (!strncmp(r.photoBy, "www.", 4)) memmove(r.photoBy, r.photoBy + 4, strlen(r.photoBy) - 3);
                }
            }
            if (img) stbi_image_free(img);
        }
        pthread_mutex_lock(&xmu);
        if (!strcmp(hex, wantHex)) { free(resultPx); result = r; resultPx = px; px = NULL; resultNew = true; }
        pthread_mutex_unlock(&xmu);
        free(px);
    }
    return NULL;
}
void tracker_init(void) {
    static bool started;
    if (started || !S.tracker_info_url[0]) return;
    started = true;
    pthread_t t; pthread_create(&t, NULL, xworker, NULL); pthread_detach(t);
}
static void request(const char *hex, const char *call) {
    pthread_mutex_lock(&xmu);
    snprintf(wantHex, sizeof wantHex, "%s", hex); snprintf(wantCall, sizeof wantCall, "%s", call);
    wantNew = hex[0] && !S.offline;
    pthread_mutex_unlock(&xmu);
}
void tracker_select(const char *hex) {
    if (!hex) hex = "";
    if (!strcasecmp(hex, g_track.hex)) return;
    if (g_track.tex) { GLuint t = g_track.tex; glDeleteTextures(1, &t); }
    memset(&g_track, 0, sizeof g_track);
    snprintf(g_track.hex, sizeof g_track.hex, "%s", hex);
    traffic_track(hex);
    if (!hex[0]) { request("", ""); return; }
    int i = traffic_find(hex);
    const char *call = i >= 0 && strcmp(g_traffic[i].callsign, g_traffic[i].hex) && strcmp(g_traffic[i].callsign, g_traffic[i].reg) ? g_traffic[i].callsign : "";
    snprintf(g_track.callsign, sizeof g_track.callsign, "%s", call);
    g_track.state = S.offline ? 3 : 1;
    request(hex, call);
}
int tracker_index(void) { return traffic_find(g_track.hex); }
void tracker_frame(void) {
    if (!g_track.hex[0]) return;
    pthread_mutex_lock(&xmu);
    if (resultNew && !strcasecmp(result.hex, g_track.hex)) {
        resultNew = false;
        unsigned keep = g_track.tex;
        g_track = result; g_track.tex = keep;
        if (resultPx) {
            if (g_track.tex) { GLuint t = g_track.tex; glDeleteTextures(1, &t); }
            GLuint t; glGenTextures(1, &t);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, result.pw, result.ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, resultPx);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_track.tex = t;
            free(resultPx); resultPx = NULL;
        }
    }
    pthread_mutex_unlock(&xmu);
    /* the world feed carries no aircraft type: take it from the lookup, so the right airframe is drawn */
    int i = tracker_index();
    if (i >= 0 && g_track.state == 2 && g_track.icaoType[0] && !g_traffic[i].type[0]) {
        snprintf(g_traffic[i].type, sizeof g_traffic[i].type, "%s", g_track.icaoType);
        g_traffic[i].atype = airframe_find(g_traffic[i].type); g_traffic[i].guessed = false;
        if (g_track.reg[0] && !g_traffic[i].reg[0]) snprintf(g_traffic[i].reg, sizeof g_traffic[i].reg, "%s", g_track.reg);
    }
    /* the callsign often arrives a report or two after the aircraft does: look the route up again then */
    if (i >= 0 && !g_track.callsign[0] && g_track.state != 1) {
        Traffic *t = &g_traffic[i];
        if (strcmp(t->callsign, t->hex) && strcmp(t->callsign, t->reg) && t->callsign[0]) {
            snprintf(g_track.callsign, sizeof g_track.callsign, "%s", t->callsign);
            g_track.state = 1;
            request(g_track.hex, t->callsign);
        }
    }
}

/* ------------------------------------------------------------------ what it is doing */
const char *tracker_phase(const Traffic *t) {
    float kt = t->gs / KT, fpm = t->vrate / FT * 60, ft = (float)(t->alt / FT);
    if (t->ground) return kt < 3 ? "PARKED" : (kt < 45 ? "TAXIING" : "TAKE-OFF / LANDING ROLL");
    bool nearDest = false;
    if (g_track.hasRoute && !strcasecmp(t->hex, g_track.hex)) nearDest = geo_dist(t->lat, t->lon, g_track.to.lat, g_track.to.lon) < 90000;
    if (fpm > 400) return ft < 4000 ? "DEPARTING" : "CLIMBING";
    if (fpm < -400) return nearDest || ft < 9000 ? "ON APPROACH" : "DESCENDING";
    if (ft > 18000) return "CRUISING";
    return nearDest ? "ON APPROACH" : "LEVEL";
}
static float by_d[MAX_TRAFFIC];
static int cmp_d(const void *a, const void *b) { float x = by_d[*(const int *)a], y = by_d[*(const int *)b]; return x < y ? -1 : x > y; }
static bool has(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return true;
    return false;
}
static bool matches(const Traffic *t, const char *q) {
    return !q[0] || has(t->callsign, q) || has(t->reg, q) || has(t->type, q) || has(t->hex, q) || has(t->country, q) || (t->atype >= 0 && !t->guessed && has(airframe_name(t->atype), q));
}
/* every aircraft in the world, nearest first, filtered by the search box; re-sorted twice a second */
static int sortIdx[MAX_TRAFFIC], nSorted, nNear;
static void sorted_refresh(double lat, double lon, bool force) {
    static Uint32 last; static char lastQ[24]; static int lastN;
    Uint32 now = SDL_GetTicks();
    if (!force && now - last < 500 && !strcmp(lastQ, g_trackSearch) && lastN == g_ntraffic) return;
    last = now; snprintf(lastQ, sizeof lastQ, "%s", g_trackSearch); lastN = g_ntraffic;
    nSorted = 0; nNear = 0;
    for (int i = 0; i < g_ntraffic; i++) {
        by_d[i] = (float)geo_dist(lat, lon, g_traffic[i].lat, g_traffic[i].lon);
        if (by_d[i] < 150000) nNear++;
        if (matches(&g_traffic[i], g_trackSearch)) sortIdx[nSorted++] = i;
    }
    qsort(sortIdx, nSorted, sizeof(int), cmp_d);
}
static int sorted(double lat, double lon, int *idx) {
    sorted_refresh(lat, lon, true);
    memcpy(idx, sortIdx, sizeof(int) * nSorted);
    return nSorted;
}
void tracker_scroll(int rows) { scroll += rows; if (scroll < 0) scroll = 0; }
void tracker_cycle(double lat, double lon, int dir) {
    if (!g_ntraffic) return;
    static int idx[MAX_TRAFFIC];
    int n = sorted(lat, lon, idx), cur = -1, sel = tracker_index();
    if (!n) return;
    for (int k = 0; k < n; k++) if (idx[k] == sel) cur = k;
    int nk = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
    tracker_select(g_traffic[idx[nk]].hex);
}

/* ------------------------------------------------------------------ drawing */
static void fmt_thousands(char *out, size_t n, long v) {
    char t[32]; snprintf(t, sizeof t, "%ld", v < 0 ? -v : v);
    int len = (int)strlen(t), k = 0; char r[40];
    if (v < 0) r[k++] = '-';
    for (int i = 0; i < len; i++) { r[k++] = t[i]; if ((len - i - 1) % 3 == 0 && i < len - 1) r[k++] = ','; }
    r[k] = 0; snprintf(out, n, "%s", r);
}
static struct { float x, y, w, h; char hex[8]; } rows[24]; static int nRows;
static float listBox[4];
bool tracker_over_list(float x, float y) { return listBox[2] > 0 && x >= listBox[0] && x <= listBox[0] + listBox[2] && y >= listBox[1] && y <= listBox[1] + listBox[3]; }
bool tracker_click(float x, float y) {
    if (!g_trackPanel) return false;
    if (tracker_over_list(x, y) && y < listBox[1] + 100 * (listBox[3] > 0 ? g_h / 1080.0f : 1) && y > listBox[1] + 70 * (g_h / 1080.0f)) { g_trackTyping = true; SDL_StartTextInput(); return true; }
    for (int i = 0; i < nRows; i++)
        if (x >= rows[i].x && x <= rows[i].x + rows[i].w && y >= rows[i].y && y <= rows[i].y + rows[i].h) {
            if (!strcasecmp(rows[i].hex, g_track.hex)) tracker_select(""); else tracker_select(rows[i].hex);
            audio_event(0);
            return true;
        }
    return false;
}
static void panel_bg(float x, float y, float w, float h) { hud_rect(x, y, w, h, 0.02f, 0.02f, 0.025f, 0.86f); hud_rect(x, y, w, 2, TRED, 0.9f); }
static void kv(float x, float y, float s, const char *k, const char *v, float r, float g, float b) {
    hud_text(x, y, 9 * s, k, TGREY, 0.95f, 0);
    hud_text(x, y + 14 * s, 12 * s, v, r, g, b, 1, 0);
}
static d3 traffic_pos(const Traffic *t, double lat, double lon, double alt) {
    (void)t;
    if (alt < -1e8) alt = terrain_height(lat, lon, NULL, NULL) + 3;
    return geo_to_ecef(lat, lon, alt);
}

void tracker_draw(const Camera *cam, float W, float H, float s, double plat, double plon, bool watching) {
    char b[160], c2[64];
    int ti = tracker_index();
    Traffic *t = ti >= 0 ? &g_traffic[ti] : NULL;
    float blink = fmodf(SDL_GetTicks() * 0.001f, 0.8f) < 0.45f ? 1.0f : 0.3f;
    /* ---- in the world: where it has been, and a box around it */
    if (t) {
        float px = 0, py = 0; bool have = false;
        for (int k = 0; k <= t->ntrail; k++) {
            d3 p;
            if (k < t->ntrail) { int j = (t->trailHead - t->ntrail + k + TRAIL * 2) % TRAIL; p = traffic_pos(t, t->trail[j][0], t->trail[j][1], t->trail[j][2]); }
            else p = traffic_pos(t, t->lat, t->lon, t->alt);
            float sx, sy;
            bool ok = world_to_screen(p, cam, &sx, &sy) && sx > -W && sx < 2 * W && sy > -H && sy < 2 * H;
            if (ok && have) hud_line(px, py, sx, sy, 2.2f * s, TAMBER, 0.25f + 0.6f * (float)k / (t->ntrail + 1));
            px = sx; py = sy; have = ok;
        }
        d3 p = geo_to_ecef(t->lat, t->lon, t->alt);
        float sx, sy, d = (float)dlen(dsub(p, cam->pos));
        bool on = world_to_screen(p, cam, &sx, &sy) && sx > 0 && sy > 0 && sx < W && sy < H;
        if (on && !watching) {
            float r = fmaxf(16 * s, 40.0f / fmaxf(d, 1) * H / (2 * tanf(cam->fov * 0.5f)) * 0.6f), l = r * 0.45f;
            r = fminf(r, H * 0.25f);
            for (int cx = -1; cx <= 1; cx += 2) for (int cy = -1; cy <= 1; cy += 2) {
                float x0 = sx + cx * r, y0 = sy + cy * r;
                hud_line(x0, y0, x0 - cx * l, y0, 2.2f * s, TRED, 0.95f);
                hud_line(x0, y0, x0, y0 - cy * l, 2.2f * s, TRED, 0.95f);
            }
            snprintf(b, sizeof b, "%s  %.1f NM", t->callsign, d / 1852);
            hud_text(sx, sy - r - 8 * s, 12 * s, b, TRED, 1, 1);
        } else if (!on && !watching) {
            /* off screen: an arrow on the edge pointing at it */
            v3 rel = d2f(dsub(p, cam->pos)), rt = vnorm(vcross(cam->fwd, cam->up)), up = vcross(rt, cam->fwd);
            float ax = vdot(rel, rt), ay = -vdot(rel, up);
            float L = sqrtf(ax * ax + ay * ay); if (L < 1e-3f) { ax = 0; ay = -1; L = 1; }
            ax /= L; ay /= L;
            float m = 46 * s, ex = W / 2 + ax * (W / 2 - m), ey = H / 2 + ay * (H / 2 - m);
            float k = fminf((W / 2 - m) / fmaxf(fabsf(ax) * (W / 2 - m), 1e-3f), (H / 2 - m) / fmaxf(fabsf(ay) * (H / 2 - m), 1e-3f));
            ex = W / 2 + ax * fminf(W / 2 - m, (W / 2 - m) * k * fabsf(ax)) ; ey = H / 2 + ay * fminf(H / 2 - m, (H / 2 - m) * k * fabsf(ay));
            float z = 14 * s;
            hud_tri(ex + ax * z, ey + ay * z, ex - ax * z * 0.6f - ay * z * 0.7f, ey - ay * z * 0.6f + ax * z * 0.7f, ex - ax * z * 0.6f + ay * z * 0.7f, ey - ay * z * 0.6f - ax * z * 0.7f, TRED, 0.95f);
            snprintf(b, sizeof b, "%s %.0f NM", t->callsign, d / 1852);
            hud_text(ex - ax * 30 * s, ey - ay * 30 * s + 4 * s, 11 * s, b, TRED, 1, 1);
        }
    }
    /* ---- the list: every aircraft in the world, nearest first */
    nRows = 0;
    if (g_trackPanel) {
        sorted_refresh(plat, plon, false);
        int n = nSorted, show = 18;
        if (scroll > n - show) scroll = n - show > 0 ? n - show : 0;
        float x = 20 * s, y = 120 * s, w = 440 * s, rh = 23 * s, h = 112 * s + show * rh + 26 * s;
        panel_bg(x, y, w, h);
        hud_title(x + 14 * s, y + 32 * s, 18 * s, "PLANE TRACKER", TRED, 1, 0);
        float age; bool wok; int world = traffic_world(&age, &wok);
        char n1[24], n2[24]; fmt_thousands(n1, sizeof n1, g_ntraffic); fmt_thousands(n2, sizeof n2, nNear);
        snprintf(b, sizeof b, "%s IN THE WORLD   %s NEAR YOU", n1, n2);
        hud_text(x + 14 * s, y + 50 * s, 10 * s, b, TWHITE, 0.95f, 0);
        if (!S.traffic || S.offline) snprintf(b, sizeof b, "LIVE TRAFFIC IS OFF (SETTINGS)");
        else if (!S.traffic_global) snprintf(b, sizeof b, "WORLD FEED OFF: NEARBY AIRCRAFT ONLY");
        else if (world > 0 || age < 1e8f) snprintf(b, sizeof b, wok ? "WORLD SNAPSHOT %.0f S AGO (OPENSKY NETWORK)" : "WORLD FEED PAUSED: RETRYING (SNAPSHOT %.0f S OLD)", age);
        else snprintf(b, sizeof b, "FETCHING THE WHOLE WORLD...");
        hud_text(x + 14 * s, y + 64 * s, 8 * s, b, TGREY, 0.95f, 0);
        /* search box */
        float sy = y + 74 * s;
        hud_rect(x + 10 * s, sy, w - 20 * s, 20 * s, 0.1f, 0.1f, 0.11f, 1);
        if (g_trackTyping) hud_rect(x + 10 * s, sy + 19 * s, w - 20 * s, 1.5f * s, TRED, 1);
        if (g_trackSearch[0] || g_trackTyping) {
            snprintf(b, sizeof b, "%s%s", g_trackSearch, g_trackTyping && fmodf(SDL_GetTicks() * 0.002f, 1) < 0.5f ? "_" : "");
            hud_text(x + 16 * s, sy + 14 * s, 11 * s, b, TWHITE, 1, 0);
            char cnt[48]; char c3[24]; fmt_thousands(c3, sizeof c3, n); snprintf(cnt, sizeof cnt, "%s FOUND", c3);
            hud_text(x + w - 16 * s, sy + 14 * s, 8 * s, cnt, TGREY, 1, 2);
        } else hud_text(x + 16 * s, sy + 14 * s, 9 * s, "/ TO SEARCH: CALLSIGN, REG, TYPE, HEX, COUNTRY", TGREY, 0.9f, 0);
        hud_text(x + 14 * s, y + 108 * s, 8 * s, "CALLSIGN     TYPE      ALT          NM", TGREY, 0.9f, 0);
        if (!n) hud_text(x + 14 * s, y + 132 * s, 11 * s, g_trackSearch[0] ? "NOTHING MATCHES" : (S.traffic && !S.offline ? "WAITING FOR THE TRAFFIC FEEDS..." : "LIVE TRAFFIC IS OFF (SETTINGS)"), TWHITE, 0.9f, 0);
        for (int k = 0; k < show && scroll + k < n; k++) {
            int ii = sortIdx[scroll + k];
            if (ii >= g_ntraffic) continue;
            Traffic *q = &g_traffic[ii];
            float ry = y + 114 * s + k * rh;
            bool sel = t && q == t;
            if (sel) hud_rect(x + 6 * s, ry, w - 18 * s, rh - 2 * s, TRED, 0.8f);
            if (q->ground) snprintf(c2, sizeof c2, "GROUND"); else snprintf(c2, sizeof c2, "FL%03d", (int)lround(fmax(q->alt, 0) / FT / 100));
            char dn[24]; float dnm = by_d[ii] / 1852;
            if (dnm < 100) snprintf(dn, sizeof dn, "%.1f", dnm); else fmt_thousands(dn, sizeof dn, lround(dnm));
            const char *ty = q->type[0] ? q->type : "-";
            snprintf(b, sizeof b, "%-12.12s %-9.9s %-8s %7s", q->callsign, ty, c2, dn);
            bool emerg = !strcmp(q->squawk, "7500") || !strcmp(q->squawk, "7600") || !strcmp(q->squawk, "7700");
            if (emerg) hud_text(x + 14 * s, ry + 16 * s, 11 * s, b, TRED, blink, 0);
            else hud_text(x + 14 * s, ry + 16 * s, 11 * s, b, TWHITE, sel ? 1.0f : 0.9f, 0);
            if (nRows < 24) { rows[nRows].x = x; rows[nRows].y = ry; rows[nRows].w = w; rows[nRows].h = rh; snprintf(rows[nRows].hex, 8, "%s", q->hex); nRows++; }
        }
        /* scroll bar */
        if (n > show) {
            float th = (show * rh) * show / (float)n, ty0 = y + 114 * s + (show * rh - fmaxf(th, 8 * s)) * scroll / (float)(n - show);
            hud_rect(x + w - 8 * s, y + 114 * s, 3 * s, show * rh, 0.25f, 0.25f, 0.27f, 1);
            hud_rect(x + w - 8 * s, ty0, 3 * s, fmaxf(th, 8 * s), TRED, 1);
        }
        hud_text(x + 14 * s, y + h - 10 * s, 8 * s, "CLICK TO TRACK   WHEEL SCROLLS   N / SHIFT+N NEXT   K CLOSE", TGREY, 0.9f, 0);
        listBox[0] = x; listBox[1] = y; listBox[2] = w; listBox[3] = h;
    } else listBox[2] = 0;
    /* ---- the card for the tracked aircraft */
    if (!g_track.hex[0]) return;
    float w = 400 * s, x = W - w - 20 * s, y = 70 * s, cy = y + 34 * s, pad = 16 * s;
    /* work out the height first: photo, then the fixed blocks */
    float photoH = 0;
    if (g_track.tex && g_track.pw > 0) photoH = fminf((w - 2 * pad) * g_track.ph / g_track.pw, 230 * s);
    float h = 34 * s + 24 * s + (photoH > 0 ? photoH + 22 * s : 0) + 118 * s + 26 * s + 3 * 42 * s + 18 * s + 2 * 42 * s + 40 * s;
    if (y + h > H - 110 * s) h = H - 110 * s - y;
    panel_bg(x, y, w, h);
    hud_clip(x, y, w, h);
    const char *title = t ? t->callsign : g_track.hex;
    hud_title(x + pad, cy, 22 * s, title, TRED, 1, 0);
    if (t && t->mil) hud_text(x + w - pad, cy - 4 * s, 10 * s, "MILITARY", TAMBER, 1, 2);
    if (!t) hud_text(x + w - pad, cy - 4 * s, 10 * s, "SIGNAL LOST", TAMBER, blink, 2);
    cy += 22 * s;
    if (g_track.airline[0]) snprintf(b, sizeof b, "%s", g_track.airline);
    else if (t && t->ownOp[0]) snprintf(b, sizeof b, "%s", t->ownOp);
    else if (g_track.owner[0]) snprintf(b, sizeof b, "%s", g_track.owner);
    else snprintf(b, sizeof b, "%s", g_track.state == 1 ? "LOOKING IT UP..." : "OPERATOR UNKNOWN");
    hud_text(x + pad, cy, 12 * s, b, TWHITE, 0.95f, 0);
    cy += 12 * s;
    if (photoH > 0) {
        hud_image(g_track.tex, x + pad, cy, w - 2 * pad, photoH);
        cy += photoH + 14 * s;
        snprintf(b, sizeof b, "PHOTO (C) %s", g_track.photoBy);
        hud_text(x + w - pad, cy, 8 * s, b, TGREY, 0.9f, 2);
        cy += 8 * s;
    }
    /* route, with how far along it is */
    cy += 26 * s;
    if (g_track.hasRoute) {
        const RouteAirport *A = &g_track.from, *B = &g_track.to;
        hud_text(x + pad, cy, 24 * s, A->iata[0] ? A->iata : A->icao, TWHITE, 1, 0);
        hud_text(x + w - pad, cy, 24 * s, B->iata[0] ? B->iata : B->icao, TWHITE, 1, 2);
        hud_text(x + w / 2, cy - 6 * s, 12 * s, "->", TRED, 1, 1);
        snprintf(b, sizeof b, "%.20s", A->city[0] ? A->city : A->name); hud_text(x + pad, cy + 16 * s, 9 * s, b, TGREY, 1, 0);
        snprintf(b, sizeof b, "%.20s", B->city[0] ? B->city : B->name); hud_text(x + w - pad, cy + 16 * s, 9 * s, b, TGREY, 1, 2);
        double total = geo_dist(A->lat, A->lon, B->lat, B->lon);
        float bx = x + pad, bw = w - 2 * pad, by = cy + 30 * s;
        hud_rect(bx, by, bw, 4 * s, 0.3f, 0.3f, 0.32f, 1);
        if (t && total > 1000) {
            double done = geo_dist(A->lat, A->lon, t->lat, t->lon), left = geo_dist(t->lat, t->lon, B->lat, B->lon);
            float k = (float)clampf((float)(done / (done + left)), 0, 1);
            hud_rect(bx, by, bw * k, 4 * s, TRED, 1);
            float mx = bx + bw * k, my = by + 2 * s, z = 8 * s;
            hud_tri(mx + z, my, mx - z * 0.7f, my - z * 0.7f, mx - z * 0.7f, my + z * 0.7f, TWHITE, 1);
            char d1[32], d2[32]; fmt_thousands(d1, sizeof d1, lround(done / 1852)); fmt_thousands(d2, sizeof d2, lround(left / 1852));
            if (t->gs > 20 && !t->ground) {
                int mins = (int)(left / t->gs / 60);
                snprintf(b, sizeof b, "%s NM FLOWN   %s NM TO GO   ETA %dH %02dM", d1, d2, mins / 60, mins % 60);
            } else snprintf(b, sizeof b, "%s NM FLOWN   %s NM TO GO", d1, d2);
            hud_text(x + pad, by + 22 * s, 9 * s, b, TWHITE, 0.95f, 0);
        }
        snprintf(b, sizeof b, "%.24s  ->  %.24s", A->name, B->name);
        hud_text(x + pad, by + 38 * s, 8 * s, b, TGREY, 0.9f, 0);
    } else {
        hud_text(x + pad, cy, 14 * s, g_track.state == 1 ? "LOOKING UP THE ROUTE..." : "ROUTE NOT PUBLISHED", TWHITE, 0.9f, 0);
        hud_text(x + pad, cy + 18 * s, 9 * s, g_track.state == 1 ? "" : "PRIVATE, MILITARY AND GENERAL AVIATION FLIGHTS OFTEN HAVE NONE", TGREY, 0.9f, 0);
    }
    cy += 92 * s;
    /* live state */
    if (t) {
        const char *ph = tracker_phase(t);
        hud_text(x + pad, cy, 13 * s, ph, TGREEN, 1, 0);
        bool emerg = !strcmp(t->squawk, "7500") || !strcmp(t->squawk, "7600") || !strcmp(t->squawk, "7700") || (t->emergency[0] && strcmp(t->emergency, "none"));
        if (emerg) {
            const char *why = !strcmp(t->squawk, "7500") ? "HIJACK" : !strcmp(t->squawk, "7600") ? "RADIO FAILURE" : !strcmp(t->squawk, "7700") ? "EMERGENCY" : t->emergency;
            snprintf(b, sizeof b, "SQUAWK %s  %s", t->squawk, why);
            hud_text(x + w - pad, cy, 11 * s, b, TRED, blink, 2);
        }
        cy += 14 * s;
        float c1 = x + pad, c2x = x + pad + (w - 2 * pad) / 3, c3 = x + pad + 2 * (w - 2 * pad) / 3;
        char v[48];
        if (t->ground) snprintf(v, sizeof v, "ON GROUND"); else { fmt_thousands(v, sizeof v, lround(t->alt / FT / 10) * 10); strcat(v, " FT"); }
        kv(c1, cy, s, "ALTITUDE", v, TWHITE);
        snprintf(v, sizeof v, "%+.0f FPM", t->vrate / FT * 60); kv(c2x, cy, s, "VERTICAL", v, TWHITE);
        if (t->navAlt > 0) snprintf(v, sizeof v, "FL%03d", (int)lround(t->navAlt / FT / 100)); else snprintf(v, sizeof v, "-");
        kv(c3, cy, s, "CLEARED TO", v, TWHITE);
        cy += 42 * s;
        snprintf(v, sizeof v, "%.0f KT", t->gs / KT); kv(c1, cy, s, "GROUND SPEED", v, TWHITE);
        if (t->mach > 0.2f) snprintf(v, sizeof v, "M%.2f", t->mach); else if (t->ias > 0) snprintf(v, sizeof v, "%.0f KT IAS", t->ias); else snprintf(v, sizeof v, "-");
        kv(c2x, cy, s, "AIRSPEED", v, TWHITE);
        snprintf(v, sizeof v, "%03.0f", fmodf(t->track + 360, 360)); kv(c3, cy, s, "TRACK", v, TWHITE);
        cy += 42 * s;
        double dd = geo_dist(plat, plon, t->lat, t->lon);
        snprintf(v, sizeof v, "%.1f NM", dd / 1852); kv(c1, cy, s, "FROM YOU", v, TWHITE);
        snprintf(v, sizeof v, "%03.0f", geo_bearing(plat, plon, t->lat, t->lon)); kv(c2x, cy, s, "BEARING", v, TWHITE);
        snprintf(v, sizeof v, "%s", t->squawk[0] ? t->squawk : "-"); kv(c3, cy, s, "SQUAWK", v, emerg ? 0.86f : 0.94f, emerg ? 0.09f : 0.93f, emerg ? 0.08f : 0.91f);
        cy += 42 * s;
    } else cy += 3 * 42 * s + 14 * s;
    /* the airframe */
    cy += 4 * s;
    hud_rect(x + pad, cy, w - 2 * pad, 1, 0.4f, 0.4f, 0.42f, 1);
    cy += 14 * s;
    {
        char v[64];
        float c1 = x + pad, c2x = x + pad + (w - 2 * pad) * 0.62f;
        if (g_track.manufacturer[0] || g_track.type[0]) snprintf(v, sizeof v, "%.20s %.24s", g_track.manufacturer, g_track.type);
        else if (t && t->desc[0]) snprintf(v, sizeof v, "%s", t->desc);
        else if (t && t->type[0]) snprintf(v, sizeof v, "%s", t->atype >= 0 ? airframe_name(t->atype) : t->type);
        else snprintf(v, sizeof v, "%s", g_track.state == 1 ? "LOOKING IT UP..." : "TYPE NOT REPORTED");
        kv(c1, cy, s, "AIRCRAFT", v, TWHITE);
        snprintf(v, sizeof v, "%s", g_track.reg[0] ? g_track.reg : (t && t->reg[0] ? t->reg : "-")); kv(c2x, cy, s, "REGISTRATION", v, TWHITE);
        cy += 42 * s;
        const char *own = g_track.owner[0] ? g_track.owner : (t && t->ownOp[0] ? t->ownOp : (t && t->country[0] ? t->country : "-"));
        snprintf(v, sizeof v, "%.30s", own); kv(c1, cy, s, "OWNER", v, TWHITE);
        if (t && t->year[0]) snprintf(v, sizeof v, "%s  BUILT %s", g_track.hex, t->year); else snprintf(v, sizeof v, "%s", g_track.hex);
        for (char *q = v; *q; q++) *q = (char)toupper((unsigned char)*q);
        kv(c2x, cy, s, "ICAO HEX", v, TWHITE);
        cy += 42 * s;
    }
    hud_clip_off();
    hud_text(x + pad, y + h - 12 * s, 8 * s, watching ? "J BACK TO YOUR AIRCRAFT   N NEXT   DEL STOP TRACKING" : "J WATCH IT   SHIFT+J FLY WITH IT   N NEXT   DEL STOP", TGREY, 0.95f, 0);
}
