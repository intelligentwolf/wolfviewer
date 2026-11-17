# WolfViewer Kraken: sources of the sounds

All four sounds were made from nothing by our own script, from numbers alone: no recording, sample or
third-party sound is in any of them. They are IntelligentWolf Ltd's own work.

The exact recipe is `region-terrain-work/kraken-2026-11-17/tools/make_sounds.py` in the firestorm
repository (ticket 823, 2026-11-17): oscillators, filtered noise, formant resonators and a short room,
a fixed random seed (823), loudness-normalised to -16 LUFS by ffmpeg's EBU R128 `loudnorm`, encoded as
Ogg Vorbis q3, mono, 44.1 kHz. WolfStorm ships the same four files (`wolfstorm/assets/sounds/kraken/`).

| file | length | what it is |
|---|---|---|
| wolf_kraken_rumble.ogg | 8.0 s | the sea churning before it rises: a deep roll, a 32 Hz swell, rising bubbles |
| wolf_kraken_roar.ogg | 4.5 s | a huge wet bellow: a slow 38-62 Hz pulse through three formant resonators, a rasp, a sub-octave |
| wolf_kraken_splash.ogg | 3.0 s | a great body hitting the water: a low thump, a broadband crash, falling droplets |
| wolf_kraken_sink.ogg | 8.0 s | going down: a falling whoosh, a vortex hum, large slow bubbles |
