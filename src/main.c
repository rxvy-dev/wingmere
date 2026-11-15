/* WINGMERE - an open-source flight simulator over the whole real Earth.  MIT licence.
 *
 * Satellite imagery and elevation stream in from public tile servers while you fly; every
 * real airport's runways (OurAirports) are laid onto the terrain with markings, lights and PAPI.
 * This file holds the game: settings, menus, input, cameras, autopilot, scoring and the HUD. */
#include "wm.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

Settings S;
static SDL_Window *win;
static RenderCfg rc = {1600, 900, 4, 2048, 1.0f, true};
static bool fullscreen;
static float volume = 0.8f;
static bool invertPitch;
static char dataDir[600], cfgPath[700], scorePath[700];

/* ------------------------------------------------------------------ state */
enum { ST_MENU, ST_SEARCH, ST_CHAL, ST_SETTINGS, ST_CREDITS, ST_LOADING, ST_FLY, ST_PAUSE };
enum { CAM_CHASE, CAM_COCKPIT, CAM_TAIL, CAM_ORBIT, CAM_TOWER, CAM_FLYBY, NCAM };
static const char *CAM_NAME[NCAM] = {"CHASE", "PILOT", "TAIL", "ORBIT", "TOWER", "FLYBY"};
static int state = ST_MENU, menuSel, setSel, chalSel, pauseSel;
static Flight FL;
static int acSel, aptSel = -1, rwSel, rwEnd, startMode, weatherSel = 1;
static float tod = 10.0f, windDir = 270, windKt = 6;
static int doy = 172;
static double utcHours;
static float simTime, realTime;
static int camMode = CAM_CHASE;
static float camYaw, camPitch = 0.16f, camZoom = 1, lookYaw, lookPitch;
static v3 chaseDir; static bool chaseInit;
static d3 flybyPos; static bool flybyOk;
static d3 towerPos; static float towerTimer;
static Camera cam;
static bool showHud = true, showMap, showFps, showHelp, paused, parkBrake;
static int mapZoom = 11;
static float simRate = 1;
static bool ap; static float apAlt, apHdg, apSpd;
static char msg[128]; static float msgTimer;
static float ctlElev, ctlAil, ctlRud;
static SDL_GameController *pad; static SDL_Joystick *stick;
static char search[32]; static int searchRes[12], nSearch, searchSel;
static float loadTimer; static int loadQueue0;
static float fps = 60;
static bool mouseLook, freezeShot;

/* landing evaluation */
typedef struct { bool have; float vs, along, across, crab, bank, speed; int rw, end; } Touch;
static Touch td; static int lastTouchdowns; static float tdTimer;
static int score; static bool scored; static char scoreWhy[64];
static float sVs, sCl, sTz, sAl;

/* landing challenges: real runways, chosen for drama */
typedef struct { const char *title, *ident, *rw; float nm, wdir, wkt, tod; int weather; const char *ac, *blurb; } Challenge;
static const Challenge CH[] = {
    {"MAHO BEACH", "TNCM", "10", 3, 100, 12, 15.5f, 1, "b737", "Skim the sunbathers on St Maarten."},
    {"LUKLA", "VNLK", "06", 2.5f, 240, 5, 8.0f, 0, "biplane", "527 m of uphill runway at 2,800 m. No go-arounds."},
    {"SABA", "TNCS", "12", 2, 90, 10, 10, 1, "biplane", "The shortest commercial runway on Earth, cliffs at both ends."},
    {"GIBRALTAR", "LXGB", "09", 4, 100, 15, 11, 2, "b737", "The Rock, a levanter wind and a runway that crosses a road."},
    {"MADEIRA", "LPMA", "05", 5, 20, 18, 9, 2, "b737", "A runway on stilts and gusts off the cliffs."},
    {"INNSBRUCK", "LOWI", "26", 8, 260, 8, 13, 1, "b737", "Thread the Inn valley between the Alps."},
    {"PARO", "VQPR", "15", 4, 150, 6, 8.5f, 1, "b737", "A Himalayan valley at 2,200 m."},
    {"QUEENSTOWN", "NZQN", "23", 6, 230, 10, 16, 1, "transport", "Mountains on every side."},
    {"ASPEN", "KASE", "15", 5, 150, 5, 12, 0, "corsair", "High, hot and steep: 7,800 ft up a valley."},
    {"SAN FRANCISCO DAWN", "KSFO", "28R", 6, 290, 12, 6.9f, 3, "b737", "Low cloud over the bay as the sun comes up."},
    {"HONG KONG NIGHT", "VHHH", "07R", 7, 70, 10, 21, 2, "b737", "Night approach past the harbour lights."},
    {"HEATHROW CROSSWIND", "EGLL", "27L", 5, 200, 22, 14, 3, "transport", "22 knots across the runway. Crab, then kick it straight."},
};
#define NCH ((int)(sizeof CH / sizeof CH[0]))
static int chalActive = -1;
static int bestScore[NCH];

static const char *WEATHER_NAME[5] = {"CLEAR", "FEW CLOUDS", "SCATTERED", "BROKEN", "OVERCAST"};
static const char *START_NAME[3] = {"ON THE RUNWAY", "ON FINAL APPROACH", "IN THE AIR"};

/* colours: black-metal red, bone white, amber, green */
#define RED 0.86f, 0.09f, 0.08f
#define WHITE 0.94f, 0.93f, 0.91f
#define AMBER 1.0f, 0.68f, 0.12f
#define GREEN 0.35f, 1.0f, 0.45f
#define GREY 0.6f, 0.6f, 0.62f

static void say(const char *m) { snprintf(msg, sizeof msg, "%s", m); msgTimer = 3.5f; }

/* ------------------------------------------------------------------ files */
static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void mkdir_p(const char *path) {
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static void dir_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *s = strrchr(out, '/');
    if (s) *s = 0; else snprintf(out, n, ".");
}
static void find_data(void) {
    char exe[600] = "", ed[600] = ".";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = 0; dir_of(exe, ed, sizeof ed); }
    char cand[8][700];
    int k = 0;
    const char *env = getenv("WINGMERE_DATA");
    if (env) snprintf(cand[k++], 700, "%s", env);
    snprintf(cand[k++], 700, "data");
    snprintf(cand[k++], 700, "%s/data", ed);
    snprintf(cand[k++], 700, "%s/../data", ed);
    snprintf(cand[k++], 700, "%s/../share/wingmere/data", ed);
    snprintf(cand[k++], 700, "/usr/local/share/wingmere/data");
    snprintf(cand[k++], 700, "/usr/share/wingmere/data");
    for (int i = 0; i < k; i++) {
        char t[760]; snprintf(t, sizeof t, "%s/airports.txt", cand[i]);
        if (file_exists(t)) { snprintf(dataDir, sizeof dataDir, "%s", cand[i]); return; }
    }
    fprintf(stderr, "wingmere: cannot find the data folder (set WINGMERE_DATA)\n");
    exit(1);
}
static void home_path(char *out, size_t n, const char *xdgVar, const char *fallback, const char *leaf) {
    const char *x = getenv(xdgVar), *home = getenv("HOME");
    if (x && x[0]) snprintf(out, n, "%s/wingmere%s", x, leaf);
    else snprintf(out, n, "%s/%s/wingmere%s", home ? home : ".", fallback, leaf);
}

/* ------------------------------------------------------------------ settings */
static void settings_defaults(void) {
    snprintf(S.imagery_url, sizeof S.imagery_url, "https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless_3857/default/g/{z}/{y}/{x}.jpg");
    snprintf(S.elevation_url, sizeof S.elevation_url, "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png");
    snprintf(S.user_agent, sizeof S.user_agent, "Wingmere/" WM_VERSION " (open-source flight simulator; https://github.com/rxvy-dev/wingmere)");
    home_path(S.cache_dir, sizeof S.cache_dir, "XDG_CACHE_HOME", ".cache", "");
    S.imagery_max_zoom = 14; S.elevation_max_zoom = 12; S.threads = 6; S.texture_budget = 1536; S.detail = 1.0f; S.offline = false;
    home_path(cfgPath, sizeof cfgPath, "XDG_CONFIG_HOME", ".config", "/wingmere.cfg");
    home_path(scorePath, sizeof scorePath, "XDG_DATA_HOME", ".local/share", "/scores.txt");
}
static char cfgAircraft[32], cfgAirport[8] = "KSFO", cfgRunway[6] = "28R";
static void settings_load(void) {
    FILE *f = fopen(cfgPath, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k) - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ') v++;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(k, "imagery_url")) snprintf(S.imagery_url, sizeof S.imagery_url, "%s", v);
        else if (!strcmp(k, "elevation_url")) snprintf(S.elevation_url, sizeof S.elevation_url, "%s", v);
        else if (!strcmp(k, "cache_dir")) snprintf(S.cache_dir, sizeof S.cache_dir, "%s", v);
        else if (!strcmp(k, "imagery_max_zoom")) S.imagery_max_zoom = atoi(v);
        else if (!strcmp(k, "elevation_max_zoom")) S.elevation_max_zoom = atoi(v);
        else if (!strcmp(k, "threads")) S.threads = atoi(v);
        else if (!strcmp(k, "texture_budget")) S.texture_budget = atoi(v);
        else if (!strcmp(k, "detail")) S.detail = (float)atof(v);
        else if (!strcmp(k, "offline")) S.offline = atoi(v);
        else if (!strcmp(k, "width")) rc.w = atoi(v);
        else if (!strcmp(k, "height")) rc.h = atoi(v);
        else if (!strcmp(k, "fullscreen")) fullscreen = atoi(v);
        else if (!strcmp(k, "msaa")) rc.msaa = atoi(v);
        else if (!strcmp(k, "render_scale")) rc.scale = (float)atof(v);
        else if (!strcmp(k, "shadow_res")) rc.shadowRes = atoi(v);
        else if (!strcmp(k, "clouds")) rc.clouds = atoi(v);
        else if (!strcmp(k, "volume")) volume = (float)atof(v);
        else if (!strcmp(k, "invert_pitch")) invertPitch = atoi(v);
        else if (!strcmp(k, "aircraft")) snprintf(cfgAircraft, sizeof cfgAircraft, "%s", v);
        else if (!strcmp(k, "airport")) snprintf(cfgAirport, sizeof cfgAirport, "%s", v);
        else if (!strcmp(k, "runway")) snprintf(cfgRunway, sizeof cfgRunway, "%s", v);
        else if (!strcmp(k, "start")) startMode = atoi(v) % 3;
        else if (!strcmp(k, "time")) tod = (float)atof(v);
        else if (!strcmp(k, "weather")) weatherSel = atoi(v) % 5;
        else if (!strcmp(k, "wind_dir")) windDir = (float)atof(v);
        else if (!strcmp(k, "wind_kt")) windKt = (float)atof(v);
    }
    fclose(f);
}
static const char *rw_ident(int ri, int end) { return end ? g_runways[ri].he : g_runways[ri].le; }
static void settings_save(void) {
    char dir[700]; dir_of(cfgPath, dir, sizeof dir); mkdir_p(dir);
    FILE *f = fopen(cfgPath, "w");
    if (!f) return;
    fprintf(f, "# WINGMERE settings - edit while the game is closed\n");
    fprintf(f, "imagery_url = %s\nelevation_url = %s\ncache_dir = %s\n", S.imagery_url, S.elevation_url, S.cache_dir);
    fprintf(f, "imagery_max_zoom = %d\nelevation_max_zoom = %d\nthreads = %d\ntexture_budget = %d\ndetail = %.2f\noffline = %d\n",
            S.imagery_max_zoom, S.elevation_max_zoom, S.threads, S.texture_budget, S.detail, S.offline);
    fprintf(f, "width = %d\nheight = %d\nfullscreen = %d\nmsaa = %d\nrender_scale = %.2f\nshadow_res = %d\nclouds = %d\nvolume = %.2f\ninvert_pitch = %d\n",
            rc.w, rc.h, fullscreen, rc.msaa, rc.scale, rc.shadowRes, rc.clouds, volume, invertPitch);
    if (g_naircraft) fprintf(f, "aircraft = %s\n", g_aircraft[acSel].id);
    if (aptSel >= 0) {
        fprintf(f, "airport = %s\n", g_airports[aptSel].ident);
        if (g_airports[aptSel].nrw) fprintf(f, "runway = %s\n", rw_ident(g_airports[aptSel].rw0 + rwSel, rwEnd));
    }
    fprintf(f, "start = %d\ntime = %.2f\nweather = %d\nwind_dir = %.0f\nwind_kt = %.0f\n", startMode, tod, weatherSel, windDir, windKt);
    fclose(f);
}
static void scores_load(void) {
    FILE *f = fopen(scorePath, "r");
    if (!f) return;
    char id[16], rw[8]; int s;
    while (fscanf(f, "%15s %7s %d", id, rw, &s) == 3)
        for (int i = 0; i < NCH; i++) if (!strcmp(CH[i].ident, id) && !strcmp(CH[i].rw, rw) && s > bestScore[i]) bestScore[i] = s;
    fclose(f);
}
static void scores_save(void) {
    char dir[700]; dir_of(scorePath, dir, sizeof dir); mkdir_p(dir);
    FILE *f = fopen(scorePath, "w");
    if (!f) return;
    for (int i = 0; i < NCH; i++) if (bestScore[i] > 0) fprintf(f, "%s %s %d\n", CH[i].ident, CH[i].rw, bestScore[i]);
    fclose(f);
}

