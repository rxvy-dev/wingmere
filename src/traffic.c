/* WINGMERE - live air traffic: real aircraft, where they really are right now.  MIT licence.
 *
 * A background thread asks an ADS-B aggregator (adsb.lol by default, open data under the ODbL)
 * for every aircraft within ~80 nautical miles of the player, every few seconds.  Between
 * reports each aircraft is flown forward on its track, speed and climb rate, and corrections
 * are blended in smoothly.  Each is drawn with the closest model the game has, scaled to size. */
#include "wm.h"
#include <curl/curl.h>
#include <pthread.h>

Traffic g_traffic[MAX_TRAFFIC];
int g_ntraffic;
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;
static double qLat, qLon; static bool qHave, tQuit, tOn;
static int lastCount; static bool lastOk;
typedef struct { char *p; size_t n, cap; } TB;
static size_t tdata(void *ptr, size_t sz, size_t nm, void *ud) {
    TB *b = ud; size_t n = sz * nm;
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, ptr, n); b->n += n; b->p[b->n] = 0;
    return n;
}

/* ------------------------------------------------------------------ a tiny JSON walker for the "ac" array */
typedef struct { char hex[8], flight[10], reg[12], type[6]; double lat, lon; float alt, gs, track, vrate; bool ground, hasPos; } Fix;
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
        Fix f; memset(&f, 0, sizeof f);
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
            else if (!strcmp(key, "baro_rate")) { f.vrate = (float)strtod(s, NULL); s = skip_value(s); }
            else s = skip_value(s);
        }
        if (*s == '}') s++;
        if (f.hasPos && f.hex[0]) out[n++] = f;
    }
    return n;
}

/* ------------------------------------------------------------------ fetching */
static Fix inbox[MAX_TRAFFIC]; static int nInbox; static bool inboxNew;
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
    static Fix tmp[MAX_TRAFFIC];
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
            int n = rc == CURLE_OK && code == 200 && b.p ? parse(b.p, tmp, MAX_TRAFFIC) : -1;
            pthread_mutex_lock(&tmu);
            if (n >= 0) { memcpy(inbox, tmp, sizeof(Fix) * n); nInbox = n; inboxNew = true; }
            static int said;
            if (said < 2 && (n >= 0) != (said == 1)) { fprintf(stderr, n >= 0 ? "wingmere: live traffic: %d aircraft nearby\n" : "wingmere: live traffic feed unavailable (%d)\n", n >= 0 ? n : (int)code); said = n >= 0 ? 2 : 1; }
            lastOk = n >= 0; lastCount = n;
            pthread_mutex_unlock(&tmu);
            free(b.p);
        }
        for (int i = 0; i < (go ? 80 : 2) && !tQuit; i++) SDL_Delay(100);   /* every 8 seconds: be polite to a free service */
    }
    curl_easy_cleanup(c);
    return NULL;
}
void traffic_init(void) {
    static bool started;
    if (started || !S.traffic_url[0]) return;
    started = true;
    pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_detach(t);
}
void traffic_enable(bool on) { pthread_mutex_lock(&tmu); tOn = on && !S.offline; pthread_mutex_unlock(&tmu); if (!on) g_ntraffic = 0; }
bool traffic_status(int *count) { pthread_mutex_lock(&tmu); bool ok = lastOk; *count = g_ntraffic; pthread_mutex_unlock(&tmu); return ok; }

/* which of our models stands in for an ICAO type code, and how big it should be */
static void pick_model(Traffic *t) {
    const char *ty = t->type;
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

/* fly everyone forward, and merge new reports */
void traffic_update(double lat, double lon, float dt) {
    pthread_mutex_lock(&tmu);
    qLat = lat; qLon = lon; qHave = true;
    if (inboxNew) {
        inboxNew = false;
        for (int i = 0; i < nInbox; i++) {
            Fix *f = &inbox[i];
            int k = -1;
            for (int j = 0; j < g_ntraffic; j++) if (!strcmp(g_traffic[j].hex, f->hex)) { k = j; break; }
            if (k < 0) { if (g_ntraffic >= MAX_TRAFFIC) continue; k = g_ntraffic++; memset(&g_traffic[k], 0, sizeof(Traffic)); g_traffic[k].lat = f->lat; g_traffic[k].lon = f->lon; g_traffic[k].alt = f->alt * FT; }
            Traffic *t = &g_traffic[k];
            snprintf(t->hex, sizeof t->hex, "%s", f->hex);
            snprintf(t->callsign, sizeof t->callsign, "%s", f->flight[0] ? f->flight : (f->reg[0] ? f->reg : f->hex));
            snprintf(t->reg, sizeof t->reg, "%s", f->reg);
            snprintf(t->type, sizeof t->type, "%s", f->type);
            t->fixLat = f->lat; t->fixLon = f->lon; t->fixAlt = f->alt * FT;
            if (t->fixT > 0.5f && t->gs > 20) { float dtr = fmodf(f->track - t->track + 540, 360) - 180; t->turn = clampf(dtr / t->fixT, -6, 6); }
            t->gs = f->gs * KT; t->track = f->track; t->vrate = f->vrate * FT / 60; t->ground = f->ground; t->age = 0; t->fresh = true;
            pick_model(t);
        }
    }
    pthread_mutex_unlock(&tmu);
    for (int i = 0; i < g_ntraffic;) {
        Traffic *t = &g_traffic[i];
        t->age += dt;
        if (t->age > 60) { g_traffic[i] = g_traffic[--g_ntraffic]; continue; }
        /* dead reckoning on the latest report, and the drawn position chases it */
        if (t->fresh) { t->fixT = 0; t->fresh = false; }
        t->fixT += dt;
        /* follow the turn it was last seen making (for a little while), so circling aircraft keep circling */
        float tt = fminf(t->fixT, 12), trk = t->track + t->turn * tt * 0.5f;
        double d = t->gs * t->fixT, pla, plo;
        geo_offset(t->fixLat, t->fixLon, trk, d, &pla, &plo);
        float bankT = t->ground ? 0 : clampf(atanf(t->gs * t->turn * DEG / 9.81f) / DEG, -35, 35);
        t->bank += (bankT - t->bank) * (1 - expf(-dt * 1.5f));
        t->prevTrack = t->track + t->turn * tt;
        double palt = t->ground ? terrain_height(pla, plo, NULL, NULL) : t->fixAlt + t->vrate * t->fixT;
        float k = 1 - expf(-dt * 0.8f);
        if (fabs(t->lat - pla) > 0.5 || fabs(t->lon - plo) > 0.5) { t->lat = pla; t->lon = plo; t->alt = palt; }
        t->lat += (pla - t->lat) * k; t->lon += (plo - t->lon) * k; t->alt += (palt - t->alt) * k;
        if (t->ground) t->alt = fmax(t->alt, terrain_height(t->lat, t->lon, NULL, NULL));
        i++;
    }
}
