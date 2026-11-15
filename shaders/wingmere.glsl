// WINGMERE - all shaders in one file.  MIT licence.
// Sections start with "//@ name"; the C side joins "common+terrain.frag" style lists and prepends
// "#version 330 core".  Run with WINGMERE_SHADERS=shaders/wingmere.glsl and press F5 to hot-reload.
// Everything 3D writes logarithmic depth so one frame can hold a cockpit and a 400 km horizon.

//@ common
uniform samplerCube uSky;
uniform vec3 uE, uU, uN, uSunDir, uSunCol, uCamPosF;
uniform float uCamAlt, uFC, uHaze, uBoost, uNight;   // uBoost = 1/exposure: lets lights be given in display units
const float PI = 3.14159265;
vec3 toEnu(vec3 d) { return vec3(dot(d, uE), dot(d, uU), dot(d, uN)); }
vec3 skyColor(vec3 dirWorld, float lod) { vec3 e = toEnu(dirWorld); e.y = max(e.y, -0.03); return textureLod(uSky, normalize(e), lod).rgb; }
// light lost and gained through an exponential atmosphere (scale height 1.4 km) between two altitudes
float fogAmount(float dist, float altA, float altB) {
    const float H = 1400.0;
    float k = (altB - altA) / H;
    float dA = exp(-clamp(altA, -200.0, 40000.0) / H);
    float integral = abs(k) > 1e-3 ? dA * (1.0 - exp(-k)) / k : dA;
    return 1.0 - exp(-dist * uHaze * integral);
}
vec3 applyFog(vec3 col, vec3 toFrag, float dist, float fragAlt) {
    float f = fogAmount(dist, uCamAlt, fragAlt);
    vec3 v = toFrag / max(dist, 1e-3);
    vec3 fogc = skyColor(normalize(v + uU * 0.03), 2.0) + uSunCol * 0.01 * pow(max(dot(v, uSunDir), 0.0), 8.0);
    return mix(col, fogc, f);
}
float D_GGX(float nh, float a) { float a2 = a * a; float d = nh * nh * (a2 - 1.0) + 1.0; return a2 / (PI * d * d + 1e-7); }
float V_Smith(float nv, float nl, float a) { float k = a * 0.5; return 0.25 / ((nv * (1.0 - k) + k) * (nl * (1.0 - k) + k) + 1e-5); }
vec3 F_Schlick(float c, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - c, 5.0); }
vec3 brdf(vec3 N, vec3 Vv, vec3 L, vec3 alb, float metal, float rough, vec3 f0) {
    vec3 H = normalize(Vv + L);
    float nl = max(dot(N, L), 0.0), nv = max(dot(N, Vv), 1e-3), nh = max(dot(N, H), 0.0), vh = max(dot(Vv, H), 0.0);
    float a = max(rough * rough, 0.002);
    vec3 F = F_Schlick(vh, f0);
    return ((1.0 - F) * (1.0 - metal) * alb / PI + F * D_GGX(nh, a) * V_Smith(nv, nl, a)) * nl;
}
uniform sampler3D uNoise3;          // r: Perlin-Worley, gba: Worley at three frequencies (tiling)
uniform float uCover, uCloudBase, uCloudTop;
uniform vec3 uCamWorldMod;
// where the clouds are: a slowly varying weather map, 0..1
float cloudCoverage(vec3 wp) {
    float c = texture(uNoise3, wp / 34000.0).r * 0.62 + texture(uNoise3, wp / 12000.0 + 0.37).r * 0.38;
    return smoothstep(1.0 - uCover - 0.12, 1.0 - uCover + 0.18, c);
}
// sunlight left after passing through the cloud layer above this point
float cloudShadow(vec3 rel, float alt) {
    float sy = dot(uSunDir, uU);
    if (uCover < 0.02 || sy < 0.03) return 1.0;
    float mid = mix(uCloudBase, uCloudTop, 0.3);
    if (alt > mid) return 1.0;
    vec3 wp = rel + uCamWorldMod + uSunDir * ((mid - alt) / sy);
    float c = cloudCoverage(wp);
    float n = texture(uNoise3, wp / 3200.0).r;
    return 1.0 - 0.72 * smoothstep(0.15, 0.6, c * (0.55 + 0.6 * n));
}
uniform sampler2DShadow uShadow;
uniform mat4 uShadowM;
uniform float uShTexel;
float planeShadow(vec3 rel, vec3 n) {
    vec4 s = uShadowM * vec4(rel + n * 0.05, 1.0);
    if (any(greaterThan(abs(s.xyz), vec3(0.98)))) return 1.0;
    vec3 c = s.xyz * 0.5 + 0.5;
    float t = uShTexel, r = 0.0;
    r += texture(uShadow, vec3(c.xy + vec2(-1.2, -0.6) * t, c.z - 0.0008));
    r += texture(uShadow, vec3(c.xy + vec2(1.1, -0.9) * t, c.z - 0.0008));
    r += texture(uShadow, vec3(c.xy + vec2(-0.2, 1.3) * t, c.z - 0.0008));
    r += texture(uShadow, vec3(c.xy + vec2(0.5, 0.3) * t, c.z - 0.0008));
    return r * 0.25;
}

//@ terrain.vert
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aH;
uniform mat4 uVP;
uniform vec3 uOffset;
uniform vec4 uImgST, uMaskST, uElevST;
out vec3 vRel, vN;
out vec2 vUV, vMUV, vEUV;
out float vH, vW;
void main() {
    vRel = aPos + uOffset;
    vN = aNrm; vUV = aUV * uImgST.xy + uImgST.zw; vMUV = aUV * uMaskST.xy + uMaskST.zw; vEUV = aUV * uElevST.xy + uElevST.zw; vH = aH;
    gl_Position = uVP * vec4(vRel, 1.0);
    vW = 1.0 + gl_Position.w;
}

