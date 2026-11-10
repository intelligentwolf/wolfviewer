#!/usr/bin/env python3
"""
WolfViewer shader pre-compile check.

Builds every GLSL program the viewer can load, stitched exactly as the viewer
stitches it at run time, and compiles and links each one with glslangValidator.
A combination of files and defines that does not compile fails the build here
instead of in a resident's viewer (ticket 643: the Water vertex shader on a
GTX 1070 failed with "call to undefined function calcAtmospherics" with every
optional graphics feature switched off).

What is reproduced, and where it comes from (read, not guessed):

  * LLShaderMgr::loadShaderFile        indra/llrender/llshadermgr.cpp:547-1094
      - the class N..1 file search, the "#version" line chosen from the GLSL
        version, the injected "#define" block (stage define, GBUFFER_FLAG_*,
        the program's defines in std::map order, IS_AMD_CARD, the indexed
        texture diffuseLookup() prelude, the GBufferInfo struct), the
        [EXTRA_CODE_HERE] placement, "#wolf_include" expansion, the 1023-byte
        line pieces and the 8192-piece table limit.
  * LLShaderMgr::attachShaderFeatures  indra/llrender/llshadermgr.cpp:69-367
      - which shared objects a program's LLShaderFeatures attach.
  * LLGLSLShader::createShader         indra/llrender/llglslshader.cpp:407-561
      - the program's own files compiled with its mDefines and its indexed
        channels (0 for wolftk/ units), the shader-level fallback on link
        failure.
  * LLViewerShaderMgr                  indra/newview/llviewershadermgr.cpp
      - loadBasicShaders (the shared objects and their defines), every
        program set up by loadShadersWater/Effects/Interface/Object/Avatar/
        Deferred, make_rigged_variant, make_gltf_variants, wolf_tk_attach /
        wolf_tk_create, and the settings they read.
  * WolfTk::Atmosphere::loadShaders    indra/newview/wolftkatmosphere.cpp:636-688
  * WolfTk::Clouds::loadShaders        indra/newview/wolftkclouds.cpp:637-740, 1237-1251
  * The benchmark program               indra/newview/llglsandbox.cpp:1090-1096

Run-time inputs the viewer gets from the GPU (GLSL version, vendor, texture
image units) and from the settings (the graphics fallback ladder, shadows,
reflections, terrain detail, ...) are enumerated as scenarios; see
GPU_PROFILES and settings_scenarios(). Every program is also built at every
lower shader level down to 1, because LLGLSLShader::createShader falls back
to the next level when a link fails.

Usage:
    python3 scripts/shader_check.py                 # full check, exit 1 on any failure
    python3 scripts/shader_check.py --quick         # the four main scenarios on two GPUs
    python3 scripts/shader_check.py --spirv         # also report OpenGL SPIR-V (-G) readiness
    python3 scripts/shader_check.py --vulkan        # also report Vulkan SPIR-V (-V) readiness
    python3 scripts/shader_check.py --filter Water  # only programs whose name matches
    python3 scripts/shader_check.py --list          # the variants, without compiling
    python3 scripts/shader_check.py --dump out/     # write the failing stitched sources

Failures the viewer is known to tolerate on a given GPU are listed, with their
reason, in scripts/shader_check_known.txt (--known); they are reported but do not
fail the run. Everything else does.

A "#define" the file never names cannot change what the compiler sees, so by
default such lines are left out of the stitched text; that keeps the distinct
units in the low thousands instead of tens of thousands with the same result.
--no-prune keeps every line the viewer injects.

Standard library only. Needs glslangValidator (glslang 11 or later) on PATH,
or --glslang PATH, or $GLSLANG_VALIDATOR. The build runs it through
indra/cmake/ShaderCheck.cmake (target check_shaders, `ctest -R shader_check`);
CI runs it in .github/workflows/wolfviewer.yml (job "shaders" and every build).
"""

import argparse
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))   # the module beside this script, also under python3 -I
import shader_vulkan   # [VULKAN BACKEND 2026-11-08] the Vulkan GLSL transform and SPIR-V output (--vulkan-out)
import concurrent.futures
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field, replace

VERT, FRAG, GEOM = "vert", "frag", "geom"   # GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_GEOMETRY_SHADER

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SHADERS_DIR = os.path.normpath(os.path.join(HERE, "..", "indra", "newview", "app_settings", "shaders"))
SHADERS_ROOT = DEFAULT_SHADERS_DIR   # set from --shaders in main(); used to shorten paths in the report


# ---------------------------------------------------------------------------
# GPU profiles and settings
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class GpuProfile:
    name: str
    gl_version: float      # gGLManager.mGLVersion = major + minor * 0.1 (llgl.cpp:1132)
    glsl_major: int        # gGLManager.mGLSLVersionMajor (llgl.cpp:2719 parse_glsl_version)
    glsl_minor: int
    vendor: str            # "nvidia" | "amd" | "intel" | "apple" (llgl.cpp:1151-1176)
    darwin: bool           # LL_DARWIN
    texture_units: int     # gGLManager.mNumTextureImageUnits (llgl.cpp:1290)
    max_ubo: int           # gGLManager.mMaxUniformBlockSize, capped at 65536 (llgl.cpp:1297-1301)

    @property
    def is_nvidia(self):
        return self.vendor == "nvidia"

    @property
    def is_amd(self):
        return self.vendor == "amd"

    @property
    def old_select(self):
        # llglslshader.cpp:424-430 and llshadermgr.cpp:551-556: LL_DARWIN && !gGLManager.mIsApple
        return self.darwin and self.vendor != "apple"

    @property
    def has_cube_map_array(self):
        return self.gl_version >= 3.99   # llgl.cpp:1470


GPU_PROFILES = [
    # The ticket-643 machine: GTX 1070, desktop GL 4.6 / GLSL 4.60, 32 texture image units.
    GpuProfile("nvidia-gl46", 4.6, 4, 60, "nvidia", False, 32, 65536),
    GpuProfile("amd-gl46", 4.6, 4, 60, "amd", False, 32, 65536),
    GpuProfile("intel-gl46", 4.6, 4, 60, "intel", False, 32, 65536),
    # Apple GPUs: GL 4.1 / GLSL 4.10, 16 texture image units (llviewershadermgr.cpp:114, 1289).
    GpuProfile("apple-gl41", 4.1, 4, 10, "apple", True, 16, 65536),
    # Intel Macs: GL 4.1 with OLD_SELECT (llglslshader.cpp:424-430).
    GpuProfile("intelmac-gl41", 4.1, 4, 10, "intel", True, 16, 65536),
    # The oldest GL the version table serves: GL 3.3 / GLSL 3.30 ("#version 330", llshadermgr.cpp:688).
    GpuProfile("gl33", 3.3, 3, 30, "intel", False, 16, 16384),
]
QUICK_PROFILES = ["nvidia-gl46", "apple-gl41"]


@dataclass(frozen=True)
class Settings:
    """The gSavedSettings the shader loaders read. Defaults: app_settings/settings.xml."""
    name: str
    shadow_detail: int = 2          # RenderShadowDetail (0..2)
    ssr: bool = False               # RenderScreenSpaceReflections
    mirrors: bool = False           # RenderMirrors
    reflections_enabled: bool = True   # RenderReflectionsEnabled
    probe_level: int = 3            # RenderReflectionProbeLevel (0..3)
    terrain_detail: int = 0         # RenderTerrainPBRDetail (-4..0)
    terrain_mapping: int = 3        # RenderTerrainPBRPlanarSampleCount (1 or 3; 2 -> 1)
    triplanar_factor: float = 8.0   # RenderTerrainPBRTriplanarBlendFactor
    emissive: bool = True           # RenderEnableEmissiveBuffer
    ssao: bool = True               # RenderDeferredSSAO
    glow_noise: bool = True         # RenderGlowNoise
    gltf: bool = False              # GLTFEnabled
    terrain_paint: bool = False     # LocalTerrainPaintEnabled
    paint_bit_depth: int = 5        # TerrainPaintBitDepth
    native_sky: bool = True         # WolfEnvironmentNativeSky
    avatar_cloth: bool = False      # RenderAvatarCloth
    transparent_water: bool = True  # RenderTransparentWater
    light_max_level: int = 3        # RenderShaderLightingMaxLevel


def settings_scenarios():
    d = Settings("defaults")
    mx = replace(d, name="max", mirrors=True, ssr=True, reflections_enabled=True, probe_level=3,
                 shadow_detail=2, terrain_detail=0, gltf=True, terrain_paint=True, avatar_cloth=True)
    out = [d, mx]
    # The graphics fallback ladder, from "max", one rung at a time
    # (llviewershadermgr.cpp:172-179 sShaderFallbackSteps).
    s = mx
    for rung, change in [("mirrors", dict(mirrors=False)),
                         ("ssr", dict(ssr=False)),
                         ("reflections", dict(reflections_enabled=False)),
                         ("shadows", dict(shadow_detail=0)),
                         ("terrain", dict(terrain_detail=-4))]:
        s = replace(s, name="ladder-" + rung, **change)
        out.append(s)
    # Ticket 643: "every optional graphics feature already switched off" on otherwise default settings.
    alloff = replace(d, name="all-off", mirrors=False, ssr=False, reflections_enabled=False,
                     shadow_detail=0, terrain_detail=-4)
    out += [alloff,
            replace(alloff, name="all-off-no-native-sky", native_sky=False),
            replace(alloff, name="all-off-no-emissive-no-ssao", emissive=False, ssao=False)]
    # Everything off or at its lowest.
    out.append(Settings("min", shadow_detail=0, ssr=False, mirrors=False, reflections_enabled=False,
                        probe_level=0, terrain_detail=-4, terrain_mapping=1, emissive=False, ssao=False,
                        glow_noise=False, gltf=False, terrain_paint=False, native_sky=False,
                        avatar_cloth=False, transparent_water=False, light_max_level=1))
    # Each setting on its own, away from the defaults.
    singles = [
        dict(shadow_detail=1), dict(shadow_detail=0), dict(ssr=True), dict(mirrors=True),
        dict(reflections_enabled=False), dict(probe_level=0), dict(probe_level=1), dict(probe_level=2),
        dict(terrain_detail=-1), dict(terrain_detail=-2), dict(terrain_detail=-3), dict(terrain_detail=-4),
        dict(terrain_mapping=1), dict(emissive=False), dict(ssao=False), dict(glow_noise=False),
        dict(gltf=True), dict(terrain_paint=True), dict(terrain_paint=True, paint_bit_depth=8),
        dict(native_sky=False), dict(avatar_cloth=True), dict(transparent_water=False),
        dict(light_max_level=1), dict(light_max_level=2),
    ]
    for ch in singles:
        out.append(replace(d, name="one-" + "-".join(f"{k}={v}" for k, v in ch.items()), **ch))
    return out


QUICK_SETTINGS = ["defaults", "max", "all-off", "min"]


# ---------------------------------------------------------------------------
# Shader features and program definitions
# ---------------------------------------------------------------------------

@dataclass
class Features:
    # Source: indra/llrender/llglslshader.h:36-63 LLShaderFeatures
    mIndexedTextureChannels: int = 0
    calculatesLighting: bool = False
    calculatesAtmospherics: bool = False
    hasLighting: bool = False
    isAlphaLighting: bool = False
    isSpecular: bool = False
    hasTransport: bool = False
    hasSkinning: bool = False
    hasObjectSkinning: bool = False
    mGLTF: bool = False
    hasAtmospherics: bool = False
    hasGamma: bool = False
    hasShadows: bool = False
    hasAmbientOcclusion: bool = False
    hasSrgb: bool = False
    isDeferred: bool = False
    hasFullGBuffer: bool = False
    hasScreenSpaceReflections: bool = False
    hasAlphaMask: bool = False
    hasReflectionProbes: bool = False
    attachNothing: bool = False
    hasHeroProbes: bool = False
    isPBRTerrain: bool = False
    hasTonemap: bool = False


@dataclass
class Program:
    name: str
    files: list                 # [(path, stage)] in mShaderFiles order
    level: int                  # mShaderLevel
    features: Features = field(default_factory=Features)
    defines: dict = field(default_factory=dict)   # mDefines (std::map: emitted in sorted key order)
    group: str = ""             # which loadShaders* created it
    tolerated: bool = False     # True: the viewer logs and carries on without it; False: the group fails
    src: str = ""               # llviewershadermgr.cpp line of the setup

    def copy(self, **kw):
        p = replace(self, files=list(self.files), features=replace(self.features), defines=dict(self.defines))
        for k, v in kw.items():
            setattr(p, k, v)
        return p


