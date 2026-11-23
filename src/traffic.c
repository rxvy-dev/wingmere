/* WINGMERE - live air traffic: every real aircraft in the world, where it really is right now.  MIT licence.
 *
 * Two feeds fill one world list:
 *  - the whole world: a snapshot of every aircraft the OpenSky Network is tracking, every couple of minutes;
 *  - close up: an ADS-B aggregator (adsb.lol by default, open data under the ODbL) for everything within
 *    ~80 nautical miles of you (or of the plane you are watching), every 8 seconds, with type and registration.
 * Between reports each aircraft is flown forward on its track, speed and climb rate, and corrections are
 * blended in smoothly.  Each is drawn with its own airframe (airframe.c) or the closest hangar model. */
#include "wm.h"
#include <curl/curl.h>
#include <pthread.h>

Traffic g_traffic[MAX_TRAFFIC];
int g_ntraffic;
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;
static double qLat, qLon; static bool qHave, tQuit, tOn;
static char trackHex[8];
static int lastCount; static bool lastOk;
typedef struct { char *p; size_t n, cap; } TB;
static size_t tdata(void *ptr, size_t sz, size_t nm, void *ud) {
    TB *b = ud; size_t n = sz * nm;
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, ptr, n); b->n += n; b->p[b->n] = 0;
    return n;
}

/* ------------------------------------------------------------------ a tiny JSON walker for the "ac" array */
typedef struct { char hex[8], flight[10], reg[12], type[6], desc[48], ownOp[48], year[6], squawk[6], emergency[16], category[4], country[24];
    double lat, lon; float alt, gs, track, vrate, navAlt, navHdg, mach, ias, altGeom, seen; bool ground, hasPos, mil; int cat; } Fix;
static const char *ws(const char *s) { while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++; return s; }
static const char *skip_value(const char *s) {
    s = ws(s);
    if (*s == '"') { s++; while (*s && *s != '"') { if (*s == '\\' && s[1]) s++; s++; } return *s ? s + 1 : s; }
    if (*s == '{' || *s == '[') {
        int depth = 0;
        do {
            if (*s == '"') { s = skip_value(s); continue; }
            if (*s == '{' || *s == '[') depth++;
            else if (*s == '}' || *s == ']') depth--;
            s++;
        } while (*s && depth > 0);
        return s;
    }
    while (*s && *s != ',' && *s != '}' && *s != ']') s++;
    return s;
}
static const char *read_str(const char *s, char *out, size_t n) {
    s = ws(s);
    size_t k = 0;
    if (*s != '"') { out[0] = 0; return skip_value(s); }
    s++;
    while (*s && *s != '"') { if (*s == '\\' && s[1]) s++; if (k + 1 < n) out[k++] = *s; s++; }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ') out[--k] = 0;
    return *s ? s + 1 : s;
}
static int parse(const char *js, Fix *out, int max) {
    const char *s = strstr(js, "\"ac\"");
    if (!s) return 0;
    s = strchr(s, '[');
    if (!s) return 0;
    s++;
    int n = 0;
    while (*s && n < max) {
        s = ws(s);
        if (*s == ']') break;
        if (*s == ',') { s++; continue; }
        if (*s != '{') { s = skip_value(s); continue; }
        s++;
        Fix f; memset(&f, 0, sizeof f); f.navAlt = f.navHdg = f.mach = f.ias = -1;
        while (*s && *s != '}') {
            s = ws(s);
            if (*s == ',') { s++; continue; }
            char key[24]; s = read_str(s, key, sizeof key);
            s = ws(s); if (*s == ':') s++;
            s = ws(s);
            if (!strcmp(key, "hex")) s = read_str(s, f.hex, sizeof f.hex);
            else if (!strcmp(key, "flight")) s = read_str(s, f.flight, sizeof f.flight);
            else if (!strcmp(key, "r")) s = read_str(s, f.reg, sizeof f.reg);
            else if (!strcmp(key, "t")) s = read_str(s, f.type, sizeof f.type);
            else if (!strcmp(key, "lat")) { f.lat = strtod(s, NULL); f.hasPos = true; s = skip_value(s); }
            else if (!strcmp(key, "lon")) { f.lon = strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "alt_baro")) { if (*s == '"') { f.ground = true; } else f.alt = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "gs")) { f.gs = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "track")) { f.track = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "baro_rate") || (!strcmp(key, "geom_rate") && f.vrate == 0)) { f.vrate = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "desc")) s = read_str(s, f.desc, sizeof f.desc);
            else if (!strcmp(key, "ownOp")) s = read_str(s, f.ownOp, sizeof f.ownOp);
            else if (!strcmp(key, "year")) s = read_str(s, f.year, sizeof f.year);
            else if (!strcmp(key, "squawk")) s = read_str(s, f.squawk, sizeof f.squawk);
            else if (!strcmp(key, "emergency")) s = read_str(s, f.emergency, sizeof f.emergency);
            else if (!strcmp(key, "category")) s = read_str(s, f.category, sizeof f.category);
            else if (!strcmp(key, "nav_altitude_mcp") || (!strcmp(key, "nav_altitude_fms") && f.navAlt < 0)) { f.navAlt = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "nav_heading")) { f.navHdg = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "mach")) { f.mach = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "ias")) { f.ias = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "alt_geom")) { f.altGeom = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "seen_pos") || (!strcmp(key, "seen") && f.seen == 0)) { f.seen = (float)strtod(s, NULL); s = skip_value(s); }
            else if (!strcmp(key, "dbFlags")) { f.mil = ((int)strtol(s, NULL, 10) & 1) != 0; s = skip_value(s); }
            else s = skip_value(s);
        }
        if (*s == '}') s++;
        if (f.hasPos && f.hex[0]) out[n++] = f;
    }
    return n;
}