/* ------------------------------------------------------------------ world setup */
static int aircraft_by_id(const char *id) {
    for (int i = 0; i < g_naircraft; i++) if (!strcmp(g_aircraft[i].id, id)) return i;
    return -1;
}
static void select_airport(int a, const char *rwPref) {
    if (a < 0 || a >= g_nairports) return;
    aptSel = a; rwSel = 0; rwEnd = 0;
    Airport *ap_ = &g_airports[a];
    float best = -1;
    for (int i = 0; i < ap_->nrw; i++) {
        Runway *r = &g_runways[ap_->rw0 + i];
        for (int e = 0; e < 2; e++) {
            if (rwPref && rwPref[0] && !strcasecmp(e ? r->he : r->le, rwPref)) { rwSel = i; rwEnd = e; return; }
            float score = r->length * (r->surface == 0 ? 1 : 0.3f) * (r->lighted ? 1.1f : 1);
            if (score > best) { best = score; rwSel = i; rwEnd = e; }
        }
    }
}
static int cur_runway(void) { return aptSel >= 0 && g_airports[aptSel].nrw ? g_airports[aptSel].rw0 + rwSel : -1; }
static void runway_end(int ri, int end, double *lat, double *lon, float *brg, float *elev) {
    Runway *r = &g_runways[ri];
    *lat = end ? r->lat2 : r->lat1; *lon = end ? r->lon2 : r->lon1;
    *brg = fmodf(r->heading + (end ? 180 : 0), 360); *elev = end ? r->e2 : r->e1;
}
static void apply_weather(float fieldElev) {
    static const float cover[5] = {0.0f, 0.30f, 0.45f, 0.62f, 0.86f}, haze[5] = {2.6e-5f, 3.4e-5f, 4.2e-5f, 5.5e-5f, 8e-5f};
    float base = fieldElev + (weatherSel >= 3 ? 700 : 1300), top = base + (weatherSel == 4 ? 900 : 1900);
    render_weather(cover[weatherSel], base, top, haze[weatherSel]);
}
static v3 wind_at(const Flight *f, float t) {
    float agl = fmaxf(f->agl, 0);
    float prof = clampf(0.55f + 0.45f * logf(1 + agl / 10) / logf(31), 0.4f, 1.35f);
    float gust = 1 + (weatherSel >= 2 ? 0.18f : 0.08f) * (sinf(t * 0.71f) * 0.6f + sinf(t * 1.93f + 1) * 0.3f + sinf(t * 4.1f + 2) * 0.1f);
    float spd = windKt * KT * prof * gust;
    float from = windDir * DEG;                       /* wind blows FROM this bearing */
    v3 to = vadd(vmul(f->north, -cosf(from)), vmul(f->east, -sinf(from)));
    v3 w = vmul(to, spd);
    if (weatherSel >= 2 && agl < 2500) {              /* a little turbulence in the mixed layer */
        float k = windKt * KT * 0.06f * (weatherSel - 1) * 0.5f;
        w = vadd(w, vmul(f->up, k * (sinf(t * 2.3f) * 0.5f + sinf(t * 5.7f + 3) * 0.3f + sinf(t * 11.1f) * 0.2f)));
    }
    return w;
}
static float approach_speed(const AircraftDef *a) { return 1.3f * flight_stall_speed(a, a->flaps ? 1.0f : 0.0f); }

static void spawn(void) {
    if (!g_naircraft) return;
    const AircraftDef *a = &g_aircraft[acSel];
    int ri = cur_runway();
    double lat = 37.6188, lon = -122.375; float brg = 280, elev = 4;
    if (ri >= 0) runway_end(ri, rwEnd, &lat, &lon, &brg, &elev);
    else if (aptSel >= 0) { lat = g_airports[aptSel].lat; lon = g_airports[aptSel].lon; elev = g_airports[aptSel].elev; }
    utcHours = tod - lon / 15.0;
    apply_weather(elev);
    int mode = state == ST_MENU || state == ST_SEARCH || state == ST_CHAL || state == ST_SETTINGS || state == ST_CREDITS ? 0 : startMode;
    if (chalActive >= 0 && state == ST_LOADING) mode = 1;
    float nm = chalActive >= 0 ? CH[chalActive].nm : 5;
    if (mode == 0) {
        double la, lo;
        geo_offset(lat, lon, brg, 30 + a->length * 0.6, &la, &lo);
        flight_reset(&FL, a, la, lo, 0, brg, 0, true);
        parkBrake = true; FL.throttle = 0;
    } else {
        double la, lo, dist = mode == 1 ? nm * 1852.0 : 16000.0;
        geo_offset(lat, lon, brg + 180, dist, &la, &lo);
        float alt = mode == 1 ? elev + 15 + (float)dist * tanf(3 * DEG) : elev + 900;
        float spd = mode == 1 ? approach_speed(a) : (a->jet ? 128.0f : a->v_cruise * 0.9f);
        flight_reset(&FL, a, la, lo, alt, brg, spd, false);
        if (mode == 1) {
            FL.flapsSet = a->flaps; FL.flaps = a->flaps ? 1 : 0;
            FL.gearDown = FL.gearCmd = 1;
        } else { FL.flapsSet = 0; FL.flaps = 0; if (a->retract) FL.gearDown = FL.gearCmd = 0; }
        FL.wind = wind_at(&FL, 0);
        FL.vel = vadd(FL.vel, FL.wind);
        flight_trim(&FL, mode == 1 ? -3.0f : 0.0f);
        parkBrake = false;
    }
    ctlElev = ctlAil = ctlRud = 0;
    ap = false; chaseInit = false; flybyOk = false; towerTimer = 0;
    td.have = false; lastTouchdowns = FL.touchdowns; tdTimer = 0; scored = false; score = 0;
    fx_clear();
    simTime = 0;
    flight_step(&FL, 1e-4f);   /* fill in the read-outs */
}
static struct { float tod, wdir, wkt; int weather, ac, apt, rw, end; bool saved; } freeCfg;
static void restore_free(void) {
    if (!freeCfg.saved) return;
    tod = freeCfg.tod; windDir = freeCfg.wdir; windKt = freeCfg.wkt; weatherSel = freeCfg.weather; acSel = freeCfg.ac;
    aptSel = freeCfg.apt; rwSel = freeCfg.rw; rwEnd = freeCfg.end; freeCfg.saved = false;
}
static void start_flight(void) {
    state = ST_LOADING; loadTimer = 0;
    spawn();
    int q, b, ok, fail; tiles_stats(&q, &b, &ok, &fail); loadQueue0 = q + b + 1;
    if (chalActive < 0) settings_save();
}
static void start_challenge(int i) {
    const Challenge *c = &CH[i];
    int a = airport_find(c->ident);
    if (a < 0) { say("THAT AIRPORT IS MISSING FROM THE DATA"); return; }
    if (!freeCfg.saved) { freeCfg.tod = tod; freeCfg.wdir = windDir; freeCfg.wkt = windKt; freeCfg.weather = weatherSel; freeCfg.ac = acSel; freeCfg.apt = aptSel; freeCfg.rw = rwSel; freeCfg.end = rwEnd; freeCfg.saved = true; }
    select_airport(a, c->rw);
    int ac = aircraft_by_id(c->ac); if (ac >= 0) acSel = ac;
    windDir = c->wdir; windKt = c->wkt; tod = c->tod; weatherSel = c->weather;
    chalActive = i;
    start_flight();
}