def attach_features(f):
    """Source: llshadermgr.cpp:69-367 LLShaderMgr::attachShaderFeatures.
    Returns (vertex objects, fragment objects, tail vertex objects) in attach order: the C++ attaches the vertex
    objects (:76-112), then the fragment objects (:121-279), then one more vertex object - objects/indexedTextureV.glsl
    or objects/nonindexedTextureV.glsl (:283-294). [VULKAN BACKEND 2026-11-08] the order is part of the program's
    SPIR-V key (shader_vulkan.py), so the tail is kept apart instead of being folded into the vertex list."""
    vobj, fobj, tail = [], [], []
    if f.attachNothing:
        return vobj, fobj, tail
    if f.calculatesAtmospherics or f.hasGamma or f.isDeferred:
        vobj.append("windlight/atmosphericsVarsV.glsl")
    if f.calculatesLighting or f.calculatesAtmospherics:
        vobj.append("windlight/atmosphericsHelpersV.glsl")
    if f.calculatesLighting:
        if f.isSpecular:
            vobj.append("lighting/lightFuncSpecularV.glsl")
            if not f.isAlphaLighting:
                vobj.append("lighting/sumLightsSpecularV.glsl")
            vobj.append("lighting/lightSpecularV.glsl")
        else:
            vobj.append("lighting/lightFuncV.glsl")
            if not f.isAlphaLighting:
                vobj.append("lighting/sumLightsV.glsl")
            vobj.append("lighting/lightV.glsl")
    if f.calculatesAtmospherics:
        vobj.append("environment/srgbF.glsl")
        vobj.append("windlight/atmosphericsFuncs.glsl")
        vobj.append("windlight/atmosphericsV.glsl")
    if f.hasSkinning:
        vobj.append("avatar/avatarSkinV.glsl")
    if f.hasObjectSkinning:
        vobj.append("avatar/objectSkinV.glsl")
    vobj.append("deferred/textureUtilV.glsl")

    fobj.append("deferred/globalF.glsl")
    if f.hasSrgb or f.hasAtmospherics or f.calculatesAtmospherics or f.isDeferred:
        fobj.append("environment/srgbF.glsl")
    if f.calculatesAtmospherics or f.hasGamma or f.isDeferred:
        fobj.append("windlight/atmosphericsVarsF.glsl")
    if f.calculatesLighting or f.calculatesAtmospherics:
        fobj.append("windlight/atmosphericsHelpersF.glsl")
    if f.isDeferred or f.hasReflectionProbes:
        fobj.append("deferred/deferredUtil.glsl")
    if f.hasFullGBuffer:
        fobj.append("deferred/gbufferUtil.glsl")
    if f.hasScreenSpaceReflections or f.hasReflectionProbes:
        fobj.append("deferred/screenSpaceReflUtil.glsl")
    if f.hasShadows:
        fobj.append("deferred/shadowUtil.glsl")
    if f.hasReflectionProbes:
        fobj.append("deferred/reflectionProbeF.glsl")
    if f.hasAmbientOcclusion:
        fobj.append("deferred/aoUtil.glsl")
    if f.hasGamma or f.isDeferred:
        fobj.append("windlight/gammaF.glsl")
    if f.hasAtmospherics or f.isDeferred:
        fobj.append("windlight/atmosphericsFuncs.glsl")
        fobj.append("windlight/atmosphericsF.glsl")
    if f.isPBRTerrain:
        fobj.append("deferred/pbrterrainUtilF.glsl")
    if f.hasTonemap:
        fobj.append("deferred/tonemapUtilF.glsl")
    if f.hasAtmospherics:
        fobj.append("environment/waterFogF.glsl")
    if f.hasLighting:
        if f.mIndexedTextureChannels <= 1:
            fobj.append("lighting/lightAlphaMaskNonIndexedF.glsl" if f.hasAlphaMask else "lighting/lightNonIndexedF.glsl")
        else:
            fobj.append("lighting/lightAlphaMaskF.glsl" if f.hasAlphaMask else "lighting/lightF.glsl")
    if f.mIndexedTextureChannels <= 1:
        tail.append("objects/nonindexedTextureV.glsl")
    else:
        tail.append("objects/indexedTextureV.glsl")
    return vobj, fobj, tail


# Shader groups and their levels: llviewershadermgr.cpp:801-824 loadAllShaders
LIGHT_CLASS, INTERFACE_CLASS, ENV_CLASS, OBJ_CLASS, EFFECT_CLASS, WL_CLASS, WATER_CLASS, DEFERRED_CLASS = 3, 2, 2, 2, 2, 2, 3, 3
AVATAR_CLASS = 1               # llviewershadermgr.cpp:878
INDEXED_TEXTURE_CHANNELS = 4   # llviewershadermgr.cpp:941 LLGLSLShader::sIndexedTextureChannels = 4
MAX_JOINTS_PER_MESH_OBJECT = 110   # lljoint.h:48 LL_MAX_JOINTS_PER_MESH_OBJECT, via LLSkinningUtil::getMaxJointCount

# llviewershadermgr.cpp:359-367 TerrainPBRDetail, :369-375 TerrainPaintType, llviewershadermgr.h:33, llmaterial.h:51
TERRAIN_PBR_DETAIL_MAX, TERRAIN_PBR_DETAIL_EMISSIVE, TERRAIN_PBR_DETAIL_NORMAL = 0, 0, -2
TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS, TERRAIN_PBR_DETAIL_MIN = -3, -4
TERRAIN_PAINT_TYPE_COUNT, TERRAIN_PAINT_TYPE_PBR_PAINTMAP = 2, 1
LL_DEFERRED_MULTI_LIGHT_COUNT = 16
LLMATERIAL_SHADER_COUNT = 16
NUM_GLTF_VARIANTS = 16   # llglslshader.h:373; ALPHA_BLEND=1 RIGGED=2 UNLIT=4 MULTI_UV=8 (:367-370)
NORMAL_DEBUG_SHADER_COUNT, NORMAL_DEBUG_SHADER_WITH_TANGENTS = 2, 1   # llviewershadermgr.h:187-191
DIFFUSE_ALPHA_MODE_BLEND = 1   # llmaterial.h LLMaterial::DIFFUSE_ALPHA_MODE_BLEND
# llglslshader.cpp:74-85 gShaderConstsKey / gShaderConstsVal
SHADER_CONST_CLOUD_MOON_DEPTH = ("LL_SHADER_CONST_CLOUD_MOON_DEPTH", "0.99998")
SHADER_CONST_STAR_DEPTH = ("LL_SHADER_CONST_STAR_DEPTH", "0.99999")


def f32(x):
    """Round a Python float to IEEE single precision, as a C++ `constexpr F32` would hold it."""
    return struct.unpack("f", struct.pack("f", x))[0]


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def clamp_terrain_mapping(m):
    # llviewershadermgr.cpp:77-83
    m = clamp(m, 1, 3)
    return 1 if m == 2 else m


def terrain_sampler_count(detail):
    # llviewershadermgr.cpp:100-107
    n = 1 + 4
    if detail >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS:
        n += 4
    if detail >= TERRAIN_PBR_DETAIL_NORMAL:
        n += 4
    if detail >= TERRAIN_PBR_DETAIL_EMISSIVE:
        n += 4
    return n + 2 + 7 + 1


def clamp_terrain_detail_to_texture_units(detail, budget):
    # llviewershadermgr.cpp:126-143
    detail = clamp(detail, TERRAIN_PBR_DETAIL_MIN, TERRAIN_PBR_DETAIL_MAX)
    if budget <= 0:
        return detail
    while detail > TERRAIN_PBR_DETAIL_MIN and terrain_sampler_count(detail) > budget:
        detail -= 1
    return detail


def wolftk_atmosphere_defines():
    """Source: wolftkatmosphere.cpp:617-635 WolfTk::Atmosphere::addDefines (sizes: wolftkatmosphere.h:43-53)."""
    return {
        "PI": "3.141592653589793",
        "TRANSMITTANCE_TEXTURE_WIDTH": "256", "TRANSMITTANCE_TEXTURE_HEIGHT": "64",
        "SCATTERING_TEXTURE_R_SIZE": "32", "SCATTERING_TEXTURE_MU_SIZE": "128",
        "SCATTERING_TEXTURE_MU_S_SIZE": "32", "SCATTERING_TEXTURE_NU_SIZE": "8",
        "IRRADIANCE_TEXTURE_WIDTH": "64", "IRRADIANCE_TEXTURE_HEIGHT": "16",
        "METER_TO_LENGTH_UNIT": "0.0010000",
        "COMBINED_SCATTERING_TEXTURES": "1", "HAS_HIGHER_ORDER_SCATTERING_TEXTURE": "1",
    }