/* ------------------------------------------------------------------ fetching */
#define MAX_NEAR 1200
static Fix inbox[MAX_NEAR]; static int nInbox; static bool inboxNew;
static void make_url(char *out, size_t n, double lat, double lon) {
    char *o = out, *end = out + n - 24;
    for (const char *s = S.traffic_url; *s && o < end; s++) {
        if (!strncmp(s, "{lat}", 5)) { o += sprintf(o, "%.4f", lat); s += 4; }
        else if (!strncmp(s, "{lon}", 5)) { o += sprintf(o, "%.4f", lon); s += 4; }
        else if (!strncmp(s, "{dist}", 6)) { o += sprintf(o, "%d", 80); s += 5; }
        else *o++ = *s;
    }
    *o = 0;
}
static void *worker(void *arg) {
    (void)arg;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, tdata);
    curl_easy_setopt(c, CURLOPT_USERAGENT, S.user_agent);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    static Fix tmp[MAX_NEAR];
    while (!tQuit) {
        pthread_mutex_lock(&tmu);
        bool go = tOn && qHave; double la = qLat, lo = qLon;
        pthread_mutex_unlock(&tmu);
        if (go) {
            char url[600]; make_url(url, sizeof url, la, lo);
            TB b = {0};
            curl_easy_setopt(c, CURLOPT_URL, url);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
            long code = 0;
            CURLcode rc = curl_easy_perform(c);
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            int n = rc == CURLE_OK && code == 200 && b.p ? parse(b.p, tmp, MAX_NEAR) : -1;
            /* the tracked aircraft, asked for by itself so it can be followed anywhere in the world */
            pthread_mutex_lock(&tmu); char th[8]; memcpy(th, trackHex, sizeof th); pthread_mutex_unlock(&tmu);
            if (th[0] && S.traffic_hex_url[0] && n >= 0 && n < MAX_NEAR) {
                bool have = false;
                for (int i = 0; i < n; i++) if (!strcasecmp(tmp[i].hex, th)) have = true;
                if (!have) {
                    char u2[600], *o = u2;
                    for (const char *q = S.traffic_hex_url; *q && o < u2 + sizeof u2 - 12; q++) {
                        if (!strncmp(q, "{hex}", 5)) { o += sprintf(o, "%s", th); q += 4; } else *o++ = *q;
                    }
                    *o = 0;
                    TB b2 = {0};
                    curl_easy_setopt(c, CURLOPT_URL, u2);
                    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b2);
                    if (curl_easy_perform(c) == CURLE_OK && b2.p) n += parse(b2.p, tmp + n, MAX_NEAR - n);
                    free(b2.p);
                }
            }
            pthread_mutex_lock(&tmu);
            if (n >= 0) { memcpy(inbox, tmp, sizeof(Fix) * n); nInbox = n; inboxNew = true; }
            static int said;
            if (said < 2 && (n >= 0) != (said == 1)) { fprintf(stderr, n >= 0 ? "wingmere: live traffic: %d aircraft nearby\n" : "wingmere: live traffic feed unavailable (%d)\n", n >= 0 ? n : (int)code); said = n >= 0 ? 2 : 1; }
            lastOk = n >= 0; lastCount = n;
            pthread_mutex_unlock(&tmu);
            free(b.p);
        }
        for (int i = 0; i < (go ? 60 : 2) && !tQuit; i++) SDL_Delay(100);   /* every 6 seconds: be polite to a free service */
    }
    curl_easy_cleanup(c);
    return NULL;
}