/* ------------------------------------------------------------------ autopilot and helpers */
static float wrap180(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static float prevPitch, prevBank;
static void autopilot(float dt) {
    float pr = (FL.pitch - prevPitch) / fmaxf(dt, 1e-3f), br = (FL.bank - prevBank) / fmaxf(dt, 1e-3f);
    /* heading -> bank -> aileron */
    float he = wrap180(apHdg - FL.heading);
    float bankT = clampf(he * 1.2f, -25, 25);
    FL.ail = clampf((bankT - FL.bank) * 0.045f - br * 0.02f, -1, 1);
    /* altitude -> pitch -> elevator */
    float pitchT = clampf((apAlt - (float)FL.alt) * 0.035f - FL.vs * 0.25f, -8, 12) + fabsf(FL.bank) * 0.05f;
    FL.elev = clampf((pitchT - FL.pitch) * 0.07f - pr * 0.03f, -1, 1);
    /* speed -> throttle */
    FL.throttle = clampf(FL.throttle + ((apSpd - FL.ias) * 0.04f - 0.0f) * dt, 0, 1);
    FL.rud = 0;
}
static void ap_toggle(void) {
    if (!ap) {
        if (FL.onGround) { say("AUTOPILOT NEEDS TO BE AIRBORNE"); return; }
        ap = true; apAlt = (float)FL.alt; apHdg = roundf(FL.heading); apSpd = FL.ias;
        say("AUTOPILOT ON - HOLDS HEADING, ALTITUDE, SPEED");
    } else { ap = false; audio_event(3); say("AUTOPILOT OFF"); }
}
/* aircraft position in a runway's frame, measured from the threshold it is heading towards */
static bool runway_frame(int ri, double lat, double lon, float hdg, float *along, float *across, int *end) {
    Runway *r = &g_runways[ri];
    float d = (float)geo_dist(r->lat1, r->lon1, lat, lon), b = (float)geo_bearing(r->lat1, r->lon1, lat, lon);
    float al = d * cosf((b - r->heading) * DEG), ac = d * sinf((b - r->heading) * DEG);
    bool rev = fabsf(wrap180(hdg - r->heading)) > 90;
    if (rev) { al = r->length - al; ac = -ac; }
    *along = al; *across = ac; *end = rev;
    return true;
}
/* the runway we look to be landing on: nearest end ahead of us, roughly lined up */
static int approach_rw = -1, approach_end;
static void find_approach(void) {
    int list[64], n = runways_near(FL.lat, FL.lon, 30000, list, 64);
    float best = 1e9f; approach_rw = -1;
    if (chalActive >= 0) {
        int a = airport_find(CH[chalActive].ident);
        if (a >= 0) for (int i = 0; i < g_airports[a].nrw; i++) {
            Runway *r = &g_runways[g_airports[a].rw0 + i];
            if (!strcasecmp(r->le, CH[chalActive].rw)) { approach_rw = g_airports[a].rw0 + i; approach_end = 0; return; }
            if (!strcasecmp(r->he, CH[chalActive].rw)) { approach_rw = g_airports[a].rw0 + i; approach_end = 1; return; }
        }
    }
    for (int k = 0; k < n; k++) {
        Runway *r = &g_runways[list[k]];
        if (r->surface == 2) continue;
        for (int e = 0; e < 2; e++) {
            float rh = fmodf(r->heading + (e ? 180 : 0), 360);
            if (fabsf(wrap180(FL.heading - rh)) > 35) continue;
            float al, ac; int ee;
            runway_frame(list[k], FL.lat, FL.lon, rh, &al, &ac, &ee);
            if (al > r->length * 0.8f || fabsf(ac) > fmaxf(-al, 0) * 0.35f + 200) continue;
            float cost = fabsf(al) + fabsf(ac) * 3;
            if (cost < best) { best = cost; approach_rw = list[k]; approach_end = e; }
        }
    }
}

/* ------------------------------------------------------------------ landing scoring */
static const char *grade_of(float fpm) {
    float v = fabsf(fpm);
    return v < 60 ? "BUTTER" : v < 180 ? "SMOOTH" : v < 300 ? "FIRM" : v < 500 ? "HARD" : "CARRIER STYLE";
}
static void on_touchdown(void) {
    td.have = true; td.vs = FL.lastTouchdownVs / FT * 60; td.speed = FL.gs; td.bank = FL.bank; td.rw = -1;
    tdTimer = 6;
    find_approach();
    int ri = approach_rw;
    if (ri < 0) {
        int list[16], n = runways_near(FL.lat, FL.lon, 3000, list, 16);
        float bd = 1e9f;
        for (int k = 0; k < n; k++) { float al, ac; int e; runway_frame(list[k], FL.lat, FL.lon, FL.heading, &al, &ac, &e); if (fabsf(ac) < bd) { bd = fabsf(ac); ri = list[k]; } }
    }
    if (ri >= 0) {
        int e; runway_frame(ri, FL.lat, FL.lon, FL.heading, &td.along, &td.across, &e);
        td.rw = ri; td.end = e;
        v3 vh = vsub(FL.vel, vmul(FL.up, vdot(FL.vel, FL.up)));
        float trackDeg = atan2f(vdot(vh, FL.east), vdot(vh, FL.north)) / DEG;
        td.crab = wrap180(FL.heading - trackDeg);
    }
}
static void evaluate_landing(void) {
    if (scored || chalActive < 0) return;
    if (FL.crashed) {
        scored = true; score = 0; snprintf(scoreWhy, sizeof scoreWhy, "%s", FL.crashWhy);
        return;
    }
    if (!td.have || !FL.onGround || FL.gs > 18 * KT) return;
    scored = true;
    Runway *r = td.rw >= 0 ? &g_runways[td.rw] : NULL;
    bool onRw = r && fabsf(td.across) < r->width * 0.5f && td.along > -5 && td.along < r->length;
    bool rightRw = r && (!strcasecmp(td.end ? r->he : r->le, CH[chalActive].rw));
    if (!onRw) { score = 0; snprintf(scoreWhy, sizeof scoreWhy, "LANDED OFF THE RUNWAY"); return; }
    sVs = 400 * clampf(1 - (fabsf(td.vs) - 60) / 540, 0, 1);
    sCl = 250 * clampf(1 - fabsf(td.across) / (r->width * 0.5f), 0, 1);
    float aim = fminf(300, r->length * 0.18f);
    sTz = 250 * clampf(1 - fabsf(td.along - aim) / fmaxf(aim * 2, 300), 0, 1);
    sAl = 100 * clampf(1 - fabsf(td.crab) / 10, 0, 1);
    score = (int)(sVs + sCl + sTz + sAl);
    if (!rightRw) { score /= 2; snprintf(scoreWhy, sizeof scoreWhy, "WRONG RUNWAY - HALF POINTS"); }
    else snprintf(scoreWhy, sizeof scoreWhy, "%s", grade_of(td.vs));
    if (score > bestScore[chalActive]) { bestScore[chalActive] = score; scores_save(); strncat(scoreWhy, " - NEW BEST", sizeof scoreWhy - strlen(scoreWhy) - 1); }
}

/* ------------------------------------------------------------------ cameras */
static void ground_clamp(d3 *p) {
    double lat, lon, h; ecef_to_geo(*p, &lat, &lon, &h);
    float g = terrain_height(lat, lon, NULL, NULL) + 1.5f;
    if (h < g) *p = geo_to_ecef(lat, lon, g);
}
static void update_camera(float dt) {
    const AircraftDef *a = FL.def;
    v3 F = q_rot(FL.rot, V(0, 0, 1)), Ub = q_rot(FL.rot, V(0, 1, 0)), Rb = vcross(F, Ub), up = FL.up;
    float size = a->cam_dist * camZoom;
    cam.fov = 55 * DEG;
    cam.up = up;
    switch (camMode) {
    case CAM_CHASE: {
        v3 dir = vnorm(vsub(F, vmul(up, vdot(F, up) * 0.7f)));
        if (!chaseInit) { chaseDir = dir; chaseInit = true; }
        chaseDir = vnorm(vlerp(chaseDir, dir, 1 - expf(-dt * 4)));
        v3 side = vnorm(vcross(chaseDir, up));
        v3 back = vmul(chaseDir, -1);
        back = vadd(vmul(back, cosf(camYaw)), vmul(side, sinf(camYaw)));
        v3 off = vadd(vmul(back, size * cosf(camPitch)), vmul(up, size * sinf(camPitch)));
        cam.pos = dadd(FL.pos, f2d(vadd(off, vmul(up, a->height * 0.25f))));
        d3 target = dadd(FL.pos, f2d(vadd(vmul(up, a->height * 0.2f), vmul(chaseDir, size * 0.08f))));
        cam.fwd = vnorm(d2f(dsub(target, cam.pos)));
        break;
    }
    case CAM_COCKPIT: {
        v3 eye = V(0, a->height * 0.22f, a->length * (a->jet ? 0.42f : 0.05f));
        cam.pos = dadd(FL.pos, f2d(q_rot(FL.rot, eye)));
        quat look = q_mul(q_axis(Ub, -lookYaw), q_axis(Rb, -lookPitch));
        cam.fwd = q_rot(look, F); cam.up = q_rot(look, Ub);
        cam.fov = 65 * DEG / camZoom;
        break;
    }
    case CAM_TAIL: {
        v3 off = V(0, a->height * 0.55f + a->length * 0.06f, a->bmin.z - a->length * 0.55f * camZoom);
        cam.pos = dadd(FL.pos, f2d(q_rot(FL.rot, off)));
        cam.fwd = vnorm(vadd(F, vmul(Ub, -0.08f)));
        cam.up = Ub;
        break;
    }
    case CAM_ORBIT: {
        float yaw = camYaw + (state == ST_FLY ? 0 : realTime * 0.08f);
        v3 dir = vadd(vmul(FL.north, cosf(yaw)), vmul(FL.east, sinf(yaw)));
        float p = clampf(camPitch, -0.3f, 1.4f);
        v3 off = vadd(vmul(dir, size * 1.1f * cosf(p)), vmul(up, size * 1.1f * sinf(p)));
        cam.pos = dadd(FL.pos, f2d(off));
        cam.fwd = vnorm(vmul(off, -1));
        if (state != ST_FLY && state != ST_PAUSE) {   /* in the menus, keep the aircraft clear of the left-hand panel */
            v3 right = vnorm(vcross(cam.fwd, up));
            cam.fwd = vnorm(vsub(vmul(off, -1), vmul(right, size * 0.42f)));
        }
        break;
    }
    case CAM_TOWER: {
        towerTimer -= dt;
        if (towerTimer <= 0) {
            towerTimer = 2;
            double dist; int ai = airport_nearest(FL.lat, FL.lon, &dist);
            if (ai >= 0 && dist < 25000 && g_airports[ai].nrw) {
                Runway *r = &g_runways[g_airports[ai].rw0];
                double mlat = (r->lat1 + r->lat2) / 2, mlon = (r->lon1 + r->lon2) / 2, tl, tn;
                geo_offset(mlat, mlon, r->heading + 90, r->width * 0.5 + 260, &tl, &tn);
                towerPos = geo_to_ecef(tl, tn, (r->e1 + r->e2) * 0.5 + 30);
            } else {
                double tl, tn; geo_offset(FL.lat, FL.lon, FL.heading + 50, 900, &tl, &tn);
                towerPos = geo_to_ecef(tl, tn, terrain_height(tl, tn, NULL, NULL) + 20);
            }
        }
        cam.pos = towerPos;
        d3 rel = dsub(FL.pos, cam.pos);
        float d = (float)dlen(rel);
        cam.fwd = vnorm(d2f(rel));
        cam.fov = clampf(2 * atanf(a->length * 2.2f / fmaxf(d, 1)), 1.5f * DEG, 60 * DEG);
        break;
    }
    case CAM_FLYBY: {
        v3 vel = vlen(FL.vel) > 5 ? vnorm(FL.vel) : F;
        if (!flybyOk || vdot(d2f(dsub(flybyPos, FL.pos)), vel) < -size * 12 || dlen(dsub(flybyPos, FL.pos)) > 2500) {
            v3 side = vnorm(vcross(vel, up));
            float sp = fmaxf(vlen(FL.vel), 20);
            flybyPos = dadd(FL.pos, f2d(vadd(vadd(vmul(vel, sp * 4), vmul(side, size * (1.5f + 1.5f * ((int)realTime & 1 ? 1 : -1)))), vmul(up, size * 0.3f))));
            ground_clamp(&flybyPos);
            flybyOk = true;
        }
        cam.pos = flybyPos;
        d3 rel = dsub(FL.pos, cam.pos);
        cam.fwd = vnorm(d2f(rel));
        cam.fov = clampf(2 * atanf(a->length * 1.6f / fmaxf((float)dlen(rel), 1)), 8 * DEG, 60 * DEG);
        break;
    }
    }
    if (camMode != CAM_COCKPIT) ground_clamp(&cam.pos);
}

/* ------------------------------------------------------------------ HUD */
static float W, H, s_;
#define SZ(v) ((v) * s_)
static void panel(float x, float y, float w, float h, float a) { hud_rect(x, y, w, h, 0.02f, 0.02f, 0.025f, a); hud_rect(x, y, w, SZ(2), RED, 0.9f); }
static void attribution(float a) {
    hud_text(W - SZ(12), H - SZ(5), SZ(9), "IMAGERY: SENTINEL-2 CLOUDLESS 2016 BY EOX (CC BY 4.0)  ELEVATION: TERRAIN TILES (MAPZEN, AWS)  AIRPORTS: OURAIRPORTS", WHITE, a, 2);
}
static void draw_compass(float cx, float y, float w) {
    float hdg = FL.heading, ppd = w / 90;
    hud_clip(cx - w / 2, y - SZ(30), w, SZ(46));
    for (int d = (int)hdg - 50; d <= (int)hdg + 50; d++) {
        if (d % 5) continue;
        float x = cx + wrap180((float)d - hdg) * ppd;
        float len = d % 10 ? SZ(5) : SZ(10);
        hud_line(x, y, x, y - len, SZ(2), WHITE, 0.9f);
        if (d % 30 == 0) {
            int dd = ((d % 360) + 360) % 360; char b[8];
            if (dd == 0) snprintf(b, 8, "N"); else if (dd == 90) snprintf(b, 8, "E"); else if (dd == 180) snprintf(b, 8, "S"); else if (dd == 270) snprintf(b, 8, "W");
            else snprintf(b, 8, "%d", dd / 10);
            hud_text(x, y - SZ(14), SZ(12), b, WHITE, 0.95f, 1);
        }
    }
    hud_clip_off();
    if (ap) { float x = cx + wrap180(apHdg - hdg) * ppd; if (fabsf(x - cx) < w / 2) hud_tri(x, y + SZ(2), x - SZ(6), y + SZ(10), x + SZ(6), y + SZ(10), GREEN, 0.95f); }
    char b[8]; snprintf(b, 8, "%03d", ((int)lroundf(hdg) + 360) % 360);
    hud_rect(cx - SZ(26), y + SZ(4), SZ(52), SZ(24), 0, 0, 0, 0.8f);
    hud_text(cx, y + SZ(23), SZ(16), b, WHITE, 1, 1);
}
static void draw_pitch_ladder(void) {
    /* drawn in the pilot view: horizon and pitch bars projected through the camera */
    v3 hf = vnorm(vsub(cam.fwd, vmul(FL.up, vdot(cam.fwd, FL.up))));
    v3 side = vnorm(vcross(hf, FL.up));
    hud_clip(W * 0.2f, H * 0.12f, W * 0.6f, H * 0.66f);
    for (int p = -30; p <= 30; p += 5) {
        v3 dir = vadd(vmul(hf, cosf(p * DEG)), vmul(FL.up, sinf(p * DEG)));
        float wdt = p == 0 ? 12 : 3.5f;
        d3 c = dadd(cam.pos, f2d(vmul(dir, 1000)));
        d3 l = dadd(c, f2d(vmul(side, -wdt * 17.5f))), r = dadd(c, f2d(vmul(side, wdt * 17.5f)));
        float x0, y0, x1, y1;
        if (!world_to_screen(l, &cam, &x0, &y0) || !world_to_screen(r, &cam, &x1, &y1)) continue;
        if (p == 0) { hud_line(x0, y0, x1, y1, SZ(2), GREEN, 0.85f); continue; }
        float g0 = 0.38f;
        float mx0 = x0 + (x1 - x0) * g0, my0 = y0 + (y1 - y0) * g0, mx1 = x1 - (x1 - x0) * g0, my1 = y1 - (y1 - y0) * g0;
        hud_line(x0, y0, mx0, my0, SZ(1.6f), GREEN, 0.8f);
        hud_line(mx1, my1, x1, y1, SZ(1.6f), GREEN, 0.8f);
        char b[8]; snprintf(b, 8, "%d", p);
        hud_text(x1 + SZ(8), y1 + SZ(5), SZ(11), b, GREEN, 0.8f, 0);
    }
    /* flight path marker: where the aircraft is actually going */
    if (vlen(FL.vel) > 8) {
        float x, y;
        if (world_to_screen(dadd(cam.pos, f2d(vmul(vnorm(FL.vel), 1000))), &cam, &x, &y)) {
            hud_circle(x, y, SZ(8), SZ(2), GREEN, 0.95f);
            hud_line(x - SZ(20), y, x - SZ(8), y, SZ(2), GREEN, 0.95f); hud_line(x + SZ(8), y, x + SZ(20), y, SZ(2), GREEN, 0.95f);
            hud_line(x, y - SZ(8), x, y - SZ(16), SZ(2), GREEN, 0.95f);
        }
    }
    hud_clip_off();
    /* boresight */
    hud_line(W / 2 - SZ(18), H / 2, W / 2 - SZ(6), H / 2 + SZ(6), SZ(2), GREEN, 0.9f);
    hud_line(W / 2 - SZ(6), H / 2 + SZ(6), W / 2, H / 2, SZ(2), GREEN, 0.9f);
    hud_line(W / 2, H / 2, W / 2 + SZ(6), H / 2 + SZ(6), SZ(2), GREEN, 0.9f);
    hud_line(W / 2 + SZ(6), H / 2 + SZ(6), W / 2 + SZ(18), H / 2, SZ(2), GREEN, 0.9f);
}
static void draw_ils(void) {
    if (approach_rw < 0) return;
    Runway *r = &g_runways[approach_rw];
    double tl, tn; float brg, el;
    runway_end(approach_rw, approach_end, &tl, &tn, &brg, &el);
    float dist = (float)geo_dist(FL.lat, FL.lon, tl, tn), b = (float)geo_bearing(tl, tn, FL.lat, FL.lon);
    float locDev = wrap180(b - (brg + 180));           /* degrees off the extended centreline */
    float hAbove = (float)FL.alt - el - 15;
    float gsDev = atan2f(hAbove, fmaxf(dist, 1)) / DEG - 3.0f;
    if (dist > 30000 || dist < 50) return;
    float cx = W / 2, cy = H * 0.5f;
    float lx = cx - SZ(150), ly = H - SZ(160), dots = SZ(26);
    /* localizer, 2.5 degrees full scale */
    for (int i = -2; i <= 2; i++) hud_circle(cx + i * dots * 1.2f, ly, SZ(4), SZ(1.5f), WHITE, i ? 0.7f : 0.0f);
    hud_line(cx, ly - SZ(9), cx, ly + SZ(9), SZ(2), WHITE, 0.8f);
    float lpos = clampf(-locDev / 1.25f, -2.4f, 2.4f) * dots * 1.2f;
    hud_tri(cx + lpos, ly - SZ(9), cx + lpos - SZ(8), ly, cx + lpos + SZ(8), ly, 0.95f, 0.3f, 0.95f, 0.95f);
    hud_tri(cx + lpos, ly + SZ(9), cx + lpos - SZ(8), ly, cx + lpos + SZ(8), ly, 0.95f, 0.3f, 0.95f, 0.95f);
    /* glideslope, 0.7 degrees full scale */
    float gx = W - SZ(290);
    for (int i = -2; i <= 2; i++) hud_circle(gx, cy + i * dots * 1.2f, SZ(4), SZ(1.5f), WHITE, i ? 0.7f : 0.0f);
    hud_line(gx - SZ(9), cy, gx + SZ(9), cy, SZ(2), WHITE, 0.8f);
    float gpos = clampf(gsDev / 0.35f, -2.4f, 2.4f) * dots * 1.2f;
    hud_tri(gx - SZ(9), cy + gpos, gx, cy + gpos - SZ(8), gx, cy + gpos + SZ(8), 0.95f, 0.3f, 0.95f, 0.95f);
    hud_tri(gx + SZ(9), cy + gpos, gx, cy + gpos - SZ(8), gx, cy + gpos + SZ(8), 0.95f, 0.3f, 0.95f, 0.95f);
    char t[64];
    snprintf(t, sizeof t, "%s %s  %.1f NM", g_airports[r->airport].ident, rw_ident(approach_rw, approach_end), dist / 1852);
    hud_text(lx + SZ(150), ly - SZ(22), SZ(13), t, 0.95f, 0.5f, 0.95f, 0.95f, 1);
}
static void bar_v(float x, float y, float w, float h, float v, float r, float g, float b, const char *label) {
    hud_rect(x, y, w, h, 0, 0, 0, 0.5f);
    hud_rect(x, y + h * (1 - v), w, h * v, r, g, b, 0.85f);
    hud_text(x + w / 2, y + h + SZ(16), SZ(11), label, WHITE, 0.9f, 1);
}
static void draw_flight_hud(void) {
    char b[128];
    float t = realTime, blink = fmodf(t, 0.8f) < 0.45f ? 1.0f : 0.25f;
    if (camMode == CAM_COCKPIT) draw_pitch_ladder();
    /* bottom panel */
    float ph = SZ(96), py = H - ph;
    panel(0, py, W, ph, 0.5f);
    /* speed */
    hud_text(SZ(40), py + SZ(26), SZ(11), "IAS KT", GREY, 0.9f, 0);
    snprintf(b, sizeof b, "%d", (int)lroundf(FL.ias / KT));
    float vs1 = flight_stall_speed(FL.def, FL.flaps);   /* IAS here is equivalent airspeed, so the sea-level figure applies */
    bool slow = !FL.onGround && FL.ias < vs1 * 1.12f;
    if (slow) hud_text(SZ(40), py + SZ(70), SZ(38), b, AMBER, 1, 0); else hud_text(SZ(40), py + SZ(70), SZ(38), b, WHITE, 1, 0);
    snprintf(b, sizeof b, "GS %d", (int)lroundf(FL.gs / KT));
    hud_text(SZ(190), py + SZ(48), SZ(13), b, WHITE, 0.85f, 0);
    snprintf(b, sizeof b, "AOA %.0f", FL.aoa);
    hud_text(SZ(190), py + SZ(70), SZ(13), b, WHITE, 0.85f, 0);
    snprintf(b, sizeof b, "G %.1f", FL.gload);
    hud_text(SZ(190), py + SZ(26), SZ(13), b, WHITE, 0.85f, 0);
    /* engine, flaps, gear */
    float bx = SZ(300);
    bar_v(bx, py + SZ(14), SZ(16), SZ(56), FL.throttle, RED, "THR");
    snprintf(b, sizeof b, "%d%%", (int)lroundf(FL.throttle * 100));
    hud_text(bx + SZ(28), py + SZ(40), SZ(13), b, WHITE, 0.95f, 0);
    if (FL.def->jet) snprintf(b, sizeof b, "N1 %.0f", FL.rpm); else snprintf(b, sizeof b, "RPM %.0f", FL.rpm);
    hud_text(bx + SZ(28), py + SZ(62), SZ(11), b, GREY, 0.95f, 0);
    float fx = SZ(430);
    if (FL.def->flaps) { snprintf(b, sizeof b, "FLAPS %d/%d", FL.flapsSet, FL.def->flaps); hud_text(fx, py + SZ(30), SZ(13), b, WHITE, 0.95f, 0); }
    if (FL.def->retract) {
        if (FL.gearDown > 0.98f) hud_text(fx, py + SZ(52), SZ(13), "GEAR DOWN", GREEN, 0.95f, 0);
        else if (FL.gearDown < 0.02f) hud_text(fx, py + SZ(52), SZ(13), "GEAR UP", GREY, 0.95f, 0);
        else hud_text(fx, py + SZ(52), SZ(13), "GEAR TRANSIT", AMBER, 0.95f, 0);
    } else hud_text(fx, py + SZ(52), SZ(13), "FIXED GEAR", GREY, 0.8f, 0);
    if (FL.brake > 0.5f) hud_text(fx, py + SZ(74), SZ(13), parkBrake ? "PARKING BRAKE" : "BRAKES", AMBER, 0.95f, 0);
    float trimY = py + SZ(44);
    hud_text(fx + SZ(170), py + SZ(30), SZ(11), "TRIM", GREY, 0.9f, 0);
    hud_rect(fx + SZ(170), trimY, SZ(60), SZ(6), 0, 0, 0, 0.6f);
    hud_rect(fx + SZ(198) + FL.trim * SZ(30), trimY - SZ(3), SZ(4), SZ(12), WHITE, 1);
    /* heading */
    draw_compass(W / 2, py + SZ(40), SZ(360));
    /* altitude */
    float ax = W - SZ(40);
    hud_text(ax, py + SZ(26), SZ(11), "ALT FT", GREY, 0.9f, 2);
    snprintf(b, sizeof b, "%d", (int)lroundf((float)FL.alt / FT / 10) * 10);
    hud_text(ax, py + SZ(70), SZ(38), b, WHITE, 1, 2);
    float fpm = FL.vs / FT * 60;
    snprintf(b, sizeof b, "VS %+d", (int)lroundf(fpm / 10) * 10);
    hud_text(W - SZ(250), py + SZ(48), SZ(13), b, fpm < -1500 ? 1.0f : 0.94f, fpm < -1500 ? 0.68f : 0.93f, fpm < -1500 ? 0.12f : 0.91f, 0.9f, 0);
    if (FL.agl < 2500 * FT) { snprintf(b, sizeof b, "RA %d", (int)lroundf(fmaxf(FL.agl, 0) / FT)); hud_text(W - SZ(250), py + SZ(70), SZ(13), b, GREEN, 0.9f, 0); }
    snprintf(b, sizeof b, "WIND %03.0f/%.0f", windDir, windKt);
    hud_text(W - SZ(250), py + SZ(26), SZ(11), b, GREY, 0.9f, 0);
    /* top left: aircraft and autopilot */
    snprintf(b, sizeof b, "%s", FL.def->name);
    hud_text(SZ(24), SZ(36), SZ(15), b, WHITE, 0.95f, 0);
    snprintf(b, sizeof b, "%s CAM", CAM_NAME[camMode]);
    hud_text(SZ(24), SZ(58), SZ(11), b, GREY, 0.9f, 0);
    if (simRate != 1) { snprintf(b, sizeof b, "SIM RATE X%.0f", simRate); hud_text(SZ(24), SZ(78), SZ(12), b, AMBER, 0.95f, 0); }
    if (ap) {
        snprintf(b, sizeof b, "AP  HDG %03.0f  ALT %d  SPD %d", fmodf(apHdg + 360, 360), (int)lroundf(apAlt / FT / 100) * 100, (int)lroundf(apSpd / KT));
        hud_text(SZ(24), SZ(100), SZ(13), b, GREEN, 0.95f, 0);
    }
    if (chalActive >= 0) {
        snprintf(b, sizeof b, "CHALLENGE: %s", CH[chalActive].title);
        hud_text(W / 2, SZ(36), SZ(14), b, RED, 0.95f, 1);
    }
    /* top right: clock and scenery status */
    float lt = fmodf((float)(utcHours + simTime / 3600.0 + FL.lon / 15.0) + 48, 24);
    snprintf(b, sizeof b, "%02d:%02d LOCAL", (int)lt, (int)(fmodf(lt, 1) * 60));
    hud_text(W - SZ(24), SZ(36), SZ(13), b, WHITE, 0.9f, 2);
    int q, bz, ok, fail; tiles_stats(&q, &bz, &ok, &fail);
    if (fail < 0) hud_text(W - SZ(24), SZ(56), SZ(11), "OFFLINE - PROCEDURAL TERRAIN", AMBER, 0.9f, 2);
    else if (q + bz > 0) { snprintf(b, sizeof b, "STREAMING %d TILES", q + bz); hud_text(W - SZ(24), SZ(56), SZ(11), b, GREY, 0.9f, 2); }
    if (showFps) {
        int dr, me, tx, el; terrain_stats(&dr, &me, &tx, &el);
        snprintf(b, sizeof b, "%.0f FPS  %d TILES  %d TEX  %d DEM", fps, dr, tx, el);
        hud_text(W - SZ(24), SZ(76), SZ(11), b, GREY, 0.9f, 2);
    }
    draw_ils();
    /* warnings */
    float wy = SZ(140);
    if (FL.stall) { hud_text(W / 2, wy, SZ(30), "STALL", RED, blink, 1); wy += SZ(40); }
    if (FL.ias > FL.def->vne) { hud_text(W / 2, wy, SZ(24), "OVERSPEED", RED, blink, 1); wy += SZ(34); }
    if (!FL.onGround && FL.agl < 600 && FL.vs < -fmaxf(FL.agl, 30) / 9 && !(FL.gearDown > 0.9f && FL.agl < 120 && FL.vs > -10)) { hud_text(W / 2, wy, SZ(30), "PULL UP", RED, blink, 1); wy += SZ(40); }
    if (FL.def->retract && FL.gearDown < 0.5f && !FL.onGround && FL.agl < 250 && FL.vs < -1) { hud_text(W / 2, wy, SZ(24), "GEAR", AMBER, blink, 1); wy += SZ(34); }
    if (fabsf(FL.bank) > 50 && !FL.onGround) { hud_text(W / 2, wy, SZ(20), "BANK ANGLE", AMBER, blink, 1); wy += SZ(30); }
    if (paused && !freezeShot) hud_text(W / 2, H * 0.42f, SZ(34), "PAUSED", WHITE, 1, 1);
    /* touchdown read-out */
    if (td.have && tdTimer > 0) {
        float a = fminf(tdTimer, 1);
        snprintf(b, sizeof b, "%+d FPM", (int)lroundf(td.vs));
        hud_text(W / 2, H * 0.26f, SZ(40), b, WHITE, a, 1);
        hud_title(W / 2, H * 0.26f + SZ(46), SZ(22), grade_of(td.vs), RED, a, 1);
        if (td.rw >= 0) {
            snprintf(b, sizeof b, "%.0f M PAST THRESHOLD   %.1f M %s OF CENTRELINE   CRAB %.0f", td.along, fabsf(td.across), td.across > 0 ? "RIGHT" : "LEFT", fabsf(td.crab));
            hud_text(W / 2, H * 0.26f + SZ(76), SZ(12), b, WHITE, a * 0.9f, 1);
        }
    }
}
static void draw_map(void) {
    float m = SZ(40);
    hud_rect(0, 0, W, H, 0, 0, 0, 0.55f);
    hud_end();
    glEnable(GL_SCISSOR_TEST); glScissor((int)m, (int)m, (int)(W - 2 * m), (int)(H - 2 * m));
    terrain_draw_map(g_progMap, FL.lat, FL.lon, mapZoom, W, H);
    glDisable(GL_SCISSOR_TEST);
    hud_begin();
    float tilePx = 256 * (H / 1080.0f) * 1.2f;
    double cx = merc_x(FL.lon, mapZoom), cy = merc_y(FL.lat, mapZoom);
    hud_clip(m, m, W - 2 * m, H - 2 * m);
    double radius = (W / tilePx) * 40075000.0 * cos(FL.lat * M_PI / 180) / (1 << mapZoom);
    int list[400], n = runways_near(FL.lat, FL.lon, fmin(radius, 400000), list, 400);
    int lastApt = -1;
    for (int k = 0; k < n; k++) {
        Runway *r = &g_runways[list[k]];
        float x0 = W / 2 + (float)((merc_x(r->lon1, mapZoom) - cx) * tilePx), y0 = H / 2 + (float)((merc_y(r->lat1, mapZoom) - cy) * tilePx);
        float x1 = W / 2 + (float)((merc_x(r->lon2, mapZoom) - cx) * tilePx), y1 = H / 2 + (float)((merc_y(r->lat2, mapZoom) - cy) * tilePx);
        hud_line(x0, y0, x1, y1, fmaxf(SZ(3), r->width * tilePx / 256 * (1 << mapZoom) / 156543.0f), WHITE, 0.9f);
        if (r->airport != lastApt) { lastApt = r->airport; hud_text((x0 + x1) / 2 + SZ(8), (y0 + y1) / 2 - SZ(8), SZ(12), g_airports[r->airport].ident, AMBER, 0.95f, 0); }
    }
    hud_clip_off();
    /* the aircraft */
    float hd = FL.heading * DEG, x = W / 2, y = H / 2, sz = SZ(14);
    float fxv = sinf(hd), fyv = -cosf(hd);
    hud_tri(x + fxv * sz, y + fyv * sz, x - fxv * sz * 0.6f - fyv * sz * 0.7f, y - fyv * sz * 0.6f + fxv * sz * 0.7f, x - fxv * sz * 0.6f + fyv * sz * 0.7f, y - fyv * sz * 0.6f - fxv * sz * 0.7f, RED, 1);
    char b[96];
    snprintf(b, sizeof b, "MAP  ZOOM %d   +/- TO ZOOM   M TO CLOSE", mapZoom);
    hud_text(m + SZ(10), m + SZ(26), SZ(14), b, WHITE, 0.95f, 0);
    snprintf(b, sizeof b, "%.4f %.4f", FL.lat, FL.lon);
    hud_text(W - m - SZ(10), m + SZ(26), SZ(12), b, WHITE, 0.9f, 2);
}
static void draw_help(void) {
    static const char *L[] = {
        "W S / UP DOWN     PITCH (S PULLS THE NOSE UP)", "A D / LEFT RIGHT  ROLL", "Q E               RUDDER AND NOSEWHEEL",
        "R F / PGUP PGDN   THROTTLE    1-9 0  SET THROTTLE   `  IDLE", "G                 LANDING GEAR", "V B               FLAPS DOWN / UP",
        "SPACE             BRAKES (SHIFT+SPACE PARKING BRAKE)", ", .               TRIM NOSE DOWN / UP", "Z                 AUTOPILOT (CONTROLS THEN ADJUST ITS TARGETS)",
        "C  F2-F7          CAMERAS     RIGHT MOUSE  LOOK / ORBIT   WHEEL  ZOOM", "M                 MAP", "P  TAB            PAUSE  SIM RATE",
        "H  F3             HIDE HUD  FPS", "T  SHIFT+T        TIME OF DAY", "F12               SCREENSHOT", "ESC               MENU",
    };
    int n = (int)(sizeof L / sizeof L[0]);
    float w = SZ(720), h = SZ(60) + n * SZ(26), x = (W - w) / 2, y = (H - h) / 2;
    panel(x, y, w, h, 0.85f);
    hud_title(x + SZ(24), y + SZ(42), SZ(22), "CONTROLS", RED, 1, 0);
    for (int i = 0; i < n; i++) hud_text(x + SZ(24), y + SZ(80) + i * SZ(26), SZ(13), L[i], WHITE, 0.95f, 0);
}

/* ------------------------------------------------------------------ menus */
typedef struct { float x, y, w, h; } Box;
static Box itemBox[24]; static int nItems;
static int mouseX, mouseY;
static bool hover(int i) { Box b = itemBox[i]; return mouseX >= b.x && mouseX <= b.x + b.w && mouseY >= b.y && mouseY <= b.y + b.h; }
static void menu_item(int i, float x, float y, const char *label, const char *value, bool sel) {
    float w = SZ(620), h = SZ(40);
    itemBox[i] = (Box){x - SZ(12), y - SZ(30), w, h};
    if (sel) { hud_rect(x - SZ(12), y - SZ(30), w, h, RED, 0.85f); hud_rect(x - SZ(12), y - SZ(30), SZ(4), h, WHITE, 1); }
    hud_text(x, y, SZ(18), label, WHITE, sel ? 1.0f : 0.85f, 0);
    if (value) {
        char b[96]; snprintf(b, sizeof b, sel ? "<  %s  >" : "%s", value);
        hud_text(x + SZ(590), y, SZ(13), b, WHITE, sel ? 1.0f : 0.75f, 2);
    }
}
static void fmt_time(char *b, size_t n, float h) { h = fmodf(h + 24, 24); snprintf(b, n, "%02d:%02d", (int)h, (int)(fmodf(h, 1) * 60 + 0.5f) % 60); }
enum { MI_FREE, MI_CHAL, MI_AIRCRAFT, MI_AIRPORT, MI_RUNWAY, MI_START, MI_TIME, MI_WEATHER, MI_WIND, MI_SETTINGS, MI_CREDITS, MI_QUIT, MI_N };
static void draw_menu(void) {
    char b[160];
    hud_rect(0, 0, SZ(700), H, 0.01f, 0.01f, 0.012f, 0.72f);
    hud_rect(SZ(700), 0, SZ(3), H, RED, 0.9f);
    hud_title(SZ(60), SZ(150), SZ(64), "WINGMERE", RED, 1, 0);
    hud_text(SZ(64), SZ(196), SZ(14), "THE WHOLE REAL EARTH. FREE AND OPEN SOURCE.", WHITE, 0.8f, 0);
    float x = SZ(64), y = SZ(270), dy = SZ(46);
    nItems = MI_N;
    for (int i = 0; i < MI_N; i++) {
        const char *label = "", *val = NULL; char vb[96] = "";
        switch (i) {
        case MI_FREE: label = "FREE FLIGHT"; break;
        case MI_CHAL: label = "LANDING CHALLENGES"; break;
        case MI_AIRCRAFT: label = "AIRCRAFT"; val = g_naircraft ? g_aircraft[acSel].name : "NONE"; break;
        case MI_AIRPORT: label = "AIRPORT";
            if (aptSel >= 0) snprintf(vb, sizeof vb, "%s  %.21s", g_airports[aptSel].ident, g_airports[aptSel].name); else snprintf(vb, sizeof vb, "-");
            val = vb; break;
        case MI_RUNWAY: label = "RUNWAY";
            if (cur_runway() >= 0) { Runway *r = &g_runways[cur_runway()]; snprintf(vb, sizeof vb, "%s  %.0f M", rw_ident(cur_runway(), rwEnd), r->length); } else snprintf(vb, sizeof vb, "-");
            val = vb; break;
        case MI_START: label = "START"; val = START_NAME[startMode]; break;
        case MI_TIME: label = "TIME OF DAY"; fmt_time(vb, sizeof vb, tod); val = vb; break;
        case MI_WEATHER: label = "WEATHER"; val = WEATHER_NAME[weatherSel]; break;
        case MI_WIND: label = "WIND"; if (windKt < 0.5f) snprintf(vb, sizeof vb, "CALM"); else snprintf(vb, sizeof vb, "%03.0f AT %.0f KT", windDir, windKt); val = vb; break;
        case MI_SETTINGS: label = "SETTINGS"; break;
        case MI_CREDITS: label = "CREDITS"; break;
        case MI_QUIT: label = "QUIT"; break;
        }
        menu_item(i, x, y + i * dy, label, val, menuSel == i);
    }
    if (g_naircraft) {
        const AircraftDef *a = &g_aircraft[acSel];
        snprintf(b, sizeof b, "%s", a->kind);
        hud_text(x, y + MI_N * dy + SZ(10), SZ(12), b, GREY, 0.95f, 0);
        snprintf(b, sizeof b, "%.0f KG   SPAN %.1f M   VNE %.0f KT   STALL %.0f KT", a->mass, a->span, a->vne / KT, flight_stall_speed(a, a->flaps ? 1 : 0) / KT);
        hud_text(x, y + MI_N * dy + SZ(32), SZ(12), b, GREY, 0.95f, 0);
    }
    hud_text(x, H - SZ(40), SZ(12), "UP DOWN SELECT   LEFT RIGHT CHANGE   ENTER CHOOSE   TYPE ON AIRPORT TO SEARCH", GREY, 0.9f, 0);
    if (aptSel >= 0) {
        snprintf(b, sizeof b, "%s, %s", g_airports[aptSel].city, g_airports[aptSel].country);
        hud_text(W - SZ(40), SZ(60), SZ(16), b, WHITE, 0.9f, 2);
        snprintf(b, sizeof b, "%.0f FT", g_airports[aptSel].elev / FT);
        hud_text(W - SZ(40), SZ(84), SZ(12), b, GREY, 0.9f, 2);
    }
    snprintf(b, sizeof b, "%d AIRPORTS  %d RUNWAYS  V" WM_VERSION, g_nairports, g_nrunways);
    hud_text(W - SZ(40), H - SZ(40), SZ(11), b, GREY, 0.8f, 2);
}
static void do_search(void) {
    nSearch = 0;
    if (!search[0]) return;
    size_t n = strlen(search);
    for (int pass = 0; pass < 3 && nSearch < 12; pass++)
        for (int i = 0; i < g_nairports && nSearch < 12; i++) {
            Airport *a = &g_airports[i];
            bool hit = pass == 0 ? !strcasecmp(a->ident, search) : pass == 1 ? !strncasecmp(a->ident, search, n) : (n >= 3 && (strcasestr(a->name, search) || strcasestr(a->city, search)));
            if (!hit) continue;
            bool dup = false;
            for (int k = 0; k < nSearch; k++) if (searchRes[k] == i) dup = true;
            if (!dup) searchRes[nSearch++] = i;
        }
    if (searchSel >= nSearch) searchSel = nSearch ? nSearch - 1 : 0;
}
static void draw_search(void) {
    draw_menu();
    float w = SZ(760), h = SZ(120) + 12 * SZ(34), x = (W - w) / 2, y = SZ(120);
    panel(x, y, w, h, 0.92f);
    hud_title(x + SZ(24), y + SZ(48), SZ(22), "FIND AN AIRPORT", RED, 1, 0);
    char b[160];
    snprintf(b, sizeof b, "%s%s", search, fmodf(realTime, 1) < 0.5f ? "_" : "");
    hud_rect(x + SZ(24), y + SZ(66), w - SZ(48), SZ(36), 0, 0, 0, 0.7f);
    hud_text(x + SZ(36), y + SZ(92), SZ(18), b, WHITE, 1, 0);
    nItems = nSearch;
    for (int i = 0; i < nSearch; i++) {
        Airport *a = &g_airports[searchRes[i]];
        float iy = y + SZ(146) + i * SZ(34);
        itemBox[i] = (Box){x + SZ(18), iy - SZ(24), w - SZ(36), SZ(32)};
        if (i == searchSel) hud_rect(x + SZ(18), iy - SZ(24), w - SZ(36), SZ(32), RED, 0.85f);
        snprintf(b, sizeof b, "%-5s %.40s", a->ident, a->name);
        hud_text(x + SZ(30), iy, SZ(14), b, WHITE, 1, 0);
        snprintf(b, sizeof b, "%.20s %s", a->city, a->country);
        hud_text(x + w - SZ(30), iy, SZ(12), b, GREY, 1, 2);
    }
    if (!nSearch) hud_text(x + SZ(30), y + SZ(146), SZ(13), search[0] ? "NO MATCHES" : "TYPE AN ICAO CODE, CITY OR NAME", GREY, 1, 0);
}
static void draw_challenges(void) {
    draw_menu();
    float w = SZ(900), h = SZ(110) + NCH * SZ(46), x = (W - w) / 2, y = (H - h) / 2;
    panel(x, y, w, h, 0.92f);
    hud_title(x + SZ(24), y + SZ(50), SZ(24), "LANDING CHALLENGES", RED, 1, 0);
    hud_text(x + w - SZ(24), y + SZ(46), SZ(12), "SCORE OUT OF 1000: SINK RATE, CENTRELINE, TOUCHDOWN ZONE, ALIGNMENT", GREY, 1, 2);
    nItems = NCH;
    char b[128];
    for (int i = 0; i < NCH; i++) {
        float iy = y + SZ(100) + i * SZ(46);
        itemBox[i] = (Box){x + SZ(16), iy - SZ(28), w - SZ(32), SZ(42)};
        if (i == chalSel) hud_rect(x + SZ(16), iy - SZ(28), w - SZ(32), SZ(42), RED, 0.85f);
        snprintf(b, sizeof b, "%s", CH[i].title);
        hud_text(x + SZ(30), iy - SZ(4), SZ(15), b, WHITE, 1, 0);
        snprintf(b, sizeof b, "%s %s - %s", CH[i].ident, CH[i].rw, CH[i].blurb);
        hud_text(x + SZ(30), iy + SZ(11), SZ(10), b, i == chalSel ? 0.95f : 0.6f, i == chalSel ? 0.9f : 0.6f, i == chalSel ? 0.88f : 0.62f, 1, 0);
        if (bestScore[i] > 0) { snprintf(b, sizeof b, "BEST %d", bestScore[i]); hud_text(x + w - SZ(30), iy + SZ(2), SZ(14), b, WHITE, 1, 2); }
    }
}
enum { SI_DETAIL, SI_SCALE, SI_MSAA, SI_CLOUDS, SI_SHADOW, SI_VOLUME, SI_INVERT, SI_FULL, SI_OFFLINE, SI_BACK, SI_N };
static void draw_settings(void) {
    draw_menu();
    float w = SZ(700), h = SZ(110) + SI_N * SZ(46), x = (W - w) / 2, y = (H - h) / 2;
    panel(x, y, w, h, 0.92f);
    hud_title(x + SZ(24), y + SZ(50), SZ(24), "SETTINGS", RED, 1, 0);
    nItems = SI_N;
    for (int i = 0; i < SI_N; i++) {
        char v[64] = ""; const char *l = "";
        switch (i) {
        case SI_DETAIL: l = "TERRAIN DETAIL"; snprintf(v, sizeof v, "%.2f", S.detail); break;
        case SI_SCALE: l = "RENDER SCALE"; snprintf(v, sizeof v, "%.0f%%", rc.scale * 100); break;
        case SI_MSAA: l = "ANTI-ALIASING"; snprintf(v, sizeof v, rc.msaa > 1 ? "%dX MSAA" : "OFF", rc.msaa); break;
        case SI_CLOUDS: l = "VOLUMETRIC CLOUDS"; snprintf(v, sizeof v, rc.clouds ? "ON" : "OFF"); break;
        case SI_SHADOW: l = "SHADOW RESOLUTION"; snprintf(v, sizeof v, "%d", rc.shadowRes); break;
        case SI_VOLUME: l = "VOLUME"; snprintf(v, sizeof v, "%.0f%%", volume * 100); break;
        case SI_INVERT: l = "INVERT PITCH"; snprintf(v, sizeof v, invertPitch ? "ON" : "OFF"); break;
        case SI_FULL: l = "FULLSCREEN"; snprintf(v, sizeof v, fullscreen ? "ON" : "OFF"); break;
        case SI_OFFLINE: l = "OFFLINE (PROCEDURAL WORLD)"; snprintf(v, sizeof v, S.offline ? "ON" : "OFF"); break;
        case SI_BACK: l = "BACK"; break;
        }
        float iy = y + SZ(110) + i * SZ(46);
        itemBox[i] = (Box){x + SZ(16), iy - SZ(30), w - SZ(32), SZ(40)};
        if (i == setSel) hud_rect(x + SZ(16), iy - SZ(30), w - SZ(32), SZ(40), RED, 0.85f);
        hud_text(x + SZ(30), iy, SZ(16), l, WHITE, 1, 0);
        if (v[0]) hud_text(x + w - SZ(30), iy, SZ(15), v, WHITE, 1, 2);
    }
    hud_text(x + SZ(30), y + h - SZ(16), SZ(10), "SCALE, MSAA AND SHADOWS APPLY ON RESTART. OFFLINE TAKES EFFECT ON RESTART.", GREY, 1, 0);
}
static char creditsText[6000];
static void load_credits(void) {
    char p[700]; snprintf(p, sizeof p, "%s/CREDITS.txt", dataDir);
    FILE *f = fopen(p, "r");
    if (!f) { snprintf(creditsText, sizeof creditsText, "CREDITS FILE MISSING"); return; }
    size_t n = fread(creditsText, 1, sizeof creditsText - 1, f); creditsText[n] = 0; fclose(f);
}
static void draw_credits(void) {
    hud_rect(0, 0, W, H, 0, 0, 0, 0.8f);
    hud_title(SZ(80), SZ(120), SZ(40), "CREDITS", RED, 1, 0);
    float y = SZ(180);
    char line[200];
    const char *s = creditsText;
    while (*s && y < H - SZ(60)) {
        size_t n = strcspn(s, "\n");
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = 0;
        s += n; if (*s) s++;
        if (line[0] == '#') continue;
        bool head = line[0] == '=';
        hud_text(SZ(80), y, head ? SZ(16) : SZ(12), head ? line + 1 : line, head ? 0.86f : 0.94f, head ? 0.09f : 0.93f, head ? 0.08f : 0.91f, 1, 0);
        y += head ? SZ(30) : SZ(20);
    }
    hud_text(SZ(80), H - SZ(30), SZ(12), "ESC TO GO BACK", GREY, 1, 0);
}
static const char *PAUSE_ITEMS[4] = {"RESUME", "RESTART", "MAIN MENU", "QUIT"};
static void draw_pause(void) {
    float w = SZ(460), h = SZ(260), x = (W - w) / 2, y = (H - h) / 2;
    panel(x, y, w, h, 0.9f);
    hud_title(x + SZ(24), y + SZ(48), SZ(26), "PAUSED", RED, 1, 0);
    nItems = 4;
    for (int i = 0; i < 4; i++) {
        float iy = y + SZ(100) + i * SZ(42);
        itemBox[i] = (Box){x + SZ(16), iy - SZ(28), w - SZ(32), SZ(38)};
        if (i == pauseSel) hud_rect(x + SZ(16), iy - SZ(28), w - SZ(32), SZ(38), RED, 0.85f);
        hud_text(x + SZ(30), iy, SZ(17), PAUSE_ITEMS[i], WHITE, 1, 0);
    }
}
static void draw_result(void) {
    char b[128];
    if (FL.crashed && chalActive < 0) {
        float w = SZ(640), h = SZ(170), x = (W - w) / 2, y = H * 0.3f;
        panel(x, y, w, h, 0.88f);
        hud_title(W / 2, y + SZ(64), SZ(34), "CRASHED", RED, 1, 1);
        hud_text(W / 2, y + SZ(106), SZ(16), FL.crashWhy, WHITE, 1, 1);
        hud_text(W / 2, y + SZ(146), SZ(13), "R  RESTART      ESC  MENU", GREY, 1, 1);
        return;
    }
    if (!scored) return;
    float w = SZ(600), h = SZ(330), x = (W - w) / 2, y = H * 0.22f;
    panel(x, y, w, h, 0.9f);
    hud_text(W / 2, y + SZ(40), SZ(14), CH[chalActive].title, WHITE, 0.9f, 1);
    snprintf(b, sizeof b, "%d", score);
    hud_title(W / 2, y + SZ(110), SZ(56), b, RED, 1, 1);
    hud_text(W / 2, y + SZ(146), SZ(15), scoreWhy, WHITE, 1, 1);
    if (td.have && !FL.crashed) {
        snprintf(b, sizeof b, "SINK RATE   %+.0f FPM   %.0f / 400", td.vs, sVs); hud_text(x + SZ(40), y + SZ(186), SZ(13), b, WHITE, 0.95f, 0);
        snprintf(b, sizeof b, "CENTRELINE  %.1f M   %.0f / 250", fabsf(td.across), sCl); hud_text(x + SZ(40), y + SZ(210), SZ(13), b, WHITE, 0.95f, 0);
        snprintf(b, sizeof b, "TOUCHDOWN   %.0f M IN   %.0f / 250", td.along, sTz); hud_text(x + SZ(40), y + SZ(234), SZ(13), b, WHITE, 0.95f, 0);
        snprintf(b, sizeof b, "ALIGNMENT   %.1f DEG   %.0f / 100", fabsf(td.crab), sAl); hud_text(x + SZ(40), y + SZ(258), SZ(13), b, WHITE, 0.95f, 0);
    }
    snprintf(b, sizeof b, "BEST %d      R  RETRY      ESC  MENU", bestScore[chalActive]);
    hud_text(W / 2, y + h - SZ(22), SZ(13), b, GREY, 1, 1);
}
static void draw_loading(void) {
    int q, b, ok, fail; tiles_stats(&q, &b, &ok, &fail);
    float p = clampf(1 - (float)(q + b) / (float)loadQueue0, 0, 1);
    p = fmaxf(p, fminf(loadTimer / 10, 1));
    hud_rect(0, 0, W, H, 0, 0, 0, 0.35f * (1 - p) + 0.1f);
    hud_title(W / 2, H * 0.45f, SZ(36), "WINGMERE", RED, 1, 1);
    char t[96];
    if (aptSel >= 0) { snprintf(t, sizeof t, "%s  %s", g_airports[aptSel].ident, g_airports[aptSel].name); hud_text(W / 2, H * 0.45f + SZ(46), SZ(16), t, WHITE, 1, 1); }
    snprintf(t, sizeof t, fail < 0 ? "BUILDING PROCEDURAL TERRAIN %d%%" : "LOADING SCENERY %d%%", (int)(p * 100));
    hud_text(W / 2, H * 0.45f + SZ(80), SZ(13), t, GREY, 1, 1);
    hud_rect(W / 2 - SZ(200), H * 0.45f + SZ(94), SZ(400), SZ(4), 0.2f, 0.2f, 0.2f, 0.8f);
    hud_rect(W / 2 - SZ(200), H * 0.45f + SZ(94), SZ(400) * p, SZ(4), RED, 1);
    hud_text(W / 2, H - SZ(60), SZ(12), "PRESS F1 IN FLIGHT FOR CONTROLS", GREY, 0.9f, 1);
}

/* ------------------------------------------------------------------ input */
static float axis(SDL_GameControllerAxis a) {
    if (!pad) return 0;
    float v = SDL_GameControllerGetAxis(pad, a) / 32767.0f;
    float dz = 0.1f;
    if (fabsf(v) < dz) return 0;
    v = (v - signf(v) * dz) / (1 - dz);
    return signf(v) * (0.4f * fabsf(v) + 0.6f * v * v);
}
static float approachf(float v, float target, float rate) { return v < target ? fminf(v + rate, target) : fmaxf(v - rate, target); }
static void flight_controls(float dt) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    SDL_Keymod mod = SDL_GetModState();
    float kp = (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) - (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]);
    float kr = (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]);
    float ky = k[SDL_SCANCODE_E] - k[SDL_SCANCODE_Q];
    if (invertPitch) kp = -kp;
    float auth = clampf(1.25f - FL.ias / (FL.def->vne * 1.2f), 0.35f, 1);
    ctlElev = approachf(ctlElev, kp * auth, (kp ? 2.2f : 3.5f) * dt);
    ctlAil = approachf(ctlAil, kr * auth, (kr ? 3.0f : 4.0f) * dt);
    ctlRud = approachf(ctlRud, ky, (ky ? 2.5f : 3.5f) * dt);
    float jp = -axis(SDL_CONTROLLER_AXIS_LEFTY), jr = axis(SDL_CONTROLLER_AXIS_LEFTX);
    if (invertPitch) jp = -jp;
    float jy = 0;
    if (pad) jy = (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) - SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) * 1.0f;
    bool hasThr = false;
    if (stick && !pad) {
        float ax0 = SDL_JoystickGetAxis(stick, 0) / 32767.0f, ax1 = SDL_JoystickGetAxis(stick, 1) / 32767.0f;
        jr = fabsf(ax0) > 0.05f ? ax0 : 0; jp = fabsf(ax1) > 0.05f ? ax1 : 0;   /* stick back = nose up */
        if (invertPitch) jp = -jp;
        if (SDL_JoystickNumAxes(stick) > 3) { float a3 = SDL_JoystickGetAxis(stick, 3) / 32767.0f; jy = fabsf(a3) > 0.08f ? a3 : 0; }
        hasThr = SDL_JoystickNumAxes(stick) > 2;
    }
    float elev = clampf(ctlElev + jp, -1, 1), ail = clampf(ctlAil + jr, -1, 1), rud = clampf(ctlRud + jy, -1, 1);
    /* throttle */
    float thrRate = 0.5f * dt;
    if (k[SDL_SCANCODE_R] || k[SDL_SCANCODE_PAGEUP] || k[SDL_SCANCODE_EQUALS] || k[SDL_SCANCODE_KP_PLUS]) FL.throttle += thrRate;
    if (k[SDL_SCANCODE_F] || k[SDL_SCANCODE_PAGEDOWN] || k[SDL_SCANCODE_MINUS] || k[SDL_SCANCODE_KP_MINUS]) FL.throttle -= thrRate;
    if (pad) FL.throttle += (axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) - axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)) * thrRate;
    static float lastStickThr = -1;
    if (hasThr) {
        float t = 1 - (SDL_JoystickGetAxis(stick, 2) / 32767.0f * 0.5f + 0.5f);
        if (lastStickThr < 0 || fabsf(t - lastStickThr) > 0.01f) { FL.throttle = t; lastStickThr = t; }
    }
    FL.throttle = clampf(FL.throttle, 0, 1);
    /* trim */
    if (k[SDL_SCANCODE_COMMA] || k[SDL_SCANCODE_HOME]) FL.trim = clampf(FL.trim - 0.25f * dt, -1, 1);
    if (k[SDL_SCANCODE_PERIOD] || k[SDL_SCANCODE_END]) FL.trim = clampf(FL.trim + 0.25f * dt, -1, 1);
    if (pad) { if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) FL.trim = clampf(FL.trim - 0.25f * dt, -1, 1);
               if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) FL.trim = clampf(FL.trim + 0.25f * dt, -1, 1); }
    /* brakes */
    bool br = k[SDL_SCANCODE_SPACE] || (pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) || (stick && !pad && SDL_JoystickGetButton(stick, 0));
    if (br && !(mod & KMOD_SHIFT)) parkBrake = false;
    if (parkBrake && FL.throttle > 0.45f) { parkBrake = false; say("PARKING BRAKE RELEASED"); }
    FL.brake = (br || parkBrake) ? 1.0f : 0.0f;
    if (ap) {
        /* with the autopilot on, the controls nudge its targets */
        apAlt += -kp * 150 * dt * (k[SDL_SCANCODE_LSHIFT] ? 4 : 1);
        apHdg = fmodf(apHdg + kr * 20 * dt + 360, 360);
        if (k[SDL_SCANCODE_R] || k[SDL_SCANCODE_PAGEUP]) apSpd += 10 * dt;
        if (k[SDL_SCANCODE_F] || k[SDL_SCANCODE_PAGEDOWN]) apSpd -= 10 * dt;
        apSpd = clampf(apSpd, flight_stall_speed(FL.def, FL.flaps) * 1.25f, FL.def->vne * 0.95f);
        if (fabsf(jp) > 0.5f || fabsf(jr) > 0.5f) { ap = false; audio_event(3); say("AUTOPILOT OFF"); }
        else { autopilot(dt); return; }
    }
    FL.elev = elev; FL.ail = ail; FL.rud = rud;
}