class Enumerator:
    """Everything LLViewerShaderMgr::setShaders would build for one GPU profile and one settings set."""

    def __init__(self, prof, st):
        self.prof = prof
        self.st = st
        self.programs = []
        self.shared = []   # [(filename, level, stage, index_channels)] - loadBasicShaders
        self.attribs = {}
        self.use_sun_shadow = DEFERRED_CLASS > 1 and st.shadow_detail > 0   # :1243, :1507
        self.has_reflection_probes = st.reflections_enabled and prof.gl_version > 3.99   # :1124
        self.wolf_water_full = True

    # -- helpers ---------------------------------------------------------------
    def add(self, p):
        self.programs.append(p)
        return p

    def common_permutations(self, p):
        # llviewershadermgr.cpp:535-543 add_common_permutations
        if self.st.emissive:
            p.defines["HAS_EMISSIVE"] = "1"

    def rigged_variant(self, p):
        # llviewershadermgr.cpp:470-484 make_rigged_variant
        r = p.copy(name="Skinned " + p.name)
        r.features.hasObjectSkinning = True
        r.defines["HAS_SKIN"] = "1"
        return self.add(r)

    def wolf_tk_attach(self, p):
        # llviewershadermgr.cpp:500-507 wolf_tk_attach
        if not self.st.native_sky:
            return
        p.files.append(("wolftk/atmosphereLibF.glsl", FRAG))
        p.defines["WOLF_TK_LIB"] = "1"
        p.defines.update(wolftk_atmosphere_defines())
        p.defines["GROUND"] = "1"

    def wolf_tk_create(self, p):
        # llviewershadermgr.cpp:509-533 wolf_tk_create: the program with the unit, and - if that will
        # not link at its level - the same program without it. Both are checked.
        self.add(p)
        if any(f == "wolftk/atmosphereLibF.glsl" for f, _ in p.files):
            q = p.copy(name=p.name + " (without aerial perspective unit)", tolerated=p.tolerated)
            q.files = [f for f in q.files if f[0] != "wolftk/atmosphereLibF.glsl"]
            q.defines.pop("WOLF_TK_LIB", None)
            # the unit's own defines stay in mDefines (only WOLF_TK_LIB is removed, :525)
            p.tolerated = True   # a failure here costs the aerial perspective, not the program
            self.add(q)

    # -- loadBasicShaders --------------------------------------------------------
    def basic_shaders(self):
        # llviewershadermgr.cpp:1066-1235
        st, prof = self.st, self.prof
        sum_lights_class = clamp(3, 1, st.light_max_level)
        a = {"MAX_JOINTS_PER_MESH_OBJECT": str(MAX_JOINTS_PER_MESH_OBJECT)}
        if st.emissive:
            a["HAS_EMISSIVE"] = "1"
        if st.shadow_detail >= 1:
            a["SUN_SHADOW"] = "1"
            if st.shadow_detail >= 2:
                a["SPOT_SHADOW"] = "1"
        if st.ssr:
            a["SSR"] = "1"
        if self.has_reflection_probes:
            a["REFMAP_LEVEL"] = str(clamp(st.probe_level, 0, 3))
            a["REF_SAMPLE_COUNT"] = "32"
        if st.mirrors:
            a["HERO_PROBES"] = "1"
        mapping = clamp_terrain_mapping(st.terrain_mapping)
        a["TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT"] = "%d" % mapping
        a["TERRAIN_TRIPLANAR_BLEND_FACTOR"] = "%.2f" % st.triplanar_factor
        requested = clamp(st.terrain_detail, TERRAIN_PBR_DETAIL_MIN, TERRAIN_PBR_DETAIL_MAX)
        a["TERRAIN_PBR_DETAIL"] = "%d" % clamp_terrain_detail_to_texture_units(requested, prof.texture_units)
        self.attribs = a

        wl, light = WL_CLASS, LIGHT_CLASS
        v = [("windlight/atmosphericsVarsV.glsl", wl), ("windlight/atmosphericsHelpersV.glsl", wl),
             ("lighting/lightFuncV.glsl", light), ("lighting/sumLightsV.glsl", sum_lights_class),
             ("lighting/lightV.glsl", light), ("lighting/lightFuncSpecularV.glsl", light),
             ("lighting/sumLightsSpecularV.glsl", sum_lights_class), ("lighting/lightSpecularV.glsl", light),
             ("windlight/atmosphericsFuncs.glsl", wl), ("windlight/atmosphericsV.glsl", wl),
             ("environment/srgbF.glsl", 1), ("avatar/avatarSkinV.glsl", 1), ("avatar/objectSkinV.glsl", 1),
             ("deferred/textureUtilV.glsl", 1)]
        if prof.glsl_major >= 2 or prof.glsl_minor >= 30:
            v.append(("objects/indexedTextureV.glsl", 1))
        v.append(("objects/nonindexedTextureV.glsl", 1))
        for name, lvl in v:
            self.shared.append((name, lvl, VERT, -1))

        ch = 1
        if prof.glsl_major > 1 or prof.glsl_minor >= 30:
            ch = max(INDEXED_TEXTURE_CHANNELS, 1)
        f = [("windlight/atmosphericsVarsF.glsl", wl, -1), ("windlight/atmosphericsHelpersF.glsl", wl, -1),
             ("windlight/gammaF.glsl", wl, -1), ("windlight/atmosphericsFuncs.glsl", wl, -1),
             ("windlight/atmosphericsF.glsl", wl, -1), ("environment/waterFogF.glsl", WATER_CLASS, -1),
             ("environment/srgbF.glsl", ENV_CLASS, -1), ("deferred/deferredUtil.glsl", 1, -1),
             ("deferred/gbufferUtil.glsl", 1, -1), ("deferred/globalF.glsl", 1, -1),
             ("deferred/shadowUtil.glsl", 1, -1), ("deferred/aoUtil.glsl", 1, -1),
             ("deferred/pbrterrainUtilF.glsl", 1, -1), ("deferred/tonemapUtilF.glsl", 1, -1),
             ("deferred/reflectionProbeF.glsl", 3 if self.has_reflection_probes else 2, -1),
             ("deferred/screenSpaceReflUtil.glsl", 3 if st.ssr else 1, -1),
             ("lighting/lightNonIndexedF.glsl", light, -1), ("lighting/lightAlphaMaskNonIndexedF.glsl", light, -1),
             ("lighting/lightF.glsl", light, ch), ("lighting/lightAlphaMaskF.glsl", light, ch)]
        for name, lvl, ich in f:
            self.shared.append((name, lvl, FRAG, ich))

    # -- loadShadersWater --------------------------------------------------------
    def water(self):
        # llviewershadermgr.cpp:1237-1405
        st, prof = self.st, self.prof
        uss = self.use_sun_shadow
        p = Program("Water Shader", [("environment/waterV.glsl", VERT), ("environment/waterF.glsl", FRAG)],
                    WATER_CLASS, group="water", src=":1264")
        p.features.calculatesAtmospherics = True
        p.features.hasAtmospherics = True
        p.features.hasGamma = True
        p.features.hasSrgb = True
        p.features.hasReflectionProbes = True
        p.features.hasTonemap = True
        p.features.hasShadows = uss
        if st.transparent_water:
            p.defines["TRANSPARENT_WATER"] = "1"
        if uss:
            p.defines["HAS_SUN_SHADOW"] = "1"
        OURS = 7
        before_ours = 13 + (6 if uss else 0)
        units = prof.texture_units
        self.wolf_water_full = units <= 0 or units >= before_ours + OURS   # :1300-1303
        if self.wolf_water_full:
            p.defines["WOLF_WATER_FULL"] = "1"
        self.wolf_tk_attach(p)
        self.wolf_tk_create(p)

        u = Program("Underwater Shader", [("environment/waterV.glsl", VERT), ("environment/underWaterF.glsl", FRAG)],
                    WATER_CLASS, group="water", src=":1331")
        u.features.calculatesAtmospherics = True
        u.features.hasAtmospherics = True
        if self.wolf_water_full:
            u.defines["WOLF_WATER_FULL"] = "1"
        if st.transparent_water:
            u.defines["TRANSPARENT_WATER"] = "1"
        self.add(u)

        passes = [("Wolf FFT Time Shader", "environment/wolffftV.glsl", "environment/wolffftTimeF.glsl"),
                  ("Wolf FFT Butterfly Shader", "environment/wolffftV.glsl", "environment/wolffftButterflyF.glsl"),
                  ("Wolf FFT Final Shader", "environment/wolffftV.glsl", "environment/wolffftFinalF.glsl"),
                  ("Wolf Wake Decay Shader", "environment/wolffftV.glsl", "environment/wolfwakeDecayF.glsl"),
                  ("Wolf Wake Stamp Shader", "environment/wolfwakeStampV.glsl", "environment/wolfwakeStampF.glsl"),
                  ("Wolf Surf Curl Shader", "environment/wolfsurfcurlV.glsl", "environment/wolfsurfcurlF.glsl")]
        for name, vs, fs in passes:   # :1355-1378
            q = Program(name, [(vs, VERT), (fs, FRAG)], WATER_CLASS, group="water", src=":1356")
            if name == "Wolf Surf Curl Shader":
                self.wolf_tk_attach(q)
            self.wolf_tk_create(q)

    # -- loadShadersEffects ------------------------------------------------------
    def effects(self):
        # llviewershadermgr.cpp:1407-1502
        lvl = EFFECT_CLASS
        self.add(Program("Glow Shader (Post)", [("effects/glowV.glsl", VERT), ("effects/glowF.glsl", FRAG)], lvl,
                         group="effects", src=":1424"))
        p = Program("Glow Extract Shader (Post)", [("effects/glowExtractV.glsl", VERT), ("effects/glowExtractF.glsl", FRAG)],
                    lvl, group="effects", src=":1441")
        if self.st.glow_noise:
            p.name += " (+Noise)"
            p.defines["HAS_NOISE"] = "1"
        self.add(p)
        self.add(Program("Vignette Post", [("post/exoPostBaseV.glsl", VERT), ("post/exoVignetteF.glsl", FRAG)], lvl,
                         group="effects", src=":1462"))
        self.add(Program("Snapshot Frame Post", [("post/exoPostBaseV.glsl", VERT), ("post/snapshotFrameF.glsl", FRAG)], lvl,
                         group="effects", src=":1473"))
        self.add(Program("Wolf Photo Filter Post", [("post/exoPostBaseV.glsl", VERT), ("post/wolfPhotoFilterF.glsl", FRAG)], lvl,
                         group="effects", tolerated=True, src=":1488"))

    # -- loadShadersInterface ----------------------------------------------------
    def interface(self):
        # llviewershadermgr.cpp:3784-4181
        st, prof = self.st, self.prof
        lvl = INTERFACE_CLASS
        I = "interface/"

        def simple(name, vs, fs, src, **kw):
            return Program(name, [(I + vs, VERT), (I + fs, FRAG)], lvl, group="interface", src=src, **kw)

        p = simple("Highlight Shader", "highlightV.glsl", "highlightF.glsl", ":3791")
        self.rigged_variant(p)
        self.add(p)
        self.add(simple("Highlight Normals Shader", "highlightNormV.glsl", "highlightF.glsl", ":3802"))
        self.add(simple("Highlight Spec Shader", "highlightSpecV.glsl", "highlightF.glsl", ":3812"))
        self.add(simple("UI Shader", "uiV.glsl", "uiF.glsl", ":3822"))
        self.add(simple("Pathfinding Shader", "pathfindingV.glsl", "pathfindingF.glsl", ":3832"))
        self.add(simple("PathfindingNoNormals Shader", "pathfindingNoNormalV.glsl", "pathfindingF.glsl", ":3842"))
        self.add(simple("Glow Combine Shader", "glowcombineV.glsl", "glowcombineF.glsl", ":3852"))
        self.add(simple("Glow CombineFXAA Shader", "glowcombineFXAAV.glsl", "glowcombineFXAAF.glsl", ":3869"))
        # LL_WINDOWS only (:3884-3916); checked everywhere, the files are platform-neutral GLSL
        self.add(simple("Two Texture Compare Shader", "twotexturecompareV.glsl", "twotexturecompareF.glsl", ":3887 (Windows)"))
        self.add(simple("One Texture Filter Shader", "onetexturefilterV.glsl", "onetexturefilterF.glsl", ":3904 (Windows)"))
        self.add(simple("Solid Color Shader", "solidcolorV.glsl", "solidcolorF.glsl", ":3920"))
        self.add(simple("Occlusion Shader", "occlusionV.glsl", "occlusionF.glsl", ":3936"))
        p = simple("Skinned Occlusion Shader", "occlusionSkinnedV.glsl", "occlusionF.glsl", ":3947")
        p.features.hasObjectSkinning = True
        self.add(p)
        self.add(simple("Occlusion Cube Shader", "occlusionCubeV.glsl", "occlusionF.glsl", ":3958"))
        p = simple("Debug Shader", "debugV.glsl", "debugF.glsl", ":3968")
        self.rigged_variant(p)
        self.add(p)
        for variant in range(NORMAL_DEBUG_SHADER_COUNT):   # :3980-4002
            p = Program("Normal Debug Shader", [(I + "normaldebugV.glsl", VERT), (I + "normaldebugG.glsl", GEOM),
                                                (I + "normaldebugF.glsl", FRAG)], lvl, group="interface", src=":3984")
            if variant == NORMAL_DEBUG_SHADER_WITH_TANGENTS:
                p.name += " (tangents)"
                p.defines["HAS_ATTRIBUTE_TANGENT"] = "1"
            self.rigged_variant(p)
            self.add(p)
        self.add(simple("Clip Shader", "clipV.glsl", "clipF.glsl", ":4007"))
        self.add(simple("Benchmark Shader", "benchmarkV.glsl", "benchmarkF.glsl", ":4017"))
        p = simple("Reflection Probe Display Shader", "reflectionprobeV.glsl", "reflectionprobeF.glsl", ":4027")
        p.features.hasReflectionProbes = True
        p.features.hasSrgb = True
        p.features.calculatesAtmospherics = True
        p.features.hasAtmospherics = True
        p.features.hasGamma = True
        p.features.isDeferred = True
        self.add(p)
        self.add(simple("Copy Shader", "copyV.glsl", "copyF.glsl", ":4043"))
        p = simple("Copy Depth Shader", "copyV.glsl", "copyF.glsl", ":4053")
        p.defines["COPY_DEPTH"] = "1"
        self.add(p)
        self.add(Program("Draw Color Shader", [("objects/simpleNoAtmosV.glsl", VERT), ("objects/simpleColorF.glsl", FRAG)],
                         OBJ_CLASS, group="interface", src=":4065"))
        if st.terrain_paint:   # :4074-4101
            bit_depth = clamp(st.paint_bit_depth, 1, 8)
            p = simple("Terrain Bake Shader RGB%o" % bit_depth, "pbrTerrainBakeV.glsl", "pbrTerrainBakeF.glsl", ":4078",
                       tolerated=True)
            p.features.isPBRTerrain = True
            p.defines["TERRAIN_PAINT_PRECISION"] = "%d" % ((1 << bit_depth) - 1)
            self.add(p)
        self.add(simple("Alpha Mask Shader", "alphamaskV.glsl", "alphamaskF.glsl", ":4105"))
        for name, fs, src in [("Reflection Mip Shader", "reflectionmipF.glsl", ":4115"),
                              ("Reflection Mip Shader (gaussian)", "gaussianF.glsl", ":4129")]:
            p = simple(name, "splattexturerectV.glsl", fs, src)
            p.features.isDeferred = True
            p.features.hasGamma = True
            p.features.hasAtmospherics = True
            p.features.calculatesAtmospherics = True
            self.add(p)
        if prof.has_cube_map_array:   # :4141-4172
            p = simple("Radiance Gen Shader", "radianceGenV.glsl", "radianceGenF.glsl", ":4143")
            p.defines["PROBE_FILTER_SAMPLES"] = "32"
            self.add(p)
            p = simple("Hero Radiance Gen Shader", "radianceGenV.glsl", "radianceGenF.glsl", ":4154")
            p.defines["HERO_PROBES"] = "1"
            p.defines["PROBE_FILTER_SAMPLES"] = "4"
            self.add(p)
            self.add(simple("Irradiance Gen Shader", "irradianceGenV.glsl", "irradianceGenF.glsl", ":4166"))
        # llglsandbox.cpp:1090-1096: the GPU benchmark builds the same files at level 1 with attachNothing
        p = Program("Benchmark Shader (llglsandbox)", [(I + "benchmarkV.glsl", VERT), (I + "benchmarkF.glsl", FRAG)], 1,
                    group="interface", tolerated=True, src="llglsandbox.cpp:1090")
        p.features.attachNothing = True
        self.add(p)

    # -- loadShadersObject -------------------------------------------------------
    def objects(self):
        # llviewershadermgr.cpp:3629-3719
        lvl = OBJ_CLASS
        p = Program("Bump Shader", [("objects/bumpV.glsl", VERT), ("objects/bumpF.glsl", FRAG)], lvl, group="object", src=":3636")
        self.rigged_variant(p)
        self.add(p)
        p = Program("No color alpha mask Shader", [("objects/simpleNoColorV.glsl", VERT), ("objects/simpleF.glsl", FRAG)], lvl,
                    group="object", src=":3658")
        p.features.calculatesLighting = True
        p.features.calculatesAtmospherics = True
        p.features.hasGamma = True
        p.features.hasAtmospherics = True
        p.features.hasLighting = True
        p.features.hasAlphaMask = True
        self.add(p)
        p = Program("Impostor Shader", [("objects/impostorV.glsl", VERT), ("objects/impostorF.glsl", FRAG)], lvl,
                    group="object", src=":3674")
        p.features.hasSrgb = True
        self.add(p)
        p = Program("Object Preview Shader", [("objects/previewV.glsl", VERT), ("objects/previewF.glsl", FRAG)], lvl,
                    group="object", src=":3685")
        self.rigged_variant(p)
        self.add(p)
        self.add(Program("Preview Physics Shader", [("objects/previewPhysicsV.glsl", VERT), ("objects/previewPhysicsF.glsl", FRAG)],
                         lvl, group="object", src=":3698"))

    # -- loadShadersAvatar -------------------------------------------------------
    def avatar(self):
        # llviewershadermgr.cpp:3721-3782
        lvl = AVATAR_CLASS
        p = Program("Avatar Shader", [("avatar/avatarV.glsl", VERT), ("avatar/avatarF.glsl", FRAG)], lvl, group="avatar", src=":3736")
        p.features.hasSkinning = True
        p.features.calculatesAtmospherics = True
        p.features.calculatesLighting = True
        p.features.hasGamma = True
        p.features.hasAtmospherics = True
        p.features.hasLighting = True
        p.features.hasAlphaMask = True
        self.add(p)
        p = Program("Avatar Eyeball Program", [("avatar/eyeballV.glsl", VERT), ("avatar/eyeballF.glsl", FRAG)], lvl,
                    group="avatar", src=":3759")
        p.features.calculatesLighting = True
        p.features.isSpecular = True
        p.features.calculatesAtmospherics = True
        p.features.hasGamma = True
        p.features.hasAtmospherics = True
        p.features.hasLighting = True
        p.features.hasAlphaMask = True
        self.add(p)

    # -- loadShadersDeferred -----------------------------------------------------
    def deferred(self):
        # llviewershadermgr.cpp:1504-3627
        st, prof = self.st, self.prof
        lvl = DEFERRED_CLASS
        uss = self.use_sun_shadow
        D = "deferred/"

        def prog(name, vs, fs, src, **kw):
            return Program(name, [(D + vs, VERT), (D + fs, FRAG)], lvl, group="deferred", src=src, **kw)

        def alpha_lit(p):   # the "alpha" feature set used at :1891-1900, :2223-2232, :2832-2841
            p.features.calculatesLighting = False
            p.features.hasLighting = False
            p.features.isAlphaLighting = True
            p.features.hasSrgb = True
            p.features.calculatesAtmospherics = True
            p.features.hasAtmospherics = True
            p.features.hasGamma = True

        p = Program("Deferred Highlight Shader", [("interface/highlightV.glsl", VERT), (D + "highlightF.glsl", FRAG)],
                    INTERFACE_CLASS, group="deferred", src=":1631")
        self.common_permutations(p)
        self.add(p)

        p = prog("Deferred Diffuse Shader", "diffuseV.glsl", "diffuseIndexedF.glsl", ":1642")
        p.features.hasSrgb = True
        p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)

        p = prog("Deferred Diffuse Alpha Mask Shader", "diffuseV.glsl", "diffuseAlphaMaskIndexedF.glsl", ":1656")
        p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)

        p = prog("Deferred Diffuse Non-Indexed Alpha Mask Shader", "diffuseV.glsl", "diffuseAlphaMaskF.glsl", ":1669")
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Diffuse Non-Indexed Alpha Mask No Color Shader", "diffuseNoColorV.glsl", "diffuseAlphaMaskNoColorF.glsl", ":1681")
        self.common_permutations(p)
        self.add(p)
        # [UNDERSEA 2026-11-09] ticket 652: the sea plants' sway program (llviewershadermgr.cpp after the No Color one);
        # its failing only loses the sway, but it must compile like every other program
        p = prog("Wolf Under Sea Plants Shader", "wolfUnderSeaV.glsl", "diffuseAlphaMaskF.glsl", ":1726")
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Bump Shader", "bumpV.glsl", "bumpF.glsl", ":1693")
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)

        for i in range(LLMATERIAL_SHADER_COUNT * 2):   # :1704-1798
            has_skin = bool(i & 0x10)
            alpha_mode = i & 0x3
            p = prog(("Skinned Material Shader %d" if has_skin else "Material Shader %d") % i, "materialV.glsl", "materialF.glsl", ":1713")
            # :1704-1711 set hasLighting = false on 1,5,9,13 (+16); every other index keeps the default (false)
            if i & 0x8:
                p.defines["HAS_NORMAL_MAP"] = "1"
            if i & 0x4:
                p.defines["HAS_SPECULAR_MAP"] = "1"
            p.defines["DIFFUSE_ALPHA_MODE"] = "%d" % alpha_mode
            if alpha_mode != 0:
                p.features.hasAlphaMask = True
                p.defines["HAS_ALPHA_MASK"] = "1"
            if uss:
                p.defines["HAS_SUN_SHADOW"] = "1"
            self.common_permutations(p)
            p.features.hasSrgb = True
            p.features.calculatesAtmospherics = True
            p.features.hasAtmospherics = True
            p.features.hasGamma = True
            p.features.hasShadows = uss
            p.features.hasReflectionProbes = True
            if has_skin:
                p.defines["HAS_SKIN"] = "1"
                p.features.hasObjectSkinning = True
            if not has_skin and alpha_mode == DIFFUSE_ALPHA_MODE_BLEND:
                self.wolf_tk_attach(p)
            self.wolf_tk_create(p)

        p = prog("Deferred PBR Opaque Shader", "pbropaqueV.glsl", "pbropaqueF.glsl", ":1802")
        p.features.hasSrgb = True
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)

        if st.gltf:   # :1821-1847 make_gltf_variants (:546-639)
            base = Program("GLTF PBR Metallic Roughness Shader", [("gltf/pbrmetallicroughnessV.glsl", VERT),
                                                                  ("gltf/pbrmetallicroughnessF.glsl", FRAG)], lvl,
                           group="deferred", tolerated=True, src=":1825")
            base.features.hasSrgb = True
            self.common_permutations(base)
            base.features.mGLTF = True
            for i in range(NUM_GLTF_VARIANTS):
                alpha_blend, rigged, unlit, multi_uv = bool(i & 1), bool(i & 2), bool(i & 4), bool(i & 8)
                v = base.copy(name="GLTF PBR Metallic Roughness Shader (variant %d%s%s%s%s)" % (
                    i, " alpha" if alpha_blend else "", " rigged" if rigged else "", " unlit" if unlit else "",
                    " multiuv" if multi_uv else ""))
                v.defines["MAX_NODES_PER_GLTF_OBJECT"] = str(prof.max_ubo // (16 * 3))
                v.defines["MAX_MATERIALS_PER_GLTF_OBJECT"] = str(prof.max_ubo // (16 * 12))
                v.defines["MAX_UBO_VEC4S"] = str(prof.max_ubo // 16)
                if rigged:
                    v.defines["HAS_SKIN"] = "1"
                if unlit:
                    v.defines["UNLIT"] = "1"
                if multi_uv:
                    v.defines["MULTI_UV"] = "1"
                if alpha_blend:
                    v.defines["ALPHA_BLEND"] = "1"
                    alpha_lit(v)
                    v.features.hasShadows = uss
                    v.features.isDeferred = True
                    v.features.hasReflectionProbes = True
                    if uss:
                        v.defines["HAS_SUN_SHADOW"] = "1"
                self.add(v)

        p = prog(" PBR Glow Shader", "pbrglowV.glsl", "pbrglowF.glsl", ":1851")
        p.features.hasSrgb = True
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)

        p = prog("HUD PBR Opaque Shader", "pbropaqueV.glsl", "pbropaqueF.glsl", ":1870")
        p.features.hasSrgb = True
        p.defines["IS_HUD"] = "1"
        self.common_permutations(p)
        self.add(p)

        p = prog("Deferred PBR Alpha Shader", "pbralphaV.glsl", "pbralphaF.glsl", ":1888")
        alpha_lit(p)
        p.features.hasShadows = uss
        p.features.isDeferred = True
        p.features.hasReflectionProbes = lvl > 0   # :1900 (S32 -> bool)
        p.defines["DIFFUSE_ALPHA_MODE"] = "%d" % DIFFUSE_ALPHA_MODE_BLEND
        p.defines["HAS_NORMAL_MAP"] = "1"
        p.defines["HAS_SPECULAR_MAP"] = "1"
        p.defines["HAS_EMISSIVE_MAP"] = "1"
        p.defines["USE_VERTEX_COLOR"] = "1"
        self.common_permutations(p)
        if uss:
            p.defines["HAS_SUN_SHADOW"] = "1"
        self.rigged_variant(p)
        self.wolf_tk_attach(p)
        self.wolf_tk_create(p)

        p = prog("HUD PBR Alpha Shader", "pbralphaV.glsl", "pbralphaF.glsl", ":1942")
        p.features.hasSrgb = True
        p.defines["IS_HUD"] = "1"
        self.common_permutations(p)
        self.add(p)

        detail = clamp_terrain_detail_to_texture_units(st.terrain_detail, prof.texture_units)   # :1964
        mapping = clamp_terrain_mapping(st.terrain_mapping)
        for paint_type in range(TERRAIN_PAINT_TYPE_COUNT):   # :1967-1994
            p = prog("Deferred PBR Terrain Shader %d %s %s" % (
                detail, "paintmap" if paint_type == TERRAIN_PAINT_TYPE_PBR_PAINTMAP else "heightmap-with-noise",
                "flat" if mapping == 1 else "triplanar"), "pbrterrainV.glsl", "pbrterrainF.glsl", ":1969")
            p.features.hasSrgb = True
            p.features.isAlphaLighting = True
            p.features.calculatesAtmospherics = True
            p.features.hasAtmospherics = True
            p.features.hasGamma = True
            p.features.hasTransport = True
            p.features.isPBRTerrain = True
            p.defines["TERRAIN_PBR_DETAIL"] = "%d" % detail
            p.defines["TERRAIN_PAINT_TYPE"] = "%d" % paint_type
            p.defines["TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT"] = "%d" % mapping
            self.common_permutations(p)
            self.add(p)

        p = prog("Deferred Tree Shader", "treeV.glsl", "treeF.glsl", ":1999")
        self.common_permutations(p)
        self.add(p)
        self.add(prog("Deferred Tree Shadow Shader", "treeShadowV.glsl", "treeShadowF.glsl", ":2012"))
        p = prog("Deferred Skinned Tree Shadow Shader", "treeShadowSkinnedV.glsl", "treeShadowF.glsl", ":2024")
        p.features.hasObjectSkinning = True
        self.add(p)
        p = prog("Deferred Impostor Shader", "impostorV.glsl", "impostorF.glsl", ":2036")
        p.features.hasSrgb = True
        self.common_permutations(p)
        self.add(p)

        def light_features(p):   # :2052-2055 and the same four at :2075-2078, :2098-2101, :2117-2120
            p.features.isDeferred = True
            p.features.hasFullGBuffer = True
            p.features.hasShadows = True
            p.features.hasSrgb = True

        p = prog("Deferred Light Shader", "pointLightV.glsl", "pointLightF.glsl", ":2051")
        light_features(p)
        self.common_permutations(p)
        self.add(p)
        for i in range(LL_DEFERRED_MULTI_LIGHT_COUNT):   # :2070-2092
            p = prog("Deferred MultiLight Shader %d" % i, "multiPointLightV.glsl", "multiPointLightF.glsl", ":2074")
            light_features(p)
            p.defines["LIGHT_COUNT"] = "%d" % (i + 1)
            self.common_permutations(p)
            self.add(p)
        p = prog("Deferred SpotLight Shader", "pointLightV.glsl", "spotLightF.glsl", ":2096")
        light_features(p)
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred MultiSpotLight Shader", "multiPointLightV.glsl", "spotLightF.glsl", ":2116")
        light_features(p)
        p.defines["MULTI_SPOTLIGHT"] = "1"
        self.common_permutations(p)
        self.add(p)

        p = prog("Deferred Sun Shader", "sunLightV.glsl", "sunLightSSAOF.glsl" if st.ssao else "sunLightF.glsl", ":2148")
        p.features.isDeferred = True
        p.features.hasShadows = True
        p.features.hasAmbientOcclusion = st.ssao
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Sun Probe Shader", "sunLightV.glsl", "sunLightF.glsl", ":2166")
        p.features.isDeferred = True
        p.features.hasShadows = True
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Blur Light Shader", "blurLightV.glsl", "blurLightF.glsl", ":2183")
        p.features.isDeferred = True
        self.common_permutations(p)
        self.add(p)

        for i in range(3):   # :2199-2271 Deferred Alpha / Skinned Deferred Alpha / HUD Alpha
            rigged, hud = i == 1, i == 2
            p = prog("HUD Alpha Shader" if hud else ("Skinned Deferred Alpha Shader" if rigged else "Deferred Alpha Shader"),
                     "alphaV.glsl", "alphaF.glsl", ":2207")
            if rigged:
                p.features.hasObjectSkinning = True
            alpha_lit(p)
            p.features.hasShadows = uss
            p.features.hasReflectionProbes = True
            p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
            p.defines["USE_VERTEX_COLOR"] = "1"
            p.defines["HAS_ALPHA_MASK"] = "1"
            p.defines["USE_INDEXED_TEX"] = "1"
            if uss:
                p.defines["HAS_SUN_SHADOW"] = "1"
            self.common_permutations(p)
            if rigged:
                p.defines["HAS_SKIN"] = "1"
            if hud:
                p.defines["IS_HUD"] = "1"
            if not rigged and not hud:
                self.wolf_tk_attach(p)
            self.wolf_tk_create(p)

        for rigged in (False, True):   # :2273-2332
            p = prog("Skinned Deferred Alpha Impostor Shader" if rigged else "Deferred Alpha Impostor Shader",
                     "alphaV.glsl", "alphaF.glsl", ":2285")
            p.features.hasSrgb = True
            p.features.isAlphaLighting = True
            p.features.hasShadows = uss
            p.features.hasReflectionProbes = True
            p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
            p.defines["USE_INDEXED_TEX"] = "1"
            p.defines["FOR_IMPOSTOR"] = "1"
            p.defines["HAS_ALPHA_MASK"] = "1"
            p.defines["USE_VERTEX_COLOR"] = "1"
            if rigged:
                p.features.hasObjectSkinning = True
                p.defines["HAS_SKIN"] = "1"
            if uss:
                p.defines["HAS_SUN_SHADOW"] = "1"
            self.common_permutations(p)
            self.add(p)

        p = prog("Deferred Avatar Eyes Shader", "avatarEyesV.glsl", "diffuseF.glsl", ":2336")
        p.features.calculatesAtmospherics = True
        p.features.hasGamma = True
        p.features.hasAtmospherics = True
        p.features.hasSrgb = True
        p.features.hasShadows = True
        self.common_permutations(p)
        self.add(p)

        def fullbright(name, vs, fs, src, indexed=True, **kw):
            p = prog(name, vs, fs, src, **kw)
            p.features.calculatesAtmospherics = True
            p.features.hasGamma = True
            p.features.hasAtmospherics = True
            p.features.hasSrgb = True
            p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
            return p

        p = fullbright("Deferred Fullbright Shader", "fullbrightV.glsl", "fullbrightF.glsl", ":2356")
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)
        p = fullbright("HUD Fullbright Shader", "fullbrightV.glsl", "fullbrightF.glsl", ":2376")
        p.defines["IS_HUD"] = "1"
        self.common_permutations(p)
        self.add(p)
        p = fullbright("Deferred Fullbright Alpha Masking Shader", "fullbrightV.glsl", "fullbrightF.glsl", ":2397")
        p.defines["HAS_ALPHA_MASK"] = "1"
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)
        p = fullbright("HUD Fullbright Alpha Masking Shader", "fullbrightV.glsl", "fullbrightF.glsl", ":2419")
        p.defines["HAS_ALPHA_MASK"] = "1"
        p.defines["IS_HUD"] = "1"
        self.common_permutations(p)
        self.add(p)
        p = fullbright("Deferred Fullbright Alpha Masking Alpha Shader", "fullbrightV.glsl", "fullbrightF.glsl", ":2441")
        p.features.isDeferred = True
        p.defines["HAS_ALPHA_MASK"] = "1"
        p.defines["IS_ALPHA"] = "1"
        self.common_permutations(p)
        self.rigged_variant(p)
        self.wolf_tk_attach(p)
        self.wolf_tk_create(p)
        p = fullbright("HUD Fullbright Alpha Masking Alpha Shader", "fullbrightV.glsl", "fullbrightF.glsl", ":2466")
        p.features.isDeferred = True
        p.defines["HAS_ALPHA_MASK"] = "1"
        p.defines["IS_ALPHA"] = "1"
        p.defines["IS_HUD"] = "1"
        self.common_permutations(p)
        self.add(p)
        p = fullbright("Deferred FullbrightShiny Shader", "fullbrightShinyV.glsl", "fullbrightShinyF.glsl", ":2490")
        p.features.hasReflectionProbes = True
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)
        p = fullbright("HUD FullbrightShiny Shader", "fullbrightShinyV.glsl", "fullbrightShinyF.glsl", ":2511")
        p.features.hasReflectionProbes = True
        p.defines["IS_HUD"] = "1"
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Emissive Shader", "emissiveV.glsl", "emissiveF.glsl", ":2533")
        p.features.calculatesAtmospherics = True
        p.features.hasGamma = True
        p.features.hasAtmospherics = True
        p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)

        p = prog("Deferred Soften Shader", "softenLightV.glsl", "softenLightF.glsl", ":2552")
        p.features.hasSrgb = True
        p.features.calculatesAtmospherics = True
        p.features.hasAtmospherics = True
        p.features.hasGamma = True
        p.features.isDeferred = True
        p.features.hasFullGBuffer = True
        p.features.hasShadows = uss
        p.features.hasReflectionProbes = lvl > 2
        self.common_permutations(p)
        if uss:
            p.defines["HAS_SUN_SHADOW"] = "1"
        if st.ssao:
            p.level = max(p.level, 2)
            p.defines["HAS_SSAO"] = "1"
        self.wolf_tk_attach(p)
        self.wolf_tk_create(p)

        for name, vs, fs, src in [("Haze Shader", "softenLightV.glsl", "hazeF.glsl", ":2588"),
                                  ("Water Haze Shader", "waterHazeV.glsl", "waterHazeF.glsl", ":2613")]:
            p = prog(name, vs, fs, src)
            p.features.hasSrgb = True
            p.features.calculatesAtmospherics = True
            p.features.hasAtmospherics = True
            p.features.hasGamma = True
            p.features.isDeferred = True
            p.features.hasShadows = uss
            p.features.hasReflectionProbes = lvl > 2
            self.common_permutations(p)
            self.add(p)

        self.add(prog("Deferred Shadow Shader", "shadowV.glsl", "shadowF.glsl", ":2639"))
        p = prog("Deferred Skinned Shadow Shader", "shadowSkinnedV.glsl", "shadowF.glsl", ":2651")
        p.features.isDeferred = True
        p.features.hasShadows = True
        p.features.hasObjectSkinning = True
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Shadow Cube Shader", "shadowCubeV.glsl", "shadowF.glsl", ":2669")
        p.features.isDeferred = True
        p.features.hasShadows = True
        self.add(p)
        p = prog("Deferred Shadow Fullbright Alpha Mask Shader", "shadowAlphaMaskV.glsl", "shadowAlphaMaskF.glsl", ":2683")
        p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
        p.defines["DEPTH_CLAMP"] = "1"
        p.defines["IS_FULLBRIGHT"] = "1"
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)
        p = prog("Deferred Shadow Alpha Mask Shader", "shadowAlphaMaskV.glsl", "shadowAlphaMaskF.glsl", ":2704")
        p.features.mIndexedTextureChannels = INDEXED_TEXTURE_CHANNELS
        self.rigged_variant(p)
        self.add(p)
        p = prog("Deferred GLTF Shadow Alpha Mask Shader", "pbrShadowAlphaMaskV.glsl", "pbrShadowAlphaMaskF.glsl", ":2719")
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)
        p = prog("Deferred GLTF Shadow Alpha Blend Shader", "pbrShadowAlphaMaskV.glsl", "pbrShadowAlphaBlendF.glsl", ":2735")
        self.common_permutations(p)
        self.rigged_variant(p)
        self.add(p)
        for name, vs, fs, src in [("Deferred Avatar Shadow Shader", "avatarShadowV.glsl", "avatarShadowF.glsl", ":2751"),
                                  ("Deferred Avatar Alpha Shadow Shader", "avatarAlphaShadowV.glsl", "avatarAlphaShadowF.glsl", ":2764"),
                                  ("Deferred Avatar Alpha Mask Shadow Shader", "avatarAlphaShadowV.glsl", "avatarAlphaMaskShadowF.glsl", ":2775")]:
            p = prog(name, vs, fs, src)
            p.features.hasSkinning = True
            self.add(p)

        p = prog("Deferred Terrain Shader", "terrainV.glsl", "terrainF.glsl", ":2787")
        p.features.hasSrgb = True
        p.features.isAlphaLighting = True
        p.features.calculatesAtmospherics = True
        p.features.hasAtmospherics = True
        p.features.hasGamma = True
        self.common_permutations(p)
        if not prof.darwin and prof.texture_units > 16:   # wolfterrainlayers.cpp:622-630 WolfTerrainLayers::supported
            p.defines["WOLF_TERRAIN_LAYERS"] = "1"
        self.add(p)

        p = prog("Deferred Avatar Shader", "avatarV.glsl", "avatarF.glsl", ":2813")
        p.features.hasSkinning = True
        self.common_permutations(p)
        p.defines["AVATAR_CLOTH"] = "1" if st.avatar_cloth else "0"
        self.add(p)

        p = prog("Deferred Avatar Alpha Shader", "alphaV.glsl", "alphaF.glsl", ":2830")
        p.features.hasSkinning = True
        alpha_lit(p)
        p.features.isDeferred = True
        p.features.hasShadows = True
        p.features.hasReflectionProbes = True
        p.defines["USE_DIFFUSE_TEX"] = "1"
        p.defines["IS_AVATAR_SKIN"] = "1"
        if uss:
            p.defines["HAS_SUN_SHADOW"] = "1"
        self.common_permutations(p)
        self.add(p)

        def post(name, fs, src, srgb=True, deferred=True, tonemap=False, defines=None, **kw):
            p = prog(name, "postDeferredNoTCV.glsl", fs, src, **kw)
            p.features.hasSrgb = srgb
            p.features.isDeferred = deferred
            p.features.hasTonemap = tonemap
            if defines:
                p.defines.update(defines)
            return p

        self.add(post("Exposure", "exposureF.glsl", ":2868", defines={"USE_LAST_EXPOSURE": "1"}))
        self.add(post("Exposure (no fade)", "exposureF.glsl", ":2883"))
        self.add(post("Luminance", "luminanceF.glsl", ":2897", srgb=False, deferred=False))
        self.add(post("Deferred Gamma Correction Post Process", "postDeferredGammaCorrect.glsl", ":2909"))
        self.add(post("Wolf God Rays Post Process", "wolfGodRaysF.glsl", ":2924", srgb=False, deferred=False))
        self.add(post("Wolf Cloud Deck Post Process", "wolfCloudDeckF.glsl", ":2939", srgb=False, deferred=False, tolerated=True))

        if st.native_sky:   # :2957-3048 (+ WolfEnvironmentRender::loadShaders -> WolfTk)
            self.wolftk_programs()
            p = Program("Wolf Env Sky Dome", [(D + "wolfEnvSkyV.glsl", VERT), ("wolftk/skyF.glsl", FRAG),
                                              (D + "wolfAuroraF.glsl", FRAG)], lvl, group="wolfenv", tolerated=True, src=":2964")
            p.features.hasSrgb = True
            p.defines.update(wolftk_atmosphere_defines())
            p.defines["GROUND"] = "1"
            p.defines["HAS_SHADOW_LENGTH"] = "1"
            self.common_permutations(p)
            self.add(p)
            for i in range(2):
                p = Program("Wolf Env Aerial Perspective + Clouds" if i else "Wolf Env Aerial Perspective",
                            [("wolftk/compositeV.glsl", VERT), ("wolftk/compositeF.glsl", FRAG)], lvl, group="wolfenv",
                            tolerated=True, src=":2984")
                p.defines.update(wolftk_atmosphere_defines())
                p.defines["GROUND"] = "1"
                if i:
                    p.defines["HAS_OVERLAY"] = "1"
                    p.defines["HAS_SHADOW_LENGTH"] = "1"
                self.add(p)
            p = Program("Wolf Env Camera Cloud Shadow", [(D + "postDeferredNoTCV.glsl", VERT), ("wolftk/cameraShadowF.glsl", FRAG),
                                                         ("wolftk/atmosphereLibF.glsl", FRAG)], lvl, group="wolfenv",
                        tolerated=True, src=":3002")
            p.defines.update(wolftk_atmosphere_defines())
            p.defines["GROUND"] = "1"
            self.add(p)
            self.add(Program("Wolf Env Frame Log Luminance", [(D + "postDeferredNoTCV.glsl", VERT), ("wolftk/logLumF.glsl", FRAG)],
                             lvl, group="wolfenv", tolerated=True, src=":3016"))
            self.add(Program("Wolf Env Fog Extinction", [(D + "postDeferredNoTCV.glsl", VERT), ("wolftk/fogSigmaF.glsl", FRAG)],
                             lvl, group="wolfenv", tolerated=True, src=":3028"))
            self.add(Program("Wolf Env Zone Light", [(D + "postDeferredNoTCV.glsl", VERT), (D + "wolfEnvZoneF.glsl", FRAG)],
                             lvl, group="wolfenv", tolerated=True, src=":3038"))

        self.add(post("Legacy Gamma Correction Post Process", "postDeferredGammaCorrect.glsl", ":3052", defines={"LEGACY_GAMMA": "1"}))
        self.add(post("Deferred Tonemap Post Process", "postDeferredTonemap.glsl", ":3067", tonemap=True))
        self.add(post("No Post Tonemap Post Process", "postDeferredTonemap.glsl", ":3082", tonemap=True, defines={"NO_POST": "1"}))
        self.add(post("Deferred Tonemap Gamma Post Process", "postDeferredTonemap.glsl", ":3098", tonemap=True,
                      defines={"GAMMA_CORRECT": "1"}))
        self.add(post("No Post Tonemap Gamma Post Process", "postDeferredTonemap.glsl", ":3114", tonemap=True,
                      defines={"GAMMA_CORRECT": "1", "NO_POST": "1"}))
        # :3133 sets isDeferred on gDeferredPostTonemapProgram by mistake, so this one has isDeferred = false
        self.add(post("Deferred Tonemap Legacy Gamma Post Process", "postDeferredTonemap.glsl", ":3131", deferred=False,
                      tonemap=True, defines={"GAMMA_CORRECT": "1", "LEGACY_GAMMA": "1"}))
        self.add(post("No Post Tonemap Legacy Gamma Post Process", "postDeferredTonemap.glsl", ":3148", tonemap=True,
                      defines={"NO_POST": "1", "GAMMA_CORRECT": "1", "LEGACY_GAMMA": "1"}))

        if prof.gl_version > 3.9:   # :3164-3215
            for preset, label in [("12", "Low"), ("23", "Medium"), ("28", "High"), ("39", "Ultra")]:
                p = prog("FXAA Shader (%s)" % label, "postDeferredV.glsl", "fxaaF.glsl", ":3176", tolerated=True)
                p.features.isDeferred = True
                p.defines["FXAA_QUALITY__PRESET"] = preset
                p.defines["FXAA_GLSL_400" if prof.gl_version > 3.9 else "FXAA_GLSL_130"] = "1"
                self.add(p)
        if prof.gl_version > 3.15:   # :3217-3327
            for preset, label in [("SMAA_PRESET_LOW", "Low"), ("SMAA_PRESET_MEDIUM", "Medium"),
                                  ("SMAA_PRESET_HIGH", "High"), ("SMAA_PRESET_ULTRA", "Ultra")]:
                defs = {}
                if prof.gl_version >= 4.0:
                    defs["SMAA_GLSL_4"] = "1"
                elif prof.gl_version >= 3.1:
                    defs["SMAA_GLSL_3"] = "1"
                else:
                    defs["SMAA_GLSL_2"] = "1"
                defs["SMAA_PREDICATION"] = "0"
                defs["SMAA_REPROJECTION"] = "0"
                defs[preset] = "1"
                for name, stem, src in [("SMAA Edge Detection (%s)", "SMAAEdgeDetect", ":3240"),
                                        ("SMAA Blending Weights (%s)", "SMAABlendWeights", ":3266"),
                                        ("SMAA Neighborhood Blending (%s)", "SMAANeighborhoodBlend", ":3292")]:
                    # GL_FRAGMENT_SHADER_ARB / GL_VERTEX_SHADER_ARB are the same enums as the non-ARB names
                    p = Program(name % label, [(D + stem + "F.glsl", FRAG), (D + stem + "V.glsl", VERT),
                                               (D + "SMAA.glsl", FRAG), (D + "SMAA.glsl", VERT)], lvl,
                                group="deferred", tolerated=True, src=src)
                    p.features.isDeferred = True
                    p.defines.update(defs)
                    self.add(p)
        if prof.gl_version > 4.05:   # :3329-3366
            p = post("Contrast Adaptive Sharpening Shader", "CASF.glsl", ":3331", deferred=False, tolerated=True)
            self.add(p)
            p = post("Contrast Adaptive Sharpening Legacy Gamma Shader", "CASF.glsl", ":3349", deferred=False, tolerated=True,
                     defines={"GAMMA_CORRECT": "1", "LEGACY_GAMMA": "1"})
            self.add(p)

        self.add(post("Deferred Post Shader", "postDeferredF.glsl", ":3370", srgb=False))
        self.add(post("Deferred CoF Shader", "cofF.glsl", ":3382", srgb=False))
        self.add(post("Deferred DoFCombine Shader", "dofCombineF.glsl", ":3394", srgb=False))
        self.add(post("Deferred Post NoDoF Shader", "postDeferredNoDoFF.glsl", ":3406", srgb=False))
        self.add(post("Deferred Post NoDoF Noise Shader", "postDeferredNoDoFF.glsl", ":3418", srgb=False, defines={"HAS_NOISE": "1"}))

        for name, src, hdri in [("Environment Map Program", ":3434", True), ("Deferred Windlight Sky Shader", ":3456", False)]:
            p = Program(name, [(D + "skyV.glsl", VERT), (D + "skyF.glsl", FRAG), (D + "wolfAuroraF.glsl", FRAG)], lvl,
                        group="deferred", src=src)
            p.features.calculatesAtmospherics = True
            p.features.hasAtmospherics = True
            p.features.hasGamma = True
            p.features.hasSrgb = True
            if hdri:
                p.defines["HAS_HDRI"] = "1"
            self.common_permutations(p)
            self.add(p)
        p = post("Wolf Aurora LUT", "wolfAuroraF.glsl", ":3479", srgb=False, deferred=False, tolerated=True,
                 defines={"WOLF_AURORA_LUT_PASS": "1"})
        self.add(p)

        p = prog("Deferred Windlight Cloud Program", "cloudsV.glsl", "cloudsF.glsl", ":3494")
        for q in (p,):
            q.features.calculatesAtmospherics = True
            q.features.hasAtmospherics = True
            q.features.hasGamma = True
            q.features.hasSrgb = True
        p.defines[SHADER_CONST_CLOUD_MOON_DEPTH[0]] = SHADER_CONST_CLOUD_MOON_DEPTH[1]
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Windlight Sun Program", "sunDiscV.glsl", "sunDiscF.glsl", ":3515")
        p.features.calculatesAtmospherics = True
        p.features.hasAtmospherics = True
        p.features.hasGamma = True
        p.features.hasSrgb = True
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Windlight Moon Program", "moonV.glsl", "moonF.glsl", ":3535")
        p.features.calculatesAtmospherics = True
        p.features.hasAtmospherics = True
        p.features.hasGamma = True
        p.features.hasSrgb = True
        p.defines[SHADER_CONST_CLOUD_MOON_DEPTH[0]] = SHADER_CONST_CLOUD_MOON_DEPTH[1]
        self.common_permutations(p)
        self.add(p)
        p = prog("Deferred Star Program", "starsV.glsl", "starsF.glsl", ":3557")
        p.defines[SHADER_CONST_STAR_DEPTH[0]] = SHADER_CONST_STAR_DEPTH[1]
        self.common_permutations(p)
        self.add(p)
        self.add(prog("Normal Map Generation Program", "normgenV.glsl", "normgenF.glsl", ":3573"))
        self.add(prog("Brdf Gen Shader", "genbrdflutV.glsl", "genbrdflutF.glsl", ":3584"))
        p = prog("Screen Space Reflection Post", "screenSpaceReflPostV.glsl", "screenSpaceReflPostF.glsl", ":3593")
        p.features.hasScreenSpaceReflections = True
        p.features.isDeferred = True
        p.level = 3
        self.add(p)
        p = post("Deferred Buffer Visualization Shader", "postDeferredVisualizeBuffers.glsl", ":3604", srgb=False, deferred=False)
        self.common_permutations(p)
        self.add(p)
        p = prog("RLVa Sphere Post Processing Shader", "rlvV.glsl", "rlvF.glsl", ":3617")
        p.features.isDeferred = True
        self.add(p)

    # -- WolfEnvironmentRender::loadShaders ------------------------------------------
    def wolftk_programs(self):
        lvl = DEFERRED_CLASS
        # wolftkatmosphere.cpp:636-688 Atmosphere::loadShaders
        defs = [("WolfTk Transmittance", 1, None), ("WolfTk Direct Irradiance (delta)", 2, "deltaIrradiance"),
                ("WolfTk Direct Irradiance", 2, "irradiance"), ("WolfTk Single Scattering (Rayleigh)", 3, "deltaRayleigh"),
                ("WolfTk Single Scattering (Mie)", 3, "deltaMie"), ("WolfTk Single Scattering", 3, "scattering"),
                ("WolfTk Scattering Density", 4, None), ("WolfTk Indirect Irradiance (delta)", 5, "deltaIrradiance"),
                ("WolfTk Indirect Irradiance", 5, "irradiance"), ("WolfTk Multiple Scattering (delta)", 6, "deltaMultipleScattering"),
                ("WolfTk Multiple Scattering", 6, "scattering")]
        sizes = {k: v for k, v in wolftk_atmosphere_defines().items() if k.endswith(("_WIDTH", "_HEIGHT", "_SIZE"))}
        for name, n, output in defs:
            p = Program(name, [("wolftk/screenV.glsl", VERT), ("wolftk/precomputeF.glsl", FRAG)], lvl, group="wolftk",
                        tolerated=True, src="wolftkatmosphere.cpp:659")
            p.defines.update(sizes)
            p.defines["WOLF_TK_PRECOMPUTE"] = "%d" % n
            if output:
                p.defines["OUTPUT"] = output
            self.add(p)
        # wolftkclouds.cpp:1237-1251 Clouds::loadShaders: the four noise programs, then qualities 1 and 2
        # (0 and 3 are built on demand, :1247 and ensureQuality; all four are checked here)
        for name, fs in [("WolfTk Cloud Shape", "cloudShapeF.glsl"), ("WolfTk Cloud Shape Detail", "cloudShapeDetailF.glsl"),
                         ("WolfTk Local Weather", "localWeatherF.glsl"), ("WolfTk Turbulence", "turbulenceF.glsl")]:
            self.add(Program(name, [("wolftk/proceduralV.glsl", VERT), ("wolftk/" + fs, FRAG)], lvl, group="wolftk",
                             tolerated=True, src="wolftkclouds.cpp:1243"))
        for q in range(4):
            self.wolftk_clouds_quality(q)

    def wolftk_clouds_quality(self, q):
        # wolftkclouds.cpp:678-740 Impl::ensureQuality; presets :428-477 (QualityPreset::get)
        lvl = DEFERRED_CLASS
        names = ["low", "medium", "high", "ultra"]
        # makeDefaults (:428-441) then the per-preset changes (:453-474); lightShafts false everywhere (:474)
        shape_detail = [False, True, True, True][q]
        turbulence = [False, False, True, True][q]
        accurate_sun_sky = [False, False, True, True][q]
        max_iter_to_ground = [0, 1, 3, 3][q]
        cascade_count = [2, 3, 3, 3][q]
        haze, light_shafts, accurate_phase, ms_octaves = True, False, False, 8
        channels = "rgba"   # CloudLayers::defaults() (:481-495) channels r, g, b, a
        c = dict(wolftk_atmosphere_defines())
        c["DEPTH_PACKING"] = "3200"
        c["LOCAL_WEATHER_CHANNELS"] = channels
        if shape_detail:
            c["SHAPE_DETAIL"] = "1"
        if turbulence:
            c["TURBULENCE"] = "1"
        if light_shafts:
            c["SHADOW_LENGTH"] = "1"
        if haze:
            c["HAZE"] = "1"
        c["MULTI_SCATTERING_OCTAVES"] = "%d" % ms_octaves
        if accurate_sun_sky:
            c["ACCURATE_SUN_SKY_LIGHT"] = "1"
        if accurate_phase:
            c["ACCURATE_PHASE_FUNCTION"] = "1"
        c["SHADOW_CASCADE_COUNT"] = "%d" % cascade_count
        c["SHADOW_SAMPLE_COUNT"] = "8"
        c["SCATTER_ANISOTROPY_1"] = "0.7000000"
        c["SCATTER_ANISOTROPY_2"] = "-0.2000000"
        c["SCATTER_ANISOTROPY_MIX"] = "0.5000000"
        # wolfcloudgenus.h:41-50: ICE_G_SCALE = 0.76f / 0.85f; MS_ATTENUATION = 1 - 0.0471f * F_PI / 0.8f (F32 arithmetic)
        f_pi = f32(3.1415926535897932384626433832795)
        c["WOLF_ICE_G_SCALE"] = "%.7f" % f32(f32(0.76) / f32(0.85))
        c["WOLF_MS_ATTENUATION"] = "%.7f" % f32(f32(1.0) - f32(f32(f32(0.0471) * f_pi) / f32(0.8)))
        c["PERSPECTIVE_CAMERA"] = "1"
        c["POWDER"] = "1"
        if 1.0 > 0.0 and max_iter_to_ground > 0:
            c["GROUND_BOUNCE"] = "1"
        s = {"SHADOW": "1", "TEMPORAL_PASS": "1", "TEMPORAL_JITTER": "1", "LOCAL_WEATHER_CHANNELS": channels,
             "CASCADE_COUNT": "%d" % cascade_count}
        if shape_detail:
            s["SHAPE_DETAIL"] = "1"
        if turbulence:
            s["TURBULENCE"] = "1"
        r = {"TEMPORAL_UPSCALE": "1"}
        if light_shafts:
            r["SHADOW_LENGTH"] = "1"
        sr = {"CASCADE_COUNT": "%d" % cascade_count}
        tag = " (%s)" % names[q]
        for name, vs, fs, d in [("WolfTk Clouds", "cloudsV.glsl", "cloudsF.glsl", c),
                                ("WolfTk Clouds Resolve", "screenV.glsl", "cloudsResolveF.glsl", r),
                                ("WolfTk Clouds Shadow", "screenV.glsl", "shadowF.glsl", s),
                                ("WolfTk Clouds Shadow Resolve", "screenV.glsl", "shadowResolveF.glsl", sr)]:
            p = Program(name + tag, [("wolftk/" + vs, VERT), ("wolftk/" + fs, FRAG)], lvl, group="wolftk", tolerated=True,
                        src="wolftkclouds.cpp:735")
            p.defines.update(d)
            self.add(p)

    def run(self):
        # llviewershadermgr.cpp:826-890 loadAllShaders: the order the groups are built in
        self.basic_shaders()
        self.water()
        self.effects()
        self.interface()
        self.objects()
        self.avatar()
        self.deferred()
        return self


