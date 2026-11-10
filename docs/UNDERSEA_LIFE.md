# Undersea life: sea plants, fish and sharks (ticket 652, 2026-11-09)

Jimmy Olsen, ticket 652 "Undersea with better scenery": undersea plants, moving fish, sharks in warm water.
Paul 2026-11-09: "there is no fish or plants under the water" - in the Vulkan release.

## Shape (the birds' and sea life's design)

- Drawn by each viewer for itself near its own camera: no objects, scripts, plugin, region restart or new request.
- The grid's part is the owner's switch and the place's climate, in an `undersea` block of the `weather.php` region
  payload every Wolf viewer already reads each minute (`wolf_ambient_lib.php wa_undersea_payload`).
- Wolf regions only, visitors included (a SEE feature: `WolfGrid::showsWolfRegionWeather`); other grids: nothing.

## Switches and default

- Region/Estate > Region: "Fish and sea plants on this region" (`wolf_ambient.undersea`, 1 / 0 / NULL).
- DEFAULT (nobody has chosen): ON on every region. Unlike dolphins and whales nothing moves on the surface: it is seen
  only under or through the water. A theme does not switch it off (first draft excluded Roleplay, but The Caribbean, the
  warm-water showcase, is listed under Roleplay); underwater builds are kept clear by the obstacle scan below, and under a
  sky that is not an Earth sky (space, a black void) the viewer shows none (the birds' rule). The owner's choice always
  wins. One constant to change if Paul wants it off by default: `WA_UNDERSEA_DEFAULT` in `wolf_ambient_lib.php`.
- Resident: Wolf World > "Fish and Sea Plants" (`WolfViewerUnderSea`, default on); `WolfViewerUnderSeaFishMax`
  (default 200, hard cap 400; the frame budget lowers it, WolfFrameGovernor). No sounds (none added).

## Where

- The sea only: terrain below the region's water height, on terrain that has arrived (`LLSurfacePatch::getHasReceivedData`,
  the sea-life aground rule). Never painted water or prim water. Over a neighbour only when the block's `near` lists it
  (its own effective switch is on) and its water is at the same height (sea life's 11-06 rule).
- Never above the water (paul-no-water-on-land): every plant's top is at least 1 m below the water height (wave troughs),
  fish keep 0.6 m under the drawn wave surface (`WolfBoatRock::drawnWaveHeight`) and 0.4 m over the seabed.
- Never inside builds: an incremental scan of the objects near the camera (3000 a frame, the birds' pace) marks 2 m cells
  with the height range of any prim reaching under the water; no plant grows where a prim spans its height, fish turn
  away from those cells. Avatars and attachments are not obstacles for plants; fish flee avatars.

## Climate (from the place's latitude, server side so WolfStorm can share it)

`warm` = 1 - smoothstep(23, 33, |lat|) (coral reefs lie within about 30 degrees of the equator, NOAA "Where are coral
reefs"); `cold` = smoothstep(55, 68, |lat|). No place known: temperate (warm 0, cold 0).
- warm: seagrass, branching and mound corals, sea fans, colourful reef fish schools, sharks (rare).
- temperate: seagrass meadows, kelp forests (cool water, NOAA "kelp forests"), rocks, silver schools, larger fish.
- cold: kelp, rocks, silver schools, cod-like fish; seagrass sparse.

[COLD SEAS 2026-11-09] Paul at TROMSO (lat 69.7, cold 1, 15 m deep at 1170,1453): "i cant see any fish or anything". The
cold sea had one silver school and kelp only on the scattered rocky patches. Now (CHOSEN after the common sights of a
North Atlantic / Norwegian rocky shallow, our own shapes):
- cold and cool seas: kelp FORESTS where a 40 m noise is high (most of a cold sea's floor 2-20 m deep, patches in a cool
  one): 1-4 kelp in 16 m2, a tall canopy (three quarters of the room under the water or more, up to 16 m); round them and on
  the rocky patches anemones, sea-urchin groups, starfish, pale sponges, stones; wrack in the shallows (under 4 m); on the
  gravel between, the odd urchin group, starfish, anemone, stone or kelp.
- fish: cod / pollock-saithe schools of 6-16 (0.35-0.65 m), 2-4 in a cold sea, 1-2 in a cool one, round the rocky patches;
  a lone big cod or a banded wolffish near the bottom; one silver school. Cool-temperate seas (e.g. Blue Ocean, 51.7 N)
  add seagrass meadows and 1-3 loose groups of wrasse (3-8 each; mottled green-brown or orange with a blue back).
- Not yet seen in world (no session allowed while Paul was using the laptop).

## Plants (redesigned 2026-11-09 after Paul: "it doesn't look right - look at some pictures of the undersea and emulate it")

Reference photographs looked at (reference only, nothing copied): Wikimedia Commons "Snorkeling Bari Reef, Bonaire"
(12840799335, 12841304654) and "Snorkeling Karpata Reef, Bonaire" (12841929245), CC BY 2.0; "Thalassia-Syringodium seagrass
bed (Graham's Harbour, San Salvador Island, Bahamas)" (15667912398), CC BY 2.0. What defines the look:
- layout: PATCHY. Reef patches (rock mounds crowded with corals, sponges, rods and fans), seagrass MEADOWS (a dense sward),
  bare pale sand between. Not an even scatter.
- shapes: grooved brain-coral domes, flat antler-like elkhorn, staghorn thickets, open tube sponges, slim sea rods, lace
  fans, rough limestone. Colours muted: ochre, tan, olive, purple-brown; the bright colours are the fish's.
- light and water: in the photographs a blue-green haze hides everything beyond about 20-40 m. That is the viewer's own
  water fog, not this feature; it was investigated 2026-11-09 (the haze runs, but the region's natural-water fog colour is
  near black, so it only darkens) and left as it is - Paul: "i didnt ask for underwater haze i just wanted the fish and
  stuff".

What is drawn:
- Placement by world hash (the same garden for everyone): reef patches on a 24 m world grid (chance 0.35 + 0.2 x warm,
  radius 3-7 m). Each patch: 3-6 mounds (0.5-1.6 m high), each mound 3-7 separate rough boulders with crevices between
  (lumpy noise surface, overhanging shelves, grey limestone with crisp encrusting crusts: coralline pink, purple and ochre
  sponge crusts, algal turf). About 1.4 items a square metre, seven in ten standing on the rock (on a boulder's surface,
  never in the air over a crevice). Warm seas: brain-coral heads 30 % (0.6-1.5 m across, a labyrinth of evenly spaced
  ridges with shaded sides), elkhorn 12 % (thick tapered paddles forking twice like antlers), staghorn 15 %, tube sponges
  18 %, sea rods 18 %, sea fans 7 % (flat, upright, taller than wide, ragged rim, standing across the current). Cool seas:
  kelp (fewer as it gets warmer), stones, pale sponges. Meadows where a 30 m noise passes 0.52, up to about 14 tufts of
  5-7 blades in 16 m2. The odd loose stone on bare sand.
- Surfaces: one 1024 x 1024 atlas of our own procedural textures made in code (value noise: rock, brain grooves, polyp cups,
  fan lace as an alpha mask, veined blades, kelp, sponge pores, fish scales), made 8 rows a frame the first time (about
  2 s), uploaded once with mip levels. Plants wait for it; nothing is downloaded or shipped as an image.
- Chunks of 32 m, one vertex buffer each, built one a frame within 72 m, drawn while the camera is under the water or
  within 60 m above it. The sway on the GPU (`deferred/wolfUnderSeaV.glsl`: texcoord1 = sway, texcoord0 = atlas).
- Never above the water (every top at least max(1 m, 0.3 x depth) under it) and never inside builds (obstacle scan).

## Fish

- Schools with flocking, keeping off the bed, the surface (under the drawn waves) and builds; they scatter from swimmers.
- Reef fish and big fish gather round a reef patch 4-22 m from the camera (goals over its edge, above its rock); the
  first school of each kind comes in view. 2-6 reef schools of 10-24 fish, 10-25 cm long, sizes varying +-30 % within a
  school; each fish its own speed and its own wandering place round the school's goal, soft alignment and cohesion: the
  schools mill loosely. Colours a fifth of the way to grey (less cartoon). Reef colourways after the reference kinds: blue-grey with a yellow side stripe and tail, blue,
  white with dark bands, yellow with blue lines. Silver schools in open water; a grouper-like or cod-like big fish.
- Sharks: warm water only, at most one (two now and then), never a new one while another is still in the water; they
  keep 8 m from everyone.

## Cost

- Off (the owner's switch, the resident's, another grid, a non-Earth sky): nothing at all.
- Camera more than 60 m above the water: no plants or fish (chunks released after 20 s).
- Under the water at The Caribbean (14 m deep, a reef patch in view), measured 2026-11-09 (us-look3-vk, the final look):
  - 26-27 chunks, about 1 040 000 plant vertices on the GPU, static (the sway is in the vertex shader);
  - 160-178 fish (one buffer written each frame): 0.16-0.33 ms a frame to write it;
  - the rest of the simulation (schools, obstacle scan, garden upkeep): about 0.5 ms a frame;
  - building one chunk: about 2 ms of CPU (it was 50-80 us before the round-3 reef: boulders and paddles are many more
    triangles). At most one chunk a frame, and only while the camera reaches ground not yet built (none when still).
- Cold seas: not measured. By the shapes' sizes a dense kelp-forest chunk is about 40-60 thousand vertices (kelp about
  230, an anemone about 450, an urchin group about 300-900), so a cold sea may carry more than the warm reef's 1 million;
  every chunk is capped at 150 000 vertices (MAX_CHUNK_VERTS).
- Frame rate on/off: not proven. The NVIDIA laptop runs were too noisy to separate (about 21 vs 21 fps under the water,
  with 48 and 118 fps outliers). A quiet-machine measurement is still owed.

## Debug

The WolfUnderSea debug tag logs a "state:" line every 1.5 s (camera depth, fish, schools, fleeing, sharks, chunks,
vertices, the nearest reef patch) and a cost line once a minute. The in-world test reads these:
`~/.cache/undersea/campaign_undersea.py`.
