#!/usr/bin/env python3
"""
wolf_sky_table_compare.py - [W114-VULKAN S8 2026-11-26] texel compare of the Wolf sky's atmosphere tables as OpenGL and
Vulkan built them.

The viewer writes the four tables in use when the debug setting WolfVulkanDebugSkyTablesDump is set (once; it turns
itself off): logs/wolf_sky_tables/gl/<table>.wtbl from OpenGL (WolfTk::Atmosphere::dumpTablesGL) and, when Vulkan
builds the tables, logs/wolf_sky_tables/vk/<table>.wtbl from the Vulkan recorder - the same writer
(WolfTk::Atmosphere::writeTableDump, wolftkatmosphere.h), the same file layout:
    "WOLFTBL1", little-endian u32 width, height, depth, channels (4), u32 wavelengths, u32 spectrum hash low, high,
    then width x height x depth x channels float32, x fastest, then y, then z.

Usage:
    wolf_sky_table_compare.py <gl dir> <vk dir> [--max-rel R] [--floor F] [--force]

Per table it prints: size, the build each side came from (wavelengths, spectrum hash), non-finite values on each side,
max and mean absolute difference, max and 99th-percentile relative difference, the share of components whose relative
difference exceeds 1e-6, 1e-4, 1e-3 and 1e-2, and the worst component (x, y, z, channel, both values).
Relative difference = |a - b| / max(|a|, |b|, floor x the table's largest |value| on the GL side); --floor sets that
fraction (default 1e-6) so values near zero do not inflate it.

Exit status: 0 compared (and, with --max-rel, every table's max relative difference within it), 1 a table over
--max-rel, 2 the files cannot be compared (missing, different sizes, or built from different spectra without --force).
The two renderers compile the same GLSL through different front ends (driver GLSL vs glslang SPIR-V) which may contract
multiply-adds differently, so the expected result is small differences, not bit equality: measure, do not assume.
"""
import argparse
import os
import struct
import sys

try:
    import numpy as np
except ImportError:  # the tool needs numpy (a 256 x 128 x 32 table is 4 million floats)
    sys.stderr.write("wolf_sky_table_compare.py needs numpy (pip install numpy)\n")
    sys.exit(2)

TABLES = ("transmittance", "irradiance", "scattering", "higher_order_scattering")   # Atmosphere::dumpName
HEADER = struct.Struct("<8s7I")   # magic, width, height, depth, channels, wavelengths, hash low, hash high


def read_table(path):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < HEADER.size:
        raise ValueError("%s: too short for a header" % path)
    magic, w, h, d, c, wavelengths, hlo, hhi = HEADER.unpack_from(raw, 0)
    if magic != b"WOLFTBL1":
        raise ValueError("%s: not a WOLFTBL1 file" % path)
    count = w * h * d * c
    body = raw[HEADER.size:]
    if len(body) != count * 4:
        raise ValueError("%s: %d x %d x %d x %d needs %d bytes of data, has %d" % (path, w, h, d, c, count * 4, len(body)))
    data = np.frombuffer(body, dtype="<f4").reshape(d, h, w, c)
    return {"w": w, "h": h, "d": d, "c": c, "wavelengths": wavelengths, "hash": (hhi << 32) | hlo, "data": data}


def compare(name, a, b, floor_frac):
    da = a["data"].astype(np.float64)
    db = b["data"].astype(np.float64)
    bad_a = int(np.count_nonzero(~np.isfinite(da)))
    bad_b = int(np.count_nonzero(~np.isfinite(db)))
    both = np.isfinite(da) & np.isfinite(db)
    diff = np.where(both, np.abs(da - db), 0.0)
    scale = float(np.max(np.abs(np.where(np.isfinite(da), da, 0.0)))) if da.size else 0.0
    floor = max(scale * floor_frac, np.finfo(np.float32).tiny)
    denom = np.maximum(np.maximum(np.abs(da), np.abs(db)), floor)
    rel = np.where(both, diff / denom, 0.0)
    flat = rel.ravel()
    worst = int(np.argmax(flat))
    z, y, x, ch = np.unravel_index(worst, rel.shape)
    n = flat.size
    out = {
        "name": name,
        "size": "%d x %d x %d" % (a["w"], a["h"], a["d"]),
        "nonfinite_gl": bad_a,
        "nonfinite_vk": bad_b,
        "max_abs": float(np.max(diff)),
        "mean_abs": float(np.mean(diff)),
        "max_rel": float(flat[worst]),
        "p99_rel": float(np.percentile(flat, 99.0)),
        "share": {t: float(np.count_nonzero(flat > t)) / n for t in (1e-6, 1e-4, 1e-3, 1e-2)},
        "worst": (int(x), int(y), int(z), int(ch), float(da[z, y, x, ch]), float(db[z, y, x, ch])),
        "scale": scale,
    }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gl_dir")
    ap.add_argument("vk_dir")
    ap.add_argument("--max-rel", type=float, default=None, help="fail (exit 1) when a table's max relative difference exceeds this")
    ap.add_argument("--floor", type=float, default=1e-6, help="relative-difference floor as a fraction of the table's largest value")
    ap.add_argument("--force", action="store_true", help="compare even when the two sides were built from different spectra")
    args = ap.parse_args()

    status = 0
    for name in TABLES:
        pa = os.path.join(args.gl_dir, name + ".wtbl")
        pb = os.path.join(args.vk_dir, name + ".wtbl")
        try:
            a = read_table(pa)
            b = read_table(pb)
        except (OSError, ValueError) as e:
            print("%s: cannot compare: %s" % (name, e))
            status = 2
            continue
        if (a["w"], a["h"], a["d"], a["c"]) != (b["w"], b["h"], b["d"], b["c"]):
            print("%s: sizes differ (gl %dx%dx%dx%d, vk %dx%dx%dx%d)" % (name, a["w"], a["h"], a["d"], a["c"], b["w"], b["h"], b["d"], b["c"]))
            status = 2
            continue
        same_build = a["wavelengths"] == b["wavelengths"] and a["hash"] == b["hash"]
        print("%s: %d x %d x %d; gl built from %d wavelengths, spectrum %016x; vk %d wavelengths, spectrum %016x%s"
              % (name, a["w"], a["h"], a["d"], a["wavelengths"], a["hash"], b["wavelengths"], b["hash"],
                 "" if same_build else "  <-- DIFFERENT BUILDS"))
        if not same_build and not args.force:
            print("  not compared (different spectra: dump both after the same build, or --force)")
            status = 2
            continue
        r = compare(name, a, b, args.floor)
        print("  non-finite: gl %d, vk %d" % (r["nonfinite_gl"], r["nonfinite_vk"]))
        print("  abs diff: max %.6g, mean %.6g (largest |gl| value %.6g)" % (r["max_abs"], r["mean_abs"], r["scale"]))
        print("  rel diff: max %.6g, 99th percentile %.6g" % (r["max_rel"], r["p99_rel"]))
        n = a["w"] * a["h"] * a["d"] * a["c"]
        print("  components over 1e-6 / 1e-4 / 1e-3 / 1e-2: " + ", ".join(
            "%d (%.6f%%)" % (round(r["share"][t] * n), 100.0 * r["share"][t]) for t in (1e-6, 1e-4, 1e-3, 1e-2)) + " of %d" % n)
        x, y, z, ch, va, vb = r["worst"]
        print("  worst: x %d y %d z %d channel %d: gl %.9g vk %.9g" % (x, y, z, ch, va, vb))
        if args.max_rel is not None and r["max_rel"] > args.max_rel and status == 0:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
