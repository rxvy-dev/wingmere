/* WINGMERE - 2D overlay: a built-in stroke font (no font files), shapes, clipping.  MIT licence. */
#include "wm.h"

/* Each glyph is a set of polylines on a 4x6 grid; a point is two digits "xy", y up. */
static const char *GLYPH[128] = {
    ['A'] = "0004264440 0242", ['B'] = "00063645443303 3342413000", ['C'] = "4536160501103041", ['D'] = "00062644422000",
    ['E'] = "40000646 0333", ['F'] = "000646 0333", ['G'] = "45361605011030414323", ['H'] = "0006 4046 0343",
    ['I'] = "1030 2026 1636", ['J'] = "0110304146", ['K'] = "0006 460340", ['L'] = "060040", ['M'] = "0006234640",
    ['N'] = "00064046", ['O'] = "100105163645413010", ['P'] = "00063645443303", ['Q'] = "100105163645413010 2240",
    ['R'] = "00063645443303 2340", ['S'] = "453616050413334241301001", ['T'] = "0646 2026", ['U'] = "060110304146",
    ['V'] = "062046", ['W'] = "0610233046", ['X'] = "0046 0640", ['Y'] = "062346 2320", ['Z'] = "06460040",
    ['0'] = "100105163645413010 1135", ['1'] = "152620 1030", ['2'] = "05163645440040", ['3'] = "05163645443313 334241301001",
    ['4'] = "30360242", ['5'] = "460603334241301001", ['6'] = "4536160501103041423303", ['7'] = "064610",
    ['8'] = "13040516364544331302011030414233", ['9'] = "0110304145361605041343",
    ['.'] = "2021", [':'] = "2122 2425", ['-'] = "1333", ['/'] = "0046", ['+'] = "0343 2125", ['!'] = "2226 2021", [','] = "2110",
    ['%'] = "0046 0516 3041", ['('] = "3615133130", [')'] = "1635331110", ['['] = "36161030", [']'] = "16363010", ['<'] = "450341",
    ['>'] = "054301", ['='] = "0242 0444", ['_'] = "0040", ['?'] = "0516364544332322 2021", ['\''] = "2524", ['"'] = "1514 3534",
    ['*'] = "0345 0541 2226", ['#'] = "1016 3036 0242 0444", ['|'] = "2026", ['^'] = "042644", ['&'] = "4011051636252301103143",
    ['@'] = "33221324433241302011050616363545", ['~'] = "03142433344445", ['$'] = "4536160504133342413010 2027",
};

static GLuint vao, vbo;
static float *buf; static int nv, cap;

void hud_init(void) {
    if (vao) return;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
}
static void vert(float x, float y, const float c[4]) {
    if (nv == cap) { cap = cap ? cap * 2 : 8192; buf = realloc(buf, sizeof(float) * 6 * cap); }
    float *v = &buf[nv++ * 6];
    v[0] = x; v[1] = y; v[2] = c[0]; v[3] = c[1]; v[4] = c[2]; v[5] = c[3];
}
static void quad4(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, const float c[4]) {
    vert(x0, y0, c); vert(x1, y1, c); vert(x2, y2, c); vert(x0, y0, c); vert(x2, y2, c); vert(x3, y3, c);
}
static void pm(float c[4], float r, float g, float b, float a) { c[0] = r * a; c[1] = g * a; c[2] = b * a; c[3] = a; }
static void seg(float x0, float y0, float x1, float y1, float th, const float c[4]) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) { dx = 1; dy = 0; l = 1; }
    dx /= l; dy /= l;
    float ex = dx * th * 0.5f, ey = dy * th * 0.5f, px = -dy * th * 0.5f, py = dx * th * 0.5f;
    quad4(x0 - ex + px, y0 - ey + py, x1 + ex + px, y1 + ey + py, x1 + ex - px, y1 + ey - py, x0 - ex - px, y0 - ey - py, c);
}

void hud_begin(void) { nv = 0; }
static void flush(void) {
    if (!nv) return;
    glUseProgram(g_progHud);
    glUniform2f(glGetUniformLocation(g_progHud, "uRes"), (float)g_w, (float)g_h);
    glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 6 * nv, buf, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv);
    glDisable(GL_BLEND);
    nv = 0;
}
void hud_end(void) { flush(); glDisable(GL_SCISSOR_TEST); }
void hud_clip(float x, float y, float w, float h) {
    flush();
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)x, (int)(g_h - y - h), (int)w, (int)h);
}
void hud_clip_off(void) { flush(); glDisable(GL_SCISSOR_TEST); }

