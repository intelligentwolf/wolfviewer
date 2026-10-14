/**
 * @file wolftk/atmosphereUniforms.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4):
 *   packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:16-24
 *
 * three-geospatial licence:
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Shota Matsuda
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Include after wolftk/brunetonDefinitions.glsl.
 */

#ifndef WOLF_TK_ATMOSPHEREUNIFORMS_GLSL
#define WOLF_TK_ATMOSPHEREUNIFORMS_GLSL

// Source: three-geospatial packages/atmosphere/src/shaders/aerialPerspectiveEffect.frag:16-24 (= sky.frag:10-18,
// stars.frag:6-14, clouds.frag:19-27): the uniforms of the Bruneton runtime, declared once here.
// [WolfViewer] SUN_/SKY_SPECTRAL_RADIANCE_TO_LUMINANCE are set by wolftkatmosphere.cpp to Bruneton's ABSOLUTE
// k values (model.cc:668-686) x the frame's pre-exposure (Filament.md.html:1723, "pre-exposed lighting"),
// so every runtime result is in the frame's units. This replaces takram's relative luminance
// (AtmosphereParameters.ts:182-191 divides by the sun's luminance to fit half floats): the pre-exposure
// serves the same purpose.
uniform AtmosphereParameters ATMOSPHERE;
uniform vec3 SUN_SPECTRAL_RADIANCE_TO_LUMINANCE;
uniform vec3 SKY_SPECTRAL_RADIANCE_TO_LUMINANCE;

uniform sampler2D transmittance_texture;
uniform sampler3D scattering_texture;
uniform sampler2D irradiance_texture;
uniform sampler3D single_mie_scattering_texture;
uniform sampler3D higher_order_scattering_texture;
#endif // WOLF_TK_ATMOSPHEREUNIFORMS_GLSL
