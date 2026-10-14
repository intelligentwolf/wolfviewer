/**
 * @file wolftk/seaGlint.glsl
 * WolfViewer (IntelligentWolf Ltd, 2026-10-13, round 11): the sun's (and the moon's) glint on the sea - one model for the
 * world's water (class3/environment/waterF.glsl under wolf_phys) and the dome's open sea beyond it (wolftk/skyF.glsl
 * wolfFarSeaSurface's caller), so the two meet at the water planes' far clip with the same highlight.
 *
 *   slopes   Cox & Munk 1954 (wind at 12.5 m), total mean square slope sigma^2 = sigma_a^2 + sigma_c^2 = 0.003 + 5.12e-3 U
 *            (Ocean Optics Web Book, "Cox-Munk Sea Surface Slope Statistics", eq. 4), the Gaussian slope pdf of eq. 1 taken
 *            isotropic (the along / cross-wind split is not kept: one wind number, no direction, reaches the shader)
 *   facets   the Beckmann distribution D = exp(-tan^2(alpha) / m^2) / (pi m^2 cos^4(alpha)), alpha = acos(N.H) (Wikipedia
 *            "Specular highlight"): an isotropic Gaussian slope pdf with per-component variance s^2 over cos^4, m^2 = 2 s^2
 *            = the total mean square slope sigma^2 above
 *   BRDF     Torrance-Sparrow f = D G F / (4 cos(theta_o) cos(theta_i)) (pbr-book 3rd ed. section 8.4 "Microfacet Models";
 *            Wikipedia "Specular highlight" Cook-Torrance) with the masking-shadowing G = 1 / (1 + Lambda(v) + Lambda(l)) and
 *            the Beckmann Lambda's rational approximation (pbr-book BeckmannDistribution::Lambda: a = 1 / (alpha tan(theta)),
 *            0 for a >= 1.6, else (1 - 1.259 a + 0.396 a^2) / (3.535 a + 2.181 a^2)), alpha = sqrt(mss) (pbr-book: the same
 *            Beckmann D with alpha = sqrt(2) x the per-axis rms slope) - without G the glint at a grazing view (V.N -> 0 at
 *            the horizon) grew as 1 / (4 V.N) without bound (round 11 harness: golden_glitter_beach 377 cd/m2 pre-exposed)
 *   Fresnel  the exact unpolarised reflectance at the facet, R = (R_s + R_p) / 2 with n = 1.333 (Wikipedia "Fresnel
 *            equations"; BEAUTY_RESEARCH.md section 7 table: R(0) = 0.0204)
 * The returned factor times the light's irradiance perpendicular to it (E_perp) is the reflected radiance:
 * L = f E_perp (N.L) = F D G E_perp / (4 (V.N)).
 */

#ifndef WOLF_TK_SEAGLINT_GLSL
#define WOLF_TK_SEAGLINT_GLSL

// Cox & Munk 1954 eq. 4 (Ocean Optics Web Book): total mean square slope from the wind speed in m/s
float wolfCoxMunkMss(float windSpeed)
{
    return 0.003 + 5.12e-3 * max(windSpeed, 0.0);
}

// Wikipedia "Fresnel equations" (n1 = 1 air, n2 = n water), unpolarised: R = (R_s + R_p) / 2
float wolfFresnelWater(float cosI)
{
    const float n = 1.333;
    float c = clamp(cosI, 0.0, 1.0);
    float sinT = sqrt(max(1.0 - c * c, 0.0)) / n;
    float cosT = sqrt(max(1.0 - sinT * sinT, 0.0));
    float rs = (c - n * cosT) / (c + n * cosT);
    float rp = (cosT - n * c) / (cosT + n * c);
    return 0.5 * (rs * rs + rp * rp);
}

// pbr-book BeckmannDistribution::Lambda (see the header); cosT = the direction's cosine to the mean normal
float wolfBeckmannLambda(float cosT, float alpha)
{
    float c = clamp(cosT, 1e-4, 1.0);
    float tanT = sqrt(max(1.0 - c * c, 0.0)) / c;
    if (tanT <= 0.0) return 0.0;
    float a = 1.0 / (alpha * tanT);
    if (a >= 1.6) return 0.0;
    return (1.0 - 1.259 * a + 0.396 * a * a) / (3.535 * a + 2.181 * a * a);
}

// n the mean surface normal, v toward the camera, l toward the light (all unit, one frame); mss = wolfCoxMunkMss.
// Returns F D G / (4 (V.N)) for N.L > 0 (times E_perp = the radiance); 0 with the light or the eye under the surface.
float wolfSeaGlint(vec3 n, vec3 v, vec3 l, float mss)
{
    float NL = dot(n, l), NV = dot(n, v);
    if (NL <= 0.0 || NV <= 0.0) return 0.0;
    vec3 h = normalize(v + l);
    float NH = max(dot(n, h), 1e-4);
    float NH2 = NH * NH;
    float tan2 = (1.0 - NH2) / NH2;
    float m2 = max(mss, 1e-4);
    float D = exp(-tan2 / m2) / (3.14159265 * m2 * NH2 * NH2);
    float F = wolfFresnelWater(dot(v, h));
    float alpha = sqrt(m2);
    float G = 1.0 / (1.0 + wolfBeckmannLambda(NV, alpha) + wolfBeckmannLambda(NL, alpha));
    return F * D * G / (4.0 * max(NV, 1e-4));
}

#endif // WOLF_TK_SEAGLINT_GLSL
