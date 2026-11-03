# WolfViewer bird calls: sources and licences

Every recording here was dedicated to the public domain under **CC0 1.0 Universal**
(https://creativecommons.org/publicdomain/zero/1.0/). CC0 needs no attribution. We credit the
recordists anyway, with thanks. The iNaturalist licences were read through the iNaturalist API
(each sound's own `license_code`: cc0, attribution "no rights reserved") on 3 November 2026; the
Commons licences through Commons' `LicenseShortName` (CC0) on 2 November 2026.

The calls were cut, cleaned and encoded by
`region-terrain-work/birdcalls-clean-2026-11-03/tools/build_clean.py` in the firestorm repository
(ticket 490). That script is the exact recipe: excerpt times, filters, noise reduction, gate, fades,
loudness and encoding. WolfStorm ships the same ten files (`wolfstorm/assets/sounds/birds/`).

## Original recordings (SHA-1 of the file as downloaded)

| # | Recording | Recordist | Where it was published | Licence | SHA-1 |
|---|---|---|---|---|---|
| A | European herring gull, flight calls, Carolles (Manche, France) | Sonothèque ADVL | https://commons.wikimedia.org/wiki/File:XC707075_-_European_Herring_Gull_-_Larus_argentatus.mp3 (originally https://xeno-canto.org/707075) | CC0 1.0 | ce240bd56d8930b07729d0f4324548bd10967e88 |
| F | House sparrows ("two house sparrows tweeting back and forth") | *ngoomie | https://commons.wikimedia.org/wiki/File:House_Sparrow_202279702.wav | CC0 1.0 | 5945ece964bc1852365fc66a9d98712e60826eeb |
| H | Common tern colony, Republic of Karelia, Russia, 11 July 2023 | Egor Filippov (iNaturalist egor_filippov) | https://www.inaturalist.org/observations/172376503 (sound 766303) | CC0 | 2936e93d4bb07e84466cf2a62b10b2824628fad0 |
| J | Common tern, Rainy River District, Ontario, Canada, 24 August 2023 | Brian Starzomski (iNaturalist bstarzomski) | https://www.inaturalist.org/observations/181696217 (sound 806175) | CC0 | fe9708c879f8ecd3ee2420e0c6a69804288cf413 |
| I | European starlings, Rouge, Toronto, Canada, 11 August 2021 | Kalvin Chan (iNaturalist kalvinchan) | https://www.inaturalist.org/observations/124845066 (sound 491330) | CC0 | ec135d2f91fefeac8258b3ede4fa39dcb2437b1f |

Recordings B, C, D, E and G of the 2 November set are no longer used (ticket 490): measured, they
were mostly background (B surf hiss, C a rumble bed with two knocks and no gull call, D wind buffets
and clipped knocks, E low rumble pulses, G broadband noise only 6 dB under the starlings).

## Shipped files

| File | Made from | High-pass | Loudness | SHA-1 |
|---|---|---|---|---|
| wolf_bird_gull_1.ogg | A, 5.97-8.85 s | 800 Hz | -16.2 LUFS | 3b85dc6e50b7cbed2b5d6727c2f28447de6c004f |
| wolf_bird_gull_2.ogg | A, 4.42-5.97 s | 800 Hz | -20.9 LUFS | ae5e9f716e4c5d3672372ed6a4c0f9e90d6a992c |
| wolf_bird_gull_3.ogg | A, 8.94-10.74 s | 800 Hz | -18.6 LUFS | 21be77203d459f5f6e32b769b3ad1b86643d3c2f |
| wolf_bird_tern_1.ogg | H, 1.17-2.41 s | 1.5 kHz | -20.9 LUFS | cc4b7379dbd95f3415da18c8513a96ef35c76b60 |
| wolf_bird_tern_2.ogg | H, 6.62-7.42 s | 1.5 kHz | -20.9 LUFS | 9692795ced5b0409a17e87a4698f14365831ff1d |
| wolf_bird_tern_3.ogg | J, 0.84-2.36 s | 1.5 kHz | -20.9 LUFS | 2d8d799a7543ab5cbcffd3142fb28a63fd6ee5cb |
| wolf_bird_small_1.ogg | F, 12.70-13.55 s | 2 kHz | -16.7 LUFS | 1e67a2400fe44cce9dfd0558dee99981b3952dda |
| wolf_bird_small_2.ogg | F, 23.35-24.52 s | 2 kHz | -18.6 LUFS | 2993c6bd61005e119ac770acb7a0f3f81b49b36e |
| wolf_bird_small_3.ogg | F, 48.02-48.85 s | 2 kHz | -19.5 LUFS | 6ec2f1e80f5dda162db4a27fcf6fabeda6de4ed9 |
| wolf_bird_flock_1.ogg | I, 24.35-27.20 s | 1.5 kHz | -22.4 LUFS | 4183d6344ee655b06b70de7b164708641d7da06c |

Ogg Vorbis, mono, 44.1 kHz, quality 3. Each call is band-passed (the high-pass above, low-pass
11 kHz), spectrally de-noised against the recording's own background, gated to silence between
calls, cut at the quietest point near each end, faded in over 15 ms and out over 60 ms, and set to
the loudness the 2 November file of the same name had (gull_3, tern_1 and tern_2, whose old files
held no call, take the level of their kind's real calls), with peaks at or under -1.5 dBFS. The
birds' bodies and wings are drawn from our own geometry in wolfbirds.cpp; no third-party model or
texture is used.
