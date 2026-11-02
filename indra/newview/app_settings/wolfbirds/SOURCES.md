# WolfViewer bird calls: sources and licences

Every recording here was dedicated to the public domain under **CC0 1.0 Universal**
(https://creativecommons.org/publicdomain/zero/1.0/). CC0 needs no attribution. We credit the
recordists anyway, with thanks.

The calls were cut, filtered and encoded by
`region-terrain-work/birds-2026-11-02/tools/build_sounds.py` in the firestorm repository. That
script is the exact recipe: excerpt times, filters, loudness and encoding.

## Original recordings (SHA-1 of the file as downloaded, 2 November 2026)

| # | Recording | Recordist | Where it was published | Licence | SHA-1 |
|---|---|---|---|---|---|
| A | European herring gull, flight calls, Carolles (Manche, France) | Sonothèque ADVL | https://commons.wikimedia.org/wiki/File:XC707075_-_European_Herring_Gull_-_Larus_argentatus.mp3 (originally https://xeno-canto.org/707075) | CC0 1.0 | ce240bd56d8930b07729d0f4324548bd10967e88 |
| B | European herring gull, Perthshire and Kinross, Scotland, 14 July 2024 | L Rose (iNaturalist lrose23) | https://www.inaturalist.org/observations/229340575 (sound 1132954) | CC0 | d114686b5637fc363993434ab1977c5ecb7a8f7b |
| C | European herring gull, Carlisle, England, 26 August 2022 | Ash David (iNaturalist corehope) | https://www.inaturalist.org/observations/132465565 (sound 530329) | CC0 | 30333569037cef8794bd4c92e8b7883d7f74d0ff |
| D | Common tern, North Wildwood, New Jersey, 22 July 2025 | Matt Schultz (iNaturalist matthias55) | https://www.inaturalist.org/observations/300246554 (sound 1578324) | CC0 | dc4831d1071ff8cf752c1d526f336ee39b35100f |
| E | Sandwich tern, Cemaes Bay, Anglesey, 14 April 2026 | Paul Round (iNaturalist paulround1985) | https://www.inaturalist.org/observations/350133466 (sound 1861618) | CC0 | 1ab286823c3d5f363066b8d512c040223cbca611 |
| F | House sparrows ("two house sparrows tweeting back and forth") | *ngoomie | https://commons.wikimedia.org/wiki/File:House_Sparrow_202279702.wav | CC0 1.0 | 5945ece964bc1852365fc66a9d98712e60826eeb |
| G | European starling, North Inch and Muirton, Perth, Scotland, 24 May 2024 | L Rose (iNaturalist lrose23) | https://www.inaturalist.org/observations/217872641 (sound 1055887) | CC0 | e983be446c5f9f48e5c9e0d6b443453ccfcc33ef |

## Shipped files

| File | Made from | SHA-1 |
|---|---|---|
| wolf_bird_gull_1.ogg | A, 6.15-8.90 s | 9de26e1b31a80641173f67b5eb55fc22294c3f00 |
| wolf_bird_gull_2.ogg | B, 4.95-6.30 s | 1c02940b16c6220d8601488f12d5f7c1a58433fd |
| wolf_bird_gull_3.ogg | C, 1.50-3.80 s | ea32479467b26dc46b115fa3527266f516added5 |
| wolf_bird_tern_1.ogg | D, 0.55-1.95 s | e7dee692a15b9b9b6337cd60b3f0436beee6c989 |
| wolf_bird_tern_2.ogg | D, 2.50-5.05 s | 419db6b51fc9710de781b10fe199cd06ddfb1f77 |
| wolf_bird_tern_3.ogg | E, 0.00-2.40 s | 3d87e721b196487853fe20419dbd6ae032ee35b7 |
| wolf_bird_small_1.ogg | F, 12.85-13.35 s | c7792585c45968ff0848f6628860e117f99afd03 |
| wolf_bird_small_2.ogg | F, 23.50-24.35 s | 2a1a785e7681c8f4507d4195450dbc356b6454d2 |
| wolf_bird_small_3.ogg | F, 48.00-48.75 s | d4dc7c3118da77f66b50b1dac60dd66241dba287 |
| wolf_bird_flock_1.ogg | G, 8.40-11.00 s | 533a6a4368c9d32ce111fe6f72e695fd771bcbae |

Ogg Vorbis, mono, 44.1 kHz, quality 3. Each call is band-passed (250 Hz to 11 kHz), faded in over
15 ms and out over 120 ms, and peak-normalised to -3 dBFS. The birds' bodies and wings are drawn
from our own geometry in wolfbirds.cpp; no third-party model or texture is used.