//@ terrain.frag
in vec3 vRel, vN;
in vec2 vUV, vMUV, vEUV;
in float vH, vW;
uniform sampler2D uImg, uDetail, uMask, uElev;
uniform float uHasImg, uHasMask, uPave, uHasElev, uElevTexelM;
uniform vec3 uCamMod;
out vec4 fragColor;
void main() {
    float dist = length(vRel);
    vec3 N = normalize(vN);
    vec3 upL = normalize(uCamPosF + vRel);
    if (uHasElev > 0.5) {
        // per-pixel slope from the elevation data: ridges and gullies sharper than the mesh can carry
        float t = 1.0 / 256.0;
        float hc = texture(uElev, vEUV).r;
        float he = texture(uElev, vEUV + vec2(t, 0.0)).r - texture(uElev, vEUV - vec2(t, 0.0)).r;
        float hn = texture(uElev, vEUV - vec2(0.0, t)).r - texture(uElev, vEUV + vec2(0.0, t)).r;
        vec3 E = normalize(cross(vec3(0.0, 0.0, 1.0), upL)), Nn = cross(upL, E);
        vec3 nd = normalize(upL - E * he / (2.0 * uElevTexelM) - Nn * hn / (2.0 * uElevTexelM));
        float keep = smoothstep(1.5, 5.0, abs(max(hc, 0.0) - vH));      // levelled runways keep the mesh normal
        if (hc > 0.5 && !any(isnan(nd))) N = normalize(mix(nd, N, keep));
    }
    vec3 alb;
    if (uHasImg > 0.5) alb = texture(uImg, vUV).rgb;
    else {
        alb = mix(vec3(0.10, 0.16, 0.07), vec3(0.32, 0.29, 0.24), smoothstep(200.0, 2200.0, vH));
        alb = mix(alb, vec3(0.8), smoothstep(2800.0, 3600.0, vH));
        if (vH < 0.3) alb = vec3(0.02, 0.05, 0.09);
    }
    // break up the softness of 10 m satellite pixels when close
    vec3 wp = vRel + uCamMod;
    vec3 aw0 = abs(upL);
    vec2 q = aw0.x > aw0.y && aw0.x > aw0.z ? wp.yz : (aw0.y > aw0.z ? wp.xz : wp.xy);   // world-locked, so the grain never swims
    float dfade = 1.0 - smoothstep(400.0, 5000.0, dist);
    if (dfade > 0.0) {
        float n1 = texture(uDetail, q / 125.0).r, n2 = texture(uDetail, q / 15.625).r;
        alb *= mix(1.0, 0.72 + 0.56 * n1, 0.55 * dfade) * mix(1.0, 0.8 + 0.4 * n2, 0.45 * dfade * (1.0 - smoothstep(60.0, 600.0, dist)));
    }
    // water: sea level, or dark blue-green flat imagery
    float lum = dot(alb, vec3(0.3, 0.6, 0.1));
    float water;
    vec4 mk = vec4(0.0, 0.0, 1.0, 0.0);
    if (uHasMask > 0.5) {
        // OpenStreetMap: exact coastlines, lakes and rivers; aprons and taxiways; roads
        mk = texture(uMask, vMUV);
        water = smoothstep(0.3, 0.7, mk.r);
        float pave = mk.g * uPave * (1.0 - water);
        if (pave > 0.0) {
            float g1 = texture(uDetail, q / 62.5).r, g2 = texture(uDetail, q / 7.8125).r;
            vec3 conc = vec3(0.16, 0.155, 0.145) * (0.8 + 0.4 * g1) * (0.92 + 0.16 * g2);
            alb = mix(alb, conc, pave);
        }
    } else {
        float lake = step(alb.r * 1.15, alb.b) * step(lum, 0.06) * step(0.97, dot(N, upL));
        water = max(step(vH, 0.3), lake * 0.85);
    }
    vec3 L = uSunDir, Vv = -vRel / max(dist, 1e-3);
    float ndl = max(dot(N, L), 0.0), fdl = max(dot(upL, L), 0.0);
    float relief = clamp((ndl + 0.04) / (fdl + 0.04), 0.25, 1.9);   // imagery already holds the flat-ground shading
    float sh = planeShadow(vRel, upL) * cloudShadow(vRel, vH);
    vec3 col = alb / PI * uSunCol * fdl * relief * sh + alb * textureLod(uSky, normalize(toEnu(upL)), 7.0).rgb;
    if (water > 0.0) {
        vec3 Rw = reflect(-Vv, upL);
        float fres = 0.02 + 0.98 * pow(1.0 - max(dot(Vv, upL), 0.0), 5.0);
        vec3 refl = skyColor(Rw, 1.0) * fres;
        vec3 H = normalize(L + Vv);
        float glint = D_GGX(max(dot(upL, H), 0.0), 0.12) * 0.25 * fdl * sh;
        col = mix(col, col * 0.6 + refl + uSunCol * glint * fres * 4.0, water * 0.85);
    }
    // at night the roads glow with street lights...
    if (uNight > 0.05 && uHasMask > 0.5) {
        float sl = smoothstep(0.62, 0.9, texture(uDetail, q / 31.25).g) * 0.7 + 0.3;
        col += vec3(1.0, 0.6, 0.28) * mk.a * sl * uNight * uBoost * 0.32 * (1.0 - fogAmount(dist, uCamAlt, vH)) * (1.0 - water);
        col += vec3(0.9, 0.85, 0.7) * mk.g * uNight * uBoost * 0.05 * (1.0 - water);   // floodlit aprons
    }
    // ...and without map data, grey built-up areas in the imagery do
    if (uNight > 0.05 && uHasImg > 0.5 && water < 0.5 && uHasMask < 0.5) {
        float mx = max(alb.r, max(alb.g, alb.b)), mn = min(alb.r, min(alb.g, alb.b));
        float urban = smoothstep(0.045, 0.012, mx - mn) * smoothstep(0.035, 0.09, lum) * smoothstep(0.45, 0.3, lum);
        vec3 aw = abs(upL);
        vec2 sq = aw.x > aw.y && aw.x > aw.z ? wp.yz : (aw.y > aw.z ? wp.xz : wp.xy);
        float speck = smoothstep(0.62, 0.9, texture(uDetail, sq / 62.5).g) + 0.35 * smoothstep(0.4, 0.8, texture(uDetail, sq / 1000.0).r);
        vec3 lights = vec3(1.0, 0.58, 0.26) * urban * speck * uNight * uBoost * 0.55;
        col += lights * (1.0 - fogAmount(dist, uCamAlt, vH));
    }
    col = applyFog(col, vRel, dist, vH);
    fragColor = vec4(col, 1.0);
    gl_FragDepth = log2(vW) * uFC;
}

