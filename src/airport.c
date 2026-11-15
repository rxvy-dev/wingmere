/* WINGMERE - real-world airports and runways (data from OurAirports, public domain).  MIT licence. */
#include "wm.h"

Airport *g_airports; int g_nairports;
Runway *g_runways; int g_nrunways;

/* runways are bucketed on a 1-degree grid for fast "what's near me" queries */
#define GW 360
#define GH 180
static int *cellStart, *cellList;

static int cell_of(double lat, double lon) {
    int cx = (int)floor(lon + 180) % GW, cy = (int)floor(lat + 90);
    if (cx < 0) cx += GW;
    cy = cy < 0 ? 0 : (cy >= GH ? GH - 1 : cy);
    return cy * GW + cx;
}

static char *field(char **s) {
    char *f = *s, *bar = strchr(f, '|');
    if (bar) { *bar = 0; *s = bar + 1; } else { char *nl = strchr(f, '\n'); if (nl) *nl = 0; *s = f + strlen(f); }
    return f;
}

bool airports_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "wingmere: cannot open %s\n", path); return false; }
    int capA = 12000, capR = 16000;
    g_airports = calloc(capA, sizeof(Airport)); g_runways = calloc(capR, sizeof(Runway));
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line + 2;
        if (line[0] == 'A' && line[1] == ' ') {
            if (g_nairports == capA) { capA *= 2; g_airports = realloc(g_airports, capA * sizeof(Airport)); }
            Airport *a = &g_airports[g_nairports++];
            memset(a, 0, sizeof *a);
            snprintf(a->ident, sizeof a->ident, "%s", field(&s));
            snprintf(a->name, sizeof a->name, "%s", field(&s));
            snprintf(a->city, sizeof a->city, "%s", field(&s));
            snprintf(a->country, sizeof a->country, "%s", field(&s));
            a->lat = atof(field(&s)); a->lon = atof(field(&s)); a->elev = (float)atof(field(&s));
            a->type = field(&s)[0];
            a->rw0 = g_nrunways;
        } else if (line[0] == 'R' && line[1] == ' ' && g_nairports) {
            if (g_nrunways == capR) { capR *= 2; g_runways = realloc(g_runways, capR * sizeof(Runway)); }
            Runway *r = &g_runways[g_nrunways++];
            memset(r, 0, sizeof *r);
            snprintf(r->le, sizeof r->le, "%s", field(&s));
            snprintf(r->he, sizeof r->he, "%s", field(&s));
            r->lat1 = atof(field(&s)); r->lon1 = atof(field(&s)); r->lat2 = atof(field(&s)); r->lon2 = atof(field(&s));
            r->width = (float)atof(field(&s)); r->e1 = (float)atof(field(&s)); r->e2 = (float)atof(field(&s));
            r->surface = atoi(field(&s)); r->lighted = atoi(field(&s));
            r->length = (float)geo_dist(r->lat1, r->lon1, r->lat2, r->lon2);
            r->heading = (float)geo_bearing(r->lat1, r->lon1, r->lat2, r->lon2);
            r->airport = g_nairports - 1;
            g_airports[g_nairports - 1].nrw++;
        }
    }
    fclose(f);
    /* bucket the runways by their midpoint */
    cellStart = calloc(GW * GH + 1, sizeof(int)); cellList = malloc(sizeof(int) * (g_nrunways + 1));
    for (int i = 0; i < g_nrunways; i++) cellStart[cell_of((g_runways[i].lat1 + g_runways[i].lat2) / 2, (g_runways[i].lon1 + g_runways[i].lon2) / 2) + 1]++;
    for (int c = 0; c < GW * GH; c++) cellStart[c + 1] += cellStart[c];
    int *fill = calloc(GW * GH, sizeof(int));
    for (int i = 0; i < g_nrunways; i++) {
        int c = cell_of((g_runways[i].lat1 + g_runways[i].lat2) / 2, (g_runways[i].lon1 + g_runways[i].lon2) / 2);
        cellList[cellStart[c] + fill[c]++] = i;
    }
    free(fill);
    fprintf(stderr, "wingmere: %d airports, %d runways\n", g_nairports, g_nrunways);
    return true;
}

int airport_find(const char *ident) {
    for (int i = 0; i < g_nairports; i++) if (!strcasecmp(g_airports[i].ident, ident)) return i;
    return -1;
}
int airport_nearest(double lat, double lon, double *dist) {
    int best = -1; double bd = 1e30;
    int out[256];
    int n = runways_near(lat, lon, 150000, out, 256);
    for (int i = 0; i < n; i++) {
        Airport *a = &g_airports[g_runways[out[i]].airport];
        double d = geo_dist(lat, lon, a->lat, a->lon);
        if (d < bd) { bd = d; best = g_runways[out[i]].airport; }
    }
    if (dist) *dist = bd;
    return best;
}
int runways_near(double lat, double lon, double radius, int *out, int max) {
    int n = 0;
    int span = (int)ceil(radius / 111000.0 / fmax(cos(lat * M_PI / 180), 0.05)) + 1, spanY = (int)ceil(radius / 111000.0) + 1;
    for (int dy = -spanY; dy <= spanY; dy++)
        for (int dx = -span; dx <= span; dx++) {
            double clat = lat + dy, clon = lon + dx;
            if (clat < -90 || clat > 90) continue;
            int c = cell_of(clat, clon);
            for (int k = cellStart[c]; k < cellStart[c + 1] && n < max; k++) {
                Runway *r = &g_runways[cellList[k]];
                if (geo_dist(lat, lon, (r->lat1 + r->lat2) / 2, (r->lon1 + r->lon2) / 2) < radius + r->length / 2) out[n++] = cellList[k];
            }
        }
    return n;
}

/* Level the ground under and around runways so they sit flat on the elevation data.
 * Works in a local flat frame around each runway (plenty accurate over a few km). */
float runway_flatten(double lat, double lon, float h, bool *on) {
    int list[32];
    int n = runways_near(lat, lon, 4500, list, 32);
    return runway_flatten_list(list, n, lat, lon, h, on);
}
float runway_flatten_list(const int *list, int n, double lat, double lon, float h, bool *on) {
    if (on) *on = false;
    for (int k = 0; k < n; k++) {
        Runway *r = &g_runways[list[k]];
        if (r->surface == 2) continue;
        double mlat = (r->lat1 + r->lat2) / 2, cl = cos(mlat * M_PI / 180);
        double ax = (r->lon2 - r->lon1) * 111320 * cl, ay = (r->lat2 - r->lat1) * 110540;
        double px = (lon - r->lon1) * 111320 * cl, py = (lat - r->lat1) * 110540;
        double L = sqrt(ax * ax + ay * ay);
        if (L < 1) continue;
        double along = (px * ax + py * ay) / L, across = fabs(px * ay - py * ax) / L;
        double margin = 90, blend = 220;
        double da = along < 0 ? -along : (along > L ? along - L : 0);
        double d = fmax(da - margin * 0.3, across - r->width * 0.5 - margin);
        if (d > blend) continue;
        float t = d <= 0 ? 1.0f : 1.0f - smooth(0, (float)blend, (float)d);
        float re = lerpf(r->e1, r->e2, (float)clampf((float)(along / L), 0, 1));
        h = lerpf(h, re, t);
        if (on && da <= 0 && across <= r->width * 0.5 + 15) *on = true;
    }
    return h;
}
