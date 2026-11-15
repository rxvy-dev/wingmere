/* WINGMERE - every sound is synthesised live; there are no audio files.  MIT licence.
 * Radial piston engines fire in uneven pops, turboprops whine over a blade-pass buzz, jets roar
 * with a fan tone on top; wind rises with airspeed, tyres rumble and squeal, the stall horn sounds. */
#include "wm.h"

#define SR 48000
#define TAU 6.2831853f
AudioState g_audio;
typedef struct { float t, amp; int kind; bool on; } Voice;
static Voice voice[16];
static SDL_AudioDeviceID dev;
static uint32_t nrng = 22222;
static float nz(void) { nrng ^= nrng << 13; nrng ^= nrng >> 17; nrng ^= nrng << 5; return (nrng >> 8) * (2.0f / 16777216.0f) - 1; }

void audio_event(int ev) {
    if (!dev) return;
    SDL_LockAudioDevice(dev);
    for (int i = 0; i < 16; i++) {
        if (voice[i].on) continue;
        voice[i].on = true; voice[i].t = 0; voice[i].kind = ev;
        voice[i].amp = ev == 0 ? 0.25f : (ev == 1 ? 0.9f : (ev == 2 ? 1.0f : 0.35f));
        break;
    }
    SDL_UnlockAudioDevice(dev);
}

/* one-pole filters */
typedef struct { float y; } LP;
static float lp(LP *f, float x, float cutoff) { float a = 1 - expf(-TAU * cutoff / SR); f->y += a * (x - f->y); return f->y; }
static LP lpRumble, lpWind, lpWind2, lpJet, lpJet2, lpRoll, lpProp, lpPop, lpGear, lpCrash;
static float phFire, phBlade, phFan, phWhine, phHorn, popEnv, popAmp = 1, phGear;
static float sRpm, sThr, sAir, sGround, sStall, sGear;

static float voice_sample(Voice *v) {
    float t = v->t, s = 0;
    switch (v->kind) {
    case 0: s = sinf(TAU * 1800 * t) * expf(-t * 60) * 0.5f; if (t > 0.05f) v->on = false; break;          /* UI click */
    case 1: {                                                                                                /* touchdown: thump + squeal */
        float thump = sinf(TAU * (60 - 30 * t) * t) * expf(-t * 18);
        float sq = sinf(TAU * (1250 + 80 * sinf(TAU * 23 * t)) * t) * expf(-t * 7) * 0.25f * fminf(1, sGround / 30);
        s = thump + sq + nz() * expf(-t * 12) * 0.3f;
        if (t > 0.9f) v->on = false;
        break;
    }
    case 2: s = lp(&lpCrash, nz(), 400 + 3000 * expf(-t * 3)) * expf(-t * 0.9f) * 3 + sinf(TAU * 38 * t) * expf(-t * 2); if (t > 4) v->on = false; break;
    case 3: {                                                                                                /* autopilot disconnect */
        float f = fmodf(t, 0.5f) < 0.25f ? 1046 : 784;
        s = (sinf(TAU * f * t) + 0.3f * sinf(TAU * f * 2 * t)) * 0.5f * (t < 1.5f);
        if (t > 1.5f) v->on = false;
        break;
    }
    default: s = (fmodf(t, 0.4f) < 0.2f) * sinf(TAU * 880 * t) * 0.6f; if (t > 1.2f) v->on = false; break;  /* warning chime */
    }
    v->t += 1.0f / SR;
    return s * v->amp;
}