//@ runway.vert
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aRw;       // metres along / across
uniform mat4 uVP;
uniform vec3 uOffset;
out vec3 vRel;
out vec2 vRw;
out float vW;
void main() {
    vRel = aPos + uOffset; vRw = aRw;
    gl_Position = uVP * vec4(vRel, 1.0);
    vW = 1.0 + gl_Position.w;
}

//@ runway.frag
in vec3 vRel;
in vec2 vRw;
in float vW;
uniform float uLen, uWid, uAlt, uSurface;
uniform ivec4 uIdent;                  // number and letter (0 none, 1 L, 2 R, 3 C) at each end
uniform sampler2D uDetail;
out vec4 fragColor;
float box(vec2 p, vec2 a, vec2 b) { return step(a.x, p.x) * step(p.x, b.x) * step(a.y, p.y) * step(p.y, b.y); }
// seven-segment glyphs painted on the tarmac, 18 m tall; p in metres from the glyph's lower-left
float seg(int d, vec2 p) {
    int m[10] = int[10](63, 6, 91, 79, 102, 109, 125, 7, 127, 111);
    int bits = d >= 0 && d < 10 ? m[d] : (d == 11 ? 56 : (d == 12 ? 115 : (d == 13 ? 57 : 0)));   // 11 L, 12 R, 13 C
    const float W = 6.0, H = 18.0, T = 1.5;
    float r = 0.0;
    if ((bits & 1) != 0) r += box(p, vec2(0, H - T), vec2(W, H));
    if ((bits & 2) != 0) r += box(p, vec2(W - T, H * 0.5), vec2(W, H));
    if ((bits & 4) != 0) r += box(p, vec2(W - T, 0), vec2(W, H * 0.5));
    if ((bits & 8) != 0) r += box(p, vec2(0, 0), vec2(W, T));
    if ((bits & 16) != 0) r += box(p, vec2(0, 0), vec2(T, H * 0.5));
    if ((bits & 32) != 0) r += box(p, vec2(0, H * 0.5), vec2(T, H));
    if ((bits & 64) != 0) r += box(p, vec2(0, H * 0.5 - T * 0.5), vec2(W, H * 0.5 + T * 0.5));
    return min(r, 1.0);
}
float designator(vec2 p, int num, int letter) {      // p: across (x) and along (y) from the glyph block's centre-bottom
    float r = 0.0;
    int tens = num / 10, ones = num - tens * 10;
    if (tens > 0) { r += seg(tens, p - vec2(-7.5, 0)); r += seg(ones, p - vec2(1.5, 0)); }
    else r += seg(ones, p - vec2(-3.0, 0));
    if (letter > 0) r += seg(10 + letter, p - vec2(-3.0, -22.0));
    return min(r, 1.0);
}
void main() {
    float along = vRw.x, across = vRw.y, aw = abs(across), hw = uWid * 0.5;
    float dist = length(vRel);
    vec3 upL = normalize(uCamPosF + vRel);
    float n = texture(uDetail, vRw * vec2(0.02, 0.05)).r, n2 = texture(uDetail, vRw * 0.37).r;
    vec3 alb = uSurface > 0.5 ? mix(vec3(0.10, 0.14, 0.06), vec3(0.17, 0.15, 0.09), n) : vec3(0.052) * (0.75 + 0.5 * n) * (0.9 + 0.2 * n2);
    float paint = 0.0;
    if (uSurface < 0.5) {
        // tyre marks in the touchdown zones
        float tz = smoothstep(150.0, 400.0, along) * (1.0 - smoothstep(700.0, 1100.0, along)) + smoothstep(150.0, 400.0, uLen - along) * (1.0 - smoothstep(700.0, 1100.0, uLen - along));
        alb *= 1.0 - 0.45 * tz * smoothstep(0.35, 0.8, n2) * (1.0 - smoothstep(4.0, 8.0, aw));
        float fw = clamp(dist * 0.0006, 0.0, 0.4);                     // fatten lines far away so they do not shimmer
        paint += box(vec2(aw, along), vec2(hw - 1.6 - fw, 0.0), vec2(hw - 0.7 + fw, uLen));   // edge lines
        float dash = mod(along - 60.0, 60.0);
        paint += step(aw, 0.45 + fw) * step(dash, 36.0) * step(60.0, along) * step(along, uLen - 60.0);   // centreline
        for (int e = 0; e < 2; e++) {
            float a = e == 0 ? along : uLen - along;
            float xx = e == 0 ? across : -across;
            if (a > 6.0 && a < 36.0) paint += step(mod(aw + 0.9, 3.6), 1.8) * step(aw, hw - 3.0) * step(3.0, aw);   // threshold piano keys
            paint += box(vec2(aw, a), vec2(hw * 0.25, 300.0), vec2(hw * 0.25 + 10.0, 345.0));                // aiming point
            for (int k = 1; k <= 5; k++) { float s = 150.0 * float(k); if (k != 2) paint += box(vec2(aw, a), vec2(hw * 0.25 + 1.0, s), vec2(hw * 0.25 + 4.0, s + 22.5)); }
            int num = e == 0 ? uIdent.x : uIdent.z, let = e == 0 ? uIdent.y : uIdent.w;
            paint += designator(vec2(xx, a - 70.0), num, let);
            paint += box(vec2(aw, a), vec2(0.0, 1.0), vec2(hw, 2.2));   // threshold bar
        }
        paint = min(paint, 1.0);
        alb = mix(alb, vec3(0.75), paint * (0.8 + 0.2 * n2));
    }
    float shoulder = smoothstep(hw - 0.3, hw + 0.2, aw);
    float fdl = max(dot(upL, uSunDir), 0.0);
    float sh = planeShadow(vRel, upL) * cloudShadow(vRel, uAlt);
    vec3 Vv = -vRel / max(dist, 1e-3), H = normalize(uSunDir + Vv);
    float spec = D_GGX(max(dot(upL, H), 0.0), 0.35) * 0.04 * (1.0 - paint);
    vec3 col = alb / PI * uSunCol * fdl * sh + alb * textureLod(uSky, normalize(toEnu(upL)), 7.0).rgb + uSunCol * spec * fdl * sh;
    col = applyFog(col, vRel, dist, uAlt);
    fragColor = vec4(col, 1.0 - shoulder);
    gl_FragDepth = log2(vW) * uFC - 2e-6;
}

