# WolfViewer Nature Sounds: sources and licences

Every recording here was dedicated to the public domain under **CC0 1.0 Universal**
(https://creativecommons.org/publicdomain/zero/1.0/). CC0 needs no attribution. We credit the
recordists anyway, with thanks. Each licence was read on the recording's own page on 3 November
2026: Freesound's "Creative Commons 0" licence line, and iNaturalist's API (`license_code` cc0).

The loops were cut, filtered, made seamless and encoded by
`region-terrain-work/nature-sounds-2026-11-03/tools/build_sounds.py` in the firestorm repository.
That script is the exact recipe: excerpt times, filters, loudness and encoding. WolfStorm ships the
same five files (`wolfstorm/assets/sounds/nature/`).

## Original recordings (SHA-1 of the file as downloaded, 3 November 2026)

| # | Recording | Recordist | Where it was published | Licence | SHA-1 |
|---|---|---|---|---|---|
| A | "Crickets at night" | LukaCafuka | https://freesound.org/people/LukaCafuka/sounds/750836/ (Freesound's high-quality Ogg preview, https://cdn.freesound.org/previews/750/750836_16236894-hq.ogg) | Creative Commons 0 | 6d875fc870e7cb9da174443e821fbd97ad7dd43f |
| B | Italian tree cricket (*Oecanthus pellucens*), Vanatori, Romania, 27 July 2021 | Patrascu Lucian-Marius (plucian) | https://www.inaturalist.org/observations/92563990 (sound 296968, https://static.inaturalist.org/sounds/296968.mp3) | CC0 | 8876e2e83c4f809fa6bf809c4bcdfe6b77ffbee8 |
| C | Italian tree cricket (*Oecanthus pellucens*), Vanatori, Romania, 9 September 2021 | Patrascu Lucian-Marius (plucian) | https://www.inaturalist.org/observations/98214060 (sound 316382, https://static.inaturalist.org/sounds/316382.mp3) | CC0 | db6396ce3e0553ae0358c90ed741862bb319ddbb |
| D | "Waves on shore of lake maggiore.wav" | BonnyOrbit | https://freesound.org/people/BonnyOrbit/sounds/442546/ (preview https://cdn.freesound.org/previews/442/442546_5902878-hq.ogg) | Creative Commons 0 | cb96b8d9503cd7665812187ca372ba2929943fc6 |
| E | "water medium gentle lake waves splash on shore close.flac" | kyles | https://freesound.org/people/kyles/sounds/451630/ (preview https://cdn.freesound.org/previews/451/451630_612689-hq.ogg) | Creative Commons 0 | b3e578ac047aa1118edd47c5d75349dff1f6701c |
| F | "Waves crash surf bright detail beach cu loop BAHAMAS 180521.wav" | TRP | https://freesound.org/people/TRP/sounds/573187/ (preview https://cdn.freesound.org/previews/573/573187_97550-hq.ogg) | Creative Commons 0 | b209e6354944e5a44a56f235e90dc2f043b739c5 |
| G | "waves ocean crash on beach nearby wide big metallic sound.flac" | kyles | https://freesound.org/people/kyles/sounds/450634/ (preview https://cdn.freesound.org/previews/450/450634_612689-hq.ogg) | Creative Commons 0 | 7060ee8eff4a21d34dc08c77c41971bcd61fc7e2 |

## Shipped files

| File | Made from | Loudness | SHA-1 |
|---|---|---|---|
| wolf_nature_insects.ogg | A 30-93 s, 2.5-12 kHz; B whole, 1.5-6 kHz, 5 dB under A, left, from 12 s with 4 s fades; C 0-63 s, 1.5-6 kHz, 8 dB under A, right | -14.0 LUFS | a48048fdd0c1e9c15c8913ae3275a3021db5a7b8 |
| wolf_nature_lapping.ogg | D 85-148 s, high-passed at 80 Hz | -21.8 LUFS | 96b95d4508d2bdc4fd2b8d949313aeae8235c594 |
| wolf_nature_surf_small.ogg | E 15-78 s, high-passed at 40 Hz | -23.5 LUFS | e8b31692c60d64e9e519a8762a5f40a58a5b679d |
| wolf_nature_surf_medium.ogg | F 165-228 s, high-passed at 30 Hz | -18.9 LUFS | e9bba2aac22994e3f01796980a669948ef2127de |
| wolf_nature_surf_big.ogg | G 45-108 s, high-passed at 30 Hz | -20.8 LUFS | 8f1526d7a1ecd25da32e6d18bb5148e1ae800373 |

Ogg Vorbis, 44.1 kHz stereo, quality 3, 60 s seamless loops (a 3 s equal-power overlap of each
excerpt's tail into its head). The water loops are the steadiest 63 s of each recording (the
smallest spread of its 5-second levels). Each is aimed at -14 LUFS like the rain and then capped at
-1.5 dBFS peak, which leaves the water loops quieter; wolfnaturesounds.cpp FILE_LUFS holds the
measured figures above, so each layer still plays at its chosen level. They were chosen by analysis
and spectrograms, not by ear.