# ---------------------------------------------------------------------------
# Stitching, exactly as LLShaderMgr::loadShaderFile does it
# ---------------------------------------------------------------------------

PRUNE_UNUSED_DEFINES = True   # --no-prune turns it off; see stitch()


def _name_in(name, text):
    return re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", text) is not None


LINE_PIECE = 1023   # fgets(line, 1024, f) reads at most 1023 bytes per call (llshadermgr.cpp:870-871)
MAX_PIECES = 8192   # shader_code_text holds 8192 + 1024 pointers; the file loop stops at 8192 (:652, :901-902)
WOLF_INCLUDE_KEY = '#wolf_include "'


class ShaderFiles:
    """The class N..1 shader tree, read once."""

    def __init__(self, root):
        self.root = root
        self.cache = {}
        self.bodies = {}
        self.name_cache = {}

    def find(self, filename, level):
        """llshadermgr.cpp:600-629: class `level` down to class 1, the first that exists."""
        for gpu_class in range(level, 0, -1):
            path = os.path.join(self.root, "class%d" % gpu_class, filename)
            if os.path.isfile(path):
                return gpu_class, path
        return 0, None

    def names(self, path, name):
        """True when `name` appears as a whole word anywhere in the file (after #wolf_include). Memoised."""
        key = (path, name)
        hit = self.name_cache.get(key)
        if hit is None:
            body = self.bodies.get(path)
            if body is None:
                body = self.bodies[path] = "".join(p[0] for p in self.read_pieces(path)[0])
            hit = self.name_cache[key] = _name_in(name, body)
        return hit

    def read_pieces(self, path, depth=0):
        """The lines of `path` as fgets(1024) would hand them over, with every `#wolf_include` line
        replaced by the included file's pieces plus one "\\n" (llshadermgr.cpp:865-898).
        Each piece is (text, origin_file, origin_line)."""
        key = (path, depth)
        if key in self.cache:
            return self.cache[key]
        with open(path, "rb") as fh:
            data = fh.read()
        pieces = []
        warnings = []
        line_no = 0
        for raw in data.splitlines(keepends=True):
            line_no += 1
            # fgets: at most 1023 bytes, stopping after a newline
            chunks = [raw[i:i + LINE_PIECE] for i in range(0, len(raw), LINE_PIECE)] or [b""]
            first = True
            for chunk in chunks:
                text = chunk.decode("utf-8", errors="replace")
                if first and depth < 8:
                    stripped = text.lstrip(" \t")
                    if stripped.startswith(WOLF_INCLUDE_KEY):
                        name = stripped[len(WOLF_INCLUDE_KEY):]
                        end = name.find('"')
                        if end >= 0:
                            inc_path = os.path.join(self.root, "class1", name[:end])
                            if os.path.isfile(inc_path):
                                sub, sub_warn = self.read_pieces(inc_path, depth + 1)
                                pieces.extend(sub)
                                warnings.extend(sub_warn)
                                pieces.append(("\n", path, line_no))
                                first = False
                                continue
                            warnings.append("wolf_include not found: %s (in %s:%d)" % (inc_path, path, line_no))
                first = False
                pieces.append((text, path, line_no))
        self.cache[key] = (pieces, warnings)
        return pieces, warnings