//@ plane.vert
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel, uVP;
out vec3 vRel, vN;
out vec2 vUV;
out float vW;
void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vRel = w.xyz; vN = mat3(uModel) * aNrm; vUV = aUV;
    gl_Position = uVP * w;
    vW = 1.0 + gl_Position.w;
}

//@ plane.frag
in vec3 vRel, vN;
in vec2 vUV;
in float vW;
uniform sampler2D uTexColor, uTexNormal, uTexRough;
uniform vec4 uColor;
uniform vec3 uMatParams, uHasTex;     // metal, rough, glass / has colour, normal, rough maps
uniform vec4 uPaint;                  // hangar paint colour + amount
uniform vec3 uFinish;                 // metal, rough, enabled
uniform float uPaintMask;
out vec4 fragColor;
void main() {
    vec3 Vv = normalize(-vRel);
    vec3 Ng = dot(vN, vN) > 1e-12 ? normalize(vN) : Vv;   // some models carry zero-length normals
    if (dot(Ng, Vv) < 0.0) Ng = -Ng;
    vec4 base = uColor;
    if (uHasTex.x > 0.5) base *= texture(uTexColor, vUV);
    float rough = uMatParams.y, metal = uMatParams.x;
    if (uHasTex.z > 0.5) rough = clamp(texture(uTexRough, vUV).r, 0.04, 1.0);
    if (uPaintMask > 0.5 && uMatParams.z < 0.5) {
        if (uPaint.a > 0.0) {   // repaint, keeping the panel lines, decals and dirt of the original
            float l = dot(base.rgb, vec3(0.3, 0.59, 0.11));
            vec3 pc = uPaint.rgb * (0.3 + 1.4 * l);
            base.rgb = mix(base.rgb, pc, uPaint.a * smoothstep(0.015, 0.08, l));
        }
        if (uFinish.z > 0.5) { metal = uFinish.x; rough = uFinish.y; }
    }
    vec3 N = Ng;
    if (uHasTex.y > 0.5) {               // tangent frame from screen-space derivatives: no tangents needed
        vec3 dp1 = dFdx(vRel), dp2 = dFdy(vRel);
        vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
        vec3 dp2p = cross(dp2, Ng), dp1p = cross(Ng, dp1);
        vec3 T = dp2p * du1.x + dp1p * du2.x, B = dp2p * du1.y + dp1p * du2.y;
        float inv = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-20);
        vec3 tn = texture(uTexNormal, vUV).xyz * 2.0 - 1.0;
        vec3 nn = normalize(T * inv * tn.x + B * inv * tn.y + Ng * tn.z);
        if (!any(isnan(nn))) N = nn;
    }
    vec3 upL = normalize(uCamPosF + vRel);
    vec3 f0 = mix(vec3(0.04), base.rgb, metal);
    float sh = planeShadow(vRel, Ng) * cloudShadow(vRel, uCamAlt + dot(vRel, uU));
    vec3 col = brdf(N, Vv, uSunDir, base.rgb, metal, rough, f0) * uSunCol * sh;
    vec3 R = reflect(-Vv, N);
    vec3 env = skyColor(R, rough * 6.0);
    float horizon = smoothstep(-0.25, 0.05, dot(R, upL));       // below the horizon reflections see the ground, not the sky
    env = mix(env * 0.18, env, horizon);
    float nv = max(dot(N, Vv), 0.0);
    vec3 Fr = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - nv, 5.0);
    vec3 irr = textureLod(uSky, normalize(toEnu(N)), 7.0).rgb * (0.55 + 0.45 * dot(N, upL));
    col += irr * base.rgb * (1.0 - metal) + env * Fr;
    float alpha = 1.0;
    if (uMatParams.z > 0.5) { alpha = clamp(base.a + Fr.r * 0.7, 0.25, 0.95); col = env * Fr * 1.2 + base.rgb * irr * 0.3; }
    col = applyFog(col, vRel, length(vRel), uCamAlt + dot(vRel, uU));
    if (any(isnan(col)) || any(isinf(col))) col = vec3(0.0);
    col = min(col, vec3(60000.0));
    fragColor = vec4(col * alpha, alpha);
    gl_FragDepth = log2(vW) * uFC;
}

//@ flame.vert
// afterburner plume: a cone of rings behind the nozzle, along -Z of the nozzle frame
layout(location = 0) in vec2 aTA;          // t along the plume 0..1, angle round it
uniform mat4 uModel, uVP;
uniform float uLen, uRad, uTime, uShape;
out float vT, vA, vW;
out vec3 vRel, vN;
void main() {
    float t = aTA.x, a = aTA.y;
    float r = uRad * mix(1.0 - 0.8 * t * t, 1.0 - 0.95 * t, uShape) * (1.0 + 0.06 * sin(t * 37.0 - uTime * 41.0));
    vec3 p = vec3(cos(a) * r, sin(a) * r, -t * uLen);
    vec4 w = uModel * vec4(p, 1.0);
    vRel = w.xyz; vN = mat3(uModel) * vec3(cos(a), sin(a), 0.25);
    vT = t; vA = a;
    gl_Position = uVP * w;
    vW = 1.0 + gl_Position.w;
}

