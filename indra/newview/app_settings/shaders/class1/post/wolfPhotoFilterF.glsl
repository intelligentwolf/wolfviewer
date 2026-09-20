/**
 * @file wolfPhotoFilterF.glsl
 * @brief WolfViewer: World > Photo Effects colour grade, post pass (pipeline.cpp wolfPhotoFilter).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/rendering/photo_filter.js WolfPhotoFilter.GLSL — wolfPhotoGrade() is
// the same function, constant for constant; change one, change both, or the two viewers stop
// agreeing on what "Vintage" looks like. The frame here is display-referred (after tonemap
// and gamma correct), which is what WolfStorm's canvas frame is too. uMode is the
// WolfViewerPhotoFilter setting: 1 Sepia, 2 Black & White, 3 Noir, 4 Vintage, 5 Warm,
// 6 Cool, 7 Faded, 8 Vivid (0 = off; the CPU never runs this pass for it).

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform int uMode;
uniform float uStrength;
uniform vec2 uTexel;   // [PAINTERS 2026-09-20] one texel of diffuseRect (pipeline.cpp wolfPhotoFilter)
in vec2 vary_fragcoord;

vec3 wolfPhotoGrade(vec3 c, vec2 uv, int mode)
{
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float v = length(uv - 0.5) * 1.41421356;   // 0 at the centre, 1 in the corners
    if (mode == 1) {
        return vec3(dot(c, vec3(0.393, 0.769, 0.189)),
                    dot(c, vec3(0.349, 0.686, 0.168)),
                    dot(c, vec3(0.272, 0.534, 0.131)));
    }
    if (mode == 2) { return vec3(l); }
    if (mode == 3) {
        float n = clamp((l - 0.5) * 1.7 + 0.5, 0.0, 1.0);
        n = n * n * (3.0 - 2.0 * n);
        return vec3(n) * (1.0 - 0.55 * smoothstep(0.45, 1.0, v));
    }
    if (mode == 4) {
        vec3 f = mix(vec3(l), c, 0.6) * 0.82 + 0.10;
        f *= vec3(1.08, 1.0, 0.84);
        return f * (1.0 - 0.4 * smoothstep(0.5, 1.0, v));
    }
    if (mode == 5) { return c * vec3(1.10, 1.0, 0.86); }
    if (mode == 6) { return c * vec3(0.88, 1.0, 1.12); }
    if (mode == 7) { return mix(vec3(l), c, 0.7) * 0.78 + 0.14; }
    if (mode == 8) { return (mix(vec3(l), c, 1.45) - 0.5) * 1.08 + 0.5; }
    return c;
}


// [PAINTERS 2026-09-20] World > Photo Effects > Picasso / Van Gogh / Monet / Seurat: the frame
// re-painted, not just re-graded, so these read the SOURCE around the pixel. Same function in
// both viewers (WolfViewer post/wolfPhotoFilterF.glsl), constant for constant. Everything is
// keyed to screen pixels through s (a 1080-line frame is 1.0) so a 4K frame gets 4K strokes,
// and to a static hash of the pixel grid so nothing crawls or flickers between frames.
float wpHash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec2 wpHash2(vec2 p)
{
    float h = wpHash(p);
    return vec2(h, wpHash(p + h + 7.13));
}
float wpNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(wpHash(i), wpHash(i + vec2(1.0, 0.0)), f.x),
               mix(wpHash(i + vec2(0.0, 1.0)), wpHash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float wpLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 wpSat(vec3 c, float k) { return mix(vec3(wpLum(c)), c, k); }
vec3 wpTap(sampler2D src, vec2 uv, vec2 texel, vec2 uvMax)
{
    return texture(src, clamp(uv, texel * 0.5, uvMax - texel * 0.5)).rgb;
}
vec3 wolfPhotoPaint(sampler2D src, vec2 uv, vec2 texel, vec2 uvMax, int mode)
{
    vec2 pxc = uv / texel;                          // pixel coordinates
    float s = max(0.5, (1.0 / texel.y) / 1080.0);   // stroke scale: a 1080-line frame is 1.0
    vec3 c = wpTap(src, uv, texel, uvMax);
    if (mode == 9) {
        // Picasso: cubist facets. Each facet (a jittered Voronoi cell ~150 px) shows its piece of
        // the picture slightly turned and shifted, flattened to six tones, tinted from a period
        // palette (ochre, blue, rose, grey), split light/dark along a random line, and drawn in.
        float S = 150.0 * s;
        vec2 g = floor(pxc / S);
        float d1 = 1e9, d2 = 1e9;
        vec2 cen = pxc, id = g;
        for (int j = -1; j <= 1; j++) {
            for (int i = -1; i <= 1; i++) {
                vec2 cell = g + vec2(float(i), float(j));
                vec2 p = (cell + wpHash2(cell)) * S;
                float d = length(pxc - p);
                if (d < d1) { d2 = d1; d1 = d; cen = p; id = cell; }
                else if (d < d2) { d2 = d; }
            }
        }
        float edge = d2 - d1;
        float ang = (wpHash(id + 3.7) - 0.5) * 0.5;
        vec2 shift = (wpHash2(id + 11.3) - 0.5) * S * 0.25;
        vec2 r = pxc - cen;
        r = vec2(r.x * cos(ang) - r.y * sin(ang), r.x * sin(ang) + r.y * cos(ang));
        vec3 f = wpTap(src, (cen + r + shift) * texel, texel, uvMax);
        f = floor(f * 6.0 + 0.5) / 6.0;
        float h = wpHash(id + 21.9);
        vec3 tint = h < 0.25 ? vec3(0.85, 0.62, 0.30) : (h < 0.5 ? vec3(0.30, 0.45, 0.70) : (h < 0.75 ? vec3(0.80, 0.55, 0.55) : vec3(0.60, 0.60, 0.58)));
        f = mix(f, tint * (0.4 + 0.6 * wpLum(f)), 0.22);
        vec2 dir = normalize(wpHash2(id + 5.5) - 0.5 + vec2(0.013, 0.007));
        f *= dot(pxc - cen, dir) > 0.0 ? 1.10 : 0.86;
        return mix(vec3(0.07, 0.06, 0.05), f, smoothstep(0.8 * s, 2.4 * s, edge));
    }
    if (mode == 10) {
        // Van Gogh: thick short strokes. The frame is cut into 18 px cells; each cell's stroke
        // direction follows the picture's edges there (luminance gradient at the cell centre)
        // and swirls with a noise field where there are none. Colour is smeared along that
        // direction (nine taps), ridged across it for impasto light, broken into dabs by a
        // dark gap, and pushed into saturated, warm-against-blue paint.
        float D = 18.0 * s;
        vec2 cell = floor(pxc / D);
        vec2 cc = (cell + 0.5) * D;                  // the cell centre decides the stroke
        vec2 cuv = cc * texel;
        float lx = wpLum(wpTap(src, cuv + vec2(2.0 * texel.x, 0.0), texel, uvMax)) - wpLum(wpTap(src, cuv - vec2(2.0 * texel.x, 0.0), texel, uvMax));
        float ly = wpLum(wpTap(src, cuv + vec2(0.0, 2.0 * texel.y), texel, uvMax)) - wpLum(wpTap(src, cuv - vec2(0.0, 2.0 * texel.y), texel, uvMax));
        vec2 grad = vec2(lx, ly);
        float gm = length(grad);
        vec2 along = gm > 1e-4 ? vec2(-grad.y, grad.x) / gm : vec2(1.0, 0.0);
        float a = (wpNoise(cc / (110.0 * s)) - 0.5) * 4.0;
        vec2 swirl = vec2(cos(a), sin(a));
        vec2 dir = normalize(mix(swirl, along, smoothstep(0.03, 0.15, gm)) + vec2(1e-4, 0.0));
        vec2 perp = vec2(-dir.y, dir.x);
        float L = 3.5 * s;
        vec3 acc = vec3(0.0);
        for (int i = -4; i <= 4; i++) {
            acc += wpTap(src, uv + dir * (float(i) * L) * texel, texel, uvMax);
        }
        vec3 f = acc / 9.0;
        vec2 rel = pxc - cc;
        float across = dot(rel, perp) / (3.2 * s);   // ridges every ~3 px across the stroke
        float ridge = 0.5 + 0.5 * sin(across * 6.2831853 + wpHash(cell) * 6.2831853);
        float gap = smoothstep(0.35, 0.5, abs(fract(dot(rel, perp) / D + wpHash(cell + 2.2)) - 0.5));   // one dark seam per cell
        f = wpSat(f, 1.6) * (0.72 + 0.42 * ridge) * (1.0 - 0.35 * gap);
        f = (f - 0.5) * 1.15 + 0.5;
        return f * vec3(1.06, 1.0, 0.92);
    }
    if (mode == 11) {
        // Monet: round dabs of colour, about 24 and 14 px, on two jittered grids, each dab the
        // colour under its centre with its own light, soft-edged, lifted to a pastel key.
        vec3 f = vec3(0.0);
        float wsum = 0.0;
        for (int layer = 0; layer < 2; layer++) {
            float D = layer == 0 ? 24.0 * s : 14.0 * s;
            float lw = layer == 0 ? 1.0 : 0.7;
            vec2 g = floor(pxc / D);
            for (int j = -1; j <= 1; j++) {
                for (int i = -1; i <= 1; i++) {
                    vec2 cell = g + vec2(float(i), float(j));
                    vec2 h2 = wpHash2(cell + float(layer) * 17.0);
                    vec2 p = (cell + h2) * D;
                    float rad = D * (0.6 + 0.3 * wpHash(cell + 2.0 + float(layer)));
                    float d = length(pxc - p);
                    if (d < rad) {
                        float w = lw * (1.0 - smoothstep(rad * 0.55, rad, d)) * (0.5 + 0.5 * h2.x);
                        f += wpTap(src, p * texel, texel, uvMax) * (0.9 + 0.2 * h2.y) * w;
                        wsum += w;
                    }
                }
            }
        }
        f = wsum > 0.0 ? f / wsum : c;
        f = wpSat(f, 0.95) * 0.86 + 0.12;
        return f * vec3(1.03, 1.0, 0.95);
    }
    if (mode == 12) {
        // Seurat: pointillism. Round dots about 12 px on a jittered grid, each the colour under
        // its centre pushed a little toward a pure hue (divisionism: the eye mixes them), on a
        // cream canvas that keeps the picture's light between the dots.
        float D = 12.0 * s;
        vec2 g = floor(pxc / D);
        float best = 1e9, bd = 1e9, bestRad = D * 0.5;
        vec3 dotc = c;
        vec2 dotId = g;
        for (int j = -1; j <= 1; j++) {
            for (int i = -1; i <= 1; i++) {
                vec2 cell = g + vec2(float(i), float(j));
                vec2 p = (cell + 0.25 + 0.5 * wpHash2(cell)) * D;
                float rr = D * (0.48 + 0.12 * wpHash(cell + 4.0));
                float dd = length(pxc - p);
                float d = dd / rr;
                if (d < best) { best = d; bd = dd; bestRad = rr; dotId = cell; dotc = wpTap(src, p * texel, texel, uvMax); }
            }
        }
        float hsel = wpHash(dotId + 9.9);
        vec3 push = hsel < 0.333 ? vec3(1.15, 0.95, 0.95) : (hsel < 0.666 ? vec3(0.95, 1.15, 0.95) : vec3(0.95, 0.95, 1.15));
        vec3 dcol = clamp(wpSat(dotc, 1.5) * push, 0.0, 1.0);
        vec3 canvas = vec3(0.94, 0.90, 0.80) * (0.55 + 0.45 * wpLum(c));
        float cover = 1.0 - smoothstep(bestRad - 1.5 * s, bestRad + 0.5 * s, bd);
        return mix(canvas, dcol, cover);
    }
    return c;
}

void main()
{
    vec4 diff = texture(diffuseRect, vary_fragcoord.xy);
    // [PAINTERS 2026-09-20] 9..12 re-paint from the source; 1..8 grade the pixel.
    if (uMode >= 9)
    {
        diff.rgb = mix(diff.rgb, clamp(wolfPhotoPaint(diffuseRect, vary_fragcoord.xy, uTexel, vec2(1.0), uMode), 0.0, 1.0), uStrength);
    }
    else
    {
        diff.rgb = mix(diff.rgb, clamp(wolfPhotoGrade(diff.rgb, vary_fragcoord.xy, uMode), 0.0, 1.0), uStrength);
    }
    frag_color = diff;
}