/* the tracked (or escorted) aircraft, by itself, every two seconds */
static Fix hbox[4]; static int nHbox; static bool hboxNew;
static void *hworker(void *arg) {
    (void)arg;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, tdata);
    curl_easy_setopt(c, CURLOPT_USERAGENT, S.user_agent);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    while (!tQuit) {
        pthread_mutex_lock(&tmu); char th[8]; memcpy(th, trackHex, sizeof th); bool go = tOn && th[0] && S.traffic_hex_url[0]; pthread_mutex_unlock(&tmu);
        if (go) {
            char u2[600], *o = u2;
            for (const char *q = S.traffic_hex_url; *q && o < u2 + sizeof u2 - 12; q++) {
                if (!strncmp(q, "{hex}", 5)) { o += sprintf(o, "%s", th); q += 4; } else *o++ = *q;
            }
            *o = 0;
            TB b = {0};
            curl_easy_setopt(c, CURLOPT_URL, u2);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
            long code = 0;
            if (curl_easy_perform(c) == CURLE_OK && (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code), code == 200) && b.p) {
                Fix tmp[4]; int n = parse(b.p, tmp, 4);
                if (n > 0) { pthread_mutex_lock(&tmu); memcpy(hbox, tmp, sizeof(Fix) * n); nHbox = n; hboxNew = true; pthread_mutex_unlock(&tmu); }
            }
            free(b.p);
        }
        for (int i = 0; i < 20 && !tQuit; i++) SDL_Delay(100);
    }
    curl_easy_cleanup(c);
    return NULL;
}

/* ------------------------------------------------------------------ the whole world (OpenSky Network state vectors) */
/* "states": [[icao24, callsign, origin_country, time_position, last_contact, longitude, latitude, baro_altitude (m),
 *             on_ground, velocity (m/s), true_track, vertical_rate (m/s), sensors, geo_altitude, squawk, spi,
 *             position_source, category], ...] */
