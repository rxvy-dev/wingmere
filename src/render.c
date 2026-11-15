/* WINGMERE - renderer.  MIT licence.
 *
 * Every position on the planet is a double-precision ECEF coordinate; the GPU only ever sees
 * float offsets from the camera, so there is no jitter anywhere on Earth.  Depth is logarithmic
 * so a cockpit 30 cm away and a mountain 300 km away share one depth buffer.
 *
 * Frame:
 *   0. sky cubemap, baked in the local east/up/north frame when the sun or altitude moves enough
 *   1. aircraft shadow map (sun-aligned, tight round the aircraft)
 *   2. HDR scene, MSAA: sky -> streamed terrain -> runways -> aircraft -> smoke and lights
 *   3. raymarched cloud layer at half resolution
 *   4. bloom chain
 *   5. composite: clouds, bloom, flare, eye adaptation, ACES
 */
#include "wm.h"
#include "shaders_gen.h" /* generated at build time from shaders/wingmere.glsl */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

int g_w = 1280, g_h = 720;
GLuint g_progHud, g_progMap;
static RenderCfg cfg;
static int rw, rh, cw, ch;
static char *src;
static GLuint pTerrain, pRunway, pPlane, pShadow, pSkyBake, pSky, pPart, pDown, pUp, pClouds, pComp;
static GLuint texDetail, texNoise3, texSky, skyFbo, fsVao;
enum { SKY_RES = 256 };
static GLuint fboMS, rbColor, rbDepth, fboScene, texScene, texDepth, fboCloud, texCloud;
static GLuint fboSh, texSh;
#define NBLOOM 6
static GLuint fboBloom[NBLOOM], texBloom[NBLOOM];
static int bloomW[NBLOOM], bloomH[NBLOOM];
static GLuint partVao, partVbo;
static float *partBuf; static int partN, partCap;

static const float FAR = 4.0e6f, NEAR = 0.3f;
static float uFCv;
static m4 matVP, matShadow;
static d3 lastCam;
static d3 sunW = {0, 0, 1};                 /* unit vector towards the sun, ECEF */
static v3 bakedSunEnu = {0, -1, 0};
static float bakedAlt = -1e9f;
static bool skyDirty = true;
static float skyLum = 0.1f, exposure = 1, lightLevel = 1;
static float wCover = 0.42f, wBase = 1500, wTop = 3400, wHaze = 4e-5f;
static double timeAcc;

/* ------------------------------------------------------------------ shaders */
static char *section(const char *name) {
    char tag[64];
    snprintf(tag, sizeof tag, "//@ %s\n", name);
    const char *s = strstr(src, tag);
    if (!s) { fprintf(stderr, "wingmere: missing shader section %s\n", name); exit(1); }
    s += strlen(tag);
    const char *e = strstr(s, "\n//@ ");
    size_t n = e ? (size_t)(e - s) + 1 : strlen(s);
    char *out = malloc(n + 1);
    memcpy(out, s, n); out[n] = 0;
    return out;
}
static GLuint compile(GLenum type, const char *names) {
    const char *parts[8] = {"#version 330 core\n"};
    char *bodies[8] = {0};
    int n = 1;
    char buf[128]; snprintf(buf, sizeof buf, "%s", names);
    for (char *tok = strtok(buf, "+"); tok && n < 8; tok = strtok(NULL, "+")) { bodies[n] = section(tok); parts[n] = bodies[n]; n++; }
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, n, parts, NULL);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; glGetShaderInfoLog(sh, sizeof log, NULL, log); fprintf(stderr, "wingmere: shader %s:\n%s\n", names, log); }
    for (int i = 1; i < n; i++) free(bodies[i]);
    return sh;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, a); glAttachShader(p, b); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[8192]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "wingmere: link %s + %s:\n%s\n", vs, fs, log); }
    glDeleteShader(a); glDeleteShader(b);
    return p;
}
static void load_programs(void) {
    const char *path = getenv("WINGMERE_SHADERS");
    free(src); src = NULL;
    if (path) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            src = malloc(n + 1);
            if (fread(src, 1, n, f) != (size_t)n) n = 0;
            src[n] = 0; fclose(f);
        }
    }
    if (!src) { src = malloc(sizeof shader_src + 1); memcpy(src, shader_src, sizeof shader_src); src[sizeof shader_src] = 0; }
    pTerrain = program("terrain.vert", "common+terrain.frag");
    pRunway = program("runway.vert", "common+runway.frag");
    pPlane = program("plane.vert", "common+plane.frag");
    pShadow = program("shadow.vert", "shadow.frag");
    pSkyBake = program("fs.vert", "skybake.frag");
    pSky = program("fs.vert", "common+sky.frag");
    pPart = program("particle.vert", "common+particle.frag");
    pDown = program("fs.vert", "down.frag");
    pUp = program("fs.vert", "up.frag");
    pClouds = program("fs.vert", "common+clouds.frag");
    pComp = program("fs.vert", "composite.frag");
    g_progHud = program("hud.vert", "hud.frag");
    g_progMap = program("map.vert", "map.frag");
}
#define U(p, n) glGetUniformLocation(p, n)
static void u3(GLuint p, const char *n, v3 v) { glUniform3f(U(p, n), v.x, v.y, v.z); }
static void u1(GLuint p, const char *n, float v) { glUniform1f(U(p, n), v); }
static void ui(GLuint p, const char *n, int v) { glUniform1i(U(p, n), v); }
static void um(GLuint p, const char *n, m4 m) { glUniformMatrix4fv(U(p, n), 1, GL_FALSE, m.m); }
static void bind(int unit, GLenum target, GLuint tex) { glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(target, tex); }
static void fs_pass(void) { glBindVertexArray(fsVao); glDrawArrays(GL_TRIANGLES, 0, 3); }

/* ------------------------------------------------------------------ procedural textures */
static uint32_t rng = 12345;
static float frand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.0f / 16777216.0f); }