//@ flame.frag
in float vT, vA, vW;
in vec3 vRel, vN;
uniform float uTime, uPower;
uniform vec3 uCore, uTail;
out vec4 fragColor;
void main() {
    vec3 V = normalize(-vRel);
    float facing = abs(dot(normalize(vN), V));            // the middle of the plume reads as thicker
    float t = vT;
    float diamonds = 0.5 + 0.5 * cos(t * 6.2832 * 4.5 - 0.6);   // shock diamonds
    float flick = 0.82 + 0.18 * sin(uTime * 53.0 + t * 23.0) * sin(uTime * 37.0 + vA * 3.0);
    float fade = smoothstep(1.0, 0.2, t) * smoothstep(0.0, 0.04, t);
    vec3 col = mix(uCore, uTail, smoothstep(0.0, 0.75, t)) * mix(1.0, 0.35 + diamonds, 0.7 * (1.0 - t));
    float a = pow(facing, 1.3) * fade * flick * uPower;
    fragColor = vec4(col * a * uBoost, 0.0);
    gl_FragDepth = log2(vW) * uFC - 1e-6;
}

//@ building.vert
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;      // walls: metres along / up; roofs: metres east / north
layout(location = 3) in vec2 aKind;    // 0 wall, 1 roof, 2 paint; a per-building random number
uniform vec3 uOffset;
uniform mat4 uVP;
out vec3 vRel, vN;
out vec2 vUV, vKind;
out float vW;
void main() {
    vRel = aPos + uOffset; vN = aNrm; vUV = aUV; vKind = aKind;
    gl_Position = uVP * vec4(vRel, 1.0);
    vW = 1.0 + gl_Position.w;
}

//@ building.frag
in vec3 vRel, vN;
in vec2 vUV, vKind;
in float vW;
uniform sampler2D uDetail;
out vec4 fragColor;
float h1(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
    float dist = length(vRel);
    vec3 N = normalize(vN), Vv = -vRel / max(dist, 1e-3);
    vec3 upL = normalize(uCamPosF + vRel);
    float hash = vKind.y;
    vec3 alb; float glass = 0.0, lit = 0.0;
    if (vKind.x > 1.5) {                                   // taxiway centreline paint
        alb = vec3(0.62, 0.46, 0.05);
    } else if (vKind.x > 0.5) {                            // roof
        float n = texture(uDetail, vUV / 37.0).r;
        alb = mix(vec3(0.21, 0.2, 0.19), vec3(0.35, 0.3, 0.27), step(0.6, hash)) * (0.8 + 0.4 * n);
    } else {                                               // facade: a palette, floors and windows
        vec3 pal[6] = vec3[6](vec3(0.42, 0.4, 0.37), vec3(0.36, 0.22, 0.17), vec3(0.55, 0.53, 0.5), vec3(0.2, 0.24, 0.28), vec3(0.47, 0.42, 0.33), vec3(0.3, 0.31, 0.32));
        alb = pal[int(hash * 5.99)];
        float fl = vUV.y / 3.4, bay = vUV.x / 3.1;
        vec2 cell = fract(vec2(bay, fl));
        float win = step(0.16, cell.x) * step(cell.x, 0.84) * step(0.28, cell.y) * step(cell.y, 0.86) * step(0.8, vUV.y);
        float glassy = step(0.8, hash);                    // a fifth of buildings are glass towers
        glass = max(win, glassy * step(0.0, vUV.y) * 0.85);
        float wf = 1.0 - smoothstep(300.0, 2500.0, dist);  // windows melt into the wall far away
        glass *= mix(0.45, 1.0, wf);
        alb = mix(alb, vec3(0.05, 0.06, 0.07), glass * 0.85);
        lit = step(0.55, h1(floor(vec2(bay, fl)) + hash * 91.0)) * win;
    }
    float fdl = max(dot(upL, uSunDir), 0.0);
    float ndl = max(dot(N, uSunDir), 0.0);
    vec3 col = alb / PI * uSunCol * ndl * cloudShadow(vRel, uCamAlt + dot(vRel, uU)) + alb * textureLod(uSky, normalize(toEnu(N)), 7.0).rgb * (0.65 + 0.35 * dot(N, upL));
    if (glass > 0.0) {
        vec3 R = reflect(-Vv, N);
        float fres = 0.04 + 0.96 * pow(1.0 - max(dot(Vv, N), 0.0), 5.0);
        col += skyColor(R, 1.5) * fres * glass * 0.8;
    }
    col += vec3(1.0, 0.78, 0.45) * lit * uNight * uBoost * 0.55;
    col = applyFog(col, vRel, dist, uCamAlt + dot(vRel, uU));
    fragColor = vec4(col, 1.0);
    gl_FragDepth = log2(vW) * uFC - (vKind.x > 1.5 ? 3e-6 : 0.0);
}

//@ shadow.vert
layout(location = 0) in vec3 aPos;
uniform mat4 uModel, uVP;
void main() { gl_Position = uVP * uModel * vec4(aPos, 1.0); }

//@ shadow.frag
void main() {}

//@ fs.vert
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

