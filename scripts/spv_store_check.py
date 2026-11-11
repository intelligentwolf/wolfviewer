#!/usr/bin/env python3
"""[G7 2026-11-11] Is a packaged Vulkan SPIR-V store (app_settings/shaders/spv) complete enough to ship?

W101 shipped with no SPIR-V at all, and the old gates only asked for the composite pair, manifest.json and ONE program
file, so a store that was mostly missing (a stale or cut-short scripts/shader_check.py --vulkan-out run, a partial copy)
still went out: every program without its SPIR-V is drawn by OpenGL only, and a Vulkan session then does almost nothing.

The SPIR-V step (shader_check.py --vulkan-out) now also writes summary.json: how many linked programs it was given
("programs"), how many it transformed and compiled ("compiled") and how many distinct program files that makes
("spv_programs"). This check asks, of the store as packaged:
  - the composite pair (wolfvk_composite.vert/.frag.spv: WolfVkScene::init cannot start without it) and manifest.json;
  - summary.json, with compiled / programs at least MIN_RATIO (and at least the ratio the build itself required);
  - every program named in manifest.json present as both .vert.spv and .frag.spv, and as many as summary.json says.
Programs the transform cannot handle yet are expected (about 30% today, all drawn by OpenGL); GL compile failures
outside scripts/shader_check_known.txt already fail the build in the check_shaders target (indra/cmake/ShaderCheck.cmake).

Used by indra/newview/viewer_manifest.py (every platform's package step) and as a command:
    python3 scripts/spv_store_check.py <spv dir>      exit 1 with the reasons when the store must not ship
The CI linux-bundle job (no checkout) repeats the same rules inline (.github/workflows/wolfviewer.yml).
"""
import json
import os
import sys

# The lowest share of linked programs that must have SPIR-V. 2026-11-11 measurements of the transform (shader_check.py
# --vulkan-out, glslang 16.6.0, this tree's shaders): 924 of 1314 (70%) with --quick; 5207 of 7254 (72%, 18766 recipe
# keys, the size of the W103 release store) with the full CI set. Below 60% the store is "mostly missing", not "the
# known transform gaps".
MIN_RATIO = 0.6


def check(spv_dir):
    """A list of reasons the store must not ship (empty: it may), and one line describing it."""
    errors = []
    if not os.path.isdir(spv_dir):
        return ["no SPIR-V directory %s" % spv_dir], ""
    for name in ("wolfvk_composite.vert.spv", "wolfvk_composite.frag.spv", "manifest.json", "summary.json"):
        p = os.path.join(spv_dir, name)
        if not os.path.isfile(p) or os.path.getsize(p) == 0:
            errors.append("no %s (the Vulkan renderer's SPIR-V step did not run or was cut short: indra/cmake/VulkanShaders.cmake)" % name)
    if errors:
        return errors, ""
    try:
        with open(os.path.join(spv_dir, "manifest.json"), encoding="utf-8") as fh:
            manifest = json.load(fh)
        with open(os.path.join(spv_dir, "summary.json"), encoding="utf-8") as fh:
            summary = json.load(fh)
        programs = int(summary["programs"])
        compiled = int(summary["compiled"])
        spv_programs = int(summary["spv_programs"])
        required = max(MIN_RATIO, float(summary.get("min_ratio", 0.0)))
    except (ValueError, KeyError, TypeError) as e:
        return ["manifest.json or summary.json unreadable: %s" % e], ""
    ratio = compiled / programs if programs else 0.0
    if programs <= 0 or ratio < required:
        errors.append("only %d of %d linked programs have SPIR-V (%.0f%%, at least %.0f%% required)" % (compiled, programs, ratio * 100, required * 100))
    keys = {v.get("spv") for v in manifest.values() if isinstance(v, dict) and v.get("spv")}
    missing = ["%s.%s.spv" % (k, st) for k in sorted(keys) for st in ("vert", "frag")
               if not os.path.isfile(os.path.join(spv_dir, "%s.%s.spv" % (k, st))) or os.path.getsize(os.path.join(spv_dir, "%s.%s.spv" % (k, st))) == 0]
    if missing:
        errors.append("%d program file(s) named in manifest.json are missing or empty (first: %s)" % (len(missing), missing[0]))
    if len(keys) != spv_programs:
        errors.append("manifest.json names %d programs, the SPIR-V step wrote %d (summary.json): the store is not one run's output" % (len(keys), spv_programs))
    line = "%d of %d linked programs with SPIR-V (%.0f%%), %d program files, %d recipe keys" % (compiled, programs, ratio * 100, len(keys), len(manifest))
    return errors, line


def main(argv):
    if len(argv) != 2:
        print("usage: spv_store_check.py <app_settings/shaders/spv>", file=sys.stderr)
        return 2
    errors, line = check(argv[1])
    if line:
        print("SPIR-V store: " + line)
    for e in errors:
        print("SPIR-V store NOT shippable: " + e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