static GLuint tex2d(GLint ifmt, int w, int h, GLenum fmt, GLenum type, GLint filter) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
static void make_detail(void) {
    /* R: tileable fractal noise (ground and tarmac grain).  G: white noise (stars). */
    enum { N = 512 };
    uint8_t *px = malloc(N * N * 4);
    static float lat[6][64][64];
    for (int o = 0; o < 6; o++) { int n = 4 << o; if (n > 64) n = 64; for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) lat[o][y][x] = frand(); }
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float a = 0, b = 0.5f;
            for (int o = 0; o < 6; o++) {
                int n = 4 << o; if (n > 64) n = 64;
                float fx = (float)x / N * n, fy = (float)y / N * n;
                int ix = (int)fx, iy = (int)fy; float tx = fx - ix, ty = fy - iy;
                tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
                float v = lerpf(lerpf(lat[o][iy % n][ix % n], lat[o][iy % n][(ix + 1) % n], tx), lerpf(lat[o][(iy + 1) % n][ix % n], lat[o][(iy + 1) % n][(ix + 1) % n], tx), ty);
                a += b * v; b *= 0.5f;
            }
            uint8_t *p = &px[(y * N + x) * 4];
            p[0] = (uint8_t)clampf((a - 0.18f) * 1.55f * 255, 0, 255); p[1] = (uint8_t)(frand() * 255); p[2] = 0; p[3] = 255;
        }
    glGenTextures(1, &texDetail); glBindTexture(GL_TEXTURE_2D, texDetail);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    free(px);
}
/* tileable 3D noise for the clouds: value-noise fBm blended with inverted Worley for billowy edges */
static void make_noise3(void) {
    enum { N = 96, C = 6 };
    static float lat[4][32][32][32];
    for (int o = 0; o < 4; o++) { int n = 4 << o; for (int z = 0; z < n; z++) for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) lat[o][z][y][x] = frand(); }
    static float pts[C][C][C][3];
    for (int z = 0; z < C; z++) for (int y = 0; y < C; y++) for (int x = 0; x < C; x++) for (int k = 0; k < 3; k++) pts[z][y][x][k] = frand();
    uint8_t *px = malloc(N * N * N);
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float a = 0, b = 0.5f;
                for (int o = 0; o < 4; o++) {
                    int n = 4 << o;
                    float fx = (float)x / N * n, fy = (float)y / N * n, fz = (float)z / N * n;
                    int ix = (int)fx, iy = (int)fy, iz = (int)fz;
                    float tx = fx - ix, ty = fy - iy, tz = fz - iz;
                    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty); tz = tz * tz * (3 - 2 * tz);
                    int x1 = (ix + 1) % n, y1 = (iy + 1) % n, z1 = (iz + 1) % n; ix %= n; iy %= n; iz %= n;
                    float c00 = lerpf(lat[o][iz][iy][ix], lat[o][iz][iy][x1], tx), c01 = lerpf(lat[o][iz][y1][ix], lat[o][iz][y1][x1], tx);
                    float c10 = lerpf(lat[o][z1][iy][ix], lat[o][z1][iy][x1], tx), c11 = lerpf(lat[o][z1][y1][ix], lat[o][z1][y1][x1], tx);
                    a += b * lerpf(lerpf(c00, c01, ty), lerpf(c10, c11, ty), tz); b *= 0.5f;
                }
                float cx = (float)x / N * C, cy = (float)y / N * C, cz = (float)z / N * C, best = 9;
                int gx = (int)cx, gy = (int)cy, gz = (int)cz;
                for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int qx = gx + dx, qy = gy + dy, qz = gz + dz;
                    float *p = pts[(qz + C) % C][(qy + C) % C][(qx + C) % C];
                    float ex = qx + p[0] - cx, ey = qy + p[1] - cy, ez = qz + p[2] - cz, d = ex * ex + ey * ey + ez * ez;
                    if (d < best) best = d;
                }
                float w = 1 - clampf(sqrtf(best), 0, 1);
                px[(z * N + y) * N + x] = (uint8_t)clampf(((a - 0.25f) * 1.6f * 0.6f + w * 0.4f) * 255, 0, 255);
            }
    glGenTextures(1, &texNoise3); glBindTexture(GL_TEXTURE_3D, texNoise3);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, N, N, N, 0, GL_RED, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateMipmap(GL_TEXTURE_3D);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    for (GLenum w = GL_TEXTURE_WRAP_S; w <= GL_TEXTURE_WRAP_R; w++) glTexParameteri(GL_TEXTURE_3D, w, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    free(px);
}

/* ------------------------------------------------------------------ sun and sky */
void render_set_time(double lat, double lon, float hours, int doy) {
    (void)lat;
    double decl = -23.44 * cos(2 * M_PI * (doy + 10) / 365.0) * M_PI / 180;
    double slon = (lon - (hours - 12.0) * 15.0) * M_PI / 180;
    sunW = D(cos(decl) * cos(slon), cos(decl) * sin(slon), sin(decl));
}
v3 render_sun_dir(void) { return d2f(sunW); }
void render_weather(float cover, float base, float top, float haze) { wCover = cover; wBase = base; wTop = top > base + 200 ? top : base + 200; wHaze = haze; }
float render_light_level(void) { return lightLevel; }

