/**
 * WolfViewer (IntelligentWolf Ltd, 2026-10-13): the light on the clouds - the sun, or the moon at night - for the
 * translated three-geospatial clouds shaders (cloudsV.glsl / cloudsF.glsl), calling the translated Bruneton runtime
 * (brunetonRuntime.glsl GetSunAndSkyScalarIrradiance / GetSunAndSkyIrradiance) exactly where the source calls it with
 * sunDirection alone: clouds.vert:46-65 sampleSunSkyIrradiance, clouds.frag sampleSunSkyIrradiance (both variants).
 */

// [WolfViewer round 8] the clouds lit by the moon as by the sun (section 10 A16): sunDirection is THE light (the sun,
// or the moon once the sun is 6 deg under - civil twilight's end), its irradiance x wolfLightScale (1 for the sun; the
// moon's illuminance above the atmosphere over the sun's, Krisciunas & Schaefer 1991, as the sky dome's moonlit air);
// the other body adds its sky irradiance alone (wolfOtherDirection x wolfOtherScale: the twilight sky while the moon
// lights, the moon's faint sky by day). <= 0 scale = 1 (the uniform unset: the sun alone).
// Set by wolftkclouds.cpp sharedUniforms from WolfTkClouds::CloudsFrame light_scale / other_dir_ecef / other_scale.
uniform float wolfLightScale;
uniform vec3 wolfOtherDirection;
uniform float wolfOtherScale;
// [WolfViewer 2026-10-28] ticket #304: per channel on THE light's direct irradiance - the moon's light as the eye sees it
// (wolfenvironmentrender.cpp updateLight moonEyeTint: the night light's hue of moonlight, faded out as the night light itself comes
// in); white for the sun. All 0 (the uniform unset) = white. The light's and the other body's sky irradiance are not tinted.
uniform vec3 wolfLightTint;
vec3 wolfLightTintOrWhite() {
  return any(greaterThan(wolfLightTint, vec3(0.0))) ? wolfLightTint : vec3(1.0);
}

// The same Bruneton lookups as takram's, in two directions: the light's sun + sky irradiance scaled, plus the other
// body's sky irradiance scaled. Scalar (clouds.vert, clouds.frag default) and cosine-weighted (ACCURATE_SUN_SKY_LIGHT).
vec3 wolfLightScalarIrradiance(const vec3 pointKm, out vec3 skyIrradiance) {
  float ls = wolfLightScale > 0.0 ? wolfLightScale : 1.0;
  vec3 sun = GetSunAndSkyScalarIrradiance(pointKm, sunDirection, skyIrradiance) * ls * wolfLightTintOrWhite();
  skyIrradiance *= ls;
  if (wolfOtherScale > 0.0) {
    vec3 skyOther;
    GetSunAndSkyScalarIrradiance(pointKm, wolfOtherDirection, skyOther);
    skyIrradiance += skyOther * wolfOtherScale;
  }
  return sun;
}
vec3 wolfLightIrradiance(const vec3 pointKm, const vec3 normal, out vec3 skyIrradiance) {
  float ls = wolfLightScale > 0.0 ? wolfLightScale : 1.0;
  vec3 sun = GetSunAndSkyIrradiance(pointKm, normal, sunDirection, skyIrradiance) * ls * wolfLightTintOrWhite();
  skyIrradiance *= ls;
  if (wolfOtherScale > 0.0) {
    vec3 skyOther;
    GetSunAndSkyIrradiance(pointKm, normal, wolfOtherDirection, skyOther);
    skyIrradiance += skyOther * wolfOtherScale;
  }
  return sun;
}