static void cycle(int *v, int n, int d) { *v = ((*v + d) % n + n) % n; }
static void menu_change(int item, int d) {
    switch (item) {
    case MI_AIRCRAFT: if (g_naircraft) { cycle(&acSel, g_naircraft, d); spawn(); } break;
    case MI_AIRPORT: break;
    case MI_RUNWAY:
        if (aptSel >= 0 && g_airports[aptSel].nrw) {
            int idx = rwSel * 2 + rwEnd, n = g_airports[aptSel].nrw * 2;
            cycle(&idx, n, d); rwSel = idx / 2; rwEnd = idx % 2; spawn();
        }
        break;
    case MI_START: cycle(&startMode, 3, d); break;
    case MI_TIME: tod = fmodf(tod + d * 0.5f + 24, 24); spawn(); break;
    case MI_WEATHER: cycle(&weatherSel, 5, d); spawn(); break;
    case MI_WIND: windKt = clampf(windKt + d * 2, 0, 40); if (d > 0 && windKt == 40) windKt = 0; break;
    }
    audio_event(0);
}
static void menu_activate(int item) {
    audio_event(0);
    switch (item) {
    case MI_FREE: chalActive = -1; start_flight(); break;
    case MI_CHAL: state = ST_CHAL; break;
    case MI_AIRPORT: state = ST_SEARCH; search[0] = 0; nSearch = 0; searchSel = 0; SDL_StartTextInput(); break;
    case MI_WIND: windDir = fmodf(windDir + 30, 360); break;
    case MI_SETTINGS: state = ST_SETTINGS; setSel = 0; break;
    case MI_CREDITS: state = ST_CREDITS; break;
    case MI_QUIT: { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); } break;
    default: menu_change(item, 1);
    }
}
static void settings_change(int d) {
    switch (setSel) {
    case SI_DETAIL: S.detail = clampf(S.detail + d * 0.25f, 0.5f, 2.5f); break;
    case SI_SCALE: rc.scale = clampf(rc.scale + d * 0.25f, 0.5f, 2.0f); break;
    case SI_MSAA: { static const int m[4] = {0, 2, 4, 8}; int i = 0; while (i < 3 && m[i] < rc.msaa) i++; cycle(&i, 4, d); rc.msaa = m[i]; break; }
    case SI_CLOUDS: rc.clouds = !rc.clouds; break;
    case SI_SHADOW: rc.shadowRes = rc.shadowRes == 4096 ? (d > 0 ? 1024 : 2048) : rc.shadowRes == 1024 ? (d > 0 ? 2048 : 4096) : (d > 0 ? 4096 : 1024); break;
    case SI_VOLUME: volume = clampf(volume + d * 0.1f, 0, 1); break;
    case SI_INVERT: invertPitch = !invertPitch; break;
    case SI_FULL: fullscreen = !fullscreen; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); break;
    case SI_OFFLINE: S.offline = !S.offline; break;
    case SI_BACK: if (d) return; state = ST_MENU; break;
    }
    settings_save();
    audio_event(0);
}
static void screenshot(void) {
    char dir[700], path[800];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof dir, "%s/Pictures", home ? home : ".");
    if (!file_exists(dir)) snprintf(dir, sizeof dir, ".");
    time_t now = time(NULL); struct tm *tmv = localtime(&now);
    snprintf(path, sizeof path, "%s/wingmere-%04d%02d%02d-%02d%02d%02d.png", dir, tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    if (render_screenshot(path)) { char m[900]; snprintf(m, sizeof m, "SAVED %s", path); say(m); }
}