@dataclass
class Unit:
    """One stitched shader object (one glCompileShader)."""
    filename: str
    requested_level: int
    found_level: int
    stage: str
    text: str
    linemap: list          # per output line (1-based index-1): (file, line) or ("<injected>", text)
    sha: str
    warnings: list
    missing: bool = False


def version_line(prof, stage):
    """llshadermgr.cpp:657-711: the #version line and (GLSL 1.x only) the precision lines."""
    major, minor = prof.glsl_major, prof.glsl_minor
    extra = []
    if major == 1 and minor < 30:
        raise ValueError("GL 3.1 or later required (GLSL %d.%d)" % (major, minor))
    if major >= 4:
        ver = "#version 420\n" if minor >= 20 else "#version 400\n"
    elif major == 3:
        ver = "#version 150\n" if minor <= 29 else "#version 330\n"
    else:
        ver = "#version 150\n" if (stage == GEOM or minor >= 50) else "#version 140\n"
        extra = ["precision mediump int;\n", "precision highp float;\n"]
    return ver, extra


def stitch(files, prof, filename, level, stage, defines, texture_index_channels):
    """LLShaderMgr::loadShaderFile (llshadermgr.cpp:547-1094) without the GL calls."""
    defines = dict(defines)
    if prof.old_select:
        defines["OLD_SELECT"] = "1"   # :551-556 (and llglslshader.cpp:424-430)
    found_level, path = files.find(filename, level)
    if not path:
        return Unit(filename, level, 0, stage, "", [], "", ["GLSL Shader file not found: class%d..1/%s" % (level, filename)], missing=True)

    ver, extra = version_line(prof, stage)
    out = [(ver, "<injected>", "#version")]
    extra = [(e, "<injected>", "precision") for e in extra]
    stage_define = "#define FRAGMENT_SHADER 1\n" if stage == FRAG else "#define VERTEX_SHADER 1\n"   # :713-720
    extra.append((stage_define, "<injected>", "stage define"))
    for line in ["#define GBUFFER_FLAG_SKIP_ATMOS   0.0 \n", "#define GBUFFER_FLAG_HAS_ATMOS    0.34\n",
                 "#define GBUFFER_FLAG_HAS_PBR      0.67\n", "#define GBUFFER_FLAG_HAS_HDRI      1.0\n",
                 "#define GET_GBUFFER_FLAG(data, flag)    (abs(data-flag)< 0.1)\n"]:   # :724-728
        extra.append((line, "<injected>", "gbuffer flags"))
    extra.append(None)   # the program's defines go here (filled in below, once the file is read)
    define_slot = len(extra) - 1
    if prof.is_amd:
        extra.append(("#define IS_AMD_CARD 1\n", "<injected>", "IS_AMD_CARD"))   # :739-742
    if texture_index_channels > 0 and stage == FRAG:   # :744-837
        extra.append(("#define HAS_DIFFUSE_LOOKUP\n", "<injected>", "diffuseLookup"))
        for i in range(texture_index_channels):
            extra.append(("uniform sampler2D tex%d;\n" % i, "<injected>", "diffuseLookup"))
        if texture_index_channels > 1:
            extra.append(("flat in int vary_texture_index;\n", "<injected>", "diffuseLookup"))
        extra.append(("vec4 diffuseLookup(vec2 texcoord)\n", "<injected>", "diffuseLookup"))
        extra.append(("{\n", "<injected>", "diffuseLookup"))
        if texture_index_channels == 1:
            extra.append(("return texture(tex0, texcoord);\n", "<injected>", "diffuseLookup"))
            extra.append(("}\n", "<injected>", "diffuseLookup"))
        elif prof.glsl_major > 1 or prof.glsl_minor >= 30:
            if prof.is_nvidia:
                for i in range(texture_index_channels):
                    extra.append(("\t%sif (vary_texture_index == %d) { return texture(tex%d, texcoord); }\n" % (
                        "else " if i > 0 else "", i, i), "<injected>", "diffuseLookup"))
                extra.append(("\treturn vec4(1,0,1,1);\n", "<injected>", "diffuseLookup"))
                extra.append(("}\n", "<injected>", "diffuseLookup"))
            else:
                extra.append(("\tvec4 ret = vec4(1,0,1,1);\n", "<injected>", "diffuseLookup"))
                extra.append(("\tswitch (vary_texture_index)\n", "<injected>", "diffuseLookup"))
                extra.append(("\t{\n", "<injected>", "diffuseLookup"))
                for i in range(texture_index_channels):
                    extra.append(("\t\tcase %d: return texture(tex%d, texcoord);\n" % (i, i), "<injected>", "diffuseLookup"))
                extra.append(("\t}\n", "<injected>", "diffuseLookup"))
                extra.append(("\treturn ret;\n", "<injected>", "diffuseLookup"))
                extra.append(("}\n", "<injected>", "diffuseLookup"))
        else:
            raise ValueError("Indexed texture rendering requires GLSL 1.30 or later.")
    extra.append(("struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };\n",
                  "<injected>", "GBufferInfo"))   # :840

    pieces, warnings = files.read_pieces(path)
    warnings = list(warnings)
    if PRUNE_UNUSED_DEFINES:
        # A macro the file never names cannot change what the compiler sees, so leaving its "#define"
        # line out gives a compile with exactly the same meaning - and collapses the thousands of
        # otherwise distinct unit texts (every program x every setting that changes a define it does
        # not use) into the few hundred that differ. A name counts as used when it appears anywhere in
        # the file (after "#wolf_include"), or in the value of another define that is used.
        used = {k for k in defines if files.names(path, k)}
        while True:
            more = {k for k in defines if k not in used and any(_name_in(k, defines[u]) for u in used)}
            if not more:
                break
            used |= more
        defines = {k: v for k, v in defines.items() if k in used}
    extra[define_slot:define_slot + 1] = [("#define " + k + " " + defines[k] + "\n", "<injected>", "define")
                                          for k in sorted(defines)]   # std::map iteration order (:730-737)
    # :842-984: the extra block goes where the first "[EXTRA_CODE_HERE]" piece is (that piece is dropped),
    # or straight after the version line when there is none. The table holds 8192 pieces in all.
    before, after = [], []
    marker_found = False
    count = 1   # the version line
    truncated = False
    for text, f, ln in pieces:
        if count >= MAX_PIECES:
            truncated = True
            break
        if not marker_found and "[EXTRA_CODE_HERE]" in text:
            marker_found = True
            count += len(extra)
            continue
        (after if marker_found else before).append((text, f, ln))
        count += 1
        if not marker_found and len(before) == len(extra):
            count += len(extra)   # the reserved slots are counted once the first extra_count pieces are in
    if truncated:
        warnings.append("%s: %d lines after #wolf_include, more than the shader loader holds; truncated" % (filename, len(pieces)))
    if marker_found:
        out += before + extra + after
    else:
        out += extra + before

    # join, and map every output line back to its origin
    text = "".join(p[0] for p in out)
    linemap = []
    pending = None
    for piece, f, ln in out:
        for i, part in enumerate(piece.split("\n")):
            if i > 0:
                linemap.append(pending if pending else (f, ln))
                pending = None
            if part:
                pending = pending or (f, ln)
    if pending:
        linemap.append(pending)
    sha = hashlib.sha1(text.encode("utf-8")).hexdigest()
    return Unit(filename, level, found_level, stage, text, linemap, sha, warnings)


