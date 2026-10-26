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
| wolf_rain_bed_light.ogg | A, 50-107 s | 4263ba71823518843c5d89587d48a6ba2e98b610 |
| wolf_rain_bed_moderate.ogg | B, 420-474 s | 7780b6a0bf34fe53b30df92a310624ddfa71fd2f |
| wolf_rain_bed_heavy.ogg | B, 495-555 s and 615-675 s laid together | 5a04cafe90c5be3aa5430d0b46da6d32bbca23e4 |
| wolf_rain_near_soft.ogg | C, whole | 8d6a592b8e173285d493fe8505d367fda3b510fe |
| wolf_rain_near_leaves.ogg | A, 0-21 s and 32-48 s | 9ef44198916edbe0206c3fe76626f3463b74aa70 |
| wolf_rain_near_roof.ogg | D, whole | ad80e5229eb3e56e2a16eb500edcc7f6c5e59545 |
| wolf_rain_drip_1..9.ogg | A, single drops at 6.503, 10.886, 16.734, 42.385, 51.759, 75.887, 87.515, 99.507 and 108.692 s | see `region-terrain-work/rain-sound/build/SHA1SUMS` |

Ogg Vorbis, 44.1 kHz, quality 3. The loops are seamless and measured at -14 LUFS. The drops are
mono.
