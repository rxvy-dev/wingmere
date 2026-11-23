/* WINGMERE - country borders (Natural Earth 1:50m, public domain): which country's airspace a point is in.
 * MIT licence.  Airspace here is the land territory plus 12 nautical miles of territorial sea. */
#include "wm.h"

typedef struct { int r0, nr; char iso[4], name[40]; float bb[4]; } Country;   /* bb: lat min/max, lon min/max */
typedef struct { int p0, np; float bb[4]; int country; } Ring;
static Country *cty; static int ncty;
static Ring *rings; static int nrings;
static float *pts; static int npts;      /* lat, lon pairs */

void borders_load(const char *datadir) {
    char path[600];
    snprintf(path, sizeof path, "%s/borders.txt", datadir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "wingmere: no country borders (%s)\n", path); return; }
    int capC = 256, capR = 4096, capP = 1 << 18;
    cty = malloc(sizeof(Country) * capC); rings = malloc(sizeof(Ring) * capR); pts = malloc(sizeof(float) * 2 * capP);
    char *line = NULL; size_t lcap = 0;
    while (getline(&line, &lcap, f) > 0) {
        if (line[0] == 'C' && line[1] == ' ') {
            if (ncty == capC) { capC *= 2; cty = realloc(cty, sizeof(Country) * capC); }
            Country *c = &cty[ncty++]; memset(c, 0, sizeof *c);
            sscanf(line + 2, "%3s", c->iso);
            const char *nm = line + 2 + strlen(c->iso); while (*nm == ' ') nm++;
            snprintf(c->name, sizeof c->name, "%s", nm); c->name[strcspn(c->name, "\n")] = 0;
            for (char *q = c->name; *q; q++) *q = (char)toupper((unsigned char)*q);
            c->r0 = nrings; c->bb[0] = c->bb[2] = 1e9f; c->bb[1] = c->bb[3] = -1e9f;
        } else if (line[0] == 'R' && ncty) {
            int n = atoi(line + 2);
            if (getline(&line, &lcap, f) <= 0 || n < 3) break;
            if (nrings == capR) { capR *= 2; rings = realloc(rings, sizeof(Ring) * capR); }
            while (npts + n > capP) { capP *= 2; pts = realloc(pts, sizeof(float) * 2 * capP); }
            Ring *r = &rings[nrings++]; r->p0 = npts; r->np = 0; r->country = ncty - 1;
            r->bb[0] = r->bb[2] = 1e9f; r->bb[1] = r->bb[3] = -1e9f;
            char *s = line, *e;
            for (int i = 0; i < n; i++) {
                float la = strtof(s, &e); if (e == s) break; s = e;
                float lo = strtof(s, &e); if (e == s) break; s = e;
                pts[npts * 2] = la; pts[npts * 2 + 1] = lo; npts++; r->np++;
                r->bb[0] = fminf(r->bb[0], la); r->bb[1] = fmaxf(r->bb[1], la); r->bb[2] = fminf(r->bb[2], lo); r->bb[3] = fmaxf(r->bb[3], lo);
            }
            Country *c = &cty[ncty - 1]; c->nr++;
            for (int k = 0; k < 4; k += 2) { c->bb[k] = fminf(c->bb[k], r->bb[k]); c->bb[k + 1] = fmaxf(c->bb[k + 1], r->bb[k + 1]); }
        }
    }
    free(line); fclose(f);
    fprintf(stderr, "wingmere: %d countries, %d border points\n", ncty, npts);
}
static bool in_ring(const Ring *r, float la, float lo) {
    if (la < r->bb[0] || la > r->bb[1] || lo < r->bb[2] || lo > r->bb[3]) return false;
    bool in = false;
    const float *p = &pts[r->p0 * 2];
    for (int i = 0, j = r->np - 1; i < r->np; j = i++) {
        float yi = p[i * 2], xi = p[i * 2 + 1], yj = p[j * 2], xj = p[j * 2 + 1];
        if ((yi > la) != (yj > la) && lo < (xj - xi) * (la - yi) / (yj - yi + 1e-12f) + xi) in = !in;
    }
    return in;
}
/* the country whose land a point is over, or -1 (sea, or nowhere we know) */
int borders_land(double lat, double lon) {
    float la = (float)lat, lo = (float)lon;
    for (int i = 0; i < nrings; i++) if (in_ring(&rings[i], la, lo)) return rings[i].country;
    return -1;
}
/* distance in metres from a point to the nearest edge of a country (0 when inside it) */
double borders_distance(int c, double lat, double lon) {
    if (c < 0 || c >= ncty) return 1e12;
    float la = (float)lat, lo = (float)lon;
    float cs = cosf(la * (float)M_PI / 180);
    double best = 1e30;
    for (int r = cty[c].r0; r < cty[c].r0 + cty[c].nr; r++) {
        Ring *g = &rings[r];
        if (in_ring(g, la, lo)) return 0;
        /* skip rings that are obviously too far to matter */
        float dla = la < g->bb[0] ? g->bb[0] - la : (la > g->bb[1] ? la - g->bb[1] : 0);
        float dlo = lo < g->bb[2] ? g->bb[2] - lo : (lo > g->bb[3] ? lo - g->bb[3] : 0);
        double boxd = sqrt((double)dla * dla + (double)dlo * cs * dlo * cs) * 111320.0;
        if (boxd > best) continue;
        const float *p = &pts[g->p0 * 2];
        for (int i = 0, j = g->np - 1; i < g->np; j = i++) {
            float ax = (p[j * 2 + 1] - lo) * cs, ay = p[j * 2] - la, bx = (p[i * 2 + 1] - lo) * cs, by = p[i * 2] - la;
            float ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
            float t = l2 > 0 ? clampf(-(ax * ex + ay * ey) / l2, 0, 1) : 0;
            float qx = ax + ex * t, qy = ay + ey * t;
            double d = sqrt((double)qx * qx + (double)qy * qy) * 111320.0;
            if (d < best) best = d;
        }
    }
    return best;
}
/* the country whose airspace this is: its land, or within 12 NM of its coast */
int borders_airspace(double lat, double lon) {
    int c = borders_land(lat, lon);
    if (c >= 0) return c;
    int best = -1; double bd = 22224;
    float la = (float)lat, lo = (float)lon;
    for (int i = 0; i < ncty; i++) {
        if (la < cty[i].bb[0] - 0.3f || la > cty[i].bb[1] + 0.3f || lo < cty[i].bb[2] - 0.4f || lo > cty[i].bb[3] + 0.4f) continue;
        double d = borders_distance(i, lat, lon);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
const char *borders_name(int c) { return c >= 0 && c < ncty ? cty[c].name : "INTERNATIONAL AIRSPACE"; }
/* outline of a country, for the map: calls fn for each ring's points */
int borders_rings(int c, int k, const float **p) {
    if (c < 0 || c >= ncty || k >= cty[c].nr) return 0;
    Ring *r = &rings[cty[c].r0 + k];
    *p = &pts[r->p0 * 2];
    return r->np;
}
