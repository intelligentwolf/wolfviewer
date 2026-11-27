#!/usr/bin/env python3
"""
[VULKAN BACKEND 2026-11-08] Vulkan GLSL transform and SPIR-V output for shader_check.py (--vulkan-out).

The viewer's 289 GLSL files are OpenGL GLSL: every uniform is a loose `uniform float x;`, every sampler a loose
`uniform sampler2D tex;`, varyings and outputs carry no locations. Vulkan GLSL (GL_KHR_vulkan_glsl) accepts none of
that: non-opaque uniforms must live in a block, every opaque uniform needs `layout(set, binding)`, and (compiling the
stages one at a time, as glslang's -o needs) the varyings must carry explicit locations so the stages agree.
shader_check.py found 0 of 7,246 program variants compiled as Vulkan GLSL, every one failing first on "non-opaque
uniforms outside a block".

This module does the port mechanically, at build time, on the stitched and preprocessed text of each program, so the
289 source files stay as they are and the OpenGL path is untouched:

  1. glslang -E (the unit's own defines are already injected by the stitcher) gives one preprocessed text per unit:
     `#ifdef` resolved, macros in array sizes expanded, `#wolf_include` already inlined.
  2. Every loose non-opaque `uniform` declaration of the program's units is gathered, per stage, into one block
         layout(std140, set = 0, binding = 0) uniform WolfUniformsV { ... };   (vertex)
         layout(std140, set = 0, binding = 1) uniform WolfUniformsF { ... };   (fragment)
     whose members are the union over the stage's units (a name declared in two units must have one type, as GL's
     linker already demands), in sorted order, and the same block text goes into every unit of the stage (GLSL
     linking: blocks with the same name must match). Members are referenced unqualified, so the shader bodies need
     no change. Struct-typed members take their `struct` definition along into units that lack it.
     Default initialisers (`uniform int gltf_node_id = 0;`) are not allowed on block members: they are removed and
     recorded in the manifest (the viewer's uniform store applies them as the starting values).
  3. Samplers get `layout(set = 1, binding = k)` with k from the sorted unique sampler names of the program; the
     viewer's existing `layout (std140) uniform Name {` blocks (ReflectionProbes, GLTFJoints, GLTFNodes, GLTFMaterials,
     WolfEepGains: llglslshader.cpp:1013-1020 ubo_names, in LLGLSLShader::UniformBlock order) get set 2, binding =
     their enum value.
  4. Vertex inputs get `layout(location = n)` with n = the LLVertexBuffer attribute index of their name
     (llshadermgr.cpp:1353-1366 mReservedAttribs, llvertexbuffer.h:103-120 AttributeType), others after 16 by name.
     Vertex outputs and fragment inputs get locations from the sorted union of their names (a matrix or array takes
     as many locations as GLSL 4.60 section 4.4.1 gives it). Fragment outputs: `frag_color` / `frag_data[N]` at 0.
  5. Each stage is compiled with `glslang -V -S <stage> -l -o <key>.<stage>.spv unit...` (all the stage's units linked
     into one module, as GL links the shader objects). The #version line becomes `#version 450` for the SPIR-V
    compile (layout(binding)/location need 4.20+; the GL 3.x profiles' 150/330 lines do not have them).

The keys: a program's SPIR-V is named by the MD5 of its "recipe" (the inputs LLShaderMgr::loadShaderFile stitches
from, not the text): per unit
    stage | file name | class found | #version line | defines | indexed channels | amd | oldselect
where defines are the sorted k=v of the defines the file USES ([SPV KEY 2026-11-26]: a name that is a whole word of the
file after #wolf_include, or of a used define's value - shader_check.used_defines; the viewer: LLShaderMgr::
wolfVkFileWords + WolfVkShaderMirror::usedDefines), without OLD_SELECT and without the specialization-constant defines
(SPEC_CONSTANTS). Before, every define was in the key, so a setting that changed a define no unit of a program reads
still gave the program a key the build never made (docs/VULKAN_BACKEND.md 6.2). Stage is vert, geom or frag.
joined with newlines in attach order (the program's own files, then the feature vertex objects, then the feature
fragment objects: LLGLSLShader::createShader llglslshader.cpp:447-478 then LLShaderMgr::attachShaderFeatures). The
viewer computes the same key at run time (indra/llrender/vk/wolfvkshader.cpp) and loads <key>.vert.spv / .frag.spv
(and <key>.geom.spv for a program with a geometry stage) from app_settings/shaders/spv/. manifest.json lists every key
with its program name, units, sampler bindings, default values and specialization constants.

Standard library only; needs glslang (the same binary shader_check.py uses).
"""

import hashlib
import json
import os
import re
import subprocess
import threading

import spv_reflect   # [W114-VULKAN spv2 2026-11-26] static use of samplers and blocks per stage module; the unused-resource strip

