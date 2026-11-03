# WolfViewer dolphins and whales: sources and licences

Every recording used here was dedicated to the public domain under **CC0 1.0 Universal**
(https://creativecommons.org/publicdomain/zero/1.0/). CC0 needs no attribution. We credit the
recordists anyway, with thanks. The licences were read on 3 November 2026: Wikimedia Commons'
`LicenseShortName` (CC0) and the iNaturalist API (each sound's own `license_code`: cc0).

Two of the sounds (a second whale blow and the small splash of a dolphin's re-entry) were made from
nothing by our own script, from filtered noise: no recording is in them.

The sounds were cut, cleaned and encoded by
`region-terrain-work/sea-life-2026-11-03/tools/build_sounds.py` in the firestorm repository (ticket
499). That script is the exact recipe: excerpt times, filters, noise reduction, gate, fades, loudness,
the two synthesised sounds and the encoding. WolfStorm ships the same ten files
(`wolfstorm/assets/sounds/sealife/`).

## Original recordings (SHA-1 of the file as downloaded)

| # | Recording | Recordist | Where it was published | Licence | SHA-1 |
|---|---|---|---|---|---|
| A | Dolphins calling under water, Caribbean Sea, Mexico, 24 July 2012 | felix.blume | https://commons.wikimedia.org/wiki/File:161691_felixblume_dolphin-screaming-underwater-in-caribbean-sea-mexico.wav (originally https://freesound.org/people/felix.blume/sounds/161691/) | CC0 1.0 | f6167835f34c12e2b9d3e5999fcc57086c3e1194 |
| B | Whales and dolphins, Nueva Esparta, Venezuela, 2007 | Wilfredo R Rodriguez H (own work) | https://commons.wikimedia.org/wiki/File:Whales_and_Dolphins_whale_nature_sounds_songs_nueva_esparta.ogg | CC0 1.0 | aedb23d43828cd8d262bfd609bd1434670311e74 |
| C | Humpback whales bubble-net feeding (hydrophone), Prince of Wales-Hyder, Alaska, 25 July 2025 | stevestevens (iNaturalist) | https://www.inaturalist.org/observations/301130821 (sound 1582536) | CC0 | 41a49d29fd004c3237d96e441fd52ad8c29bb74b |
| D | Humpback whales (hydrophone), Hoonah-Angoon, Alaska, 2 August 2025 | stevestevens (iNaturalist) | https://www.inaturalist.org/observations/303343228 (sound 1591653) | CC0 | 896d786cea4ce650769ef2d17d2a69f257539950 |
| E | Humpback whale, trumpeting blows, 17 February 2024 | stevestevens (iNaturalist) | https://www.inaturalist.org/observations/199644345 (sound 900064) | CC0 | 4784ac16cf8141b3ea54efd41c753375d6c71f77 |

## Shipped files

| File | Made from | Band | Loudness | SHA-1 |
|---|---|---|---|---|
| wolf_sea_dolphin_1.ogg | A, 8.74-12.59 s, gated | 1-16 kHz | -22 LUFS | cd1ac221888e5f4efb30b0fb41f65041dde465c6 |
| wolf_sea_dolphin_2.ogg | A, 16.58-18.90 s, gated | 1-16 kHz | -22 LUFS (peak-capped) | fca2b32020c1ec307b8443924cfd65d604baa385 |
| wolf_sea_dolphin_3.ogg | A, 31.54-35.20 s, gated | 1-16 kHz | -22 LUFS (peak-capped) | ff3659f25ecc304a347591eeb007df098aab6790 |
| wolf_sea_dolphin_4.ogg | A, 36.60-40.02 s, gated | 1-16 kHz | -22 LUFS (peak-capped) | fbff62ddf3f1c218ab762db2cc22746254aabb55 |
| wolf_sea_whale_song_1.ogg | B, 118.14-157.90 s | 60 Hz-4 kHz | -22 LUFS (peak-capped) | 2a3e6bdcb256d09029a1bc824e94295fce1c2e00 |
| wolf_sea_whale_song_2.ogg | C, 0.12-9.75 s | 120 Hz-6 kHz | -22 LUFS | 78a0327c7c7e53070de03aa751aecc9e1652573f |
| wolf_sea_whale_song_3.ogg | D, 0.35-15.97 s | 120 Hz-6 kHz | -22 LUFS | 141211dc9d9c52d427ac2b34473f484332233b38 |
| wolf_sea_whale_blow_1.ogg | E, 1.36-2.80 s | 60 Hz-6 kHz | -18 LUFS | da74b8110f28685daddf33a3ba97cf421aa55a1c |
| wolf_sea_whale_blow_2.ogg | our own synthesis (build_sounds.py synth_blow) | 150 Hz-2.5 kHz | -18 LUFS | 1291d0e18d5a5bb58825fa2d60205438e06abcaf |
| wolf_sea_splash_1.ogg | our own synthesis (build_sounds.py synth_splash) | 0.5-7 kHz | -24 LUFS | 1b9ac7054373e9681ca853f782d82c5cdac36608 |

Ogg Vorbis, mono, 44.1 kHz, quality 3. Each excerpt is band-passed, spectrally de-noised against
the recording's own background, cut at the quietest point near each end and faded (dolphin calls
also gated to silence between calls; whale song and blows keep their tails), and set to the loudness
above with peaks at or under -1.5 dBFS. The dolphins' and whales' bodies, fins, flukes and spray are
drawn from our own geometry in wolfsealife.cpp; no third-party model or texture is used.