//@ skybake.frag
// one face of the sky cubemap, in a local east/up/north frame at the camera's altitude
in vec2 vUV;
uniform vec3 uFaceR, uFaceU, uFaceF, uSunEnu;
uniform float uAltitude;
out vec4 fragColor;
const float PI = 3.14159265;
vec2 raySphere(vec3 r0, vec3 rd, float sr) {
    float b = dot(rd, r0), c = dot(r0, r0) - sr * sr, d = b * b - c;
    if (d < 0.0) return vec2(1e9, -1e9);
    d = sqrt(d);
    return vec2(-b - d, -b + d);
}
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    vec3 r = normalize(uFaceF + p.x * uFaceR + p.y * uFaceU);
    const float rP = 6371e3, rA = 6471e3;
    const vec3 kR = vec3(5.5e-6, 13.0e-6, 22.4e-6);
    const float kM = 21e-6, hR = 8.0e3, hM = 1.2e3, g = 0.76;
    vec3 r0 = vec3(0.0, rP + max(uAltitude, 1.0), 0.0);
    vec2 pa = raySphere(r0, r, rA);
    float t0 = max(pa.x, 0.0), tEnd = pa.y;
    vec2 pp = raySphere(r0, r, rP);
    bool ground = pp.x > 0.0 && pp.x < tEnd;
    if (ground) tEnd = pp.x;
    vec3 sun = normalize(uSunEnu);
    const int NI = 24, NJ = 6;
    float ds = (tEnd - t0) / float(NI), t = t0, odR = 0.0, odM = 0.0;
    vec3 totR = vec3(0.0), totM = vec3(0.0);
    for (int i = 0; i < NI; i++) {
        vec3 pos = r0 + r * (t + ds * 0.5);
        float h = length(pos) - rP;
        float dR = exp(-h / hR) * ds, dM = exp(-h / hM) * ds;
        odR += dR; odM += dM;
        float js = raySphere(pos, sun, rA).y / float(NJ), jt = 0.0, jR = 0.0, jM = 0.0;
        bool lit = raySphere(pos, sun, rP).x < 0.0 || raySphere(pos, sun, rP).y < 0.0;
        for (int j = 0; j < NJ; j++) {
            vec3 jp = pos + sun * (jt + js * 0.5);
            float jh = max(length(jp) - rP, 0.0);
            jR += exp(-jh / hR) * js; jM += exp(-jh / hM) * js;
            jt += js;
        }
        vec3 att = exp(-(kM * (odM + jM) + kR * (odR + jR)));
        if (lit) { totR += dR * att; totM += dM * att; }
        t += ds;
    }
    float mu = dot(r, sun), mumu = mu * mu, gg = g * g;
    float pR = 3.0 / (16.0 * PI) * (1.0 + mumu);
    float pM = 3.0 / (8.0 * PI) * ((1.0 - gg) * (mumu + 1.0)) / (pow(1.0 + gg - 2.0 * mu * g, 1.5) * (2.0 + gg));
    vec3 col = 20.0 * 2.4 * (pR * kR * totR + pM * kM * totM);   // x2.4 stands in for multiple scattering
    if (ground) col += vec3(0.04, 0.045, 0.05) * max(sun.y, 0.0) * exp(-(kM * odM + kR * odR) * 1.0);   // a faint ground under the haze
    col += vec3(0.0008, 0.0012, 0.0025);                                                                // night sky glow
    fragColor = vec4(col, 1.0);
}

//@ sky.frag
in vec2 vUV;
uniform vec3 uCamR, uCamUp, uCamF, uSunEnu;
uniform float uTan, uAspect, uTime;
uniform sampler2D uDetail;
out vec4 fragColor;
float h21(vec2 p) { return texelFetch(uDetail, ivec2(p) & 511, 0).g; }
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    vec3 d = normalize(uCamF + p.x * uTan * uAspect * uCamR + p.y * uTan * uCamUp);
    vec3 e = toEnu(d);
    vec3 col = textureLod(uSky, e, 0.0).rgb;
    float mu = dot(e, normalize(uSunEnu)), ang = acos(clamp(mu, -1.0, 1.0));
    const float SR = 0.0047;
    vec3 sunTint = uSunCol / max(max(uSunCol.r, uSunCol.g), max(uSunCol.b, 1e-4));
    float sunUp = smoothstep(-0.02, 0.01, e.y);
    col += sunTint * max(uSunCol.r, 0.0) * (smoothstep(SR, SR * 0.8, ang) * 4000.0 + exp(-ang * 300.0) * 6.0 + 0.0008 / (ang * ang + 0.0008)) * 0.06 * sunUp;
    // stars once the sky is dark
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    if (e.y > 0.0 && lum < 0.05) {
        vec3 a = abs(d); vec2 uv; float face;
        if (a.x > a.y && a.x > a.z) { uv = d.yz / a.x; face = d.x > 0.0 ? 0.0 : 1.0; }
        else if (a.y > a.z) { uv = d.xz / a.y; face = d.y > 0.0 ? 2.0 : 3.0; }
        else { uv = d.xy / a.z; face = d.z > 0.0 ? 4.0 : 5.0; }
        vec2 g = uv * 160.0, id = floor(g) + face * 61.0, f = fract(g) - 0.5;
        float r1 = h21(id), r2 = h21(id + 131.0), r3 = h21(id + 257.0);
        float b = max(pow(r3, 24.0) - 0.05, 0.0) * 4.0;
        col += b * smoothstep(0.12, 0.0, length(f - (vec2(r1, r2) - 0.5) * 0.7)) * clamp(1.0 - lum * 20.0, 0.0, 1.0) * vec3(0.9, 0.95, 1.0) * uBoost * 0.35;
    }
    fragColor = vec4(col, 1.0);
}

//@ particle.vert
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
uniform mat4 uVP;
out vec2 vUV; out vec4 vCol; out vec3 vRel; out float vW;
void main() { vUV = aUV; vCol = aCol; vRel = aPos; gl_Position = uVP * vec4(aPos, 1.0); vW = 1.0 + gl_Position.w; }

