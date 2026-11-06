#!/usr/bin/env python3
"""
[SOUND EFFECTS 2026-11-06] WolfViewer's own teleport sounds (wolfsoundeffects.h), made here from scratch: sine partials,
filtered noise and envelopes only, no recording and no sample from anywhere (Paul: no copied material, memory
paul-careful-copyright-trademarks). Run it to rebuild indra/newview/app_settings/wolfteleport/*.ogg:

    python3 scripts/wolfteleport/make_sounds.py

Every sound is mono 44.1 kHz, about 1 to 2.5 seconds, peaks at -9 dBFS (softer than Firestorm's standard teleport sound,
which residents call loud - ticket 568), fades in over 10 ms and out to silence, and is written as Ogg Vorbis (ffmpeg,
libvorbis quality 5). The random parts use fixed seeds, so the files come out the same every run.
"""
import os, subprocess, sys, tempfile, wave
import numpy as np
from scipy.signal import butter, sosfilt

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'indra', 'newview', 'app_settings', 'wolfteleport')
PEAK_DB = -9.0


def t_axis(secs):
    return np.arange(int(secs * RATE)) / RATE


def env(t, attack, decay_tau, total):
    """Rises over `attack` seconds, then falls exponentially, reaching silence at `total`."""
    a = np.clip(t / max(attack, 1e-4), 0, 1) ** 2
    d = np.exp(-np.clip(t - attack, 0, None) / decay_tau)
    tail = np.clip((total - t) / 0.15, 0, 1)        # the last 150 ms go to zero, never a click
    return a * d * tail


def bandpass(x, lo, hi, order=2):
    return sosfilt(butter(order, [lo, hi], btype='band', fs=RATE, output='sos'), x)


def shimmer():
    """Soft shimmer: a bell-like chord whose partials glide up a little as it swells and fades."""
    secs = 2.2; t = t_axis(secs); x = np.zeros_like(t)
    for f, g in ((523.25, 1.0), (659.25, 0.7), (783.99, 0.6), (1046.5, 0.45), (1567.98, 0.25), (2093.0, 0.18)):
        glide = 1 + 0.03 * (t / secs)
        trem = 1 + 0.15 * np.sin(2 * np.pi * (5 + f / 400) * t)
        x += g * np.sin(2 * np.pi * f * glide * t) * trem
    return x * env(t, 0.35, 0.7, secs)


def whoosh():
    """Gentle whoosh: soft noise whose band sweeps up then away, like air rushing past."""
    secs = 1.6; t = t_axis(secs)
    rng = np.random.default_rng(568)
    noise = rng.standard_normal(len(t))
    out = np.zeros_like(t)
    blocks = 48
    hop = len(t) // blocks
    win = np.hanning(2 * hop + 1)[:2 * hop]          # 50 % overlapping Hann windows sum to one: no steps between bands
    for b in range(-1, blocks + 1):
        c = 300 * (12 ** (min(max(b, 0), blocks - 1) / (blocks - 1)))
        seg = bandpass(noise, c * 0.6, min(c * 1.6, RATE / 2 - 100))
        lo = b * hop - hop
        w = np.zeros_like(t)
        a0, a1 = max(lo, 0), min(lo + 2 * hop, len(t))
        if a1 > a0:
            w[a0:a1] = win[a0 - lo:a1 - lo]
        out += seg * w
    out = sosfilt(butter(2, 5000, btype='low', fs=RATE, output='sos'), out)
    return out * env(t, 0.5, 0.35, secs)