/* sunlight left after the trip through the atmosphere to a point at altitude alt, sun at sin(elevation)=mu */
static v3 sun_transmittance(float alt, float mu) {
    const double rP = 6371e3, rA = 6471e3, hR = 8000, hM = 1200;
    const double kR[3] = {5.5e-6, 13.0e-6, 22.4e-6}, kM = 21e-6;
    double r = rP + fmax(alt, 1.0);
    double dip = -sqrt(fmax(0.0, 1.0 - (rP / r) * (rP / r)));
    double m = fmax(mu, dip + 0.002);
    double ox = 0, oy = r, dx = sqrt(1 - m * m), dy = m;
    double b = dy * oy, c = oy * oy - rA * rA, disc = b * b - c;
    double tEnd = -b + sqrt(fmax(disc, 0));
    double odR = 0, odM = 0;
    const int N = 48;
    for (int i = 0; i < N; i++) {
        double t = (i + 0.5) / N * tEnd, px = ox + dx * t, py = oy + dy * t;
        double h = sqrt(px * px + py * py) - rP;
        if (h < 0) h = 0;
        odR += exp(-h / hR) * tEnd / N; odM += exp(-h / hM) * tEnd / N;
    }
    float vis = smooth((float)dip - 0.006f, (float)dip + 0.006f, mu);
    return V((float)exp(-(kR[0] * odR + kM * odM)) * vis, (float)exp(-(kR[1] * odR + kM * odM)) * vis, (float)exp(-(kR[2] * odR + kM * odM)) * vis);
}
static void sky_alloc(void) {
    glGenTextures(1, &texSky); glBindTexture(GL_TEXTURE_CUBE_MAP, texSky);
    for (int i = 0; i < 6; i++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F, SKY_RES, SKY_RES, 0, GL_RGB, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glGenFramebuffers(1, &skyFbo);
}
static void sky_bake(v3 sunEnu, float alt) {
    static const float F[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float Uu[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    static const float R[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    glBindFramebuffer(GL_FRAMEBUFFER, skyFbo);
    glUseProgram(pSkyBake); glViewport(0, 0, SKY_RES, SKY_RES); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    u3(pSkyBake, "uSunEnu", sunEnu); u1(pSkyBake, "uAltitude", alt);
    for (int i = 0; i < 6; i++) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, texSky, 0);
        glUniform3fv(U(pSkyBake, "uFaceF"), 1, F[i]); glUniform3fv(U(pSkyBake, "uFaceU"), 1, Uu[i]); glUniform3fv(U(pSkyBake, "uFaceR"), 1, R[i]);
        fs_pass();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bind(0, GL_TEXTURE_CUBE_MAP, texSky);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    /* average brightness of the sky overhead drives the exposure */
    int top = 0; for (int s = SKY_RES; s > 1; s >>= 1) top++;
    float px[3] = {0, 0, 0};
    glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, top, GL_RGB, GL_FLOAT, px);
    skyLum = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
    bakedSunEnu = sunEnu; bakedAlt = alt; skyDirty = false;
}

/* ------------------------------------------------------------------ render targets */
static void make_targets(void) {
    rw = (int)(g_w * cfg.scale); rh = (int)(g_h * cfg.scale);
    if (rw < 64) rw = 64;
    if (rh < 64) rh = 64;
    cw = rw / 2; ch = rh / 2;
    if (fboScene) {
        glDeleteFramebuffers(1, &fboScene); glDeleteTextures(1, &texScene); glDeleteTextures(1, &texDepth);
        glDeleteFramebuffers(1, &fboCloud); glDeleteTextures(1, &texCloud);
        if (fboMS) { glDeleteFramebuffers(1, &fboMS); glDeleteRenderbuffers(1, &rbColor); glDeleteRenderbuffers(1, &rbDepth); fboMS = 0; }
        glDeleteFramebuffers(NBLOOM, fboBloom); glDeleteTextures(NBLOOM, texBloom);
    }
    texScene = tex2d(GL_RGBA16F, rw, rh, GL_RGBA, GL_FLOAT, GL_LINEAR);
    texDepth = tex2d(GL_DEPTH_COMPONENT32F, rw, rh, GL_DEPTH_COMPONENT, GL_FLOAT, GL_NEAREST);
    glGenFramebuffers(1, &fboScene); glBindFramebuffer(GL_FRAMEBUFFER, fboScene);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texScene, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texDepth, 0);
    if (cfg.msaa > 1) {
        glGenFramebuffers(1, &fboMS); glBindFramebuffer(GL_FRAMEBUFFER, fboMS);
        glGenRenderbuffers(1, &rbColor); glBindRenderbuffer(GL_RENDERBUFFER, rbColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, cfg.msaa, GL_RGBA16F, rw, rh);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbColor);
        glGenRenderbuffers(1, &rbDepth); glBindRenderbuffer(GL_RENDERBUFFER, rbDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, cfg.msaa, GL_DEPTH_COMPONENT32F, rw, rh);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "wingmere: MSAA unavailable, continuing without\n"); cfg.msaa = 0; }
    }
    texCloud = tex2d(GL_RGBA16F, cw, ch, GL_RGBA, GL_FLOAT, GL_LINEAR);
    glGenFramebuffers(1, &fboCloud); glBindFramebuffer(GL_FRAMEBUFFER, fboCloud);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texCloud, 0);
    int w = rw / 2, h = rh / 2;
    for (int i = 0; i < NBLOOM; i++) {
        bloomW[i] = w > 1 ? w : 1; bloomH[i] = h > 1 ? h : 1;
        texBloom[i] = tex2d(GL_RGBA16F, bloomW[i], bloomH[i], GL_RGBA, GL_FLOAT, GL_LINEAR);
        glGenFramebuffers(1, &fboBloom[i]); glBindFramebuffer(GL_FRAMEBUFFER, fboBloom[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texBloom[i], 0);
        w /= 2; h /= 2;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool render_init(RenderCfg c) {
    cfg = c; g_w = c.w; g_h = c.h;
    if (cfg.scale <= 0.1f) cfg.scale = 1;
    GLint maxTex = 4096; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (cfg.shadowRes > maxTex) cfg.shadowRes = maxTex;
    if (cfg.shadowRes < 512) cfg.shadowRes = 2048;
    load_programs();
    glGenVertexArrays(1, &fsVao);
    make_detail();
    make_noise3();
    sky_alloc();
    texSh = tex2d(GL_DEPTH_COMPONENT24, cfg.shadowRes, cfg.shadowRes, GL_DEPTH_COMPONENT, GL_FLOAT, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &fboSh); glBindFramebuffer(GL_FRAMEBUFFER, fboSh);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texSh, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenVertexArrays(1, &partVao); glBindVertexArray(partVao);
    glGenBuffers(1, &partVbo); glBindBuffer(GL_ARRAY_BUFFER, partVbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (void *)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, (void *)20);
    partCap = 6 * 16384;
    partBuf = malloc(sizeof(float) * 9 * partCap);
    uFCv = 1.0f / log2f(FAR + 1.0f);
    make_targets();
    hud_init();
    return true;
}
void render_resize(int w, int h) { if (w < 1 || h < 1) return; g_w = w; g_h = h; make_targets(); }
void render_reload_shaders(void) { load_programs(); skyDirty = true; }

/* ------------------------------------------------------------------ effects */
typedef struct { d3 pos; v3 vel; float size, grow, life, maxLife, c[4]; } Particle;
typedef struct { d3 pos; float size, c[3], inten; } Light;
enum { MAX_PART = 6000, MAX_LIGHT = 9000 };
static Particle part[MAX_PART]; static int nPart;
static Light light[MAX_LIGHT]; static int nLight;
void fx_emit(d3 pos, v3 vel, float size, float grow, float life, float r, float g, float b, float a) {
    if (nPart >= MAX_PART) return;
    Particle *p = &part[nPart++];
    p->pos = pos; p->vel = vel; p->size = size; p->grow = grow; p->life = p->maxLife = life;
    p->c[0] = r; p->c[1] = g; p->c[2] = b; p->c[3] = a;
}
void fx_light(d3 pos, float size, float r, float g, float b, float inten) {
    if (nLight >= MAX_LIGHT) return;
    Light *l = &light[nLight++];
    l->pos = pos; l->size = size; l->c[0] = r; l->c[1] = g; l->c[2] = b; l->inten = inten;
}
void fx_clear(void) { nPart = 0; nLight = 0; }
static void fx_update(float dt) {
    for (int i = 0; i < nPart;) {
        Particle *p = &part[i];
        p->life -= dt;
        if (p->life <= 0) { part[i] = part[--nPart]; continue; }
        p->pos = dfmad(p->pos, p->vel, dt);
        p->vel = vmul(p->vel, expf(-dt * 0.6f));
        p->size += p->grow * dt;
        i++;
    }
}
static void pq(v3 c, v3 ax, v3 ay, float r, float g, float b, float a) {
    static const float uv[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    if (partN + 6 > partCap) return;
    for (int i = 0; i < 6; i++) {
        float *v = &partBuf[partN * 9];
        v3 p = vadd(c, vadd(vmul(ax, uv[i][0]), vmul(ay, uv[i][1])));
        v[0] = p.x; v[1] = p.y; v[2] = p.z; v[3] = uv[i][0]; v[4] = uv[i][1]; v[5] = r; v[6] = g; v[7] = b; v[8] = a;
        partN++;
    }
}

/* ------------------------------------------------------------------ runways near the camera */
typedef struct { d3 pos; uint8_t type, end, idx; } RwLight;  /* type: 0 edge 1 threshold/end 2 approach 3 strobe 4 PAPI */
typedef struct {
    int idx; GLuint vao, vbo; int nv; d3 center; float len, wid, alt; int ident[4];
    RwLight *lights; int nlights; d3 thr[2]; v3 dirOut[2]; float thrAlt[2];
} RwMesh;
enum { MAX_RWM = 48 };
static RwMesh rwm[MAX_RWM]; static int nRwm;
static double rwLat = 1e9, rwLon = 1e9;

static void parse_ident(const char *s, float heading, int *num, int *let) {
    int n = 0, i = 0;
    while (s[i] >= '0' && s[i] <= '9') n = n * 10 + s[i++] - '0';
    if (i == 0 || n < 1 || n > 36) { n = (int)lroundf(fmodf(heading + 360, 360) / 10); if (n <= 0) n += 36; }
    *num = n;
    *let = s[i] == 'L' ? 1 : (s[i] == 'R' ? 2 : (s[i] == 'C' ? 3 : 0));
}
static void rw_free(RwMesh *m) { glDeleteVertexArrays(1, &m->vao); glDeleteBuffers(1, &m->vbo); free(m->lights); memset(m, 0, sizeof *m); }
static void rw_add_light(RwMesh *m, int *cap, double lat, double lon, float h, int type, int end, int idx) {
    if (m->nlights == *cap) { *cap = *cap ? *cap * 2 : 256; m->lights = realloc(m->lights, sizeof(RwLight) * *cap); }
    RwLight *l = &m->lights[m->nlights++];
    l->pos = geo_to_ecef(lat, lon, h); l->type = (uint8_t)type; l->end = (uint8_t)end; l->idx = (uint8_t)idx;
}
static void rw_build(RwMesh *m, int idx) {
    const Runway *r = &g_runways[idx];
    m->idx = idx; m->len = r->length; m->wid = r->width; m->alt = (r->e1 + r->e2) * 0.5f;
    parse_ident(r->le, r->heading, &m->ident[0], &m->ident[1]);
    parse_ident(r->he, r->heading + 180, &m->ident[2], &m->ident[3]);
    double clat = (r->lat1 + r->lat2) * 0.5, clon = (r->lon1 + r->lon2) * 0.5;
    m->center = geo_to_ecef(clat, clon, m->alt);
    int nseg = (int)(r->length / 40) + 1;
    if (nseg > 400) nseg = 400;
    float hw = r->width * 0.5f + 1.5f;
    float *vb = malloc(sizeof(float) * 5 * 2 * (nseg + 1));
    double brg = geo_bearing(r->lat1, r->lon1, r->lat2, r->lon2);
    for (int i = 0; i <= nseg; i++) {
        float t = (float)i / nseg, along = t * r->length;
        double lat, lon;
        geo_offset(r->lat1, r->lon1, brg, along, &lat, &lon);
        float h = lerpf(r->e1, r->e2, t) + 0.12f;
        for (int s = 0; s < 2; s++) {
            float across = s ? hw : -hw;         /* positive to the right of a pilot rolling from le to he */
            double la, lo;
            geo_offset(lat, lon, brg + 90, across, &la, &lo);
            v3 p = d2f(dsub(geo_to_ecef(la, lo, h), m->center));
            float *v = &vb[(i * 2 + s) * 5];
            v[0] = p.x; v[1] = p.y; v[2] = p.z; v[3] = along; v[4] = across;
        }
    }
    m->nv = 2 * (nseg + 1);
    glGenVertexArrays(1, &m->vao); glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo); glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 5 * m->nv, vb, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
    free(vb);
    /* lights: edges, thresholds, approach bars, sequenced strobes, PAPI */
    int cap = 0;
    for (int e = 0; e < 2; e++) {
        double tl = e ? r->lat2 : r->lat1, tn = e ? r->lon2 : r->lon1;
        double out = e ? brg : brg + 180;        /* bearing pointing away from the runway, out along the approach */
        float te = e ? r->e2 : r->e1;
        m->thr[e] = geo_to_ecef(tl, tn, te); m->thrAlt[e] = te;
        double ol, oo; geo_offset(tl, tn, out, 100, &ol, &oo);
        m->dirOut[e] = vnorm(d2f(dsub(geo_to_ecef(ol, oo, te), m->thr[e])));
        if (r->lighted) {
            for (int k = -6; k <= 6; k++) { double la, lo; geo_offset(tl, tn, brg + 90, k * r->width / 12.5, &la, &lo); rw_add_light(m, &cap, la, lo, te + 0.4f, 1, e, 0); }
            if (r->length > 1500) {
                for (int s = 1; s <= 30; s++) {
                    double ca, co; geo_offset(tl, tn, out, s * 30.0, &ca, &co);
                    for (int k = -2; k <= 2; k++) { double la, lo; geo_offset(ca, co, brg + 90, k * 1.0, &la, &lo); rw_add_light(m, &cap, la, lo, te + 0.8f, 2, e, 0); }
                    if (s == 10) for (int k = -7; k <= 7; k++) { double la, lo; geo_offset(ca, co, brg + 90, k * 2.0, &la, &lo); rw_add_light(m, &cap, la, lo, te + 0.8f, 2, e, 0); }
                    if (s >= 11) rw_add_light(m, &cap, ca, co, te + 1.6f, 3, e, s - 11);
                }
            }
        }
        /* PAPI: four boxes left of the aiming point */
        double pa, po; geo_offset(tl, tn, e ? brg + 180 : brg, 320, &pa, &po);
        for (int k = 0; k < 4; k++) { double la, lo; geo_offset(pa, po, (e ? brg + 180 : brg) - 90, r->width * 0.5 + 15 + k * 9, &la, &lo); rw_add_light(m, &cap, la, lo, te + 0.9f, 4, e, 3 - k); }
    }
    if (r->lighted)
        for (float a = 0; a <= r->length; a += 60) {
            double lat, lon; geo_offset(r->lat1, r->lon1, brg, a, &lat, &lon);
            float h = lerpf(r->e1, r->e2, a / fmaxf(r->length, 1)) + 0.4f;
            for (int s = -1; s <= 1; s += 2) { double la, lo; geo_offset(lat, lon, brg + 90, s * (r->width * 0.5 + 1.5), &la, &lo); rw_add_light(m, &cap, la, lo, h, 0, 0, 0); }
        }
}
static void runways_update(double lat, double lon) {
    if (geo_dist(lat, lon, rwLat, rwLon) < 1500) return;
    rwLat = lat; rwLon = lon;
    int near[MAX_RWM], n = runways_near(lat, lon, 30000, near, MAX_RWM);
    int keep = 0;
    for (int i = 0; i < nRwm; i++) {
        bool still = false;
        for (int k = 0; k < n; k++) if (near[k] == rwm[i].idx) still = true;
        if (still) rwm[keep++] = rwm[i]; else rw_free(&rwm[i]);
    }
    for (int i = keep; i < nRwm; i++) memset(&rwm[i], 0, sizeof rwm[i]);
    nRwm = keep;
    for (int k = 0; k < n && nRwm < MAX_RWM; k++) {
        bool have = false;
        for (int i = 0; i < nRwm; i++) if (rwm[i].idx == near[k]) have = true;
        if (have || g_runways[near[k]].surface == 2 || g_runways[near[k]].length < 200) continue;
        rw_build(&rwm[nRwm++], near[k]);
    }
}
static void runway_lights(d3 cam, float t) {
    float night = 1 - lightLevel;
    for (int i = 0; i < nRwm; i++) {
        RwMesh *m = &rwm[i];
        for (int k = 0; k < m->nlights; k++) {
            RwLight *l = &m->lights[k];
            float r = 1, g = 0.9f, b = 0.7f, inten = 2.5f, size = 0.35f;
            if (l->type == 4) {                   /* PAPI: white above the unit's angle, red below */
                v3 d = d2f(dsub(cam, l->pos));
                float horiz = vdot(d, m->dirOut[l->end]);
                if (horiz < 50) continue;
                float vert = (float)(dlen(cam) - dlen(l->pos));
                float ang = atan2f(vert, horiz) / DEG;
                float edge = 2.5f + l->idx * (1.0f / 3.0f) + 0.0f;
                bool white = ang > edge;
                r = 1; g = white ? 0.95f : 0.06f; b = white ? 0.85f : 0.03f;
                inten = 4 + 2 * (1 - night); size = 0.9f;
                fx_light(l->pos, size, r, g, b, inten);
                continue;
            }
            if (night < 0.25f) continue;
            if (l->type == 1) {                   /* green seen from outside the runway, red from on it */
                bool outside = vdot(d2f(dsub(cam, m->thr[l->end])), m->dirOut[l->end]) > 0;
                if (outside) { r = 0.1f; g = 1; b = 0.3f; } else { r = 1; g = 0.08f; b = 0.05f; }
                inten = 3;
            } else if (l->type == 2) { inten = 2.8f; size = 0.45f; }
            else if (l->type == 3) {              /* the rabbit: strobes run towards the runway twice a second */
                float ph = fmodf(t * 2, 1) * 20;
                if (fabsf(ph - (19 - l->idx)) > 0.6f) continue;
                r = g = b = 1; inten = 10; size = 0.6f;
            }
            fx_light(l->pos, size, r, g, b, inten * smooth(0.25f, 0.6f, night));
        }
    }
}

/* ------------------------------------------------------------------ aircraft effects */
static int lastTouchdowns = -1;
static d3 lastTrail[4]; static bool haveTrail;
static void aircraft_fx(const Flight *fl, float dt, float t) {
    const AircraftDef *a = fl->def;
    v3 up = fl->up;
    if (lastTouchdowns < 0 || fl->touchdowns < lastTouchdowns) lastTouchdowns = fl->touchdowns;
    if (fl->touchdowns > lastTouchdowns) {
        lastTouchdowns = fl->touchdowns;
        if (fl->gs > 25)
            for (int g = 0; g < 2; g++)
                for (int k = 0; k < 10; k++) {
                    d3 p = dadd(fl->pos, f2d(q_rot(fl->rot, a->gear[g])));
                    fx_emit(p, vadd(vmul(fl->vel, 0.25f + 0.05f * frand()), vmul(up, 1.5f * frand())), 0.6f, 2.5f + frand() * 2, 2.0f + frand() * 1.5f, 0.75f, 0.74f, 0.72f, 0.55f);
                }
    }
    /* contrails in cold high air */
    if (a->jet && fl->alt > 8200 && !fl->crashed) {
        for (int e = 0; e < a->nengines; e++) {
            d3 p = dadd(fl->pos, f2d(q_rot(fl->rot, vadd(a->engine[e], V(0, 0, -6)))));
            if (haveTrail) {
                float d = (float)dlen(dsub(p, lastTrail[e]));
                int n = (int)(d / 3) + 1; if (n > 12) n = 12;
                for (int k = 0; k < n; k++) {
                    double s = (k + frand()) / n;
                    d3 q = dadd(lastTrail[e], dmul(dsub(p, lastTrail[e]), s));
                    fx_emit(q, V(0, 0, 0), 1.2f, 2.2f, 30, 1.0f, 1.0f, 1.0f, 0.22f * smooth(8200, 9500, (float)fl->alt));
                }
            }
            lastTrail[e] = p;
        }
        haveTrail = true;
    } else haveTrail = false;
    /* wreckage burns */
    if (fl->crashed) {
        for (int k = 0; k < 3; k++) {
            v3 o = V(frand() - 0.5f, frand() * 0.5f, frand() - 0.5f);
            d3 p = dadd(fl->pos, f2d(vmul(o, a->span * 0.3f)));
            fx_emit(p, vadd(vmul(up, 5 + 4 * frand()), V(frand() - 0.5f, frand() - 0.5f, frand() - 0.5f)), 2.5f, 3.5f, 14, 0.08f, 0.075f, 0.07f, 0.55f);
        }
        for (int k = 0; k < 6; k++) {
            v3 o = V(frand() - 0.5f, frand() * 0.3f, frand() - 0.5f);
            fx_light(dadd(fl->pos, f2d(vmul(o, a->span * 0.35f))), 2.5f + 2 * frand(), 1.0f, 0.42f, 0.1f, 2 + 3 * frand());
        }
        (void)dt;
        return;
    }
    /* navigation lights, beacon and strobes */
    m4 R = m4_trs(V(0, 0, 0), fl->rot, 1);
    float night = 1 - lightLevel;
    float navI = 1.2f + 2.5f * night;
    fx_light(dadd(fl->pos, f2d(m4_point(R, a->tipL))), 0.12f, 1.0f, 0.05f, 0.03f, navI);
    fx_light(dadd(fl->pos, f2d(m4_point(R, a->tipR))), 0.12f, 0.05f, 1.0f, 0.25f, navI);
    fx_light(dadd(fl->pos, f2d(m4_point(R, V(0, a->tail.y * 0.3f, a->bmin.z)))), 0.1f, 1, 1, 1, navI * 0.6f);
    float bp = fmodf(t, 1.1f);
    if (bp < 0.12f) fx_light(dadd(fl->pos, f2d(m4_point(R, V(0, a->bmax.y * 0.55f + 0.2f, 0)))), 0.16f, 1, 0.1f, 0.05f, 2 + 4 * night);
    float sp = fmodf(t, 1.3f);
    if (sp < 0.05f || (sp > 0.14f && sp < 0.18f)) {
        fx_light(dadd(fl->pos, f2d(m4_point(R, a->tipL))), 0.25f, 1, 1, 1, 3 + 6 * night);
        fx_light(dadd(fl->pos, f2d(m4_point(R, a->tipR))), 0.25f, 1, 1, 1, 3 + 6 * night);
    }
}

/* ------------------------------------------------------------------ frame */
static void frustum_planes(m4 vp, float out[6][4]) {
    const float *m = vp.m;
    for (int i = 0; i < 6; i++) {
        int row = i >> 1; float s = (i & 1) ? -1.0f : 1.0f;
        for (int k = 0; k < 4; k++) out[i][k] = m[k * 4 + 3] + s * m[k * 4 + row];
        float l = sqrtf(out[i][0] * out[i][0] + out[i][1] * out[i][1] + out[i][2] * out[i][2]);
        for (int k = 0; k < 4; k++) out[i][k] /= l;
    }
}
bool world_to_screen(d3 p, const Camera *cam, float *sx, float *sy) {
    v3 r = d2f(dsub(p, cam->pos));
    float *m = matVP.m;
    float x = m[0] * r.x + m[4] * r.y + m[8] * r.z + m[12], y = m[1] * r.x + m[5] * r.y + m[9] * r.z + m[13], w = m[3] * r.x + m[7] * r.y + m[11] * r.z + m[15];
    *sx = (x / fabsf(w) * 0.5f + 0.5f) * g_w; *sy = (1 - (y / fabsf(w) * 0.5f + 0.5f)) * g_h;
    return w > 0.01f;
}
static v3 E_, U_, N_, camF_;
static void set_common(GLuint p, v3 camPosF, float camAlt, v3 sunDir, v3 sunCol) {
    ui(p, "uSky", 0); ui(p, "uShadow", 1); ui(p, "uDetail", 3);
    u3(p, "uE", E_); u3(p, "uU", U_); u3(p, "uN", N_);
    u3(p, "uSunDir", sunDir); u3(p, "uSunCol", sunCol); u3(p, "uCamPosF", camPosF);
    u1(p, "uCamAlt", camAlt); u1(p, "uFC", uFCv); u1(p, "uHaze", wHaze);
    um(p, "uShadowM", matShadow); u1(p, "uShTexel", 1.0f / cfg.shadowRes);
    u1(p, "uBoost", 1.0f / exposure); u1(p, "uNight", 1 - lightLevel);
    um(p, "uVP", matVP);
}

void render_frame(const Camera *cam, const Flight *fl, float dt, bool drawAircraft) {
    timeAcc += dt;
    float t = (float)fmod(timeAcc, 10000.0);
    double clat, clon, calt;
    ecef_to_geo(cam->pos, &clat, &clon, &calt);
    enu_basis(clat, clon, &E_, &N_, &U_);
    v3 sunDir = d2f(sunW);
    v3 sunEnu = V(vdot(sunDir, E_), vdot(sunDir, U_), vdot(sunDir, N_));
    float sinE = sunEnu.y, alt = (float)calt;
    v3 sunCol = vmul(sun_transmittance(alt, sinE), 20.0f);
    float tolAlt = fmaxf(150.0f, alt * 0.08f);
    if (skyDirty || vdot(sunEnu, bakedSunEnu) < cosf(0.25f * DEG) || fabsf(alt - bakedAlt) > tolAlt) sky_bake(sunEnu, fmaxf(alt, 1));
    /* eye adaptation: aim mid-grey ground at a pleasant level for whatever light there is */
    float sunLum = 0.2126f * sunCol.x + 0.7152f * sunCol.y + 0.0722f * sunCol.z;
    float key = 0.18f / PI * sunLum * fmaxf(sinE, 0) + 0.18f * skyLum + 0.00012f;
    float target = clampf(0.2f / key, 0.05f, 28);
    exposure = dt > 0 && dt < 1 ? exposure * powf(target / exposure, 1 - expf(-dt * 1.5f)) : target;
    lightLevel = smooth(-0.08f, 0.16f, sinE);

    v3 F = vnorm(cam->fwd), R = vnorm(vcross(F, cam->up)), Up = vcross(R, F);
    camF_ = F;
    float aspect = (float)rw / rh, fov = cam->fov;
    m4 view = m4_look(V(0, 0, 0), F, Up), proj = m4_persp(fov, aspect, NEAR, FAR);
    matVP = m4_mul(proj, view);
    float fr[6][4]; frustum_planes(matVP, fr);
    terrain_update(cam->pos, (const float(*)[4])fr, tanf(fov * 0.5f), rh);
    runways_update(clat, clon);
    lastCam = cam->pos;
    fx_update(dt);
    if (fl) aircraft_fx(fl, dt, t);
    runway_lights(cam->pos, t);
    v3 camPosF = d2f(cam->pos);

    /* 1. aircraft shadow */
    m4 model = m4_ident();
    bool plane = drawAircraft && fl && fl->def;
    matShadow = m4_ortho(-1, 1, -1, 1, -1, 1);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, fboSh); glViewport(0, 0, cfg.shadowRes, cfg.shadowRes);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (fl && fl->def) {
        v3 rel = d2f(dsub(fl->pos, cam->pos));
        model = m4_trs(rel, fl->rot, 1);
        float half = 0.55f * fmaxf(fl->def->span, fl->def->length) + 2;
        float range = clampf(fmaxf(fl->agl, 0) / fmaxf(sinE, 0.1f) + 200, 200, 9000);
        v3 L = sunDir;
        m4 lv = m4_look(V(0, 0, 0), vmul(L, -1), fabsf(vdot(L, U_)) < 0.95f ? U_ : E_);
        v3 c = m4_point(lv, rel);
        matShadow = m4_mul(m4_ortho(c.x - half, c.x + half, c.y - half, c.y + half, -c.z - range, -c.z + range), lv);
        if (sinE > -0.02f) {
            glUseProgram(pShadow); um(pShadow, "uVP", matShadow);
            glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(1.5f, 2.0f);
            aircraft_draw(pShadow, fl->def, model, fl->propAngle, fl->gearDown, true);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    /* 2. scene */
    glBindFramebuffer(GL_FRAMEBUFFER, cfg.msaa > 1 ? fboMS : fboScene);
    glViewport(0, 0, rw, rh);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bind(0, GL_TEXTURE_CUBE_MAP, texSky); bind(1, GL_TEXTURE_2D, texSh); bind(3, GL_TEXTURE_2D, texDetail);
    /* sky behind everything */
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glUseProgram(pSky); set_common(pSky, camPosF, alt, sunDir, sunCol);
    u3(pSky, "uCamR", R); u3(pSky, "uCamUp", Up); u3(pSky, "uCamF", F); u3(pSky, "uSunEnu", sunEnu);
    u1(pSky, "uTan", tanf(fov * 0.5f)); u1(pSky, "uAspect", aspect); u1(pSky, "uTime", t);
    fs_pass();
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    /* terrain */
    glUseProgram(pTerrain); set_common(pTerrain, camPosF, alt, sunDir, sunCol);
    ui(pTerrain, "uImg", 2);
    d3 cm = D(fmod(cam->pos.x, 1000.0), fmod(cam->pos.y, 1000.0), fmod(cam->pos.z, 1000.0));
    u3(pTerrain, "uCamMod", d2f(cm));
    glActiveTexture(GL_TEXTURE2);
    terrain_draw(pTerrain, cam->pos);
    /* runways */
    glUseProgram(pRunway); set_common(pRunway, camPosF, alt, sunDir, sunCol);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < nRwm; i++) {
        RwMesh *m = &rwm[i];
        v3 off = d2f(dsub(m->center, cam->pos));
        if (vlen(off) > 60000) continue;
        u3(pRunway, "uOffset", off);
        u1(pRunway, "uLen", m->len); u1(pRunway, "uWid", m->wid); u1(pRunway, "uAlt", m->alt);
        u1(pRunway, "uSurface", (float)g_runways[m->idx].surface);
        glUniform4i(U(pRunway, "uIdent"), m->ident[0], m->ident[1], m->ident[2], m->ident[3]);
        glBindVertexArray(m->vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, m->nv);
    }
    /* aircraft */
    if (plane) {
        glUseProgram(pPlane); set_common(pPlane, camPosF, alt, sunDir, sunCol);
        ui(pPlane, "uTexColor", 4); ui(pPlane, "uTexNormal", 5); ui(pPlane, "uTexRough", 6);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        aircraft_draw(pPlane, fl->def, model, fl->propAngle, fl->gearDown, false);
    }
    /* smoke, contrails and lights */
    partN = 0;
    for (int i = 0; i < nPart; i++) {
        Particle *p = &part[i];
        float k = p->life / p->maxLife, a = p->c[3] * smooth(0, 0.3f, k) * smooth(1.0f, 0.92f, k);
        v3 c = d2f(dsub(p->pos, cam->pos));
        pq(c, vmul(R, p->size), vmul(Up, p->size), p->c[0], p->c[1], p->c[2], a);
    }
    float pix = tanf(fov * 0.5f) * 2 / rh;
    for (int i = 0; i < nLight; i++) {
        Light *l = &light[i];
        v3 c = d2f(dsub(l->pos, cam->pos));
        float d = vlen(c), minR = d * pix * 1.6f, s = fmaxf(l->size, minR);
        float inten = l->inten / exposure * clampf(powf(l->size / minR, 2), 0.07f, 1);   /* intensities are in display units */
        pq(c, vmul(R, s), vmul(Up, s), l->c[0], l->c[1], l->c[2], -inten);
    }
    nLight = 0;
    if (partN) {
        glUseProgram(pPart); set_common(pPart, camPosF, alt, sunDir, sunCol);
        glDepthMask(GL_FALSE);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(partVao); glBindBuffer(GL_ARRAY_BUFFER, partVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * partN, partBuf, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, partN);
        glDepthMask(GL_TRUE);
    }
    glDisable(GL_BLEND);
    if (cfg.msaa > 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboMS); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboScene);
        glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glDisable(GL_DEPTH_TEST);

    /* 3. clouds */
    bool clouds = cfg.clouds && wCover > 0.02f;
    if (clouds) {
        glBindFramebuffer(GL_FRAMEBUFFER, fboCloud); glViewport(0, 0, cw, ch);
        glUseProgram(pClouds); set_common(pClouds, camPosF, alt, sunDir, sunCol);
        bind(2, GL_TEXTURE_2D, texDepth); bind(5, GL_TEXTURE_3D, texNoise3);
        ui(pClouds, "uDepth", 2); ui(pClouds, "uNoise3", 5);
        u3(pClouds, "uCamR", R); u3(pClouds, "uCamUp", Up); u3(pClouds, "uCamF", F);
        d3 wm = D(fmod(cam->pos.x, 6552000.0), fmod(cam->pos.y, 6552000.0), fmod(cam->pos.z, 6552000.0));
        u3(pClouds, "uCamWorldMod", d2f(wm));
        u1(pClouds, "uTan", tanf(fov * 0.5f)); u1(pClouds, "uAspect", aspect); u1(pClouds, "uTime", t);
        u1(pClouds, "uCover", wCover); u1(pClouds, "uCloudBase", wBase); u1(pClouds, "uCloudTop", wTop);
        fs_pass();
    }

    /* 4. bloom */
    glUseProgram(pDown); ui(pDown, "uTex", 0);
    for (int i = 0; i < NBLOOM; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fboBloom[i]); glViewport(0, 0, bloomW[i], bloomH[i]);
        bind(0, GL_TEXTURE_2D, i == 0 ? texScene : texBloom[i - 1]);
        glUniform2f(U(pDown, "uTexel"), 1.0f / (i == 0 ? rw : bloomW[i - 1]), 1.0f / (i == 0 ? rh : bloomH[i - 1]));
        u1(pDown, "uFirst", i == 0); u1(pDown, "uThresh", 1.6f / exposure);
        fs_pass();
    }
    glUseProgram(pUp); ui(pUp, "uTex", 0);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    for (int i = NBLOOM - 1; i > 0; i--) {
        glBindFramebuffer(GL_FRAMEBUFFER, fboBloom[i - 1]); glViewport(0, 0, bloomW[i - 1], bloomH[i - 1]);
        bind(0, GL_TEXTURE_2D, texBloom[i]);
        glUniform2f(U(pUp, "uTexel"), 1.0f / bloomW[i], 1.0f / bloomH[i]);
        fs_pass();
    }
    glDisable(GL_BLEND);

    /* 5. composite */
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, g_w, g_h);
    glUseProgram(pComp);
    bind(0, GL_TEXTURE_2D, texScene); bind(1, GL_TEXTURE_2D, texDepth); bind(2, GL_TEXTURE_2D, texBloom[0]); bind(4, GL_TEXTURE_2D, texCloud);
    ui(pComp, "uScene", 0); ui(pComp, "uDepth", 1); ui(pComp, "uBloom", 2); ui(pComp, "uCloudTex", 4);
    glUniform2f(U(pComp, "uRes"), (float)g_w, (float)g_h);
    glUniform2f(U(pComp, "uCloudTexel"), 0.75f / cw, 0.75f / ch);
    float sx = 0, sy = 0, vis = 0;
    {
        v3 d = sunDir;
        float *m = matVP.m;
        float x = m[0] * d.x + m[4] * d.y + m[8] * d.z, y = m[1] * d.x + m[5] * d.y + m[9] * d.z, w = m[3] * d.x + m[7] * d.y + m[11] * d.z;
        if (w > 0.01f) {
            sx = x / w * 0.5f + 0.5f; sy = y / w * 0.5f + 0.5f;
            vis = smooth(1.25f, 0.9f, fmaxf(fabsf(sx - 0.5f), fabsf(sy - 0.5f)) * 2) * smooth(-0.01f, 0.05f, sinE);
        }
    }
    glUniform2f(U(pComp, "uSunScreen"), sx, sy); u1(pComp, "uSunVis", vis);
    u1(pComp, "uAspect", (float)g_w / g_h); u1(pComp, "uTime", t); u1(pComp, "uExposure", exposure); u1(pComp, "uClouds", clouds ? 1.0f : 0.0f);
    fs_pass();
}

bool render_screenshot(const char *path) {
    uint8_t *px = malloc((size_t)g_w * g_h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g_w, g_h, GL_RGB, GL_UNSIGNED_BYTE, px);
    stbi_flip_vertically_on_write(1);
    size_t n = strlen(path);
    int ok;
    if (n > 4 && !strcmp(path + n - 4, ".jpg")) ok = stbi_write_jpg(path, g_w, g_h, 3, px, 92);
    else ok = stbi_write_png(path, g_w, g_h, 3, px, g_w * 3);
    free(px);
    return ok != 0;
}