static Fix gbox[MAX_TRAFFIC]; static int nGbox; static bool gboxNew;
static float gAge = 1e9f; static bool gOk; static int gCount;
static int parse_states(const char *js, Fix *out, int max) {
    const char *tt = strstr(js, "\"time\"");
    double now = tt ? strtod(strchr(tt, ':') + 1, NULL) : 0;
    const char *s = strstr(js, "\"states\"");
    if (!s) return -1;
    s = strchr(s, ':'); if (!s) return -1;
    s = ws(s + 1);
    if (*s != '[') return 0;          /* "states": null - nothing in the air (or nothing visible) */
    s++;
    int n = 0;
    while (*s && n < max) {
        s = ws(s);
        if (*s == ']') break;
        if (*s == ',') { s++; continue; }
        if (*s != '[') { s = skip_value(s); continue; }
        s++;
        Fix f; memset(&f, 0, sizeof f); f.navAlt = f.navHdg = f.mach = f.ias = -1;
        bool hasLat = false, hasLon = false;
        for (int k = 0; *s && *s != ']'; k++) {
            s = ws(s);
            if (*s == ',') { s++; k--; continue; }
            bool null = !strncmp(s, "null", 4);
            switch (k) {
            case 0: s = read_str(s, f.hex, sizeof f.hex); break;
            case 1: s = read_str(s, f.flight, sizeof f.flight); break;
            case 2: s = read_str(s, f.country, sizeof f.country); break;
            case 3: if (!null && now > 0) f.seen = (float)fmax(now - strtod(s, NULL), 0); s = skip_value(s); break;
            case 5: if (!null) { f.lon = strtod(s, NULL); hasLon = true; } s = skip_value(s); break;
            case 6: if (!null) { f.lat = strtod(s, NULL); hasLat = true; } s = skip_value(s); break;
            case 7: if (!null) f.alt = (float)(strtod(s, NULL) / FT); s = skip_value(s); break;
            case 8: f.ground = !strncmp(s, "true", 4); s = skip_value(s); break;
            case 9: if (!null) f.gs = (float)(strtod(s, NULL) / KT); s = skip_value(s); break;
            case 10: if (!null) f.track = (float)strtod(s, NULL); s = skip_value(s); break;
            case 11: if (!null) f.vrate = (float)(strtod(s, NULL) / FT * 60); s = skip_value(s); break;
            case 13: if (!null && f.alt == 0 && !f.ground) f.alt = (float)(strtod(s, NULL) / FT); s = skip_value(s); break;
            case 14: s = read_str(s, f.squawk, sizeof f.squawk); break;
            case 17: if (!null) f.cat = (int)strtol(s, NULL, 10); s = skip_value(s); break;
            default: s = skip_value(s); break;
            }
        }
        if (*s == ']') s++;
        f.hasPos = hasLat && hasLon;
        for (char *c = f.hex; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (f.hasPos && f.hex[0]) out[n++] = f;
    }
    return n;
}
/* optional OpenSky account (API client id + secret): ten times the daily allowance, so a fresher world */
static char token[2048]; static double tokenUntil;
static void get_token(CURL *c) {
    if (!S.sky_id[0] || !S.sky_secret[0] || SDL_GetTicks() / 1000.0 < tokenUntil) return;
    TB b = {0};
    char body[400], *ei = curl_easy_escape(c, S.sky_id, 0), *es = curl_easy_escape(c, S.sky_secret, 0);
    snprintf(body, sizeof body, "grant_type=client_credentials&client_id=%s&client_secret=%s", ei, es);
    curl_free(ei); curl_free(es);
    curl_easy_setopt(c, CURLOPT_URL, "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    if (curl_easy_perform(c) == CURLE_OK && b.p) {
        const char *t = strstr(b.p, "\"access_token\"");
        if (t) { t = strchr(t + 14, ':'); if (t) { read_str(t + 1, token, sizeof token); tokenUntil = SDL_GetTicks() / 1000.0 + 25 * 60; } }
    }
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    free(b.p);
}
static void *gworker(void *arg) {
    (void)arg;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, tdata);
    curl_easy_setopt(c, CURLOPT_USERAGENT, S.user_agent);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    static Fix tmp[MAX_TRAFFIC];
    double wait = 0;
    while (!tQuit) {
        pthread_mutex_lock(&tmu); bool go = tOn && S.traffic_global && S.global_url[0]; pthread_mutex_unlock(&tmu);
        if (go && wait <= 0) {
            get_token(c);
            struct curl_slist *hd = NULL;
            if (token[0]) { char h[2100]; snprintf(h, sizeof h, "Authorization: Bearer %s", token); hd = curl_slist_append(hd, h); }
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, hd);
            TB b = {0};
            curl_easy_setopt(c, CURLOPT_URL, S.global_url);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
            long code = 0;
            CURLcode rc = curl_easy_perform(c);
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, NULL);
            curl_slist_free_all(hd);
            int n = rc == CURLE_OK && code == 200 && b.p ? parse_states(b.p, tmp, MAX_TRAFFIC) : -1;
            if (n >= 0) {
                pthread_mutex_lock(&tmu);
                memcpy(gbox, tmp, sizeof(Fix) * n); nGbox = n; gboxNew = true; gOk = true; gCount = n;
                pthread_mutex_unlock(&tmu);
                fprintf(stderr, "wingmere: world traffic: %d aircraft\n", n);
                wait = fmaxf(S.global_interval, token[0] ? 15 : 60);
            } else {
                /* over the daily allowance (429) or unreachable: try again later, not in a tight loop */
                pthread_mutex_lock(&tmu); gOk = false; pthread_mutex_unlock(&tmu);
                wait = code == 429 ? 600 : 90;
                fprintf(stderr, "wingmere: world traffic unavailable (%ld), retrying in %.0f s\n", code, wait);
            }
            free(b.p);
        }
        SDL_Delay(500); wait -= 0.5;
    }
    curl_easy_cleanup(c);
    return NULL;
}
void traffic_init(void) {
    static bool started;
    if (started || !S.traffic_url[0]) return;
    started = true;
    pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_detach(t);
    pthread_t g; pthread_create(&g, NULL, gworker, NULL); pthread_detach(g);
    pthread_t hw; pthread_create(&hw, NULL, hworker, NULL); pthread_detach(hw);
}
void traffic_track(const char *hex) { pthread_mutex_lock(&tmu); snprintf(trackHex, sizeof trackHex, "%s", hex ? hex : ""); pthread_mutex_unlock(&tmu); }
/* hex -> index, so a 15,000-aircraft snapshot merges in a blink */
#define HMAP 65536
static int hmap[HMAP];
static unsigned hhash(const char *h) { unsigned v = 2166136261u; for (; *h; h++) v = (v ^ (unsigned)tolower((unsigned char)*h)) * 16777619u; return v; }
static void hput(int i) { unsigned k = hhash(g_traffic[i].hex) & (HMAP - 1); while (hmap[k]) k = (k + 1) & (HMAP - 1); hmap[k] = i + 1; }
static void hrebuild(void) { memset(hmap, 0, sizeof hmap); for (int i = 0; i < g_ntraffic; i++) hput(i); }
int traffic_find(const char *hex) {
    if (!hex || !hex[0]) return -1;
    for (unsigned k = hhash(hex) & (HMAP - 1); hmap[k]; k = (k + 1) & (HMAP - 1))
        if (!strcasecmp(g_traffic[hmap[k] - 1].hex, hex)) return hmap[k] - 1;
    return -1;
}
int traffic_world(float *age, bool *ok) { pthread_mutex_lock(&tmu); if (age) *age = gAge; if (ok) *ok = gOk; int n = gCount; pthread_mutex_unlock(&tmu); return n; }
void traffic_enable(bool on) { pthread_mutex_lock(&tmu); tOn = on && !S.offline; pthread_mutex_unlock(&tmu); if (!on) { g_ntraffic = 0; hrebuild(); } }
bool traffic_status(int *count) { pthread_mutex_lock(&tmu); bool ok = lastOk; *count = g_ntraffic; pthread_mutex_unlock(&tmu); return ok; }

