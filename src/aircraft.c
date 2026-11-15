/* WINGMERE - aircraft definitions (aircraft.cfg) and 3D models (model.wmdl + textures).  MIT licence.
 * Every folder in data/aircraft/ is one aircraft; add a folder to add a plane. */
#include "wm.h"
#include <dirent.h>
#include "../third_party/stb_image.h"

AircraftDef g_aircraft[8];
int g_naircraft;

static GLuint load_texture(const char *dir, const char *name, bool srgb) {
    if (!name[0]) return 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int w, h, n;
    unsigned char *px = stbi_load(path, &w, &h, &n, 4);
    if (!px) { fprintf(stderr, "wingmere: missing texture %s\n", path); return 0; }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.0f);
    stbi_image_free(px);
    return t;
}

static void parse_cfg(AircraftDef *a, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k) - 1; e > k && *e == ' '; e--) *e = 0;
        while (*v == ' ') v++;
        v[strcspn(v, "\r\n")] = 0;
#define F(name) else if (!strcmp(k, #name)) a->name = (float)atof(v);
#define I(name) else if (!strcmp(k, #name)) a->name = atoi(v);
        if (!strcmp(k, "name")) snprintf(a->name, sizeof a->name, "%s", v);
        else if (!strcmp(k, "kind")) snprintf(a->kind, sizeof a->kind, "%s", v);
        else if (!strcmp(k, "sound")) snprintf(a->sound, sizeof a->sound, "%s", v);
        else if (!strcmp(k, "engine")) a->jet = !strcmp(v, "jet");
        else if (!strncmp(k, "gear", 4) && k[4] >= '0' && k[4] <= '2') { int i = k[4] - '0'; sscanf(v, "%f %f %f", &a->gear[i].x, &a->gear[i].y, &a->gear[i].z); }
        F(mass) F(wing_area) F(chord) F(span) F(length) F(height) F(cl0) F(cla) F(alpha_stall) F(clmax) F(cd0) F(k) F(thrust) F(prop_v)
        F(flap_cl) F(flap_cd) F(gear_cd) F(cm_alpha) F(cm_de) F(cm_q) F(cl_da) F(cl_p) F(cl_beta) F(cn_beta) F(cn_dr) F(cn_r)
        F(ixx) F(iyy) F(izz) F(v_rotate) F(v_cruise) F(vne) F(cam_dist)
        I(flaps) I(retract) I(tricycle)
#undef F
#undef I
    }
    fclose(f);
}

