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
from, not the text, so a settings-dependent define that a file never uses still finds the same output): per unit
    stage | file name | class found | #version line | defines (sorted k=v, OLD_SELECT excluded) | indexed channels | amd | oldselect
joined with newlines in attach order (the program's own files, then the feature vertex objects, then the feature
fragment objects: LLGLSLShader::createShader llglslshader.cpp:447-478 then LLShaderMgr::attachShaderFeatures). The
viewer computes the same key at run time (indra/llrender/vk/wolfvkshader.cpp) and loads <key>.vert.spv / .frag.spv
from app_settings/shaders/spv/. manifest.json lists every key with its program name, units, sampler bindings and
default values.

Standard library only; needs glslang (the same binary shader_check.py uses).
"""

import hashlib
import json
import os
import re
import subprocess

# llglslshader.cpp:1013-1020 ubo_names in LLGLSLShader::UniformBlock order (llglslshader.h:117-124)
VIEWER_BLOCKS = ["ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials", "WolfEepGains"]
VIEWER_BLOCK_SET = 2
UNIFORM_SET = 0
SAMPLER_SET = 1
BLOCK_BINDING = {"vert": 0, "frag": 1}
BLOCK_NAME = {"vert": "WolfUniformsV", "frag": "WolfUniformsF"}

# llshadermgr.cpp:1353-1366 mReservedAttribs = llvertexbuffer.h:103-120 AttributeType order
ATTRIB_NAMES = ["position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3", "diffuse_color", "emissive",
                "tangent", "weight", "weight4", "clothing", "joint", "texture_index"]

OPAQUE_PREFIXES = ("sampler", "isampler", "usampler", "image", "iimage", "uimage", "texture", "itexture", "utexture",
                   "subpassInput", "atomic_uint")

# a loose uniform: uniform [precision] type name [N] [= init];
UNIFORM_RE = re.compile(r"^\s*uniform\s+(?:(lowp|mediump|highp)\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(\[\s*(\d+)\s*\])?\s*(?:=\s*([^;]+?))?\s*;(.*)$")
# the viewer's own std140 blocks: layout (std140) uniform Name
BLOCK_RE = re.compile(r"^\s*layout\s*\(\s*std140\s*\)\s*uniform\s+([A-Za-z_]\w*)\s*(\{?)\s*$")
# varyings / vertex inputs / fragment outputs: [qualifiers] in|out type name [N];
# the fragment stage's entry point, renamed for the coverage wrapper (one definition per linked stage)
MAIN_RE = re.compile(r"\bvoid\s+main\s*\(\s*(?:void)?\s*\)")
IO_RE = re.compile(r"^\s*((?:(?:flat|smooth|noperspective|centroid|sample|invariant|precise)\s+)*)(in|out)\s+(?:(lowp|mediump|highp)\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(\[\s*(\d+)\s*\])?\s*;(.*)$")
STRUCT_RE = re.compile(r"^\s*struct\s+([A-Za-z_]\w*)\s*\{", re.M)

# GLSL 4.60 section 4.4.1 "Input Layout Qualifiers": a non-matrix takes one location (doubles aside, unused here),
# a matrix as many as it has columns, an array its length times that.
MATRIX_COLUMNS = {"mat2": 2, "mat3": 3, "mat4": 4, "mat2x2": 2, "mat2x3": 2, "mat2x4": 2, "mat3x2": 3, "mat3x3": 3,
                  "mat3x4": 3, "mat4x2": 4, "mat4x3": 4, "mat4x4": 4}


def unit_recipe(stage, filename, found_level, version_line, defines, channels, is_amd, old_select):
    """The per-unit key text; indra/llrender/vk/wolfvkshader.cpp builds the same string."""
    defs = ";".join("%s=%s" % (k, defines[k]) for k in sorted(defines) if k != "OLD_SELECT")
    return "|".join([stage, filename, str(found_level), version_line.strip(), defs, str(channels),
                     "amd" if is_amd else "", "oldselect" if old_select else ""])


def program_key(unit_recipes):
    return hashlib.md5("\n".join(unit_recipes).encode("utf-8")).hexdigest()


def preprocess(glslang, path, stage):
    r = subprocess.run([glslang, "-E", "-S", stage, path], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        return None, (r.stdout or "") + (r.stderr or "")
    return r.stdout, ""


def _struct_text(text, name):
    """The `struct Name { ... };` definition in text, or None."""
    m = re.search(r"struct\s+%s\s*\{" % re.escape(name), text)
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
                return text[m.start():end + 1] if end >= 0 else None
        i += 1
    return None


def _io_locations(type_name, array_size):
    return MATRIX_COLUMNS.get(type_name, 1) * (array_size or 1)


class Transformed:
    """One program's Vulkan GLSL texts, one per unit, plus the manifest data."""
    def __init__(self):
        self.texts = []          # [(unit, text)]
        self.samplers = {}       # name -> binding
        self.defaults = {}       # uniform name -> initialiser text
        self.uniforms = {"vert": {}, "frag": {}}   # name -> (type, array_size)
        self.coverage_location = 0   # the wolf_coverage fragment output's location (one past the shader's own outputs)
        self.error = ""