/* which of our models stands in for an ICAO type code, and how big it should be */
static void pick_model(Traffic *t) {
    const char *ty = t->type;
    t->atype = airframe_find(ty);
    t->guessed = false;
    if (!ty[0]) {
        /* the global feed has no type: pick an airframe from the emitter category, or from how it flies */
        static const char *BYCAT[] = {NULL, NULL, "C172", "E75L", "A320", "B752", "B77W", NULL, NULL};
        const char *g = t->cat >= 2 && t->cat <= 6 ? BYCAT[t->cat] : (t->cat == 8 ? NULL : (t->gs > 150 * KT || t->alt > 6000 ? (t->gs > 260 * KT && t->alt > 9500 && t->callsign[0] && isdigit((unsigned char)t->callsign[3]) ? "A320" : "E75L") : "C172"));
        if (g) { t->atype = airframe_find(g); t->guessed = true; }
    }
    t->liv = airframe_livery(t->callsign, t->mil);
    static const char *wide[] = {"A33", "A34", "A35", "A38", "B74", "B76", "B77", "B78", "A30", "A31", "MD1", "IL9", "C5", "C17", NULL};
    static const char *narrow[] = {"A31", "A32", "A2", "B73", "B3", "B75", "B71", "MD8", "MD9", "E19", "E29", "E17", "E75", "CRJ", "BCS", "A22", "SU9", "C91", "GLF", "GL", "CL6", "F9", "LJ", "C25", "C56", "C68", "C75", "E55", "E50", "H25", NULL};
    static const char *turbo[] = {"AT4", "AT7", "DH8", "C130", "C30", "SF3", "B350", "BE20", "BE9", "PC12", "PC24", "SW4", "D328", "JS4", "L410", "C208", "TBM", "P180", "A400", "E120", "C27", NULL};
    static const char *fighter[] = {"F16", "F18", "F15", "F35", "FA18", "EUFI", "F22", "RFAL", "TOR", "GRIF", "HAWK", "T38", "M346", "F5", NULL};
    t->model = "b737"; t->scale = 1;
    if (!ty[0]) { if (t->gs < 180) { t->model = "biplane"; } return; }
    if (!strcmp(ty, "F14")) { t->model = "f14"; return; }
    if (!strcmp(ty, "F117")) { t->model = "f117"; return; }
    for (int i = 0; fighter[i]; i++) if (!strncmp(ty, fighter[i], strlen(fighter[i]))) { t->model = "f35"; t->scale = 0.95f; return; }
    for (int i = 0; turbo[i]; i++) if (!strncmp(ty, turbo[i], strlen(turbo[i]))) {
        t->model = "transport";
        t->scale = !strncmp(ty, "C130", 4) || !strncmp(ty, "A400", 4) || !strncmp(ty, "C30", 3) ? 1.0f : (!strncmp(ty, "AT", 2) || !strncmp(ty, "DH8", 3) ? 0.85f : 0.5f);
        return;
    }
    if (!strcmp(ty, "A388") || !strncmp(ty, "B74", 3) || !strncmp(ty, "A38", 3)) { t->scale = 1.9f; return; }
    for (int i = 0; wide[i]; i++) if (!strncmp(ty, wide[i], strlen(wide[i])) && strncmp(ty, "A31", 3) && strncmp(ty, "A32", 3)) { t->scale = 1.6f; return; }
    for (int i = 0; narrow[i]; i++) if (!strncmp(ty, narrow[i], strlen(narrow[i]))) {
        t->scale = !strncmp(ty, "E1", 2) || !strncmp(ty, "CRJ", 3) || !strncmp(ty, "E7", 2) ? 0.8f : (ty[0] == 'G' || ty[0] == 'L' || ty[0] == 'C' || ty[0] == 'H' || ty[0] == 'E' ? 0.55f : 1.0f);
        if (!strncmp(ty, "A32", 3) || !strncmp(ty, "A21", 3)) t->scale = !strcmp(ty, "A321") || !strcmp(ty, "A21N") ? 1.12f : 0.98f;
        return;
    }
    if (ty[0] == 'C' || ty[0] == 'P' || !strncmp(ty, "SR2", 3) || !strncmp(ty, "DA", 2) || !strncmp(ty, "BE", 2) || !strncmp(ty, "M20", 3)) { t->model = "biplane"; t->scale = 1; return; }
    if (ty[0] == 'H' || ty[0] == 'R' || !strncmp(ty, "EC", 2) || !strncmp(ty, "AS3", 3) || !strncmp(ty, "B06", 3)) { t->model = NULL; return; }   /* helicopters: label only */
}