static void mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    float *out = (float *)stream;
    int n = len / (int)(sizeof(float) * 2);
    AudioState a = g_audio;
    for (int i = 0; i < n; i++) {
        float k = 1.0f / (SR * 0.08f);      /* smooth parameter changes over ~80 ms */
        sRpm += (a.rpm - sRpm) * k; sThr += (a.throttle - sThr) * k; sAir += (a.airspeed - sAir) * k;
        sGround += (a.onGround * a.groundSpeed - sGround) * k; sStall += (a.stall - sStall) * k * 2; sGear += (a.gearMoving - sGear) * k;
        float eng = 0;
        if (a.crash < 0.5f) {
            if (a.engine == 0) {
                /* radial: 9 firings per crank turn (18 cylinders, four-stroke); each a short noisy pop */
                float ff = sRpm / 60 * 9;
                phFire += ff / SR;
                if (phFire >= 1) { phFire -= 1; popEnv = 1; popAmp = 0.7f + 0.6f * (nz() * 0.5f + 0.5f); }
                popEnv *= expf(-1.0f / (SR * 0.006f));
                float pop = lp(&lpPop, nz(), 900 + 1500 * sThr) * popEnv * popAmp;
                float body = sinf(TAU * phFire) * 0.35f + sinf(TAU * 2 * phFire) * 0.2f;
                float rumble = lp(&lpRumble, nz(), 140) * 2.5f;
                /* three-bladed propeller passing the cockpit */
                phBlade += sRpm / 60 * 0.6f * 3 / SR; if (phBlade > 1) phBlade -= 1;
                float blade = (phBlade < 0.5f ? 1.0f : -1.0f) * 0.12f;
                eng = (pop * 1.6f + body * 0.5f + rumble + lp(&lpProp, blade, 300)) * (0.25f + 0.75f * sThr) * fminf(sRpm / 600, 1);
            } else if (a.engine == 1) {
                /* turboprop: turbine whine, four-bladed propeller buzz, broadband */
                float n1 = sRpm / 2600;
                phWhine += (3200 + 2600 * n1) / SR; if (phWhine > 1) phWhine -= 1;
                phBlade += (sRpm / 60 * 0.42f * 4) / SR; if (phBlade > 1) phBlade -= 1;
                float buzz = lp(&lpProp, (phBlade * 2 - 1), 500) * 0.8f;
                float whine = sinf(TAU * phWhine) * 0.05f;
                float roar = lp(&lpJet, nz(), 600 + 1200 * sThr) * (0.8f + 1.2f * sThr);
                eng = (buzz * (0.3f + 0.7f * sThr) + whine + roar * 0.8f + lp(&lpRumble, nz(), 90) * 1.5f) * fminf(sRpm / 500, 1);
            } else {
                /* jet: N1 in percent drives a fan tone and a roar that opens up with thrust */
                float n1 = sRpm / 100;
                phFan += (900 + 2400 * n1) / SR; if (phFan > 1) phFan -= 1;
                float fan = (sinf(TAU * phFan) + 0.4f * sinf(TAU * 2.01f * phFan)) * 0.06f * (0.4f + n1);
                float roar = lp(&lpJet, nz(), 300 + 2500 * n1 * n1) * (0.3f + 2.5f * n1 * n1);
                float low = lp(&lpJet2, nz(), 70) * 3 * n1 * n1;
                eng = fan + roar + low;
            }
        }
        /* wind over the airframe */
        float sp = sAir / 80;
        float wind = (lp(&lpWind, nz(), 250 + 900 * fminf(sp, 2)) * 1.5f + lp(&lpWind2, nz(), 2500) * 0.15f * sp) * fminf(sp * sp, 3) * 0.6f;
        /* tyres on the ground */
        float roll = lp(&lpRoll, nz(), 60 + sGround * 2) * fminf(sGround / 25, 1.5f) * 2.0f;
        /* gear motor */
        phGear += 180.0f / SR; if (phGear > 1) phGear -= 1;
        float gear = lp(&lpGear, (phGear * 2 - 1) + nz() * 0.3f, 400) * 0.25f * sGear;
        /* stall horn */
        phHorn += 1000.0f / SR; if (phHorn > 1) phHorn -= 1;
        float horn = (phHorn < 0.5f ? 0.12f : -0.12f) * (sStall > 0.5f);
        float ev = 0;
        for (int v = 0; v < 16; v++) if (voice[v].on) ev += voice_sample(&voice[v]);
        float m = tanhf((eng * 0.35f + wind + roll + gear + horn + ev) * 0.7f * a.master);
        out[i * 2] = m; out[i * 2 + 1] = m;
    }
}

void audio_init(void) {
    SDL_AudioSpec want = {0}, have;
    want.freq = SR; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = mix;
    g_audio.master = 1;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    else fprintf(stderr, "wingmere: no audio device (%s), continuing silently\n", SDL_GetError());
}
