# WolfViewer weather sounds: sources and licences

Every recording here was dedicated to the public domain under **CC0 1.0 Universal**
(https://creativecommons.org/publicdomain/zero/1.0/). CC0 needs no attribution. We credit the
recordists anyway, with thanks.

The files were cut, cleaned and looped by
`region-terrain-work/rain-sound/tools/build_assets.py` in the firestorm repository. That script
is the exact recipe: excerpt times, filters, loudness and encoding.

## Original recordings (SHA-1 of the file as downloaded)

| # | Recording | Recordist | Where it was published | Licence | SHA-1 |
|---|---|---|---|---|---|
| A | "Light Rain Distant Thunder July 5th 2016" (48 kHz float stereo WAV; Marantz PMD661 and 2x Sennheiser ME66, Illinois woodland) | kvgarlic | https://commons.wikimedia.org/wiki/File:Light_Rain_Distant_Thunder_July_5th_2016.wav (originally https://freesound.org/people/kvgarlic/sounds/349454/) | CC0 1.0 | f01eb449e15f0a7647ba37ea3b54713ebb82d71c |
| B | "Amb Rain Loop 2", `amb_rain2.flac` (48 kHz 16-bit stereo, GoPro Hero 12 raw audio) | Kresiek The Furry | https://opengameart.org/content/amb-rain-loop-2 | CC0 | a4a0919a972b0a2350b6aed1c9effd249fc3111c |
| C | "rain loopable", `4.ogg` from `Rain OGG.zip` (zip SHA-1 f56de6b6c318a5f999f6e2f3b3c486bba2c3e1db) | Ylmir | https://opengameart.org/content/rain-loopable | CC0 | 73b37fff7d05d5298c539e3ac8ecea6cc791db84 |
| D | "Rain Gutter Loop", `rain-gutter-loop_0.mp3` | Ogrebane | https://opengameart.org/content/rain-gutter-loop | CC0 | 10312cc39e7872b35cabb56bc7d03083c935c864 |

## Shipped files

| File | Made from | SHA-1 |
|---|---|---|
| wolf_rain_bed_light.ogg | A, 50-107 s | 03d5cd62199300ba7b4bd9e7dda1c659fd0744fc |
| wolf_rain_bed_moderate.ogg | B, 420-474 s | b1e81ecec8d93dc60c5b169fb0176884b6158a5e |
| wolf_rain_bed_heavy.ogg | B, 495-555 s and 615-675 s laid together | 662bea05d8f86e3859058818729a969e0476693a |
| wolf_rain_near_soft.ogg | C, whole | 56ee547022d39e10440ca3c4e6e78625b3924bfa |
| wolf_rain_near_leaves.ogg | A, 0-21 s and 32-48 s | e0c01dd9cdc8c41d4df0ae2a82aff3a909a1fd90 |
| wolf_rain_near_roof.ogg | D, whole | 37014692f79cf1d4a8ae0c5ffcee34fe6827bf6a |
| wolf_rain_drip_1..9.ogg | A, single drops at 6.503, 10.886, 16.734, 42.385, 51.759, 75.887, 87.515, 99.507 and 108.692 s | see `region-terrain-work/rain-sound/build/SHA1SUMS` |

Ogg Vorbis, 44.1 kHz, quality 3. The loops are seamless and measured at -14 LUFS. The drops are
mono.

2026-10-28 (ticket 301, "frequent stops and hiccups" in light rain): the six loops were rebuilt.
The old recipe turned the whole bed down under every loud drop, so the light bed dropped out about
85 times a minute. Now the drops that hit the microphone are patched out with the rain from just
beside them, and nothing turns the rain down. The drops are unchanged.