# ---------------------------------------------------------------------------
# Building the scenario: units, shared objects, programs, links
# ---------------------------------------------------------------------------

@dataclass
class LinkJob:
    key: tuple               # the unit shas, in attach order
    units: list              # [Unit]
    instances: list = field(default_factory=list)   # [(scenario, program name, level, tolerated, group, src)]
    recipes: dict = field(default_factory=dict)      # [VULKAN BACKEND 2026-11-08] program key -> [unit recipe strings] (shader_vulkan.py)


class Builder:
    def __init__(self, files):
        self.files = files
        self.units = {}      # sha -> Unit
        self.stitched = {}   # (profile, file, level, stage, defines, channels) -> Unit
        self.links = {}      # key -> LinkJob
        self.instances = 0

    def unit(self, prof, filename, level, stage, defines, channels):
        # The same file is stitched with the same inputs thousands of times across the scenarios:
        # remember the result per input set.
        key = (prof.name, filename, level, stage, tuple(sorted(defines.items())), channels)
        u = self.stitched.get(key)
        if u is None:
            u = stitch(self.files, prof, filename, level, stage, defines, channels)
            if not u.missing:
                u = self.units.setdefault(u.sha, u)
            self.stitched[key] = u
        return u

    def scenario(self, prof, st, name_filter=None):
        en = Enumerator(prof, st).run()
        scen = "%s/%s" % (prof.name, st.name)
        # loadBasicShaders: the shared objects, keyed by (filename, stage)
        shared = {}
        for filename, level, stage, ich in en.shared:
            shared[(filename, stage)] = self.unit(prof, filename, level, stage, en.attribs, ich)
            self.add_link((shared[(filename, stage)],), scen, "[shared] %s" % filename, level, False, "basic", ":1089-1222",
                          compile_only=True)
        for p in en.programs:
            if name_filter and not name_filter.search(p.name):
                continue
            vobj, fobj, tail_vobj = attach_features(p.features)
            # llglslshader.cpp:496-502: a failed link retries at every lower level down to 0 (level 0 = class 1 files)
            seen = set()
            for level in range(p.level, 0, -1):
                units = []
                recipes = []   # [VULKAN BACKEND 2026-11-08] the run-time key parts, in attach order (shader_vulkan.unit_recipe)
                ok = True
                for filename, stage in p.files:
                    wolf_unit = filename.startswith("wolftk/")   # llglslshader.cpp:462-464
                    channels = 0 if wolf_unit else p.features.mIndexedTextureChannels
                    u = self.unit(prof, filename, level, stage, p.defines, channels)
                    units.append(u)
                    if u.missing:
                        ok = False
                    else:
                        recipes.append(shader_vulkan.unit_recipe(stage, filename, u.found_level, version_line(prof, stage)[0], p.defines, channels, prof.is_amd, prof.old_select))
                for name in vobj:
                    u = shared[(name, VERT)]
                    units.append(u)
                    if not u.missing:
                        recipes.append(shader_vulkan.unit_recipe(VERT, name, u.found_level, version_line(prof, VERT)[0], en.attribs, [ich for f, l, st, ich in en.shared if f == name and st == VERT][0], prof.is_amd, prof.old_select))
                for name in fobj:
                    u = shared[(name, FRAG)]
                    units.append(u)
                    if not u.missing:
                        recipes.append(shader_vulkan.unit_recipe(FRAG, name, u.found_level, version_line(prof, FRAG)[0], en.attribs, [ich for f, l, st, ich in en.shared if f == name and st == FRAG][0], prof.is_amd, prof.old_select))
                for name in tail_vobj:   # llshadermgr.cpp:283-294, attached last
                    u = shared[(name, VERT)]
                    units.append(u)
                    if not u.missing:
                        recipes.append(shader_vulkan.unit_recipe(VERT, name, u.found_level, version_line(prof, VERT)[0], en.attribs, [ich for f, l, st, ich in en.shared if f == name and st == VERT][0], prof.is_amd, prof.old_select))
                key = tuple(u.sha if not u.missing else "MISSING:" + u.filename for u in units)
                if key in seen:
                    continue   # no class file changes below this level: the same program again
                seen.add(key)
                if not ok and level < p.level:
                    # A main file that exists only at a higher class (e.g. class2/deferred/alphaF.glsl): the
                    # viewer's fallback to this level cannot find it either. Not a shader error - reported
                    # only when the nominal level is missing a file.
                    continue
                tag = "" if level == p.level else " [fallback level %d]" % level
                job = self.add_link(units, scen, p.name + tag, level, p.tolerated, p.group, p.src)
                if ok and job is not None:
                    job.recipes[shader_vulkan.program_key(recipes)] = recipes

    def add_link(self, units, scen, name, level, tolerated, group, src, compile_only=False):
        key = tuple(u.sha if not u.missing else "MISSING:" + u.filename for u in units)
        if compile_only:
            key = ("compile",) + key
        job = self.links.get(key)
        if not job:
            job = self.links[key] = LinkJob(key, list(units))
        job.instances.append((scen, name, level, tolerated, group, src))
        self.instances += 1
        return job