//@ particle.frag
in vec2 vUV; in vec4 vCol; in vec3 vRel; in float vW;
out vec4 fragColor;
void main() {
    float d = dot(vUV, vUV);
    if (d > 1.0) discard;
    float dist = length(vRel), alt = uCamAlt + dot(vRel, uU);
    if (vCol.a < 0.0) {                       // a light: additive, dimmed by the haze in front of it
        float g = (exp(-d * 6.0) + 0.15 * exp(-d * 1.5)) * -vCol.a;
        fragColor = vec4(vCol.rgb * g * (1.0 - fogAmount(dist, uCamAlt, alt)), 0.0);
    } else {                                  // smoke, spray, contrails: lit by sun and sky
        float a = smoothstep(1.0, 0.2, d) * vCol.a;
        vec3 lit = vCol.rgb * (uSunCol * 0.06 + textureLod(uSky, vec3(0, 1, 0), 7.0).rgb * 1.5);
        lit = applyFog(lit, vRel, dist, alt);
        fragColor = vec4(lit * a, a);
    }
    gl_FragDepth = log2(vW) * uFC;
}

//@ down.frag
in vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uFirst, uThresh;
out vec4 fragColor;
vec3 pre(vec3 c) { if (any(isnan(c))) return vec3(0.0); float l = max(c.r, max(c.g, c.b)); return c * max(l - uThresh, 0.0) / max(l, 1e-6) / (1.0 + 0.02 * l / uThresh); }
void main() {
    vec2 o = uTexel;
    vec3 s0 = texture(uTex, vUV).rgb, s1 = texture(uTex, vUV + vec2(-o.x, -o.y)).rgb, s2 = texture(uTex, vUV + vec2(o.x, -o.y)).rgb;
    vec3 s3 = texture(uTex, vUV + vec2(-o.x, o.y)).rgb, s4 = texture(uTex, vUV + vec2(o.x, o.y)).rgb;
    vec3 c = uFirst > 0.5 ? pre(min(s0, vec3(5e3))) * 0.5 + (pre(min(s1, vec3(5e3))) + pre(min(s2, vec3(5e3))) + pre(min(s3, vec3(5e3))) + pre(min(s4, vec3(5e3)))) * 0.125
                          : s0 * 0.5 + (s1 + s2 + s3 + s4) * 0.125;
    fragColor = vec4(c, 1.0);
}

//@ up.frag
in vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uTexel;
out vec4 fragColor;
void main() {
    vec2 o = uTexel;
    vec3 c = texture(uTex, vUV).rgb * 4.0;
    c += (texture(uTex, vUV + vec2(o.x, 0)).rgb + texture(uTex, vUV - vec2(o.x, 0)).rgb + texture(uTex, vUV + vec2(0, o.y)).rgb + texture(uTex, vUV - vec2(0, o.y)).rgb) * 2.0;
    c += texture(uTex, vUV + o).rgb + texture(uTex, vUV - o).rgb + texture(uTex, vUV + vec2(o.x, -o.y)).rgb + texture(uTex, vUV + vec2(-o.x, o.y)).rgb;
    fragColor = vec4(c / 16.0, 1.0);
}