def chime():
    """Crystal chime: three rising glass-like notes, each with a quick bloom and a long soft ring."""
    secs = 2.4; t = t_axis(secs); x = np.zeros_like(t)
    for k, f in enumerate((880.0, 1108.73, 1318.51)):
        start = 0.12 * k
        tt = np.clip(t - start, 0, None); on = (t >= start).astype(float)
        mod = 1.0 * np.exp(-tt / 0.3) * np.sin(2 * np.pi * f * 2.0 * tt)     # a touch of FM: the glassy edge
        note = np.sin(2 * np.pi * f * tt + mod) + 0.25 * np.sin(2 * np.pi * f * 2.76 * tt) * np.exp(-tt / 0.25)
        x += on * note * np.sin(np.pi / 2 * np.clip(tt / 0.012, 0, 1)) ** 2 * np.exp(-tt / 0.6) * (1 - 0.15 * k)
    return x * env(t, 0.005, 10.0, secs)


def warp():
    """Warp: a smooth rising sweep with a soft shimmer of noise under it, ending in a quick bright lift."""
    secs = 1.8; t = t_axis(secs)
    f = 160 * (2 ** (3.2 * (t / secs) ** 1.6))                    # 160 Hz up about three octaves
    phase = 2 * np.pi * np.cumsum(f) / RATE
    vib = 0.004 * np.sin(2 * np.pi * 6 * t)
    tone = np.sin(phase * (1 + vib)) + 0.35 * np.sin(2 * phase) + 0.15 * np.sin(3 * phase)
    rng = np.random.default_rng(97)
    hiss = bandpass(rng.standard_normal(len(t)), 2000, 7000) * 0.25 * (t / secs) ** 2
    return (tone + hiss) * env(t, 0.6, 0.45, secs)


def sparkle():
    """Magic sparkle: a soft pad with little high pings scattered over it, thinning out as it fades."""
    secs = 2.3; t = t_axis(secs)
    pad = sum(g * np.sin(2 * np.pi * f * t) for f, g in ((392.0, 0.5), (587.33, 0.35), (783.99, 0.25)))
    pad *= env(t, 0.4, 0.8, secs) * 0.6
    rng = np.random.default_rng(571)
    pings = np.zeros_like(t)
    for _ in range(26):
        start = rng.uniform(0.05, 1.5) ** 1.3
        f = rng.choice([1567.98, 1760.0, 2093.0, 2349.32, 2637.02, 3135.96])
        tt = np.clip(t - start, 0, None); on = (t >= start).astype(float)
        pings += on * np.sin(2 * np.pi * f * tt) * np.sin(np.pi / 2 * np.clip(tt / 0.006, 0, 1)) ** 2 * np.exp(-tt / 0.12) * rng.uniform(0.2, 0.5)
    return pad + pings * np.clip((secs - t) / 0.3, 0, 1)


def pop():
    """Quiet pop: a short, round 'bloop' - for anyone who wants to know they went, and little more."""
    secs = 0.7; t = t_axis(secs)
    f = 520 * np.exp(-t / 0.08) + 180
    phase = 2 * np.pi * np.cumsum(f) / RATE
    return np.sin(phase) * env(t, 0.008, 0.12, secs)


SOUNDS = {
    'wolf_tp_shimmer.ogg': shimmer,
    'wolf_tp_whoosh.ogg': whoosh,
    'wolf_tp_chime.ogg': chime,
    'wolf_tp_warp.ogg': warp,
    'wolf_tp_sparkle.ogg': sparkle,
    'wolf_tp_pop.ogg': pop,
}


def finish(x):
    x = x - np.mean(x)
    x *= (10 ** (PEAK_DB / 20)) / max(np.max(np.abs(x)), 1e-9)
    fade = int(0.01 * RATE)
    x[:fade] *= np.linspace(0, 1, fade)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x


def write_ogg(path, x):
    with tempfile.TemporaryDirectory() as d:
        wav = os.path.join(d, 'x.wav')
        with wave.open(wav, 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, '-c:a', 'libvorbis', '-q:a', '5', '-map_metadata', '-1',
                        '-fflags', '+bitexact', path], check=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, make in SOUNDS.items():
        x = finish(make())
        write_ogg(os.path.join(OUT, name), x)
        print('%-22s %.2f s' % (name, len(x) / RATE))


if __name__ == '__main__':
    sys.exit(main())