# llglslshader.cpp:1013-1020 ubo_names in LLGLSLShader::UniformBlock order (llglslshader.h:117-124)
VIEWER_BLOCKS = ["ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials", "WolfEepGains"]
VIEWER_BLOCK_SET = 2
UNIFORM_SET = 0
SAMPLER_SET = 1
BLOCK_BINDING = {"vert": 0, "frag": 1}
BLOCK_NAME = {"vert": "WolfUniformsV", "frag": "WolfUniformsF"}
# [GEOMETRY 2026-11-26] the stages a program may have, in pipeline order (LLGLSLShader attaches GL_GEOMETRY_SHADER files
# for gNormalDebugProgram / gSkinnedNormalDebugProgram, llviewershadermgr.cpp:4072-4079). The geometry stage gets
# <key>.geom.spv; it has no uniform block of its own (no binding for one exists in wolfvkpipeline.h SET_UNIFORMS), and a
# geometry unit with loose uniforms is reported rather than given an invented binding.
STAGES = ("vert", "geom", "frag")

# llshadermgr.cpp:1353-1366 mReservedAttribs = llvertexbuffer.h:103-120 AttributeType order
ATTRIB_NAMES = ["position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3", "diffuse_color", "emissive",
                "tangent", "weight", "weight4", "clothing", "joint", "texture_index"]

OPAQUE_PREFIXES = ("sampler", "isampler", "usampler", "image", "iimage", "uimage", "texture", "itexture", "utexture",
                   "subpassInput", "atomic_uint")

# a loose uniform: uniform [precision] type name [N] [= init];   or, the array size on the type (GLSL 4.60 section 4.1.9
# "float[5] a;"), uniform [precision] type[N] name [= init];   - the PBR programs' "uniform vec4[2] texture_base_color_transform;"
# (deferred/pbropaqueV.glsl), 1,218 programs of the 8 Nov full run failed on it as "non-opaque uniforms outside a block".
# Groups: precision, type, type array size, name, name array size, initialiser, rest.
UNIFORM_RE = re.compile(r"^\s*uniform\s+(?:(lowp|mediump|highp)\s+)?([A-Za-z_]\w*)\s*(?:\[\s*(\d+)\s*\])?\s+([A-Za-z_]\w*)\s*(?:\[\s*(\d+)\s*\])?\s*(?:=\s*([^;]+?))?\s*;(.*)$")
# the viewer's own std140 blocks: layout (std140) uniform Name
BLOCK_RE = re.compile(r"^\s*layout\s*\(\s*std140\s*\)\s*uniform\s+([A-Za-z_]\w*)\s*(\{?)\s*$")
# varyings / vertex inputs / fragment outputs: [qualifiers] in|out type name [N];
# the fragment stage's entry point, renamed for the coverage wrapper (one definition per linked stage)
MAIN_RE = re.compile(r"\bvoid\s+main\s*\(\s*(?:void)?\s*\)")
# An explicit "layout(location = N)" already in the source (the wolftk programs: wolftk/cloudsF.glsl outputs 0-2,
# wolftk/proceduralV.glsl position at 0) is kept as it is: GL uses that location too, so the draw buffers and attributes
# stay where GL has them. The array size may follow the type ("out vec4[10] vary_coords;", deferred/pbrterrainV.glsl).
# Groups: explicit location, qualifiers, direction, precision, type, type array size, name, name array size, rest.
IO_RE = re.compile(r"^\s*(?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?((?:(?:flat|smooth|noperspective|centroid|sample|invariant|precise)\s+)*)(in|out)\s+(?:(lowp|mediump|highp)\s+)?([A-Za-z_]\w*)\s*(?:\[\s*(\d*)\s*\])?\s+([A-Za-z_]\w*)\s*(?:\[\s*(\d*)\s*\])?\s*;(.*)$")
# [CONST UNIFORMS 2026-11-26] Uniforms whose initialiser is their only value: the viewer never sets them (no reference in
# any C++ file), so in GL they keep the initialiser; on the Vulkan side they become the same "const", initialiser and all
# (a multi-line one stays where it is). A block member could not carry the initialiser (the uniform image starts at 0).
#   POISSON3D_SAMPLES: class3/deferred/screenSpaceReflUtil.glsl:187 "uniform vec3 POISSON3D_SAMPLES[128] = vec3[128](" -
#   128 constant vectors; 968 program variants (every SSR one) failed on it in the 26 Nov full run.
INITIALISED_CONSTANTS = {"POISSON3D_SAMPLES"}
INITIALISED_CONSTANT_RE = re.compile(r"^\s*uniform\s+[A-Za-z_]\w*\s*(?:\[\s*\d*\s*\])?\s+([A-Za-z_]\w*)\s*(?:\[\s*\d*\s*\])?\s*=")
STRUCT_RE = re.compile(r"^\s*struct\s+([A-Za-z_]\w*)\s*\{", re.M)

# GLSL 4.60 section 4.4.1 "Input Layout Qualifiers": a non-matrix takes one location (doubles aside, unused here),
# a matrix as many as it has columns, an array its length times that.
MATRIX_COLUMNS = {"mat2": 2, "mat3": 3, "mat4": 4, "mat2x2": 2, "mat2x3": 2, "mat2x4": 2, "mat3x2": 3, "mat3x3": 3,
                  "mat3x4": 3, "mat4x2": 4, "mat4x3": 4, "mat4x4": 4}