/* merge one report into the world list */
static void merge(const Fix *f, bool detailed) {
    int k = traffic_find(f->hex);
    if (k >= 0 && !detailed && g_traffic[k].detAge < 90) {
        /* the nearby feed is fresher: only fill in what it lacks */
        Traffic *t = &g_traffic[k];
        if (!t->country[0]) snprintf(t->country, sizeof t->country, "%s", f->country);
        if (!t->cat) t->cat = f->cat;
        return;
    }
    bool isNew = k < 0;
    if (isNew) {
        if (g_ntraffic >= MAX_TRAFFIC) return;
        k = g_ntraffic++;
        memset(&g_traffic[k], 0, sizeof(Traffic));
        g_traffic[k].lat = f->lat; g_traffic[k].lon = f->lon; g_traffic[k].alt = f->ground ? -1e9 : f->alt * FT;
        g_traffic[k].detAge = 1e9f;
        snprintf(g_traffic[k].hex, sizeof g_traffic[k].hex, "%s", f->hex);
        hput(k);
    }
    Traffic *t = &g_traffic[k];
    if (f->flight[0]) snprintf(t->callsign, sizeof t->callsign, "%s", f->flight);
    else if (!t->callsign[0] || !strcmp(t->callsign, t->hex)) snprintf(t->callsign, sizeof t->callsign, "%s", f->reg[0] ? f->reg : f->hex);
    if (f->reg[0]) snprintf(t->reg, sizeof t->reg, "%s", f->reg);
    if (f->type[0]) snprintf(t->type, sizeof t->type, "%s", f->type);
    if (f->country[0]) snprintf(t->country, sizeof t->country, "%s", f->country);
    if (f->cat) t->cat = f->cat;
    t->fixLat = f->lat; t->fixLon = f->lon; t->fixAlt = f->alt * FT;
    if (t->fixT > 0.5f && t->fixT < 30 && t->gs > 20) { float dtr = fmodf(f->track - t->track + 540, 360) - 180; t->turn = clampf(dtr / t->fixT, -6, 6); }
    else if (!detailed) t->turn = 0;
    if (detailed) {
        snprintf(t->desc, sizeof t->desc, "%s", f->desc); snprintf(t->ownOp, sizeof t->ownOp, "%s", f->ownOp);
        snprintf(t->year, sizeof t->year, "%s", f->year); snprintf(t->emergency, sizeof t->emergency, "%s", f->emergency);
        snprintf(t->category, sizeof t->category, "%s", f->category);
        t->navAlt = f->navAlt >= 0 ? f->navAlt * FT : -1; t->navHdg = f->navHdg; t->mach = f->mach; t->ias = f->ias; t->altGeom = f->altGeom * FT; t->mil = f->mil;
        t->detAge = 0;
    } else if (isNew) t->navAlt = -1;
    if (f->squawk[0]) snprintf(t->squawk, sizeof t->squawk, "%s", f->squawk);
    {   /* breadcrumb trail: one point per report, if it has moved */
        int last = (t->trailHead + TRAIL - 1) % TRAIL;
        if (!t->ntrail || fabsf(t->trail[last][0] - (float)f->lat) + fabsf(t->trail[last][1] - (float)f->lon) > 1e-4f) {
            t->trail[t->trailHead][0] = (float)f->lat; t->trail[t->trailHead][1] = (float)f->lon;
            t->trail[t->trailHead][2] = f->ground ? -1e9f : f->alt * FT;
            t->trailHead = (t->trailHead + 1) % TRAIL; if (t->ntrail < TRAIL) t->ntrail++;
        }
    }
    t->gs = f->gs * KT; t->track = f->track; t->vrate = f->vrate * FT / 60; t->ground = f->ground; t->age = 0; t->fresh = true;
    t->fixT = clampf(f->seen, 0, 30);           /* the report was already this old when it arrived */
    /* keep drawing it where it was; the difference to the new report fades out over a couple of seconds */
    if (!isNew && t->alt > -1e8) {
        double pla, plo; geo_offset(t->fixLat, t->fixLon, t->track, t->gs * t->fixT, &pla, &plo);
        t->cLat = t->lat - pla; t->cLon = remainder(t->lon - plo, 360.0);
        t->cAlt = t->ground ? 0 : t->alt - (t->fixAlt + t->vrate * t->fixT);
        if (fabs(t->cLat) > 0.05 || fabs(t->cLon) > 0.05 || fabs(t->cAlt) > 600) t->cLat = t->cLon = t->cAlt = 0;
    } else t->cLat = t->cLon = t->cAlt = 0;
    t->cT = 0;
    pick_model(t);
}

