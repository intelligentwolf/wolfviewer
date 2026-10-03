/**
 * @file class1\deferred\moonF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, 2020 Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

uniform vec4 color;
uniform vec3 moon_dir;
uniform float moon_brightness;
uniform sampler2D diffuseMap;
// <WolfViewer 2026-10-03> Jimmy Olsen's Automatic Environment: the real moon.
// wolf_moon_phase.xyz = direction to the sun in the moon disc's own frame (x along the quad's right
// axis = +u, y along its up axis = +v, z towards the viewer); w = 1 for the real moon, 0 for stock.
// wolf_moon_north.xy = lunar north on the disc (unit), .z = night 0..1 (earthshine shows at night).
// altDiffuseMap = NASA SVS CGI Moon Kit LRO colour map (equirectangular, centred on 0 deg longitude,
// east to the right, north at v = 1 after llpngwrapper's bottom-up load).
// Set in lldrawpoolwlsky.cpp.
uniform vec4 wolf_moon_phase;
uniform vec4 wolf_moon_north;
uniform sampler2D altDiffuseMap;

in vec2 vary_texcoord0;

float wolf_moon_night() { return clamp(wolf_moon_north.z, 0.0, 1.0); }

void main()
{
    // Restore Pre-EEP alpha fade moon near horizon
    float fade = 1.0;
    if( moon_dir.z > 0 )
        fade = clamp( moon_dir.z*moon_dir.z*4.0, 0.0, 1.0 );

    // <WolfViewer 2026-10-03> The real moon: a lit sphere with the LRO surface, the near side
    // towards the viewer, lunar north where it really is, the real phase.
    if (wolf_moon_phase.w > 0.5)
    {
        vec2  p  = (vary_texcoord0.xy - vec2(0.5)) * 2.0;
        float r  = length(p);
        if (r > 1.0) discard;
        vec3  n  = vec3(p, sqrt(max(1.0 - r * r, 0.0)));
        vec3  north = vec3(normalize(wolf_moon_north.xy), 0.0);
        vec3  east  = vec3(north.y, -north.x, 0.0);          // lunar east = right with north up
        const float PI = 3.14159265;
        float lon = atan(dot(n, east), n.z);
        float lat = asin(clamp(dot(n, north), -1.0, 1.0));
        vec3 albedo = texture(altDiffuseMap, vec2(0.5 + lon / (2.0 * PI), 0.5 + lat / PI)).rgb;

        // Lommel-Seeliger, the classic lunar law: radiance ~ mu0 / (mu0 + mu). It is why the full
        // moon looks flat to the limb and the terminator stays crisp. x2 so the full-moon centre is 1.
        vec3  s   = normalize(wolf_moon_phase.xyz);
        float mu0 = max(dot(n, s), 0.0);
        float mu  = max(n.z, 0.0);
        float ls  = 2.0 * mu0 / (mu0 + mu + 1e-4);
        vec3  lit = albedo * ls * 1.35;
        // Earthshine: the night side's faint grey, only at night.
        vec3  earth = albedo * 0.03 * wolf_moon_night();
        vec3  col = lit + earth;

        // In daylight the unlit part is not there at all (the blue sky shows through), and the
        // dark maria are thinner than the highlands, as the real daytime moon. At night the whole
        // disc is solid. Soft 2% edge.
        float lum   = dot(lit, vec3(0.299, 0.587, 0.114));
        float alpha = clamp(max(lum * 1.8, wolf_moon_night() * 0.95), 0.0, 1.0);
        alpha *= smoothstep(1.0, 0.98, r) * fade;

        frag_data[1] = vec4(0.0);
        frag_data[2] = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
        frag_data[0] = vec4(0);
        frag_data[3] = vec4(col, alpha);
#else
        frag_data[0] = vec4(col, alpha);
#endif
        return;
    }

    vec4 c      = texture(diffuseMap, vary_texcoord0.xy);

    // SL-14113 Don't write to depth; prevent moon's quad from hiding stars which should be visible
    // Moon texture has transparent pixels <0x55,0x55,0x55,0x00>
    if (c.a <= 2./255.) // 0.00784
    {
        discard;
    }

    c.rgb *= moon_brightness;
    c.a   *= fade;

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0.0);
    frag_data[2] = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(c.rgb, c.a);
#else
    frag_data[0] = vec4(c.rgb, c.a);
#endif

    // Added and commented out for a ground truth.  Do not uncomment - Geenz
    //gl_FragDepth = 0.999985f;
}