# [SPEC CONSTANTS 2026-11-26] Defines whose value comes straight from a free-valued setting, so no build can list every
# value the viewer may stitch: on the Vulkan side each becomes a specialization constant (GLSL_KHR_vulkan_glsl
# "layout(constant_id = N) const"), left out of the recipe key, and the viewer gives it the value the GL text has at
# pipeline creation (WolfVkShaderMirror::create, indra/llrender/vk/wolfvkshader.h WOLFVK_SPEC_CONSTANTS - the same
# table). The GL text and GL behaviour are unchanged.
#   TERRAIN_TRIPLANAR_BLEND_FACTOR: llviewershadermgr.cpp:1192-1193 llformat("%.2f", RenderTerrainPBRTriplanarBlendFactor),
#   an F32 setting (settings.xml), read only as a value (deferred/pbrterrainUtilF.glsl:251 "float sharpness = ...").
SPEC_CONSTANTS = {"TERRAIN_TRIPLANAR_BLEND_FACTOR": (0, "float")}
# [POINT SIZE 2026-11-26] Vulkan: a pipeline that draws points must write PointSize unless maintenance5 is enabled
# (VUID-VkGraphicsPipelineCreateInfo-topology-08773, Vulkan-Docs chapters/pipelines.adoc: with a vertex stage only and
# POINT_LIST topology "a PointSize decorated variable must be written to if the maintenance5 feature is not enabled";
# chapters/primsrast.adoc "Basic Point Rasterization": "If the maintenance5 feature is enabled, and a value is not
# written to PointSize, the point size takes a default value of 1.0"). OpenGL 4.6 core 14.4 "Points": "If program
# point size mode is disabled, the derived point size is specified with the command void PointSize( float size ) ...
# The default value is 1.0" - and the viewer never enables it (no
# GL_PROGRAM_POINT_SIZE in indra/), so the size is always glPointSize's. The
# viewer calls glPointSize with 8 (pipeline.cpp:5513, :5742), 9 / 18 (llreflectionmapmanager.cpp:1451-1456) and
# deg_point_size() (llmodelpreview.cpp:4860), under whatever program is bound. So every vertex stage gets one more
# member in its uniform block, POINT_SIZE_UNIFORM, and its main() wrapper writes gl_PointSize from it; the viewer starts
# it at 1.0 (WolfVkShaderMirror::create) and the draw recorder sets GL's current point size before a POINTS draw.
# A geometry stage is left alone: the only one (interface/normaldebugG.glsl) emits line strips, whose rasterisation
# does not read PointSize; transform() refuses a geometry unit that outputs points.
POINT_SIZE_UNIFORM = "wolf_point_size"
GEOM_POINTS_RE = re.compile(r"\blayout\s*\([^)]*\bpoints\b[^)]*\)\s*out\b")
DEFINE_LINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+(.*?)[ \t]*$")


def unit_recipe(stage, filename, found_level, version_line, defines, channels, is_amd, old_select):
    """The per-unit key text; indra/llrender/vk/wolfvkshader.cpp builds the same string. defines: the ones the file
    uses (shader_check.recipe_defines; the viewer: WolfVkShaderMirror::usedDefines)."""
    defs = ";".join("%s=%s" % (k, defines[k]) for k in sorted(defines) if k != "OLD_SELECT" and k not in SPEC_CONSTANTS)
    return "|".join([stage, filename, str(found_level), version_line.strip(), defs, str(channels),
                     "amd" if is_amd else "", "oldselect" if old_select else ""])


def program_key(unit_recipes):
    return hashlib.md5("\n".join(unit_recipes).encode("utf-8")).hexdigest()