def transform(glslang, cache_dir, job, unit_path):
    """Turns a shader_check LinkJob (units in attach order) into Vulkan GLSL. Returns a Transformed (error set on failure)."""
    out = Transformed()
    for u in job.units:
        if u.stage not in ("vert", "frag"):
            # the viewer's few geometry-shader programs are not drawn by the Vulkan pass in this cut; the transform
            # keeps to the two stages it maps (declared in docs/VULKAN_BACKEND.md)
            out.error = "stage %s is not handled by the Vulkan transform yet (%s)" % (u.stage, u.filename)
            return out
    pre = []   # [(unit, preprocessed text)]
    for u in job.units:
        text, err = preprocess(glslang, unit_path(cache_dir, u), u.stage)
        if text is None:
            out.error = "preprocess %s: %s" % (u.filename, err.strip())
            return out
        pre.append((u, text))

    # ---- gather: loose uniforms per stage, samplers per program, io per stage
    sampler_decls = {}             # name -> (type, array_size)
    vert_ins, vert_outs, frag_ins, frag_outs = {}, {}, {}, {}
    per_unit = []                  # per unit: dict of parsed line kinds
    for u, text in pre:
        kinds = {}                 # line index -> ("uniform", name) | ("sampler", name, type, n) | ("block", name) | ("io", ...)
        lines = text.split("\n")
        for i, line in enumerate(lines):
            m = UNIFORM_RE.match(line)
            if m:
                precision, typ, name, _, n, init, rest = m.groups()
                n = int(n) if n else 0
                if typ.startswith(OPAQUE_PREFIXES):
                    prev = sampler_decls.get(name)
                    if prev and prev != (typ, n):
                        out.error = "sampler %s declared as %s and %s" % (name, prev, (typ, n))
                        return out
                    sampler_decls[name] = (typ, n)
                    kinds[i] = ("sampler", name, typ, n)
                else:
                    table = out.uniforms[u.stage]
                    prev = table.get(name)
                    if prev and prev != (typ, n):
                        out.error = "uniform %s declared as %s and %s in the %s stage" % (name, prev, (typ, n), u.stage)
                        return out
                    table[name] = (typ, n)
                    if init:
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
                quals, direction, precision, typ, name, _, n, rest = m.groups()
                n = int(n) if n else 0
                if name.startswith("gl_"):
                    continue   # built-in redeclarations (gl_FragDepth layout) stay as they are
                target = {("vert", "in"): vert_ins, ("vert", "out"): vert_outs, ("frag", "in"): frag_ins, ("frag", "out"): frag_outs}[(u.stage, direction)]
                prev = target.get(name)
                if prev and prev != (typ, n):
                    out.error = "%s %s %s declared as %s and %s" % (u.stage, direction, name, prev, (typ, n))
                    return out
                target[name] = (typ, n)
                kinds[i] = ("io", direction, name, quals.strip(), typ, n)
        per_unit.append((u, lines, kinds))

    # ---- assign: sampler bindings, varying locations, attribute locations
    for k, name in enumerate(sorted(sampler_decls)):
        out.samplers[name] = k
    varyings = {}
    loc = 0
    for name in sorted(set(vert_outs) | set(frag_ins)):
        typ, n = vert_outs.get(name) or frag_ins.get(name)
        if name in vert_outs and name in frag_ins and vert_outs[name] != frag_ins[name]:
            out.error = "varying %s is %s in the vertex shader and %s in the fragment shader" % (name, vert_outs[name], frag_ins[name])
            return out
        varyings[name] = loc
        loc += _io_locations(typ, n)
    attribs = {}
    extra = 16
    for name in sorted(vert_ins):
        if name in ATTRIB_NAMES:
            attribs[name] = ATTRIB_NAMES.index(name)
        else:
            attribs[name] = extra
            extra += _io_locations(*vert_ins[name])
    frag_out_loc = {}
    loc = 0
    for name in sorted(frag_outs, key=lambda s: (s != "frag_color" and s != "frag_data", s)):
        frag_out_loc[name] = loc
        loc += frag_outs[name][1] or 1
    # [COVERAGE 2026-11-08] one more fragment output, wolf_coverage, written 1.0 by every fragment that reaches the end
    # of main (a discard never gets there). The Vulkan renderer draws into its own layer and lays it over the OpenGL
    # picture by this coverage, not by the shader's alpha: GL passes drawn with the alpha write masked off (the
    # fullbright pass, lldrawpoolsimple.cpp:69 setColorMask(true, false)) leave alpha 0 in their vertex colours, so
    # alpha says nothing about where the pass drew. main() is renamed wolf_main() and a new main() calls it.
    out.coverage_location = loc

    # ---- emit
    all_texts = "\n".join(t for _, t in pre)
    for u, lines, kinds in per_unit:
        stage_uniforms = out.uniforms[u.stage]
        header = []
        if stage_uniforms:
            # struct definitions the block needs and this unit lacks
            unit_text = "\n".join(lines)
            for name in sorted(stage_uniforms):
                typ = stage_uniforms[name][0]
                if typ[0].isupper() or (typ not in MATRIX_COLUMNS and not re.match(r"^(float|int|uint|bool|double|[biud]?vec[234])$", typ)):
                    if not re.search(r"struct\s+%s\s*\{" % re.escape(typ), unit_text):
                        definition = _struct_text(all_texts, typ)
                        if not definition:
                            out.error = "struct %s for uniform %s not found in any unit" % (typ, name)
                            return out
                        header.append(definition)
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
                    location = varyings[name]
                new_lines.append("layout(location = %d) %s%s %s %s%s;" % (location, quals + " " if quals else "", direction, typ, name, "[%d]" % n if n else ""))
        if header and insert_at >= len(lines):
            new_lines.extend(header)
        text = "\n".join(new_lines)
        if u.stage == "frag":
            text, n_main = MAIN_RE.subn("void wolf_main()", text, count=1)
            if n_main:
                text += ("\n\nlayout(location = %d) out float wolf_coverage;\n"
                         "void main()\n{\n    wolf_main();\n    wolf_coverage = 1.0;\n}\n" % out.coverage_location)
        out.texts.append((u, text))
    return out


def compile_program(glslang, work_dir, key, transformed, out_dir):
    """Writes the transformed units and compiles each stage to out_dir/<key>.<stage>.spv. Returns (ok, output)."""
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
    for stage in ("vert", "frag"):
        paths = by_stage.get(stage)
        if not paths:
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
    return ok, "\n".join(output)


def write_manifest(path, entries):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(entries, fh, indent=1, sort_keys=True)