//@ clouds.frag
// raymarched cumulus, at half resolution: rgb = light scattered toward us, a = transmittance.
// Flat dark bases, cauliflower tops, Beer-powder lighting with multiple-scattering octaves.
in vec2 vUV;
uniform sampler2D uDepth;
uniform vec3 uCamR, uCamUp, uCamF;
uniform float uTan, uAspect, uTime;
out vec4 fragColor;
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
float hg(float c, float g) { float g2 = g * g; return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5)); }
float cloudDensity(vec3 wp, float alt, bool detail) {
    float h = (alt - uCloudBase) / (uCloudTop - uCloudBase);
    if (h < 0.0 || h > 1.0) return 0.0;
    float cov = cloudCoverage(wp);
    if (cov < 0.01) return 0.0;
    float top = mix(0.3, 1.0, cov);                                    // bigger towers where cover is thick
    float prof = smoothstep(0.0, 0.06, h) * smoothstep(top, top * 0.55, h);
    vec4 n = texture(uNoise3, wp / 2900.0);
    float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float shape = n.r * 0.6 + fbm * 0.4;
    float d = cov * prof - (1.0 - shape) * 0.62;
    if (d <= 0.0) return 0.0;
    if (detail) {                                                      // eat away the edges: wispy at the base, billowy at the top
        vec4 dn = texture(uNoise3, wp / 540.0 + vec3(uTime * 0.0015, 0.0, 0.0));
        float det = dn.g * 0.625 + dn.b * 0.25 + dn.a * 0.125;
        d -= mix(1.0 - det, det, clamp(h * 3.0, 0.0, 1.0)) * 0.2;
    }
    return clamp(d * 3.5, 0.0, 1.0) * 0.045;
}
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    vec3 rd = normalize(uCamF + p.x * uTan * uAspect * uCamR + p.y * uTan * uCamUp);
    float depth = texture(uDepth, vUV).r;
    float sceneW = exp2(depth / uFC) - 1.0;
    float sceneT = depth >= 0.99999 ? 1e9 : sceneW / max(dot(rd, uCamF), 1e-3);
    float mu = dot(rd, uU), R = 6371000.0;
    float tA = 0.0, tB = 0.0;
    float disc0 = mu * mu + 2.0 * (uCamAlt - uCloudBase) / R, disc1 = mu * mu + 2.0 * (uCamAlt - uCloudTop) / R;
    if (uCamAlt < uCloudBase) { if (disc0 >= 0.0 && mu > 0.0) { tA = R * (mu - sqrt(disc0)); tB = disc1 >= 0.0 ? R * (mu - sqrt(disc1)) : R * (mu + sqrt(disc0)); } else tB = -1.0; }
    else if (uCamAlt > uCloudTop) { if (disc1 >= 0.0 && mu < 0.0) { tA = R * (mu + sqrt(disc1)); tB = disc0 >= 0.0 ? R * (mu + sqrt(disc0)) : R * (mu - sqrt(disc1)); } else tB = -1.0; }
    else { tA = 0.0; tB = mu > 0.0 ? (disc1 >= 0.0 ? R * (mu - sqrt(disc1)) : 1e5) : (disc0 >= 0.0 ? R * (mu + sqrt(disc0)) : 1e5); }
    tB = min(tB, min(sceneT, 120000.0));
    vec3 acc = vec3(0.0);
    float T = 1.0;
    if (tB > tA) {
        const int N = 72;
        float len = min(tB - tA, 40000.0), ds = len / float(N);
        float t = tA + ds * ign(gl_FragCoord.xy + fract(uTime * 7.0) * 37.0);
        vec3 skyTop = textureLod(uSky, vec3(0.0, 1.0, 0.0), 5.0).rgb, skyHor = textureLod(uSky, normalize(vec3(1.0, 0.1, 0.0)), 5.0).rgb;
        float cosS = dot(rd, uSunDir);
        float sunUp = smoothstep(-0.05, 0.1, dot(uSunDir, uU));
        for (int i = 0; i < N; i++) {
            if (t > tB) break;
            float alt = uCamAlt + t * mu - t * t / (2.0 * R);
            vec3 wp = rd * t + uCamWorldMod;
            float d = cloudDensity(wp, alt, t < 25000.0);
            if (d > 0.0) {
                float od = 0.0, st = 60.0;
                vec3 lp = wp; float la = alt;
                for (int j = 0; j < 6; j++) { lp += uSunDir * st; la += dot(uSunDir, uU) * st; od += cloudDensity(lp, la, false) * st; st *= 1.7; }
                float h = clamp((alt - uCloudBase) / (uCloudTop - uCloudBase), 0.0, 1.0);
                vec3 Ls = vec3(0.0); float a = 1.0, b = 1.0, c = 1.0;
                for (int k = 0; k < 3; k++) { Ls += uSunCol * exp(-od * b) * (hg(cosS, 0.65 * c) * 0.8 + hg(cosS, -0.25 * c) * 0.2) * a; a *= 0.5; b *= 0.45; c *= 0.5; }
                float powder = 1.0 - exp(-d * 120.0);
                vec3 amb = mix(skyHor * 0.35 + vec3(0.02), skyTop * 1.25 + skyHor * 0.2, h);   // dark flat bases, bright tops
                vec3 L = Ls * mix(0.6, 1.0, powder) * 3.2 * sunUp + amb;
                float tr = exp(-d * ds);
                acc += T * (1.0 - tr) * L;
                T *= tr;
                if (T < 0.015) break;
                t += ds;
            } else t += ds * 1.6;
        }
        float far = 1.0 - smoothstep(45000.0, 115000.0, tA);
        T = mix(1.0, T, far); acc *= far;
        float fogA = fogAmount(max(tA, 0.0), uCamAlt, clamp(uCamAlt, uCloudBase, uCloudTop));
        vec3 fogc = skyColor(normalize(rd + uU * 0.03), 2.0);
        acc = mix(acc, fogc * (1.0 - T), fogA);
    }
    fragColor = vec4(acc, T);
}

//@ composite.frag
// clouds, bloom, lens flare, tone mapping
in vec2 vUV;
uniform sampler2D uScene, uDepth, uBloom, uCloudTex;
uniform vec2 uRes, uSunScreen, uCloudTexel;
uniform float uAspect, uTime, uExposure, uSunVis, uClouds;
out vec4 fragColor;
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
    vec3 col = texture(uScene, vUV).rgb;
    if (uClouds > 0.5) {
        vec2 o = uCloudTexel;
        vec4 c = texture(uCloudTex, vUV) * 0.4 + (texture(uCloudTex, vUV + vec2(o.x, o.y)) + texture(uCloudTex, vUV + vec2(-o.x, o.y)) + texture(uCloudTex, vUV + vec2(o.x, -o.y)) + texture(uCloudTex, vUV - o)) * 0.15;
        col = col * c.a + c.rgb;
    }
    col += texture(uBloom, vUV).rgb * 0.5;
    if (uSunVis > 0.0) {
        vec2 asp = vec2(uAspect, 1.0), s = uSunScreen, cc = vec2(0.5);
        float open = step(0.9999, texture(uDepth, s).r);
        vec3 fl = vec3(1.0, 0.8, 0.6) * 0.04 / (abs(vUV.y - s.y) * 120.0 + 1.0) * exp(-abs(vUV.x - s.x) * asp.x * 2.0);
        for (int i = 0; i < 4; i++) {
            vec2 g = cc + (s - cc) * (-0.4 - float(i) * 0.45);
            float dd = length((vUV - g) * asp) / (0.03 + 0.025 * float(i % 3));
            fl += vec3(0.7, 0.75 + 0.05 * float(i), 0.9) * smoothstep(1.0, 0.75, dd) * 0.02;
        }
        col += fl * uSunVis * open;
    }
    col *= uExposure;
    col = aces(col);
    col = pow(col, vec3(1.0 / 2.2));
    vec2 q = vUV - 0.5;
    col *= 1.0 - 0.3 * dot(q, q);
    col += (ign(gl_FragCoord.xy + fract(uTime) * 61.0) - 0.5) / 255.0;
    fragColor = vec4(col, 1.0);
}

//@ hud.vert
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aCol;
uniform vec2 uRes;
out vec4 vCol;
void main() { vCol = aCol; gl_Position = vec4(aPos / uRes * 2.0 - 1.0, 0.0, 1.0); gl_Position.y = -gl_Position.y; }

//@ hud.frag
in vec4 vCol;
out vec4 fragColor;
void main() { fragColor = vCol; }

//@ map.vert
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform vec2 uRes;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos / uRes * 2.0 - 1.0, 0.0, 1.0); gl_Position.y = -gl_Position.y; }

//@ map.frag
in vec2 vUV;
uniform sampler2D uTex;
out vec4 fragColor;
void main() { vec3 c = texture(uTex, vUV).rgb; fragColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0); }