static bool running = true;
static void handle_event(SDL_Event *e) {
    if (e->type == SDL_QUIT) { running = false; return; }
    if (e->type == SDL_WINDOWEVENT && (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
        int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh); render_resize(dw, dh);
        if (!fullscreen) { rc.w = e->window.data1; rc.h = e->window.data2; }
        return;
    }
    if (e->type == SDL_CONTROLLERDEVICEADDED && !pad) { pad = SDL_GameControllerOpen(e->cdevice.which); return; }
    if (e->type == SDL_CONTROLLERDEVICEREMOVED && pad) { SDL_GameControllerClose(pad); pad = NULL; return; }
    if (e->type == SDL_MOUSEMOTION) {
        int dw, dh, ww, wh; SDL_GL_GetDrawableSize(win, &dw, &dh); SDL_GetWindowSize(win, &ww, &wh);
        mouseX = e->motion.x * dw / (ww ? ww : 1); mouseY = e->motion.y * dh / (wh ? wh : 1);
        if (mouseLook) {
            if (camMode == CAM_COCKPIT) { lookYaw = clampf(lookYaw + e->motion.xrel * 0.004f, -2.6f, 2.6f); lookPitch = clampf(lookPitch + e->motion.yrel * 0.004f, -1.2f, 1.2f); }
            else { camYaw -= e->motion.xrel * 0.005f; camPitch = clampf(camPitch + e->motion.yrel * 0.004f, -0.25f, 1.45f); }
        } else if (state != ST_FLY) {
            for (int i = 0; i < nItems; i++) if (hover(i)) {
                if (state == ST_MENU) menuSel = i; else if (state == ST_SETTINGS) setSel = i; else if (state == ST_CHAL) chalSel = i;
                else if (state == ST_SEARCH) searchSel = i; else if (state == ST_PAUSE) pauseSel = i;
            }
        }
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN) {
        if (e->button.button == SDL_BUTTON_RIGHT || e->button.button == SDL_BUTTON_MIDDLE) { mouseLook = true; SDL_SetRelativeMouseMode(SDL_TRUE); return; }
        if (e->button.button == SDL_BUTTON_LEFT) {
            for (int i = 0; i < nItems; i++) if (hover(i)) {
                if (state == ST_MENU) { menuSel = i; menu_activate(i); }
                else if (state == ST_SETTINGS) { setSel = i; settings_change(i == SI_BACK ? 0 : 1); }
                else if (state == ST_CHAL) { chalSel = i; start_challenge(i); }
                else if (state == ST_SEARCH) { select_airport(searchRes[i], NULL); spawn(); SDL_StopTextInput(); state = ST_MENU; }
                else if (state == ST_PAUSE) { pauseSel = i; SDL_Event k = {0}; k.type = SDL_KEYDOWN; k.key.keysym.sym = SDLK_RETURN; handle_event(&k); }
                return;
            }
        }
        return;
    }
    if (e->type == SDL_MOUSEBUTTONUP && (e->button.button == SDL_BUTTON_RIGHT || e->button.button == SDL_BUTTON_MIDDLE)) { mouseLook = false; SDL_SetRelativeMouseMode(SDL_FALSE); return; }
    if (e->type == SDL_MOUSEWHEEL) {
        if (showMap) mapZoom = (int)clampf((float)(mapZoom + e->wheel.y), 5, (float)S.imagery_max_zoom);
        else camZoom = clampf(camZoom * (e->wheel.y > 0 ? 0.88f : 1.13f), 0.3f, 4.0f);
        return;
    }
    if (e->type == SDL_TEXTINPUT && state == ST_SEARCH) {
        size_t n = strlen(search);
        for (const char *c = e->text.text; *c && n < sizeof search - 1; c++) if ((unsigned char)*c >= 32 && (unsigned char)*c < 127) search[n++] = *c;
        search[n] = 0; do_search();
        return;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN && state == ST_FLY) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: if (FL.def->retract) { FL.gearCmd = FL.gearCmd > 0.5f ? 0 : 1; } break;
        case SDL_CONTROLLER_BUTTON_X: if (FL.flapsSet < FL.def->flaps) FL.flapsSet++; break;
        case SDL_CONTROLLER_BUTTON_Y: if (FL.flapsSet > 0) FL.flapsSet--; break;
        case SDL_CONTROLLER_BUTTON_BACK: cycle(&camMode, NCAM, 1); break;
        case SDL_CONTROLLER_BUTTON_START: state = ST_PAUSE; pauseSel = 0; break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: ap_toggle(); break;
        }
        return;
    }
    if (e->type != SDL_KEYDOWN) return;
    SDL_Keycode key = e->key.keysym.sym;
    bool shift = e->key.keysym.mod & KMOD_SHIFT;
    if (key == SDLK_F12) { screenshot(); return; }
    if (key == SDLK_F5 && !e->key.repeat) { render_reload_shaders(); say("SHADERS RELOADED"); return; }
    switch (state) {
    case ST_MENU:
        if (key == SDLK_UP) { cycle(&menuSel, MI_N, -1); audio_event(0); }
        else if (key == SDLK_DOWN) { cycle(&menuSel, MI_N, 1); audio_event(0); }
        else if (key == SDLK_LEFT) menu_change(menuSel, -1);
        else if (key == SDLK_RIGHT) menu_change(menuSel, 1);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) menu_activate(menuSel);
        else if (key == SDLK_ESCAPE) running = false;
        else if (menuSel == MI_AIRPORT && ((key >= SDLK_a && key <= SDLK_z) || (key >= SDLK_0 && key <= SDLK_9))) {
            state = ST_SEARCH; search[0] = 0; nSearch = 0; searchSel = 0; SDL_StartTextInput();
        }
        break;
    case ST_SEARCH:
        if (key == SDLK_ESCAPE) { state = ST_MENU; SDL_StopTextInput(); }
        else if (key == SDLK_BACKSPACE) { size_t n = strlen(search); if (n) search[n - 1] = 0; do_search(); }
        else if (key == SDLK_UP && nSearch) searchSel = (searchSel + nSearch - 1) % nSearch;
        else if (key == SDLK_DOWN && nSearch) searchSel = (searchSel + 1) % nSearch;
        else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && nSearch) { select_airport(searchRes[searchSel], NULL); spawn(); SDL_StopTextInput(); state = ST_MENU; audio_event(0); }
        break;
    case ST_CHAL:
        if (key == SDLK_ESCAPE) state = ST_MENU;
        else if (key == SDLK_UP) cycle(&chalSel, NCH, -1);
        else if (key == SDLK_DOWN) cycle(&chalSel, NCH, 1);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) start_challenge(chalSel);
        break;
    case ST_SETTINGS:
        if (key == SDLK_ESCAPE) state = ST_MENU;
        else if (key == SDLK_UP) cycle(&setSel, SI_N, -1);
        else if (key == SDLK_DOWN) cycle(&setSel, SI_N, 1);
        else if (key == SDLK_LEFT) settings_change(-1);
        else if (key == SDLK_RIGHT) settings_change(1);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) settings_change(setSel == SI_BACK ? 0 : 1);
        break;
    case ST_CREDITS:
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) state = ST_MENU;
        break;
    case ST_PAUSE:
        if (key == SDLK_ESCAPE) state = ST_FLY;
        else if (key == SDLK_UP) cycle(&pauseSel, 4, -1);
        else if (key == SDLK_DOWN) cycle(&pauseSel, 4, 1);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (pauseSel == 0) state = ST_FLY;
            else if (pauseSel == 1) { if (chalActive >= 0) start_challenge(chalActive); else start_flight(); }
            else if (pauseSel == 2) { state = ST_MENU; chalActive = -1; restore_free(); camMode = CAM_ORBIT; spawn(); }
            else running = false;
        }
        break;
    case ST_LOADING:
        if (key == SDLK_ESCAPE) { state = ST_MENU; chalActive = -1; restore_free(); camMode = CAM_ORBIT; spawn(); }
        else if (key == SDLK_RETURN || key == SDLK_SPACE) state = ST_FLY;
        break;
    case ST_FLY:
        if (key == SDLK_ESCAPE) { if (showMap) showMap = false; else if (showHelp) showHelp = false; else { state = ST_PAUSE; pauseSel = 0; } }
        else if (key == SDLK_g && FL.def->retract) { if (FL.onGround) say("GEAR HANDLE LOCKED ON THE GROUND"); else { FL.gearCmd = FL.gearCmd > 0.5f ? 0 : 1; audio_event(0); } }
        else if (key == SDLK_v && FL.flapsSet < FL.def->flaps) { FL.flapsSet++; audio_event(0); }
        else if (key == SDLK_b && FL.flapsSet > 0) { FL.flapsSet--; audio_event(0); }
        else if (key == SDLK_SPACE && shift) { parkBrake = !parkBrake; say(parkBrake ? "PARKING BRAKE SET" : "PARKING BRAKE RELEASED"); }
        else if (key == SDLK_z) ap_toggle();
        else if (key == SDLK_c) { cycle(&camMode, NCAM, shift ? -1 : 1); flybyOk = false; towerTimer = 0; lookYaw = lookPitch = 0; }
        else if (key >= SDLK_F2 && key <= SDLK_F7) { camMode = key - SDLK_F2; flybyOk = false; towerTimer = 0; lookYaw = lookPitch = 0; }
        else if (key == SDLK_KP_5) { camYaw = 0; camPitch = 0.16f; camZoom = 1; lookYaw = lookPitch = 0; }
        else if (key == SDLK_m) showMap = !showMap;
        else if (key == SDLK_h) showHud = !showHud;
        else if (key == SDLK_F3) showFps = !showFps;
        else if (key == SDLK_F1) showHelp = !showHelp;
        else if (key == SDLK_p) paused = !paused;
        else if (key == SDLK_TAB) { simRate = simRate >= 4 ? 1 : simRate * 2; }
        else if (key == SDLK_t) { utcHours += shift ? -0.25 : 0.25; }
        else if (key == SDLK_BACKQUOTE) FL.throttle = 0;
        else if (key >= SDLK_1 && key <= SDLK_9 && !showMap) FL.throttle = (key - SDLK_0) * 0.1f;
        else if (key == SDLK_0) FL.throttle = 1;
        else if ((key == SDLK_EQUALS || key == SDLK_KP_PLUS) && showMap) mapZoom = mapZoom < S.imagery_max_zoom ? mapZoom + 1 : mapZoom;
        else if ((key == SDLK_MINUS || key == SDLK_KP_MINUS) && showMap) mapZoom = mapZoom > 5 ? mapZoom - 1 : mapZoom;
        else if (key == SDLK_r && (FL.crashed || scored)) { if (chalActive >= 0) start_challenge(chalActive); else start_flight(); }
        break;
    }
}

