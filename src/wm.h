/* WINGMERE - shared header: math, geodesy, and every module's interface.  MIT licence. */
#ifndef WM_H
#define WM_H
#define GL_GLEXT_PROTOTYPES 1
#include <SDL.h>
#include <SDL_opengl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WM_VERSION "0.3.0"
#define PI 3.14159265358979f
#define DEG (PI / 180.0f)
#define KT 0.514444f     /* metres/second per knot */
#define FT 0.3048f       /* metres per foot */

/* ------------------------------------------------------------------ float math */
typedef struct { float x, y, z; } v3;
typedef struct { float x, y, z, w; } quat;
typedef struct { float m[16]; } m4; /* column-major */

static inline v3 V(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 vadd(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 vsub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 vmul(v3 a, float s) { return V(a.x * s, a.y * s, a.z * s); }
static inline v3 vmad(v3 a, v3 b, float s) { return V(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline float vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 vcross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float vlen(v3 a) { return sqrtf(vdot(a, a)); }
static inline v3 vnorm(v3 a) { float l = vlen(a); return l > 1e-12f ? vmul(a, 1.0f / l) : V(0, 1, 0); }
static inline v3 vlerp(v3 a, v3 b, float t) { return vadd(a, vmul(vsub(b, a), t)); }
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static inline float signf(float x) { return x < 0 ? -1.0f : 1.0f; }

static inline quat q_ident(void) { quat q = {0, 0, 0, 1}; return q; }
static inline quat q_axis(v3 a, float ang) { float s = sinf(ang * 0.5f); quat q = {a.x * s, a.y * s, a.z * s, cosf(ang * 0.5f)}; return q; }
static inline quat q_mul(quat a, quat b) {
    quat q = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return q;
}
static inline quat q_norm(quat q) { float l = 1.0f / sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); quat r = {q.x * l, q.y * l, q.z * l, q.w * l}; return r; }
static inline quat q_conj(quat q) { quat r = {-q.x, -q.y, -q.z, q.w}; return r; }
static inline v3 q_rot(quat q, v3 v) { v3 u = V(q.x, q.y, q.z); v3 t = vmul(vcross(u, v), 2); return vadd(vadd(v, vmul(t, q.w)), vcross(u, t)); }
/* rotation whose +Z points along f and +Y as close to up as possible */
quat q_look(v3 f, v3 up);

m4 m4_ident(void);
m4 m4_mul(m4 a, m4 b);
m4 m4_persp(float fovy, float aspect, float zn, float zf);
m4 m4_ortho(float l, float r, float b, float t, float n, float f);
m4 m4_look(v3 eye, v3 fwd, v3 up);
m4 m4_trs(v3 pos, quat rot, float scale);
m4 m4_inv(m4 a);
v3 m4_point(m4 a, v3 p);

/* ------------------------------------------------------------------ double math (positions on Earth) */
typedef struct { double x, y, z; } d3;
static inline d3 D(double x, double y, double z) { d3 r = {x, y, z}; return r; }
static inline d3 dadd(d3 a, d3 b) { return D(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline d3 dsub(d3 a, d3 b) { return D(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline d3 dmul(d3 a, double s) { return D(a.x * s, a.y * s, a.z * s); }
static inline double ddot(d3 a, d3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline double dlen(d3 a) { return sqrt(ddot(a, a)); }
static inline d3 dnorm(d3 a) { double l = dlen(a); return l > 0 ? dmul(a, 1.0 / l) : D(0, 0, 1); }
static inline v3 d2f(d3 a) { return V((float)a.x, (float)a.y, (float)a.z); }
static inline d3 f2d(v3 a) { return D(a.x, a.y, a.z); }
static inline d3 dfmad(d3 a, v3 b, double s) { return D(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }

/* ------------------------------------------------------------------ geodesy (geo.c) - degrees, metres, WGS84 */
#define EARTH_R 6371008.8
d3   geo_to_ecef(double lat, double lon, double h);
void ecef_to_geo(d3 p, double *lat, double *lon, double *h);
void enu_basis(double lat, double lon, v3 *east, v3 *north, v3 *up);
double geo_dist(double lat1, double lon1, double lat2, double lon2);
double geo_bearing(double lat1, double lon1, double lat2, double lon2); /* degrees true */
void geo_offset(double lat, double lon, double bearing_deg, double dist, double *olat, double *olon);
double merc_x(double lon, int z);
double merc_y(double lat, int z);
double merc_lon(double x, int z);
double merc_lat(double y, int z);

/* ------------------------------------------------------------------ settings (main.c) */
typedef struct {
    char imagery_url[512], elevation_url[512], cache_dir[512], user_agent[128], vector_tilejson[512], vector_url[512];
    bool buildings; float pave;   /* how strongly to paint OSM pavement over the imagery */
    int imagery_max_zoom, elevation_max_zoom, threads, texture_budget;
    float detail; bool offline;
    char traffic_url[512]; bool traffic, labels;   /* live ADS-B traffic: {lat} {lon} {dist} in the URL */
} Settings;
extern Settings S;

/* ------------------------------------------------------------------ tile downloads (tiles.c) */
enum { LAYER_ELEV = 0, LAYER_IMG = 1, LAYER_VEC = 2 };
/* a decoded OpenStreetMap vector tile (mvt.c) */
typedef struct { int n; uint8_t *mask; uint8_t *water; int nw; float *bv; int nbv; float *bc; int nb; float *lights; int nl; float *park; int npark; } VecTile;
typedef struct { d3 ground; double lat, lon; float heading; uint32_t seed; } ParkSpot;   /* an airliner parked at a gate */
int terrain_parked(d3 cam, float maxDist, ParkSpot *out, int max);
VecTile *mvt_decode(const uint8_t *data, size_t len, int z, int x, int y);
void vectile_free(VecTile *v);
enum { TILE_OK = 1, TILE_MISSING = 2, TILE_FAILED = 3 };
typedef struct { int layer, z, x, y, status, w, h; unsigned char *rgba; float *height; VecTile *vec; } TileData;
void tiles_init(void);
void tiles_request(int layer, int z, int x, int y, float priority);
int  tiles_poll(TileData *out, int max);   /* finished tiles; caller frees rgba/height */
void tiles_frame(void);                    /* call once per frame */
void tiles_stats(int *queued, int *busy, int *net_ok, int *net_fail);
void tiles_shutdown(void);

/* ------------------------------------------------------------------ airports (airport.c) */
typedef struct { char le[6], he[6]; double lat1, lon1, lat2, lon2; float width, e1, e2, length, heading; int surface, lighted, airport; } Runway;
typedef struct { char ident[8], name[72], city[48], country[4]; double lat, lon; float elev; char type; int rw0, nrw; } Airport;
extern Airport *g_airports; extern int g_nairports;
extern Runway *g_runways; extern int g_nrunways;
bool airports_load(const char *path);
int  airport_find(const char *ident);
int  airport_nearest(double lat, double lon, double *dist);
int  runways_near(double lat, double lon, double radius, int *out, int max);
float runway_flatten(double lat, double lon, float h, bool *on_runway); /* height with runways levelled */
float runway_flatten_list(const int *list, int n, double lat, double lon, float h, bool *on_runway);

/* ------------------------------------------------------------------ terrain (terrain.c) */
void  terrain_init(void);
void  terrain_update(d3 cam, const float frustum[6][4], float tanHalfFov, int screenH);
void  terrain_draw(GLuint prog, d3 cam);
float terrain_height(double lat, double lon, bool *water, bool *runway);
void  terrain_stats(int *drawn, int *meshes, int *textures, int *elev);
bool  terrain_ready(double lat, double lon);
void  terrain_draw_map(GLuint prog, double lat, double lon, int zoom, float w, float h); /* 2D map overlay */
void  terrain_draw_city(GLuint prog, d3 cam, float maxDist, int what);   /* what: 0 buildings, 1 taxiway paint */
void  terrain_lights(d3 cam, float night);

/* ------------------------------------------------------------------ aircraft + flight (aircraft.c, flight.c) */
#define MAX_PARTS 160
typedef struct {
    char id[32], name[64], kind[96], sound[16];
    float mass, wing_area, chord, span, length, height;
    float cl0, cla, alpha_stall, clmax, cd0, k, thrust, prop_v;
    int flaps, retract, tricycle, jet;
    float flap_cl, flap_cd, gear_cd, cm_alpha, cm_de, cm_q, cl_da, cl_p, cl_beta, cn_beta, cn_dr, cn_r, ixx, iyy, izz;
    float v_rotate, v_cruise, vne, cam_dist;   /* vne: never-exceed indicated airspeed */
    v3 gear[3];
    /* model */
    int nparts; struct { GLuint vao, vbo, ibo; int nidx, mat, kind; v3 pivot; } part[MAX_PARTS];
    int nmats; struct { float rgba[4], metal, rough; int glass; GLuint tex[3]; } mat[64];
    v3 bmin, bmax, tipL, tipR, tail;   /* bounding box, wingtips (+x is left), top of the fin */
    int nengines; v3 engine[4];        /* propeller hubs / fan faces */
    float afterburner;                 /* total thrust with reheat, N (0 = none) */
    int nnozzle; v3 nozzle[4]; float nozzleR[4];
    int npilot; v3 pilot[4];           /* crew heads you can see in the cockpit */
    int pax;                           /* passenger seats */
} AircraftDef;
extern AircraftDef g_aircraft[16]; extern int g_naircraft;
void aircraft_load_all(const char *dir);
void aircraft_draw(GLuint prog, const AircraftDef *a, m4 model, float propAngle, float gearDown, bool shadowPass);

typedef struct {
    const AircraftDef *def;
    d3 pos; v3 vel; quat rot; v3 omega;            /* ECEF position, velocity, attitude, angular velocity (world frame) */
    v3 wind;                                        /* air mass velocity, world frame */
    float elev, ail, rud, throttle, trim, brake;    /* controls: -1..1, throttle 0..1 */
    int flapsSet; float flaps, gearDown, gearCmd;   /* flaps 0..1 (animated), gear 0 up .. 1 down */
    float propAngle, rpm, ab;                       /* ab: afterburner 0..1 */
    /* outputs */
    double lat, lon, alt; float agl, ias, gs, vs, heading, pitch, bank, aoa, gload, slip;
    bool onGround, stall, crashed; char crashWhy[48];
    float touchdownVs, groundSpeedSmooth, lastTouchdownVs; int touchdowns; bool contact[3];
    v3 up, east, north;
    int wheelsDown;
} Flight;
void flight_reset(Flight *f, const AircraftDef *a, double lat, double lon, double alt, float heading_deg, float speed, bool onGround);
void flight_step(Flight *f, float dt);
void flight_auto_trim(Flight *f);   /* set elevator trim for level flight at the current speed */
void flight_move_to(Flight *f, double lat, double lon, double alt, float turn_deg);   /* carry attitude and velocity to a new place */
void flight_cruise(Flight *f, float dt);   /* time warp: fly straight along the great circle */
void flight_trim(Flight *f, float gamma_deg);  /* attitude, trim and throttle for steady flight on a climb/descent angle */
float flight_stall_speed(const AircraftDef *a, float flaps); /* sea-level 1g stall speed, m/s */

/* ------------------------------------------------------------------ render, hud, audio */
typedef struct { int w, h, msaa, shadowRes; float scale; bool clouds; } RenderCfg;
typedef struct { d3 pos; v3 fwd, up; float fov; } Camera;
bool render_init(RenderCfg cfg);
void render_resize(int w, int h);
void render_frame(const Camera *cam, const Flight *fl, float dt, bool drawAircraft);
bool render_screenshot(const char *path);
void render_reload_shaders(void);
void render_weather(float cover, float base_m, float top_m, float haze); /* cover 0..1, cloud layer in metres MSL */
float render_light_level(void);   /* 0 night .. 1 full daylight at the camera */
bool world_to_screen(d3 p, const Camera *cam, float *sx, float *sy);
/* effects: smoke, spray and lights, positioned on the Earth */
void fx_emit(d3 pos, v3 vel, float size, float grow, float life, float r, float g, float b, float a);
void fx_light(d3 pos, float size, float r, float g, float b, float intensity);   /* lasts one frame */
void fx_clear(void);
/* how the player has customised the aircraft (hangar) */
typedef struct { float paint[3], amount; int finish; float smokeCol[3]; bool smokeOn; } Look;
extern Look g_look;
void render_set_time(double lat, double lon, float hours_local_solar, int day_of_year);
v3   render_sun_dir(void);
extern int g_w, g_h;
extern GLuint g_progHud, g_progMap;

void hud_init(void);
void hud_begin(void);
void hud_end(void);
void hud_text(float x, float y, float size, const char *s, float r, float g, float b, float a, int align);
void hud_rect(float x, float y, float w, float h, float r, float g, float b, float a);
void hud_line(float x0, float y0, float x1, float y1, float th, float r, float g, float b, float a);
float hud_text_width(const char *s, float size);
void hud_title(float x, float y, float size, const char *s, float r, float g, float b, float a, int align);
void hud_tri(float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a);
void hud_quad(const float p[8], float r, float g, float b, float a);
void hud_circle(float x, float y, float rad, float th, float r, float g, float b, float a);
void hud_disc(float x, float y, float rad, float r, float g, float b, float a);
void hud_clip(float x, float y, float w, float h);
void hud_clip_off(void);

void audio_init(void);
typedef struct { float rpm, throttle, airspeed, stall, onGround, groundSpeed, gearMoving, master, crash; int engine; /* 0 radial 1 turboprop 2 jet */ } AudioState;
extern AudioState g_audio;
void audio_event(int ev); /* 0 click, 1 touchdown, 2 crash, 3 ap off, 4 warning, 5 screams, 6 applause */


/* ------------------------------------------------------------------ live air traffic (traffic.c) */
#define MAX_TRAFFIC 400
typedef struct {
    char hex[8], callsign[12], reg[12], type[6];
    double lat, lon, alt;                       /* drawn position (metres above sea level) */
    double fixLat, fixLon, fixAlt; float fixT;  /* last report and the time since it */
    float gs, track, vrate, age, bank, turn, prevTrack;  /* m/s, degrees true, m/s */
    bool ground, fresh;
    const char *model; float scale;             /* which of our aircraft stands in for it */
} Traffic;
extern Traffic g_traffic[MAX_TRAFFIC];
extern int g_ntraffic;
void traffic_init(void);
void traffic_enable(bool on);
void traffic_update(double lat, double lon, float dt);
bool traffic_status(int *count);

#endif