/* fly everyone forward, and merge new reports */
void traffic_update(double lat, double lon, float dt) {
    static unsigned frame;
    frame++;
    pthread_mutex_lock(&tmu);
    qLat = lat; qLon = lon; qHave = true;
    gAge += dt;
    bool changed = false;
    if (gboxNew) { gboxNew = false; gAge = 0; for (int i = 0; i < nGbox; i++) merge(&gbox[i], false); changed = true; }
    if (inboxNew) { inboxNew = false; for (int i = 0; i < nInbox; i++) merge(&inbox[i], true); }
    if (hboxNew) { hboxNew = false; for (int i = 0; i < nHbox; i++) merge(&hbox[i], true); }
    pthread_mutex_unlock(&tmu);
    float worldLife = fmaxf(S.global_interval * 3, 420);
    float coslat = cosf((float)(lat * M_PI / 180));
    for (int i = 0; i < g_ntraffic;) {
        Traffic *t = &g_traffic[i];
        t->age += dt; t->detAge += dt;
        bool tracked = trackHex[0] && !strcasecmp(t->hex, trackHex);
        float life = tracked ? 900 : (t->detAge < 1e8f && t->age < 70 ? 60 : worldLife);
        if (t->age > life) { g_traffic[i] = g_traffic[--g_ntraffic]; changed = true; continue; }
        t->fresh = false;
        t->fixT += dt; t->cT += dt;
        /* aircraft near you are flown smoothly every frame; the rest of the world a slice at a time */
        bool near = fabs(t->lat - lat) < 2 && fabs(remainder(t->lon - lon, 360.0)) * coslat < 2;
        if (!near && !tracked && ((unsigned)i % 24) != frame % 24) { i++; continue; }
        float T = t->fixT + t->ahead;
        float tt = fminf(T, 12), trk = t->track + t->turn * tt * 0.5f;
        double d = t->gs * fminf(T, t->ahead > 0 ? 1e6f : 1800), pla, plo;
        if (t->ahead > 0 && t->hasDest) {
            /* predicted ahead of the live data: along the great circle to where it is going */
            double left = geo_dist(t->fixLat, t->fixLon, t->destLat, t->destLon);
            trk = (float)geo_bearing(t->fixLat, t->fixLon, t->destLat, t->destLon);
            d = fmin(d, fmax(left - 3000, 0));
        } else if (t->ahead > 0) trk = t->track;
        geo_offset(t->fixLat, t->fixLon, trk, d, &pla, &plo);
        float bankT = t->ground ? 0 : clampf(atanf(t->gs * t->turn * DEG / 9.81f) / DEG, -35, 35);
        t->bank += (bankT - t->bank) * (1 - expf(-dt * 1.5f));
        t->prevTrack = t->track + t->turn * tt;
        double palt;
        if (near) {
            /* dead reckoning on the report, plus the leftover correction fading out: no lag behind the real thing */
            palt = t->ground ? terrain_height(pla, plo, NULL, NULL) : t->fixAlt + t->vrate * fminf(t->fixT, t->ahead > 0 ? 20 : 120);
            float fade = expf(-t->cT / 2.5f);
            t->lat = pla + t->cLat * fade; t->lon = plo + t->cLon * fade; t->alt = palt + t->cAlt * fade;
            if (t->ground) t->alt = fmax(t->alt, terrain_height(t->lat, t->lon, NULL, NULL));
        } else {
            palt = t->ground ? t->fixAlt : t->fixAlt + t->vrate * fminf(t->fixT, 120);
            t->lat = pla; t->lon = plo; t->alt = palt;
        }
        i++;
    }
    if (changed) hrebuild();
}