/* ------------------------------------------------------------------ headless flight test */
static void run_flight_test(void) {
    int ri = -1, a = airport_find("KSFO");
    if (a >= 0) for (int i = 0; i < g_airports[a].nrw; i++) if (!strcmp(g_runways[g_airports[a].rw0 + i].he, "28R")) ri = g_airports[a].rw0 + i;
    if (ri < 0) { fprintf(stderr, "test: KSFO 28R missing\n"); return; }
    printf("%-18s %8s %8s %8s %9s %9s %9s %8s\n", "aircraft", "roll m", "lof kt", "vr kt", "1000ft s", "vmax kt", "stall kt", "calc kt");
    for (int ac = 0; ac < g_naircraft; ac++) {
        const AircraftDef *d = &g_aircraft[ac];
        double lat, lon; float brg, el;
        runway_end(ri, 1, &lat, &lon, &brg, &el);
        double la, lo; geo_offset(lat, lon, brg, 30 + d->length * 0.6, &la, &lo);
        flight_reset(&FL, d, la, lo, 0, brg, 0, true);
        FL.brake = 0; FL.throttle = 1;
        if (d->flaps) { FL.flapsSet = (d->flaps + 1) / 2; FL.flaps = (float)FL.flapsSet / d->flaps; }
        double sLat = FL.lat, sLon = FL.lon;
        float dt = 1.0f / 120, t = 0, roll = -1, lof = 0, t1000 = -1, vmax = 0, vstall = -1;
        int phase = 0; float phaseT = 0;
        prevPitch = FL.pitch; prevBank = FL.bank;
        while (t < 900 && !FL.crashed) {
            float pr = (FL.pitch - prevPitch) / dt, brr = (FL.bank - prevBank) / dt;
            prevPitch = FL.pitch; prevBank = FL.bank;
            float pitchT = 0;
            FL.ail = clampf(-FL.bank * 0.05f - brr * 0.02f, -1, 1);
            FL.rud = clampf(wrap180(brg - FL.heading) * 0.15f, -1, 1) * (FL.onGround ? 1 : 0);
            if (phase == 0) {                                     /* take-off roll and rotation */
                pitchT = FL.ias > d->v_rotate ? (d->tricycle ? 9.0f : 6.0f) : (d->tricycle ? 0.0f : FL.pitch);
                if (!FL.onGround && roll < 0 && FL.agl > 1.0f) { roll = (float)geo_dist(sLat, sLon, FL.lat, FL.lon); lof = FL.ias / KT; }
                if (roll >= 0) phase = 1;
                if (FL.onGround && !d->tricycle && FL.ias < d->v_rotate) { FL.elev = FL.ias > d->v_rotate * 0.6f ? -0.3f : 0.3f; goto step; }
            }
            if (phase == 1) {                                     /* climb */
                pitchT = 8;
                if (FL.agl > 60 && d->retract) FL.gearCmd = 0;
                if (FL.agl > 150) FL.flapsSet = 0;
                if (t1000 < 0 && FL.alt - el > 1000 * FT) t1000 = t;
                if (FL.alt - el > 900) { phase = 2; phaseT = 0; apAlt = (float)FL.alt; }
            }
            if (phase >= 2) {                                     /* level: full power for top speed, then idle to the stall */
                pitchT = clampf((apAlt - (float)FL.alt) * 0.035f - FL.vs * 0.25f, -8, 25);
                phaseT += dt;
                if (phase == 2) { FL.throttle = 1; vmax = fmaxf(vmax, FL.ias / KT); if (phaseT > 150) { phase = 3; phaseT = 0; } }
                else { FL.throttle = 0; if (FL.stall || FL.vs < -8) { vstall = FL.ias / KT; break; } }
            }
            FL.elev = clampf((pitchT - FL.pitch) * 0.07f - pr * 0.03f, -1, 1);
        step:
            flight_step(&FL, dt);
            t += dt;
        }
        printf("%-18s %8.0f %8.0f %8.0f %9.0f %9.0f %9.0f %8.0f%s%s\n", d->name, roll, lof, d->v_rotate / KT, t1000, vmax, vstall,
               flight_stall_speed(d, 0) / KT, FL.crashed ? "  CRASH: " : "", FL.crashed ? FL.crashWhy : "");
    }
}