def preprocess(glslang, path, stage):
    r = subprocess.run([glslang, "-E", "-S", stage, path], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        return None, (r.stdout or "") + (r.stderr or "")
    return r.stdout, ""


def _struct_span(text, name):
    """(start, end) of the `struct Name { ... };` definition in text, or None."""
    m = re.search(r"\bstruct\s+%s\s*\{" % re.escape(name), text)
    if not m:
        return None
    depth = 0
    i = m.end() - 1
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = text.find(";", i)
                return (m.start(), end + 1) if end >= 0 else None
        i += 1
    return None


def _struct_text(text, name):
    """The `struct Name { ... };` definition in text, or None."""
    span = _struct_span(text, name)
    return text[span[0]:span[1]] if span else None


def _struct_closure(text, name, order, path):
    """Appends struct name and every struct type its members use to order, contained ones first (each once).
    Returns an error string, or "" when every definition was found."""
    if name in order:
        return ""
    if name in path:
        return "struct %s contains itself" % name
    definition = _struct_text(text, name)
    if not definition:
        return "struct %s not found in any unit" % name
    body = definition[definition.index("{") + 1:definition.rindex("}")]
    defined = set(STRUCT_RE.findall(text))
    for inner in sorted(set(re.findall(r"\b([A-Za-z_]\w*)\b", body)) & defined - {name}, key=body.find):
        err = _struct_closure(text, inner, order, path + [name])
        if err:
            return err
    order.append(name)
    return ""


def _io_locations(type_name, array_size, text=""):
    """Locations one in/out variable takes. GLSL 4.60 section 4.4.1: a non-matrix one (doubles aside, unused here), a
    matrix as many as it has columns, an array its length times that, and a structure the sum of its members in
    order (the wolftk clouds' "out GroundIrradiance vGroundIrradiance;", wolftk/cloudsTypes.glsl: two vec3 = 2)."""
    count = array_size or 1
    if type_name in MATRIX_COLUMNS:
        return MATRIX_COLUMNS[type_name] * count
    definition = _struct_text(text, type_name) if text and type_name not in _IO_SCALARS else None
    if not definition:
        return count
    body = definition[definition.index("{") + 1:definition.rindex("}")]
    total = 0
    for decl in body.split(";"):
        m = re.match(r"^\s*(?:(?:lowp|mediump|highp|flat|smooth|noperspective|centroid)\s+)*([A-Za-z_]\w*)\s*(?:\[\s*(\d+)\s*\])?\s+(.+?)\s*$", decl, re.S)
        if not m:
            continue
        member_type, type_n = m.group(1), m.group(2)
        for name in m.group(3).split(","):
            nm = re.match(r"^\s*[A-Za-z_]\w*\s*(?:\[\s*(\d+)\s*\])?\s*$", name)
            if nm:
                total += _io_locations(member_type, int(type_n or nm.group(1) or 0), text)
    return total * count


_IO_SCALARS = {"float", "int", "uint", "bool", "double", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3",
               "uvec4", "bvec2", "bvec3", "bvec4", "dvec2", "dvec3", "dvec4"}


def _assign_locations(names, sizes, explicit):
    """Locations for names in order: an explicit one (the source's layout(location = N)) as it is, the rest packed from
    0 upwards around the explicit ranges. Without explicit locations this is the plain running sum."""
    taken = set()
    for name, at in explicit.items():
        taken.update(range(at, at + sizes[name]))
    result = dict(explicit)
    loc = 0
    for name in names:
        if name in result:
            continue
        while any(l in taken for l in range(loc, loc + sizes[name])):
            loc += 1
        result[name] = loc
        taken.update(range(loc, loc + sizes[name]))
        loc += sizes[name]
    return result


class Transformed:
    """One program's Vulkan GLSL texts, one per unit, plus the manifest data."""
    def __init__(self):
        self.texts = []          # [(unit, text)]
        self.samplers = {}       # name -> binding
        self.defaults = {}       # uniform name -> initialiser text
        self.uniforms = {"vert": {}, "geom": {}, "frag": {}}   # name -> (type, array_size)
        self.spec_constants = {}   # [SPEC CONSTANTS 2026-11-26] define name -> constant_id, for the manifest
        self.coverage_location = 0   # the wolf_coverage fragment output's location (one past the shader's own outputs)
        # [W114-VULKAN spv2 2026-11-26] filled by compile_program from the compiled modules (spv_reflect): per stage, the
        # samplers / blocks the module statically uses, {name: descriptor count}, and what the strip removed
        self.used = {}           # stage -> {"samplers": {name: count}, "blocks": [names]}
        self.stripped = {}       # stage -> [names]
        self.error = ""


_PREPROCESS_LOCK = threading.Lock()
_PREPROCESS_KEY_LOCKS = {}
_PREPROCESS_CACHE = {}


def _preprocess_unit(glslang, cache_dir, u, unit_path):
    """Per-unit specialization-constant strip + glslang -E, locked and memoized per unit file.
    Avoids concurrent ThreadPoolExecutor workers overwriting vulkan-spec/<unit> while glslang.exe holds it open on
    Windows (PermissionError / ERROR_SHARING_VIOLATION), and runs glslang -E once per distinct stitched unit (~2,200)
    rather than once per attached unit across all ~7,270 linked programs."""
    orig_path = unit_path(cache_dir, u)
    st_mtime = os.stat(orig_path).st_mtime_ns
    cache_key = (glslang, orig_path, u.stage, st_mtime)
    with _PREPROCESS_LOCK:
        if cache_key in _PREPROCESS_CACHE:
            return _PREPROCESS_CACHE[cache_key]
        key_lock = _PREPROCESS_KEY_LOCKS.setdefault(cache_key, threading.Lock())
    with key_lock:
        with _PREPROCESS_LOCK:
            if cache_key in _PREPROCESS_CACHE:
                return _PREPROCESS_CACHE[cache_key]
        with open(orig_path, encoding="utf-8") as fh:
            stitched = fh.read().split("\n")
        found = {}
        for i, line in enumerate(stitched):
            m = DEFINE_LINE_RE.match(line)
            if m and m.group(1) in SPEC_CONSTANTS:
                found[m.group(1)] = m.group(2)
                stitched[i] = ""   # the name stays in the text unexpanded and names the specialization constant
        path = orig_path
        if found:
            for line in stitched:
                st = line.strip()
                if st.startswith("#") and re.match(r"#\s*(if|ifdef|ifndef|elif)\b", st) and any(re.search(r"\b%s\b" % n, st) for n in found):
                    res = (None, None, "%s: specialization-constant define used in a preprocessor condition (%s)" % (u.filename, st))
                    with _PREPROCESS_LOCK:
                        _PREPROCESS_CACHE[cache_key] = res
                    return res
            spec_dir = os.path.join(cache_dir, "vulkan-spec")
            os.makedirs(spec_dir, exist_ok=True)
            path = os.path.join(spec_dir, os.path.basename(orig_path))
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("\n".join(stitched))
        text, err = preprocess(glslang, path, u.stage)
        if text is None:
            res = (None, None, "preprocess %s: %s" % (u.filename, err.strip()))
        else:
            res = (dict(found), text, "")
        with _PREPROCESS_LOCK:
            _PREPROCESS_CACHE[cache_key] = res
        return res


def transform(glslang, cache_dir, job, unit_path):
    """Turns a shader_check LinkJob (units in attach order) into Vulkan GLSL. Returns a Transformed (error set on failure)."""
    out = Transformed()
    for u in job.units:
        if u.stage not in STAGES:
            out.error = "stage %s of %s is not one of %s" % (u.stage, u.filename, ", ".join(STAGES))
            return out
    has_geom = any(u.stage == "geom" for u in job.units)
    pre = []   # [(unit, preprocessed text)]
    spec_values = []   # per unit: {spec name: default value text}
    for u in job.units:
        found, text, err = _preprocess_unit(glslang, cache_dir, u, unit_path)
        if text is None:
            out.error = err
            return out
        spec_values.append(found)
        pre.append((u, text))

    # ---- gather: loose uniforms per stage, samplers per program, io per stage
    sampler_decls = {}             # name -> (type, array_size)
    vert_ins, vert_outs, frag_ins, frag_outs = {}, {}, {}, {}
    geom_ins, geom_outs = {}, {}   # [GEOMETRY 2026-11-26] interface/normaldebugG.glsl: "in vec4 normal_g[];" / "out vec4 vertex_color;"
    explicit_loc = {}              # (stage, direction, name) -> the source's own layout(location = N)
    per_unit = []                  # per unit: dict of parsed line kinds
    for u, text in pre:
        kinds = {}                 # line index -> ("uniform", name) | ("sampler", name, type, n) | ("block", name) | ("io", ...)
        lines = text.split("\n")
        for i, line in enumerate(lines):
            m = INITIALISED_CONSTANT_RE.match(line)
            if m and m.group(1) in INITIALISED_CONSTANTS:
                kinds[i] = ("const",)
                continue
            m = UNIFORM_RE.match(line)
            if m:
                precision, typ, type_n, name, name_n, init, rest = m.groups()
                if type_n and name_n:
                    out.error = "uniform %s is an array of arrays (%s[%s] %s[%s]), which the transform does not map" % (name, typ, type_n, name, name_n)
                    return out
                n = int(type_n or name_n or 0)
                if typ.startswith(OPAQUE_PREFIXES):
                    prev = sampler_decls.get(name)
                    if prev and prev != (typ, n):
                        out.error = "sampler %s declared as %s and %s" % (name, prev, (typ, n))
                        return out
                    sampler_decls[name] = (typ, n)
                    kinds[i] = ("sampler", name, typ, n)
                else:
                    if u.stage not in BLOCK_BINDING:
                        out.error = "loose uniform %s in the %s stage (%s): no uniform block binding exists for that stage" % (name, u.stage, u.filename)
                        return out
                    table = out.uniforms[u.stage]
                    prev = table.get(name)
                    if prev and prev != (typ, n):
                        out.error = "uniform %s declared as %s and %s in the %s stage" % (name, prev, (typ, n), u.stage)
                        return out
                    table[name] = (typ, n)
                    if init:
                        # A block member starts as 0 in the viewer's uniform image (WolfVkShaderMirror::create assigns the
                        # image zero-filled) and the manifest's defaults are not read at run time, so only a zero
                        # initialiser keeps GL's starting value (gltf/pbrmetallicroughnessV.glsl:240 gltf_node_id = 0).
                        if not re.match(r"^(0|0\.0*|0\.0*f|false|u?int\(0\)|float\(0(\.0*)?\))u?$", init.strip()):
                            out.error = "uniform %s has the non-zero initialiser %s, which the viewer's uniform image would not start with" % (name, init.strip())
                            return out
                        out.defaults[name] = init.strip()
                    kinds[i] = ("uniform", name)
                continue
            m = BLOCK_RE.match(line)
            if m:
                name = m.group(1)
                if name not in VIEWER_BLOCKS:
                    out.error = "uniform block %s is not one of the viewer's blocks (llglslshader.cpp:1013-1020)" % name
                    return out
                kinds[i] = ("block", name, m.group(2))
                continue
            m = IO_RE.match(line)
            if m:
                explicit, quals, direction, precision, typ, type_n, name, name_n, rest = m.groups()
                if type_n is not None and name_n is not None:
                    out.error = "%s %s %s is an array of arrays, which the transform does not map" % (u.stage, direction, name)
                    return out
                if name.startswith("gl_"):
                    continue   # built-in redeclarations (gl_FragDepth layout) stay as they are
                per_vertex = u.stage == "geom" and direction == "in"
                if (type_n == "" or name_n == "") != per_vertex:
                    # GLSL 4.60 section 4.3.4: a geometry shader input is an array over the primitive's vertices, sized by
                    # the input layout ("[]"); anywhere else an unsized array is not an interface variable
                    out.error = "%s %s %s: an unsized array is only mapped for geometry-shader inputs" % (u.stage, direction, name)
                    return out
                n = 0 if per_vertex else int(type_n or name_n or 0)
                target = {("vert", "in"): vert_ins, ("vert", "out"): vert_outs, ("frag", "in"): frag_ins, ("frag", "out"): frag_outs,
                          ("geom", "in"): geom_ins, ("geom", "out"): geom_outs}[(u.stage, direction)]
                prev = target.get(name)
                if prev and prev != (typ, n):
                    out.error = "%s %s %s declared as %s and %s" % (u.stage, direction, name, prev, (typ, n))
                    return out
                target[name] = (typ, n)
                if explicit is not None:
                    key = (u.stage, direction, name)
                    if explicit_loc.get(key, int(explicit)) != int(explicit):
                        out.error = "%s %s %s has two explicit locations (%s, %d)" % (u.stage, direction, name, explicit, explicit_loc[key])
                        return out
                    explicit_loc[key] = int(explicit)
                kinds[i] = ("io", direction, name, quals.strip(), typ, n)
        per_unit.append((u, lines, kinds))

    for u, text in pre:
        if u.stage == "geom" and GEOM_POINTS_RE.search(text):
            out.error = "%s: a geometry stage that outputs points is not mapped (its emitted vertices would need PointSize)" % u.filename
            return out
    if POINT_SIZE_UNIFORM in out.uniforms["vert"] or POINT_SIZE_UNIFORM in out.uniforms["frag"] or POINT_SIZE_UNIFORM in sampler_decls:
        out.error = "the shader declares %s itself, the name of the transform's point-size uniform" % POINT_SIZE_UNIFORM
        return out
    out.uniforms["vert"][POINT_SIZE_UNIFORM] = ("float", 0)   # [POINT SIZE 2026-11-26]

    # ---- assign: sampler bindings, varying locations, attribute locations
    all_pre = "\n".join(t for _, t in pre)   # every unit's text: the struct definitions an in/out of struct type needs
    for k, name in enumerate(sorted(sampler_decls)):
        out.samplers[name] = k
    def explicit_of(*keys):
        found = {explicit_loc[k] for k in keys if k in explicit_loc}
        return found.pop() if len(found) == 1 else (None if not found else -1)
    # Varyings: one location table per stage interface, by name (GLSL linking matches by name; Vulkan by location).
    # Without a geometry stage that is vertex out -> fragment in; with one, vertex out -> geometry in (each geometry
    # input being the per-vertex array of a vertex output, the same locations as one element: GLSL 4.60 section
    # 4.4.1) and geometry out -> fragment in.
    interfaces = [(("vert", "out", vert_outs), ("geom", "in", geom_ins)), (("geom", "out", geom_outs), ("frag", "in", frag_ins))] \
        if has_geom else [(("vert", "out", vert_outs), ("frag", "in", frag_ins))]
    varyings = {}   # (stage, direction, name) -> location
    for (s_a, d_a, side_a), (s_b, d_b, side_b) in interfaces:
        sizes, explicit = {}, {}
        for name in sorted(set(side_a) | set(side_b)):
            typ, n = side_a.get(name) or side_b.get(name)
            if name in side_a and name in side_b and side_a[name] != side_b[name]:
                out.error = "varying %s is %s in the %s stage and %s in the %s stage" % (name, side_a[name], s_a, side_b[name], s_b)
                return out
            sizes[name] = _io_locations(typ, n, all_pre)
            e = explicit_of((s_a, d_a, name), (s_b, d_b, name))
            if e == -1:
                out.error = "varying %s has different explicit locations in the %s and %s stages" % (name, s_a, s_b)
                return out
            if e is not None:
                explicit[name] = e
        for name, at in _assign_locations(sorted(sizes), sizes, explicit).items():
            if name in side_a:
                varyings[(s_a, d_a, name)] = at
            if name in side_b:
                varyings[(s_b, d_b, name)] = at
    attribs = {}
    extra = 16
    for name in sorted(vert_ins):
        if ("vert", "in", name) in explicit_loc:
            attribs[name] = explicit_loc[("vert", "in", name)]   # the source's own location, as GL binds it
        elif name in ATTRIB_NAMES:
            attribs[name] = ATTRIB_NAMES.index(name)
        else:
            attribs[name] = extra
            extra += _io_locations(vert_ins[name][0], vert_ins[name][1], all_pre)
    frag_out_sizes = {name: frag_outs[name][1] or 1 for name in frag_outs}
    frag_out_explicit = {name: explicit_loc[("frag", "out", name)] for name in frag_outs if ("frag", "out", name) in explicit_loc}
    frag_out_loc = _assign_locations(sorted(frag_outs, key=lambda s: (s != "frag_color" and s != "frag_data", s)), frag_out_sizes, frag_out_explicit)
    loc = max([frag_out_loc[name] + frag_out_sizes[name] for name in frag_out_loc] or [0])
    # [COVERAGE 2026-11-08] one more fragment output, wolf_coverage, written 1.0 by every fragment that reaches the end
    # of main (a discard never gets there). The Vulkan renderer draws into its own layer and lays it over the OpenGL
    # picture by this coverage, not by the shader's alpha: GL passes drawn with the alpha write masked off (the
    # fullbright pass, lldrawpoolsimple.cpp:69 setColorMask(true, false)) leave alpha 0 in their vertex colours, so
    # alpha says nothing about where the pass drew. main() is renamed wolf_main() and a new main() calls it.
    out.coverage_location = loc

    # ---- emit
    all_texts = "\n".join(t for _, t in pre)
    for (u, lines, kinds), specs in zip(per_unit, spec_values):
        stage_uniforms = out.uniforms[u.stage]
        header = []
        for name in sorted(specs):
            spec_id, spec_type = SPEC_CONSTANTS[name]
            header.append("layout(constant_id = %d) const %s %s = %s;" % (spec_id, spec_type, name, specs[name]))
            out.spec_constants[name] = spec_id
        if stage_uniforms:
            # The struct definitions the block needs, with the structs they contain, inner ones first (the wolftk
            # AtmosphereParameters holds DensityProfile, which holds DensityProfileLayer: wolftk/atmosphereF.glsl). They
            # go into the header above the block; a unit that defines one of them itself has its own copy blanked (the
            # lines stay, so error lines still map), since the block above it already needs the type.
            needed = []
            for name in sorted(stage_uniforms):
                typ = stage_uniforms[name][0]
                if typ[0].isupper() or (typ not in MATRIX_COLUMNS and not re.match(r"^(float|int|uint|bool|double|[biud]?vec[234])$", typ)):
                    err = _struct_closure(all_texts, typ, needed, [])
                    if err:
                        out.error = "%s (uniform %s)" % (err, name)
                        return out
            if needed:
                unit_text = "\n".join(lines)
                for typ in needed:
                    span = _struct_span(unit_text, typ)
                    if span:
                        a, b = span
                        unit_text = unit_text[:a] + "\n" * unit_text.count("\n", a, b) + unit_text[b:]
                    header.append(_struct_text(all_texts, typ))
                lines = unit_text.split("\n")
            header.append("layout(std140, set = %d, binding = %d) uniform %s" % (UNIFORM_SET, BLOCK_BINDING[u.stage], BLOCK_NAME[u.stage]))
            header.append("{")
            for name in sorted(stage_uniforms):
                typ, n = stage_uniforms[name]
                header.append("    %s %s%s;" % (typ, name, "[%d]" % n if n else ""))
            header.append("};")
        # the header goes after the #version line and any #extension lines that follow it
        insert_at = 0
        for i, line in enumerate(lines):
            st = line.strip()
            if st.startswith("#version") or st.startswith("#extension") or not st or st.startswith("//"):
                insert_at = i + 1
                continue
            break
        new_lines = []
        for i, line in enumerate(lines):
            if i == insert_at and header:
                new_lines.extend(header)
            kind = kinds.get(i)
            if kind is None and line.strip().startswith("#version"):
                # [SHADER VERSION 2026-11-08] the Vulkan compile needs layout(binding) and layout(location) on outputs: GLSL
                # 4.20+ (or ARB_shading_language_420pack), which the viewer's GL 3.x profiles do not declare (#version
                # 150/330 -> "'binding' : not supported for this version", 3,892 programs in the first full run). Every
                # unit is compiled as GLSL 4.50 for SPIR-V; the viewer's own version line stays in the recipe key.
                new_lines.append("#version 450   // was: " + line.strip())
            elif kind is None:
                new_lines.append(line)
            elif kind[0] == "uniform":
                new_lines.append("")   # hoisted into the block
            elif kind[0] == "const":
                new_lines.append(re.sub(r"^(\s*)uniform\b", r"\1const", line, count=1))
            elif kind[0] == "sampler":
                _, name, typ, n = kind
                new_lines.append("layout(set = %d, binding = %d) uniform %s %s%s;" % (SAMPLER_SET, out.samplers[name], typ, name, "[%d]" % n if n else ""))
            elif kind[0] == "block":
                _, name, brace = kind
                new_lines.append("layout(std140, set = %d, binding = %d) uniform %s %s" % (VIEWER_BLOCK_SET, VIEWER_BLOCKS.index(name), name, brace))
            elif kind[0] == "io":
                _, direction, name, quals, typ, n = kind
                if u.stage == "vert" and direction == "in":
                    location = attribs[name]
                elif u.stage == "frag" and direction == "out":
                    location = frag_out_loc[name]
                else:
                    location = varyings[(u.stage, direction, name)]
                size = "[]" if (u.stage == "geom" and direction == "in") else ("[%d]" % n if n else "")
                new_lines.append("layout(location = %d) %s%s %s %s%s;" % (location, quals + " " if quals else "", direction, typ, name, size))
        if header and insert_at >= len(lines):
            new_lines.extend(header)
        text = "\n".join(new_lines)
        if u.stage == "frag":
            text, n_main = MAIN_RE.subn("void wolf_main()", text, count=1)
            if n_main:
                text += ("\n\nlayout(location = %d) out float wolf_coverage;\n"
                         "void main()\n{\n    wolf_main();\n    wolf_coverage = 1.0;\n}\n" % out.coverage_location)
        elif u.stage == "vert":
            # [POINT SIZE 2026-11-26] the same wrapper for the vertex stage: the unit that defines main() ends by writing
            # gl_PointSize (after wolf_main, so a shader that wrote it itself would be overridden - none does: no
            # gl_PointSize in app_settings/shaders)
            text, n_main = MAIN_RE.subn("void wolf_main()", text, count=1)
            if n_main:
                text += "\n\nvoid main()\n{\n    wolf_main();\n    gl_PointSize = %s;\n}\n" % POINT_SIZE_UNIFORM
        out.texts.append((u, text))
    return out


def compile_program(glslang, work_dir, key, transformed, out_dir, strip_unused=False):
    """Writes the transformed units and compiles each stage to out_dir/<key>.<stage>.spv. Returns (ok, output).
    [W114-VULKAN spv2 2026-11-26] Each module is then read back (spv_reflect): the viewer's static-use rule must agree
    with the call-tree rule of the Vulkan spec on every sampler and block (else the program fails here, at build time),
    the statically used ones go to transformed.used for the manifest, and with strip_unused the declared-but-unused
    resource variables are removed from the module (bindings of the kept ones unchanged)."""
    os.makedirs(work_dir, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    by_stage = {}
    for i, (u, text) in enumerate(transformed.texts):
        p = os.path.join(work_dir, "%s_%d.%s" % (key, i, u.stage))
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        by_stage.setdefault(u.stage, []).append(p)
    output = []
    ok = True
    for stage in STAGES:
        paths = by_stage.get(stage)
        if not paths:
            if stage == "geom":
                continue   # the geometry stage is optional (GL: a program without one); vertex and fragment are not
            output.append("no %s units" % stage)
            ok = False
            continue
        target = os.path.join(out_dir, "%s.%s.spv" % (key, stage))
        args = [glslang, "-V", "-S", stage, "-l", "-o", target] + paths
        try:
            r = subprocess.run(args, capture_output=True, text=True, timeout=300)
        except subprocess.TimeoutExpired:
            output.append("glslang timed out on %s" % stage)
            ok = False
            continue
        if r.returncode != 0:
            ok = False
            output.append((r.stdout or "") + (r.stderr or ""))
            continue
        try:
            with open(target, "rb") as fh:
                blob = fh.read()
            disagree = spv_reflect.check_module(blob)
            if disagree:
                ok = False
                output.append("ERROR: %s stage: static use differs between the viewer's rule and the call tree: %s" % (stage, "; ".join(disagree)))
                continue
            if strip_unused:
                blob, removed = spv_reflect.strip_unused(blob)
                transformed.stripped[stage] = removed
                if removed:
                    with open(target, "wb") as fh:
                        fh.write(blob)
            res = spv_reflect.reflect(blob)
        except (OSError, ValueError, IndexError) as e:
            ok = False
            output.append("ERROR: %s stage: reading back %s: %s" % (stage, target, e))
            continue
        transformed.used[stage] = {"samplers": {r.name: r.count for r in res if r.kind == "sampler" and r.used_calltree},
                                   "blocks": sorted(r.name for r in res if r.kind == "block" and r.used_calltree)}
    return ok, "\n".join(output)


def tree_md5(shaders_dir):
    """[STORE STAMP 2026-11-26] MD5 over the shader tree: every *.glsl under class1, class2, ... in sorted order of its
    path relative to shaders_dir with '/' separators, each as  path NUL bytes NUL. The viewer computes the same
    (WolfVkShaderMirror::shaderTreeMD5) from the tree it was packaged with."""
    paths = []
    for root, dirs, names in os.walk(shaders_dir):
        rel_root = os.path.relpath(root, shaders_dir).replace(os.sep, "/")
        if not re.match(r"^class\d+(/|$)", rel_root):
            if rel_root == ".":
                dirs[:] = [d for d in dirs if re.match(r"^class\d+$", d)]
            continue
        for n in names:
            if n.endswith(".glsl"):
                paths.append(rel_root + "/" + n)
    h = hashlib.md5()
    for rel in sorted(paths):
        h.update(rel.encode("utf-8") + b"\0")
        with open(os.path.join(shaders_dir, rel), "rb") as fh:
            h.update(fh.read())
        h.update(b"\0")
    return h.hexdigest()


TRANSFORM_SCRIPTS = ("shader_check.py", "shader_vulkan.py", "spv_reflect.py")


def transform_md5():
    """[STORE STAMP 2026-11-26] MD5 over the scripts that decide the keys and the SPIR-V (TRANSFORM_SCRIPTS, in that order,
    bytes as on disk with CR removed so a Windows checkout gives the same). indra/llrender/CMakeLists.txt computes
    the same value into WOLFVK_TRANSFORM_MD5 for wolfvkshader.cpp."""
    h = hashlib.md5()
    here = os.path.dirname(os.path.abspath(__file__))
    for name in TRANSFORM_SCRIPTS:
        with open(os.path.join(here, name), "rb") as fh:
            h.update(fh.read().replace(b"\r", b""))
    return h.hexdigest()


def store_stamp(shaders_dir, glslang_version, options):
    """What a store was made from: the scripts, the shader tree, the compiler, the run's options (quick, strip, ...)."""
    return {"transform": transform_md5(), "shaders": tree_md5(shaders_dir), "glslang": glslang_version, "options": options}


def write_manifest(path, entries):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(entries, fh, indent=1, sort_keys=True)


if __name__ == "__main__":
    import sys
    if sys.argv[1:] == ["--transform-md5"]:
        sys.stdout.write(transform_md5())
    elif len(sys.argv) == 3 and sys.argv[1] == "--tree-md5":
        sys.stdout.write(tree_md5(sys.argv[2]))
    else:
        sys.exit("usage: shader_vulkan.py --transform-md5 | --tree-md5 <app_settings/shaders>")