void hud_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    float c[4]; pm(c, r, g, b, a);
    quad4(x, y, x + w, y, x + w, y + h, x, y + h, c);
}
void hud_line(float x0, float y0, float x1, float y1, float th, float r, float g, float b, float a) {
    float c[4]; pm(c, r, g, b, a);
    seg(x0, y0, x1, y1, th, c);
}
void hud_tri(float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
    float c[4]; pm(c, r, g, b, a);
    vert(x0, y0, c); vert(x1, y1, c); vert(x2, y2, c);
}
void hud_quad(const float p[8], float r, float g, float b, float a) {
    float c[4]; pm(c, r, g, b, a);
    quad4(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], c);
}
void hud_circle(float x, float y, float rad, float th, float r, float g, float b, float a) {
    float c[4]; pm(c, r, g, b, a);
    int n = rad > 60 ? 64 : 32;
    for (int i = 0; i < n; i++) {
        float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
        float ri = rad - th * 0.5f, ro = rad + th * 0.5f;
        quad4(x + cosf(a0) * ri, y + sinf(a0) * ri, x + cosf(a0) * ro, y + sinf(a0) * ro, x + cosf(a1) * ro, y + sinf(a1) * ro, x + cosf(a1) * ri, y + sinf(a1) * ri, c);
    }
}
void hud_disc(float x, float y, float rad, float r, float g, float b, float a) {
    float c[4]; pm(c, r, g, b, a);
    int n = rad > 60 ? 64 : 24;
    for (int i = 0; i < n; i++) {
        float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
        vert(x, y, c); vert(x + cosf(a0) * rad, y + sinf(a0) * rad, c); vert(x + cosf(a1) * rad, y + sinf(a1) * rad, c);
    }
}

static float adv_of(float size) { return size * (0.667f + 0.30f); }
float hud_text_width(const char *s, float size) { size_t n = strlen(s); return n ? n * adv_of(size) - size * 0.30f : 0; }
static void glyph_lines(float x, float y, float size, const char *s, int align, float th, const float c[4], float spike) {
    float adv = adv_of(size), u = size / 6.0f;
    if (align == 1) x -= hud_text_width(s, size) * 0.5f; else if (align == 2) x -= hud_text_width(s, size);
    float cx = x;
    for (const char *ch = s; *ch; ch++, cx += adv) {
        int k = (unsigned char)*ch & 127;
        if (k >= 'a' && k <= 'z') k -= 32;
        const char *gl = GLYPH[k];
        if (!gl) continue;
        float px = 0, py = 0; bool have = false;
        for (const char *q = gl;; ) {
            bool end = !*q || *q == ' ';
            if (end) {
                have = false;
                if (!*q) break;
                q++; continue;
            }
            float gx = cx + (q[0] - '0') * u, gy = y - (q[1] - '0') * u;
            if (have) {
                seg(px, py, gx, gy, th, c);
                if (spike > 0) {
                    /* a hairline that overshoots each stroke on both sides */
                    float dx = gx - px, dy = gy - py, l = sqrtf(dx * dx + dy * dy);
                    if (l > 1e-3f) {
                        dx /= l; dy /= l;
                        seg(px - dx * spike, py - dy * spike, px, py, th * 0.35f, c);
                        seg(gx, gy, gx + dx * spike, gy + dy * spike, th * 0.35f, c);
                    }
                }
            }
            px = gx; py = gy; have = true; q += 2;
        }
    }
}
/* align: 0 left, 1 centre, 2 right.  y is the baseline.  Text carries a dark outline so it reads on sky and snow. */
void hud_text(float x, float y, float size, const char *s, float r, float g, float b, float a, int align) {
    float c[4];
    pm(c, 0, 0, 0, a * 0.55f);
    glyph_lines(x, y, size, s, align, size * 0.26f, c, 0);
    pm(c, r, g, b, a);
    glyph_lines(x, y, size, s, align, size * 0.11f, c, 0);
}
/* black-metal title lettering: thick red strokes with hairline thorns */
void hud_title(float x, float y, float size, const char *s, float r, float g, float b, float a, int align) {
    float c[4];
    pm(c, 0, 0, 0, a * 0.7f);
    glyph_lines(x, y, size, s, align, size * 0.3f, c, size * 0.32f);
    pm(c, r, g, b, a);
    glyph_lines(x, y, size, s, align, size * 0.13f, c, size * 0.28f);
    pm(c, 1, 0.85f, 0.8f, a * 0.5f);
    glyph_lines(x, y, size, s, align, size * 0.035f, c, 0);
}