/* ------------------------------------------------------------------ main */
static void usage(void) {
    printf("WINGMERE " WM_VERSION " - open-source flight simulator over the real Earth\n\n"
           "  --airport ICAO      start airport (default KSFO)\n"
           "  --runway ID         runway end, e.g. 28R\n"
           "  --aircraft ID       biplane, corsair, transport, b737\n"
           "  --start MODE        runway, approach or air\n"
           "  --fly               skip the menu and start flying\n"
           "  --challenge N       start landing challenge N (1-%d)\n"
           "  --time HH.H         local solar time\n"
           "  --weather N         0 clear .. 4 overcast\n"
           "  --wind DIR/KT       e.g. 270/12\n"
           "  --cam N             0 chase 1 pilot 2 tail 3 orbit 4 tower 5 flyby\n"
           "  --view Y,P,Z        camera yaw and pitch in degrees, zoom factor\n"
           "  --offline           procedural world, no downloads\n"
           "  --windowed / --fullscreen, --size WxH, --detail F\n"
           "  --shot FILE         render, wait for scenery, save a screenshot and quit\n"
           "  --frames N          most frames to wait for --shot (default 900)\n"
           "  --test-flight       headless performance test of every aircraft\n", NCH);
}
int main(int argc, char **argv) {
    settings_defaults();
    settings_load();
    const char *shot = NULL, *argAirport = NULL, *argRunway = NULL, *argAircraft = NULL;
    int maxFrames = 900, argChallenge = -1, argCam = -1;
    bool fly = false, testFlight = false, forceWindowed = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) { printf("wingmere " WM_VERSION "\n"); return 0; }
        else if (!strcmp(a, "--airport") && n) { argAirport = n; i++; }
        else if (!strcmp(a, "--runway") && n) { argRunway = n; i++; }
        else if (!strcmp(a, "--aircraft") && n) { argAircraft = n; i++; }
        else if (!strcmp(a, "--start") && n) { startMode = !strcmp(n, "approach") ? 1 : !strcmp(n, "air") ? 2 : 0; i++; }
        else if (!strcmp(a, "--fly")) fly = true;
        else if (!strcmp(a, "--challenge") && n) { argChallenge = atoi(n) - 1; i++; }
        else if (!strcmp(a, "--time") && n) { tod = (float)atof(n); i++; }
        else if (!strcmp(a, "--weather") && n) { weatherSel = atoi(n) % 5; i++; }
        else if (!strcmp(a, "--wind") && n) { sscanf(n, "%f/%f", &windDir, &windKt); i++; }
        else if (!strcmp(a, "--cam") && n) { argCam = atoi(n) % NCAM; i++; }
        else if (!strcmp(a, "--view") && n) { sscanf(n, "%f,%f,%f", &camYaw, &camPitch, &camZoom); camYaw *= DEG; camPitch *= DEG; i++; }
        else if (!strcmp(a, "--offline")) S.offline = true;
        else if (!strcmp(a, "--windowed")) { fullscreen = false; forceWindowed = true; }
        else if (!strcmp(a, "--fullscreen")) fullscreen = true;
        else if (!strcmp(a, "--size") && n) { sscanf(n, "%dx%d", &rc.w, &rc.h); i++; }
        else if (!strcmp(a, "--detail") && n) { S.detail = (float)atof(n); i++; }
        else if (!strcmp(a, "--shot") && n) { shot = n; i++; }
        else if (!strcmp(a, "--frames") && n) { maxFrames = atoi(n); i++; }
        else if (!strcmp(a, "--test-flight")) testFlight = true;
        else { fprintf(stderr, "wingmere: unknown option %s (try --help)\n", a); return 1; }
    }
    if (shot || testFlight) { fullscreen = false; forceWindowed = true; }
    (void)forceWindowed;
    find_data();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr, "wingmere: SDL: %s\n", SDL_GetError()); return 1; }
    if (!testFlight && !shot) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (testFlight) flags |= SDL_WINDOW_HIDDEN;
    win = SDL_CreateWindow("WINGMERE", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, rc.w, rc.h, flags);
    if (!win) { fprintf(stderr, "wingmere: window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "wingmere: OpenGL 3.3 core is required: %s\n", SDL_GetError()); return 1; }
    if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(shot ? 0 : 1);
    if (shot) SDL_GL_SetSwapInterval(0);
    fprintf(stderr, "wingmere: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    char p[760];
    snprintf(p, sizeof p, "%s/airports.txt", dataDir);
    airports_load(p);
    snprintf(p, sizeof p, "%s/aircraft", dataDir);
    aircraft_load_all(p);
    if (!g_naircraft) { fprintf(stderr, "wingmere: no aircraft found in %s\n", p); return 1; }
    fprintf(stderr, "wingmere: %d airports, %d runways, %d aircraft\n", g_nairports, g_nrunways, g_naircraft);
    load_credits();
    scores_load();
    int ai = argAircraft ? aircraft_by_id(argAircraft) : aircraft_by_id(cfgAircraft);
    acSel = ai >= 0 ? ai : (aircraft_by_id("corsair") >= 0 ? aircraft_by_id("corsair") : 0);
    int api = airport_find(argAirport ? argAirport : cfgAirport);
    if (api < 0) api = airport_find("KSFO");
    select_airport(api, argRunway ? argRunway : (argAirport ? NULL : cfgRunway));

    terrain_init();
    if (testFlight) { run_flight_test(); return 0; }
    tiles_init();
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    rc.w = dw; rc.h = dh;
    RenderCfg r0 = rc;
    render_init(r0);
    SDL_GetWindowSize(win, &rc.w, &rc.h);
    audio_init();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { if (!pad) pad = SDL_GameControllerOpen(i); }
        else if (!stick) stick = SDL_JoystickOpen(i);
    }

    camMode = CAM_ORBIT;
    SDL_StopTextInput();
    spawn();
    if (argChallenge >= 0 && argChallenge < NCH) start_challenge(argChallenge);
    else if (fly) { chalActive = -1; start_flight(); }
    if (argCam >= 0) camMode = argCam; else if (state == ST_LOADING) camMode = CAM_CHASE;
    if (shot && state == ST_LOADING) { state = ST_FLY; paused = true; freezeShot = true; }

    Uint64 last = SDL_GetPerformanceCounter();
    int frame = 0, quietFrames = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        if (shot) dt = 1.0f / 30;
        dt = clampf(dt, 0.0001f, 0.05f);
        fps = fps * 0.95f + 0.05f / dt;
        realTime += dt;
        msgTimer -= dt; tdTimer -= dt;
        tiles_frame();
        dw = 0; SDL_GL_GetDrawableSize(win, &dw, &dh);
        W = (float)dw; H = (float)dh; s_ = H / 1080.0f;

        bool flying = state == ST_FLY || state == ST_PAUSE;
        if (state == ST_LOADING) {
            loadTimer += dt;
            int q, b, ok, fail; tiles_stats(&q, &b, &ok, &fail);
            if ((terrain_ready(FL.lat, FL.lon) && q + b < 8 && loadTimer > 1.5f) || loadTimer > 14) { state = ST_FLY; if (camMode == CAM_ORBIT) camMode = CAM_CHASE; }
        }
        if (state == ST_FLY && !paused && !showHelp) {
            flight_controls(dt);
            float sdt = dt * simRate;
            FL.wind = wind_at(&FL, simTime);
            flight_step(&FL, sdt);
            simTime += sdt;
            if (FL.touchdowns > lastTouchdowns) { lastTouchdowns = FL.touchdowns; on_touchdown(); }
            if (ap && (FL.onGround || FL.crashed)) { ap = false; audio_event(3); }
            if ((frame & 15) == 0) find_approach();
            evaluate_landing();
        } else if (!flying && state != ST_LOADING) {
            FL.throttle = 0; FL.brake = 1;
            flight_step(&FL, dt);       /* settle on the gear in the menu */
        }
        prevPitch = FL.pitch; prevBank = FL.bank;
        double hours = utcHours + simTime / 3600.0 + FL.lon / 15.0;
        render_set_time(FL.lat, FL.lon, (float)fmod(hours + 48, 24), doy);
        update_camera(dt);
        /* audio */
        g_audio.rpm = FL.rpm; g_audio.throttle = FL.throttle; g_audio.airspeed = vlen(vsub(FL.vel, FL.wind));
        g_audio.stall = FL.stall && state == ST_FLY; g_audio.onGround = FL.onGround; g_audio.groundSpeed = FL.gs;
        g_audio.gearMoving = FL.gearDown > 0.02f && FL.gearDown < 0.98f; g_audio.crash = FL.crashed;
        g_audio.engine = FL.def->jet ? 2 : (!strcmp(FL.def->sound, "turboprop") ? 1 : 0);
        g_audio.master = (state == ST_FLY && !paused) ? volume : volume * 0.35f;

        render_frame(&cam, &FL, (state == ST_FLY && paused) ? 0 : dt * (state == ST_FLY ? simRate : 1), camMode != CAM_COCKPIT);
        hud_begin();
        if (state == ST_FLY || state == ST_PAUSE) {
            if (showHud) draw_flight_hud();
            if (showMap) draw_map();
            draw_result();
            if (showHelp) draw_help();
            if (state == ST_PAUSE) draw_pause();
        } else if (state == ST_MENU) draw_menu();
        else if (state == ST_SEARCH) draw_search();
        else if (state == ST_CHAL) draw_challenges();
        else if (state == ST_SETTINGS) draw_settings();
        else if (state == ST_CREDITS) draw_credits();
        else if (state == ST_LOADING) draw_loading();
        if (msgTimer > 0) hud_text(W / 2, H * 0.17f, SZ(16), msg, WHITE, fminf(msgTimer, 1), 1);
        attribution(state == ST_FLY ? 0.45f : 0.7f);
        hud_end();

        if (shot) {
            int q, b, ok, fail; tiles_stats(&q, &b, &ok, &fail);
            quietFrames = (q + b == 0) ? quietFrames + 1 : 0;
            if ((frame > 120 && quietFrames > 45) || frame >= maxFrames) {
                render_screenshot(shot);
                fprintf(stderr, "wingmere: wrote %s after %d frames\n", shot, frame);
                running = false;
            }
        }
        SDL_GL_SwapWindow(win);
        frame++;
    }
    settings_save();
    tiles_shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