static bool load_model(AircraftDef *a, const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/model.wmdl", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return false; }
    fclose(f);
    if (memcmp(b, "WMDL", 4)) { free(b); return false; }
    size_t off = 20;
#define RD(T) (*(T *)(b + (off += sizeof(T)) - sizeof(T)))
    int nm = RD(uint32_t);
    a->nmats = nm < 32 ? nm : 32;
    for (int i = 0; i < nm; i++) {
        off += 48;
        float rgba[4]; memcpy(rgba, b + off, 16); off += 16;
        float metal = RD(float), rough = RD(float); uint32_t flags = RD(uint32_t);
        char tex[3][48]; memcpy(tex, b + off, 144); off += 144;
        if (i >= 32) continue;
        memcpy(a->mat[i].rgba, rgba, 16); a->mat[i].metal = metal; a->mat[i].rough = rough; a->mat[i].glass = flags & 1;
        a->mat[i].tex[0] = load_texture(dir, tex[0], true);
        a->mat[i].tex[1] = load_texture(dir, tex[1], false);
        a->mat[i].tex[2] = load_texture(dir, tex[2], false);
    }
    int np = RD(uint32_t);
    a->bmin = V(1e9f, 1e9f, 1e9f); a->bmax = V(-1e9f, -1e9f, -1e9f);
    a->tipL = V(-1e9f, 0, 0); a->tipR = V(1e9f, 0, 0); a->tail = V(0, -1e9f, 0);
    for (int i = 0; i < np; i++) {
        off += 48;
        uint32_t kind = RD(uint32_t);
        v3 piv; memcpy(&piv, b + off, 12); off += 24;
        uint32_t mat = RD(uint32_t), nv = RD(uint32_t), ni = RD(uint32_t);
        float *verts = (float *)(b + off); off += nv * 32;
        uint32_t *idx = (uint32_t *)(b + off); off += ni * 4;
        if (a->nparts >= MAX_PARTS) continue;
        for (uint32_t k = 0; k < nv; k++) {
            verts[k * 8 + 7] = 1.0f - verts[k * 8 + 7];   /* FBX texture space starts at the bottom row; images load top row first */
            v3 q = V(verts[k * 8], verts[k * 8 + 1], verts[k * 8 + 2]);
            if (kind == 0) {
                if (q.x > a->tipL.x) a->tipL = q;
                if (q.x < a->tipR.x) a->tipR = q;
                if (q.y > a->tail.y && q.z < 0) a->tail = q;
            }
            a->bmin = V(fminf(a->bmin.x, q.x), fminf(a->bmin.y, q.y), fminf(a->bmin.z, q.z));
            a->bmax = V(fmaxf(a->bmax.x, q.x), fmaxf(a->bmax.y, q.y), fmaxf(a->bmax.z, q.z));
        }
        if (kind == 1 && a->nengines < 4) {
            bool dup = false;
            for (int e = 0; e < a->nengines; e++) if (vlen(vsub(a->engine[e], piv)) < 0.5f) dup = true;
            if (!dup) a->engine[a->nengines++] = piv;
        }
        typeof(a->part[0]) *p = &a->part[a->nparts++];
        p->kind = (int)kind; p->pivot = piv; p->mat = (int)mat < a->nmats ? (int)mat : 0; p->nidx = (int)ni;
        glGenVertexArrays(1, &p->vao); glBindVertexArray(p->vao);
        glGenBuffers(1, &p->vbo); glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
        glBufferData(GL_ARRAY_BUFFER, nv * 32, verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
        glGenBuffers(1, &p->ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * 4, idx, GL_STATIC_DRAW);
    }
#undef RD
    free(b);
    return true;
}

static int by_mass(const void *x, const void *y) { float d = ((const AircraftDef *)x)->mass - ((const AircraftDef *)y)->mass; return d < 0 ? -1 : d > 0; }

void aircraft_load_all(const char *root) {
    DIR *d = opendir(root);
    if (!d) { fprintf(stderr, "wingmere: no aircraft folder at %s\n", root); return; }
    struct dirent *e;
    while ((e = readdir(d)) && g_naircraft < 8) {
        if (e->d_name[0] == '.') continue;
        char dir[1024], cfg[1100];
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        snprintf(cfg, sizeof cfg, "%s/aircraft.cfg", dir);
        AircraftDef *a = &g_aircraft[g_naircraft];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", e->d_name);
        a->cam_dist = 20; a->cla = 5; a->clmax = 1.4f; a->alpha_stall = 15; a->cd0 = 0.03f; a->k = 0.05f;
        parse_cfg(a, cfg);
        if (a->mass <= 0 || !load_model(a, dir)) continue;
        a->alpha_stall *= DEG;
        if (a->vne <= 0) a->vne = a->jet ? 175 : a->v_cruise * 1.35f;
        fprintf(stderr, "wingmere: aircraft %-10s %s (%d parts)\n", a->id, a->name, a->nparts);
        g_naircraft++;
    }
    closedir(d);
    qsort(g_aircraft, g_naircraft, sizeof(AircraftDef), by_mass);
}

void aircraft_draw(GLuint prog, const AircraftDef *a, m4 model, float propAngle, float gearDown, bool shadowPass) {
    GLint uModel = glGetUniformLocation(prog, "uModel");
    GLint uCol = glGetUniformLocation(prog, "uColor"), uMat = glGetUniformLocation(prog, "uMatParams"), uHas = glGetUniformLocation(prog, "uHasTex");
    for (int pass = 0; pass < 2; pass++) {          /* opaque first, glass second */
        if (shadowPass && pass) break;
        for (int i = 0; i < a->nparts; i++) {
            const typeof(a->part[0]) *p = &a->part[i];
            const typeof(a->mat[0]) *mt = &a->mat[p->mat];
            if ((mt->glass != 0) != (pass == 1)) continue;
            m4 m = model;
            if (p->kind == 1) {          /* propeller / fan: spin about the part's own hub, on the forward axis */
                m4 t1 = m4_trs(p->pivot, q_axis(V(0, 0, 1), propAngle * (p->pivot.x < -0.3f ? -1.0f : 1.0f)), 1);
                m4 t0 = m4_trs(vmul(p->pivot, -1), q_ident(), 1);
                m = m4_mul(model, m4_mul(t1, t0));
            } else if (p->kind == 2) {   /* landing gear slides up into the wells */
                if (gearDown < 0.1f) continue;
                m = m4_mul(model, m4_trs(V(0, (1 - gearDown) * fminf(a->height * 0.12f, 1.5f), 0), q_ident(), 1));
            }
            glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
            if (!shadowPass) {
                glUniform4fv(uCol, 1, mt->rgba);
                glUniform3f(uMat, mt->metal, mt->rough, (float)mt->glass);
                glUniform3f(uHas, mt->tex[0] ? 1.0f : 0.0f, mt->tex[1] ? 1.0f : 0.0f, mt->tex[2] ? 1.0f : 0.0f);
                for (int t = 0; t < 3; t++) { glActiveTexture(GL_TEXTURE4 + t); glBindTexture(GL_TEXTURE_2D, mt->tex[t]); }
            }
            glBindVertexArray(p->vao);
            glDrawElements(GL_TRIANGLES, p->nidx, GL_UNSIGNED_INT, 0);
        }
    }
}