# ---------------------------------------------------------------------------
# glslang
# ---------------------------------------------------------------------------

def find_glslang(explicit):
    cands = []
    if explicit:
        cands.append(explicit)
    if os.environ.get("GLSLANG_VALIDATOR"):
        cands.append(os.environ["GLSLANG_VALIDATOR"])
    for n in ("glslangValidator", "glslang"):
        cands.append(n)
        if os.environ.get("VULKAN_SDK"):
            cands.append(os.path.join(os.environ["VULKAN_SDK"], "bin", n))
    for c in cands:
        try:
            r = subprocess.run([c, "--version"], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0 and "Glslang" in r.stdout:
            return c, r.stdout.splitlines()[0].strip()
    return None, None


def unit_path(cache_dir, u):
    return os.path.join(cache_dir, "%s.%s" % (u.sha, u.stage))


def write_units(cache_dir, units):
    os.makedirs(cache_dir, exist_ok=True)
    for u in units:
        p = unit_path(cache_dir, u)
        if not os.path.exists(p):
            with open(p, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(u.text)


ERR_RE = re.compile(r"^(ERROR|WARNING): (\d+):(\d+): (.*)$")


def map_errors(output, units):
    """Rewrite glslang's `ERROR: 0:LINE:` lines to the shader file and line they came from.
    glslang numbers the source string 0 and lines from 1; the file name it prints is the unit's."""
    by_name = {os.path.basename(unit_path("", u)): u for u in units}
    out = []
    current = None
    for line in output.splitlines():
        base = os.path.basename(line.strip())
        if base in by_name:
            current = by_name[base]
            out.append("  unit %s (class%d/%s, %s)" % (current.sha[:10], current.found_level, current.filename, current.stage))
            continue
        m = ERR_RE.match(line)
        if m and current and current.linemap:
            lineno = int(m.group(3))
            if 1 <= lineno <= len(current.linemap):
                f, ln = current.linemap[lineno - 1]
                if f == "<injected>":
                    where = "injected %s (stitched line %d)" % (ln, lineno)
                else:
                    where = "%s:%s (stitched line %d)" % (os.path.relpath(f, SHADERS_ROOT) if f.startswith(SHADERS_ROOT) else f, ln, lineno)
                out.append("  %s: %s: %s" % (m.group(1), where, m.group(4)))
                continue
        out.append("  " + line)
    return "\n".join(out)


def run_glslang(glslang, cache_dir, job, spirv=None):
    """Compile (one unit) or link (several) with glslangValidator. spirv: None (plain GLSL),
    "-G" (OpenGL SPIR-V) or "-V" (Vulkan SPIR-V). Returns (ok, output)."""
    paths = [unit_path(cache_dir, u) for u in job.units if not u.missing]
    if any(u.missing for u in job.units):
        return False, "\n".join("  " + w for u in job.units if u.missing for w in u.warnings)
    args = [glslang]
    if spirv:
        args += [spirv, "--auto-map-locations", "--auto-map-bindings", "-o", os.devnull]
    if len(paths) > 1 or spirv:
        args.append("-l")
    args += paths
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return False, "  glslangValidator timed out"
    output = (r.stdout or "") + (r.stderr or "")
    return r.returncode == 0, map_errors(output, job.units)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def group_spirv_reasons(outputs):
    """The top reasons SPIR-V generation fails: the message with identifiers and line numbers taken out."""
    counts = {}
    for out in outputs:
        seen = set()
        for line in out.splitlines():
            if "ERROR" not in line:
                continue
            msg = re.sub(r"^\s*ERROR: \S+ ", "", line)
            # keep the quoted construct (it names what is wrong: 'gl_FragData', 'non-opaque uniforms outside a block'),
            # drop numbers so lines and counts do not split one reason into many
            msg = re.sub(r"\d+", "N", msg)
            msg = msg.strip()
            if msg and msg not in seen:
                seen.add(msg)
                counts[msg] = counts.get(msg, 0) + 1
    return sorted(counts.items(), key=lambda kv: -kv[1])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shaders", default=DEFAULT_SHADERS_DIR, help="app_settings/shaders directory")
    ap.add_argument("--glslang", help="path to glslangValidator (default: $GLSLANG_VALIDATOR, PATH, $VULKAN_SDK/bin)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quick", action="store_true", help="only %s on %s" % (", ".join(QUICK_SETTINGS), ", ".join(QUICK_PROFILES)))
    ap.add_argument("--profile", action="append", help="GPU profile name(s) to run (default: all)")
    ap.add_argument("--scenario", action="append", help="settings scenario name(s) to run (default: all)")
    ap.add_argument("--filter", help="regular expression on the program name")
    ap.add_argument("--spirv", action="store_true", help="also compile every linked program to OpenGL SPIR-V (-G) and report readiness")
    ap.add_argument("--vulkan", action="store_true", help="also try every linked program as Vulkan SPIR-V (-V) and report the top reasons it fails")
    ap.add_argument("--vulkan-out", metavar="DIR",
                    help="[VULKAN BACKEND 2026-11-08] transform every linked program to Vulkan GLSL (shader_vulkan.py) and write <key>.vert.spv / <key>.frag.spv and manifest.json into DIR")
    ap.add_argument("--known", default=os.path.join(HERE, "shader_check_known.txt"),
                    help="file of known, accepted failures (profile|program regex|reason per line); they are reported but do not fail the run")
    ap.add_argument("--cache", help="directory for the stitched sources (default: a temporary one that is removed)")
    ap.add_argument("--dump", help="write the stitched sources of failing programs here")
    ap.add_argument("--json", help="write the full result as JSON here")
    ap.add_argument("--list", action="store_true", help="list the programs and variants without compiling")
    ap.add_argument("--allow-tolerated", action="store_true",
                    help="exit 0 when the only failures are in programs the viewer carries on without")
    ap.add_argument("--no-prune", action="store_true",
                    help="keep every #define line the viewer injects, used by the file or not (slower; the compile result is the same)")
    ap.add_argument("--no-fail", action="store_true",
                    help="[VULKAN BACKEND 2026-11-08] exit 0 whatever the GL results (the Vulkan SPIR-V output step runs this way, so a GL failure is reported by the check target, not twice)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    global PRUNE_UNUSED_DEFINES, SHADERS_ROOT
    PRUNE_UNUSED_DEFINES = not args.no_prune
    SHADERS_ROOT = os.path.abspath(args.shaders)

    t0 = time.time()
    if not os.path.isdir(os.path.join(args.shaders, "class1")):
        print("shader_check: no class1/ under %s" % args.shaders, file=sys.stderr)
        return 2

    profiles = GPU_PROFILES
    scenarios = settings_scenarios()
    if args.quick:
        profiles = [p for p in profiles if p.name in QUICK_PROFILES]
        scenarios = [s for s in scenarios if s.name in QUICK_SETTINGS]
    if args.profile:
        profiles = [p for p in profiles if p.name in args.profile]
        if not profiles:
            print("shader_check: no such profile; known: %s" % ", ".join(p.name for p in GPU_PROFILES), file=sys.stderr)
            return 2
    if args.scenario:
        scenarios = [s for s in scenarios if s.name in args.scenario]
        if not scenarios:
            print("shader_check: no such scenario; known: %s" % ", ".join(s.name for s in settings_scenarios()), file=sys.stderr)
            return 2
    name_filter = re.compile(args.filter) if args.filter else None

    files = ShaderFiles(args.shaders)
    b = Builder(files)
    for prof in profiles:
        for st in scenarios:
            b.scenario(prof, st, name_filter)

    compile_jobs = [j for j in b.links.values() if j.key[0] == "compile"]
    link_jobs = [j for j in b.links.values() if j.key[0] != "compile"]
    n_programs = len({(i[1].split(" [fallback")[0]) for j in link_jobs for i in j.instances})
    print("shader_check: %d GPU profiles x %d settings scenarios = %d scenarios" % (len(profiles), len(scenarios), len(profiles) * len(scenarios)))
    print("shader_check: %d distinct program names, %d program instances, %d distinct stitched units, %d distinct links"
          % (n_programs, b.instances, len(b.units), len(link_jobs)))

    stitch_warnings = sorted({w for u in b.units.values() for w in u.warnings})
    for w in stitch_warnings:
        print("WARNING: " + w)

    if args.list:
        for j in sorted(link_jobs, key=lambda j: j.instances[0][1]):
            scen, name, level, tol, group, src = j.instances[0]
            print("%-70s level %d  %s  x%d  first: %s  files: %s" % (
                name, level, "tolerated" if tol else "fatal", len(j.instances), scen,
                " ".join("%s@c%d" % (u.filename, u.found_level) for u in j.units)))
        return 0

    glslang, glslang_version = find_glslang(args.glslang)
    if not glslang:
        print("shader_check: glslangValidator not found (install glslang, or pass --glslang / set GLSLANG_VALIDATOR)", file=sys.stderr)
        return 2
    print("shader_check: using %s (%s)" % (glslang, glslang_version))

    import tempfile
    tmp = None
    cache_dir = args.cache
    if not cache_dir:
        tmp = tempfile.TemporaryDirectory(prefix="shader_check_")
        cache_dir = tmp.name
    write_units(cache_dir, b.units.values())

    # 1. compile every unit on its own, so a compile error is reported once with its file and line
    unit_jobs = {u.sha: LinkJob(("unit", u.sha), [u]) for u in b.units.values()}
    results_unit = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_glslang, glslang, cache_dir, j): sha for sha, j in unit_jobs.items()}
        for fut in concurrent.futures.as_completed(futs):
            results_unit[futs[fut]] = fut.result()
    bad_units = {sha for sha, (ok, _) in results_unit.items() if not ok}
    print("shader_check: compiled %d units, %d failed (%.1fs)" % (len(unit_jobs), len(bad_units), time.time() - t0))

    # 2. link every program whose units all compiled
    results_link = {}
    linkable = [j for j in link_jobs if not any(u.missing or u.sha in bad_units for u in j.units)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_glslang, glslang, cache_dir, j): j.key for j in linkable}
        for fut in concurrent.futures.as_completed(futs):
            results_link[futs[fut]] = fut.result()
    bad_links = {k for k, (ok, _) in results_link.items() if not ok}
    print("shader_check: linked %d programs, %d failed (%.1fs)" % (len(linkable), len(bad_links), time.time() - t0))

    # 3. optionally, OpenGL SPIR-V (-G) and Vulkan SPIR-V (-V)
    spirv_results, vulkan_results = {}, {}
    good = [j for j in linkable if j.key not in bad_links]
    for flag, enabled, store in (("-G", args.spirv, spirv_results), ("-V", args.vulkan, vulkan_results)):
        if not enabled:
            continue
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(run_glslang, glslang, cache_dir, j, flag): j.key for j in good}
            for fut in concurrent.futures.as_completed(futs):
                store[futs[fut]] = fut.result()
        n_ok = sum(1 for ok, _ in store.values() if ok)
        print("shader_check: SPIR-V (%s): %d of %d linked programs compile as they stand (%.1fs)" % (flag, n_ok, len(good), time.time() - t0))

    # 4. [VULKAN BACKEND 2026-11-08] the Vulkan transform + SPIR-V output for the viewer's Vulkan renderer
    vk_results = {}
    if args.vulkan_out:
        vk_dir = os.path.abspath(args.vulkan_out)
        vk_work = os.path.join(cache_dir, "vulkan")
        manifest = {}
        def vk_one(j):
            key = next(iter(j.recipes)) if j.recipes else None
            if key is None:
                return j.key, (True, "", None)
            t = shader_vulkan.transform(glslang, cache_dir, j, unit_path)
            if t.error:
                return j.key, (False, "  transform: " + t.error, None)
            ok, output = shader_vulkan.compile_program(glslang, vk_work, key, t, vk_dir)
            return j.key, (ok, output, (key, t))
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for fut in concurrent.futures.as_completed([ex.submit(vk_one, j) for j in good]):
                k, (ok, output, extra) = fut.result()
                vk_results[k] = (ok, output)
                if ok and extra:
                    key, t = extra
                    j = b.links[k]
                    names = sorted({i[1] for i in j.instances})
                    for rkey, recipes in j.recipes.items():
                        manifest[rkey] = {"spv": key, "names": names, "units": recipes, "samplers": t.samplers, "defaults": t.defaults,
                                          "uniforms_vert": sorted(t.uniforms["vert"]), "uniforms_frag": sorted(t.uniforms["frag"])}
        shader_vulkan.write_manifest(os.path.join(vk_dir, "manifest.json"), manifest)
        n_ok = sum(1 for ok, _ in vk_results.values() if ok)
        print("shader_check: Vulkan SPIR-V output: %d of %d linked programs transformed and compiled, %d recipe keys in %s/manifest.json (%.1fs)"
              % (n_ok, len(good), len(manifest), vk_dir, time.time() - t0))
        if n_ok < len(good):
            reasons = group_spirv_reasons(out for ok, out in vk_results.values() if not ok)
            print("  top reasons the rest fail:")
            for msg, n in reasons[:25]:
                print("    %5d  %s" % (n, msg))
            if args.verbose or args.dump:
                for k, (ok, out) in sorted(vk_results.items(), key=lambda kv: b.links[kv[0]].instances[0][1]):
                    if ok: continue
                    print("  FAILED %s" % " | ".join(sorted({i[1] for i in b.links[k].instances}))[:160])
                    print("\n".join("    " + l for l in out.splitlines()[:12]))

    # -- report ------------------------------------------------------------------
    failures = []   # (fatal, title, detail, instances)
    for j in link_jobs:
        ok = True
        detail = []
        for u in j.units:
            if u.missing:
                ok = False
                detail.append("\n".join("  " + w for w in u.warnings))
            elif u.sha in bad_units:
                ok = False
                detail.append(results_unit[u.sha][1])
        if ok and j.key in bad_links:
            ok = False
            detail.append(results_link[j.key][1])
        if not ok:
            fatal = any(not i[3] for i in j.instances)
            names = sorted({i[1] for i in j.instances})
            scens = sorted({i[0] for i in j.instances})
            title = "%s  (level %d, %d scenario%s, e.g. %s)" % (" | ".join(names), j.instances[0][2], len(scens),
                                                               "" if len(scens) == 1 else "s", scens[0])
            failures.append((fatal, title, "\n".join(d for d in detail if d), j))
    for j in compile_jobs:   # shared objects that do not compile
        u = j.units[0]
        if u.missing or u.sha in bad_units:
            fatal = True
            scens = sorted({i[0] for i in j.instances})
            title = "%s  (shared object, level %d, %d scenario%s, e.g. %s)" % (j.instances[0][1], j.instances[0][2], len(scens),
                                                                           "" if len(scens) == 1 else "s", scens[0])
            failures.append((fatal, title, results_unit[u.sha][1] if not u.missing else "\n".join(u.warnings), j))

    known = load_known(args.known)
    failures.sort(key=lambda f: (not f[0], f[1]))
    print()
    unknown = [f for f in failures if not known_reason(known, f[3])]
    if not failures:
        print("shader_check: OK - every program variant compiles and links")
    else:
        n_fatal = sum(1 for f in failures if f[0])
        print("shader_check: %d FAILING program variant(s), %d of them fatal to a shader group, %d known and accepted (%s):"
              % (len(failures), n_fatal, len(failures) - len(unknown), os.path.relpath(args.known)))
        for fatal, title, detail, j in failures:
            print()
            reason = known_reason(known, j)
            print("%s %s" % ("KNOWN    " if reason else ("FATAL    " if fatal else "TOLERATED"), title))
            if reason:
                print("  accepted: " + reason)
            if args.verbose:
                for i in sorted({(x[0], x[1]) for x in j.instances})[:12]:
                    print("    in %s: %s" % i)
            print("  files: " + " ".join("%s@c%d" % (u.filename, u.found_level) for u in j.units if not u.missing))
            print(detail)
            if args.dump:
                os.makedirs(args.dump, exist_ok=True)
                stem = re.sub(r"[^A-Za-z0-9]+", "_", j.instances[0][1])[:80]
                for u in j.units:
                    if u.missing:
                        continue
                    with open(os.path.join(args.dump, "%s.%s.%s" % (stem, u.sha[:8], u.stage)), "w", encoding="utf-8") as fh:
                        fh.write(u.text)

    for flag, label, store in (("-G", "OpenGL SPIR-V", spirv_results), ("-V", "Vulkan SPIR-V", vulkan_results)):
        if not store:
            continue
        reasons = group_spirv_reasons(out for ok, out in store.values() if not ok)
        print()
        print("%s readiness (glslangValidator %s --auto-map-locations --auto-map-bindings):" % (label, flag))
        n_ok = sum(1 for ok, _ in store.values() if ok)
        print("  %d of %d linked program variants compile to %s as they stand" % (n_ok, len(store), label))
        by_prog = {}
        for key, (ok, _) in store.items():
            for i in b.links[key].instances:
                by_prog.setdefault(i[1].split(" [fallback")[0], []).append(ok)
        n_prog_ok = sum(1 for v in by_prog.values() if all(v))
        print("  %d of %d program names compile in every variant" % (n_prog_ok, len(by_prog)))
        if reasons:
            print("  top reasons:")
            for msg, n in reasons[:25]:
                print("    %5d  %s" % (n, msg))

    if args.json:
        data = {
            "glslang": glslang_version, "profiles": [p.name for p in profiles], "scenarios": [s.name for s in scenarios],
            "units": len(b.units), "links": len(link_jobs), "instances": b.instances,
            "failures": [{"fatal": f[0], "title": f[1], "detail": f[2],
                          "instances": [list(i) for i in f[3].instances]} for f in failures],
            "known_accepted": len(failures) - len(unknown),
            "spirv": {"ok": sum(1 for ok, _ in spirv_results.values() if ok), "total": len(spirv_results),
                      "reasons": group_spirv_reasons(out for ok, out in spirv_results.values() if not ok)} if args.spirv else None,
            "vulkan": {"ok": sum(1 for ok, _ in vulkan_results.values() if ok), "total": len(vulkan_results),
                       "reasons": group_spirv_reasons(out for ok, out in vulkan_results.values() if not ok)} if args.vulkan else None,
            # [VULKAN BACKEND 2026-11-08] the transform + SPIR-V output step: every failed program with its first error lines,
            # so the blockers can be read from the summary without a verbose re-run
            "vulkan_out": {"ok": sum(1 for ok, _ in vk_results.values() if ok), "total": len(vk_results),
                           "reasons": group_spirv_reasons(out for ok, out in vk_results.values() if not ok),
                           "failed": [{"names": sorted({i[1] for i in b.links[k].instances}),
                                       "units": ["%s@c%d %s" % (u.filename, u.found_level, u.stage) for u in b.links[k].units],
                                       "errors": [l for l in out.splitlines() if "ERROR" in l or "transform:" in l][:12]}
                                      for k, (ok, out) in sorted(vk_results.items(), key=lambda kv: b.links[kv[0]].instances[0][1]) if not ok]}
                          if args.vulkan_out else None,
        }
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=1)

    if tmp:
        tmp.cleanup()
    print()
    print("shader_check: done in %.1fs" % (time.time() - t0))
    if args.no_fail:
        return 0
    if any(f[0] for f in unknown):
        return 1
    if unknown and not args.allow_tolerated:
        return 1
    return 0


def load_known(path):
    """scripts/shader_check_known.txt: `profile glob | program-name regex | reason`, # comments."""
    out = []
    if not path or not os.path.isfile(path):
        return out
    import fnmatch
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|", 2)]
            if len(parts) != 3:
                raise SystemExit("%s:%d: expected 'profile|program regex|reason'" % (path, n))
            out.append((parts[0], re.compile(parts[1]), parts[2], fnmatch.fnmatch))
    return out


def known_reason(known, job):
    """The reason a failing job is accepted, when EVERY instance of it matches one known entry."""
    if not known:
        return None
    reasons = set()
    for scen, name, level, tol, group, src in job.instances:
        prof = scen.split("/", 1)[0]
        hit = next((k for k in known if k[3](prof, k[0]) and k[1].search(name)), None)
        if not hit:
            return None
        reasons.add(hit[2])
    return "; ".join(sorted(reasons))


if __name__ == "__main__":
    sys.exit(main())
