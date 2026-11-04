# WolfViewer VR: hand-over note (2026-11-04, written at about 20:30Z)

This note is for an agent starting with no memory. Read it all before touching anything. The working tree is
`~/NetBeansProjects/wolfviewer-vrpipe` on branch **`w94-combined`**, which holds VR plus the Wolf terrain screen.
Every commit is local: there has been no push and no CI.

## 1. Goal and targets

Paul (the owner) wants real PC-VR from WolfViewer, a Firestorm fork, on his Meta Quest 3.
- The PC renders.
- The headset is a WebXR page in the Quest Browser, with no app to install.
- It should work the way Virtual Desktop, Steam Link and ALVR do, and be better than WolfStorm VR. His words: "if we
  just build wolfstorm vr in the headset its stupid".
- He wants a proper VR interface: the viewer's HUDs, windows (floaters) and menus shown in the headset, and usable
  there.

Targets:
- **45–72 pictures a second.**
- **Head-pose-to-shown under 60 ms.** Good wireless PC-VR is about 30–40 ms motion-to-photon.
- **Sharpness first:** keep 1728 px eyes. Paul said "the sharpness was really nice ... the version before was
  aweful" (that was 1152 px). Never step the default back down. Win speed some other way.
- **Simple for older and disabled users:** big targets, no codes, no confirm steps, British English.

**VR is NOT for release until Paul says so.**

## 2. Architecture

### PC side (`indra/newview/`)

**`wolfvrscreen.cpp`** handles capture, per-eye camera, HUD layer, panels, pointer and input.
- `displayFrame()` (:1219): one `display()` per eye in stereo. The left eye's picture is not swapped. The desktop is
  paused (one real frame every 3 s).
  - It sets the eye-size world view (`WolfVREyeRenderSize`: each eye is drawn into a square the eye's size, at most
    the window height) via `LLViewerWindow::wolfVRSetWorldViewRaw` (llviewerwindow.cpp:6941).
- `buildMap()` (:220): where the head is.
  - Avatar shown: 1.75 m behind its eyes and 0.45 m above them (`kBehind`, `kAbove`).
  - Avatar hidden (`WolfVRHideAvatar`): at its eyes.
  - Plus the headset's lean.
- `applyHeadCamera()` (:1341): each eye's own off-axis frustum from the XRView tangents (`eyeCrop`, applied in
  llviewercamera.cpp setPerspective, around line 381), at each eye's own position.
- `captureInto()` (:1028) and `collectCapture()` (:972): back buffer → texture → PBO.
  - The newest finished read is collected at the start of the next frame (`pumpInput`). Depth is blitted at 1/8.
- `captureLayer()` (:625): the HUD/UI layer, drawn in this order:
  1. HUD attachments into `gLayer.rt`;
  2. the 2D UI into `gLayer.ui`;
  3. an allow-list clear of the UI target (keeps floaters, menus/pie, popups, toasts; anything as big as half the
     window is replaced by its children);
  4. the Wolf self-drawn screens drawn again (`wolfScreens()` :328);
  5. the UI composited over the HUDs, premultiplied;
  6. scale to ≤1600×900 and read back.
- When collected, a pixel with colour but alpha 0 (a HUD attachment pixel) becomes opaque.
- `sendPanels()` (:344): the floater list (`{"t":"panels"}`), each with its rectangle and the rectangles of floaters
  in front of it ("o"). The VR window `wolf_vr_stream` is never included.
- `updatePointer()` (:1521): the controller ray → UI layer (layerHasPixel) or a world raycast
  (`lineSegmentIntersectInWorld`) → `handleMouseMove`.
  - `aimedObject()` (:1449) is the object used by the bar's Sit.
- `applyEvents()` (:1672): page input → viewer.
  - Keys, text, sticks, turn, teleport, actions.
  - Enter also delivers the character 13 (see section 5).
- `shareCull()` (:1209): the right eye reuses the left eye's cull, sort and shadow maps.
  - In llviewerdisplay.cpp around lines 1008–1135: `vr_shared_cull`, `LLPipeline::wolfVRReuseSunShadow`
    (pipeline.cpp:11344).
- `reuseShadowsThisFrame()` (:1201): `WolfVRShadowEvery` (default 2).

**`wolfvrpipe.cpp`** is the server: HTTPS/WSS on the LAN (port 4333, falling back to the next free one), the encoder
thread and the layer thread.
- `frameLoop()` (:1274): encodes with nvh264enc (GStreamer) or OpenH264, then builds the picture header.
  - The header is 200 bytes; stage stamps are at 160..195, depth after the picture.
- `layerLoop()` (:1593): the HUD layer as JPEG (colour on top, coverage below, stacked).
- `onText()` (:900): every message from the page (pose, in, act, key, summary, log ...).
- `WsClient::send` (:598): world pictures go ahead of layer pictures (`pending_world`).
- `Service::start` (:1725): computes the page id (a hash of the served files), sent in "welcome".

**`wolfvrstream.cpp`** handles the settings, start and stop, quality steps and the desktop reconciler.
- Quality steps `kQuality` (:99).
- `reconcileDesktop()` (:287), 4 times a second: hides the VR window while a headset is connected, hides the
  toolbars only in 3D Mode, and gives back only what it hid. This covers every way out.
- `headsetInVR()` (:402): used to skip the main loop's background-yield sleep (llappviewer.cpp, around line 1903).

**`wolfvrui.cpp`** is the action bar's viewer side.
- `run()` / `runCommand()` (:111 / :129) answer every press with `{"t":"acted"}`.
- `kHuds` (:62): the viewer's own HUDs, each run through its desktop menu item's registered function and parameter.
- Also `closefloater`, `sit` (on the aimed object), `hideavatar`.
- `pump()` (:228) sends the bar state: `on`/`off`, `huds`, `typing` (keyboard focus in an editable text field),
  `lang`, `sit`, favourites.

### Headset page (`indra/newview/app_settings/wolfvr/`)

The page is three r185 JavaScript modules. There is no build step. Run `node --check` on every file you change.

**`wolfvr.js`**: connection, decode (WebCodecs, `optimizeForLatency`), the frame loop, input, probes and test hooks.
- Decode path: `onPicture()` (:526), `onDecoded()` (:553).
- `eyesOf()` (:910): XRView eye offsets and the per-eye tangents, sent as "e" and "pt".
- `animate()` (:1685): the frame loop.
- `predictPose()` (:1657): pose prediction.
- Pointer: `aimAt()` (:1441) checks, in order: the open bar, the keyboard, the Menu tab, panel ✕, panels, then the
  HUD band.
- `pollWorldPointer()` (:1471), `onSelect()` (:1565, hands), `pollControllers()` (:1593).
- `HEADSETS` (:766): the Quest 3 starts at step 1, 1728 px. `adaptQuality()` (:816) has no pictures-a-second rule.
- Device report, once per 30 s in "headset says: device: …":
  - `deviceLine()` (:171), `layerProbe()` (:110), `disparityProbe()` (:197), `depthLine()` (:233).
- Stage breakdown: `noteStages()` (:93), in the "stages:" line.
- Test hooks: `window.__wolfvr.*` (end of file).

**`vrlayer.js`**: the HUD band and the window panels.
- The band stands in the room. `recentre()` (:317) places it in front when first shown and whenever the bar opens;
  the grip moves it.
- The layer picture goes through a canvas, then a `CanvasTexture` (the Quest needs this, see section 5).
- `_applyRects()` (:164): builds the panels: world-locked, side by side (`_freeYaw`), each with a red ✕ above its
  top-right corner.
- `hitPanels()` (:244), `hit()` (:442).

**`vrbar.js`**: the action bar, standing in the room.
- `ACTIONS` (:24): three fixed rows.
- `HUD_ITEMS` (:61): the HUDs page.
- `hit(o, d, wo, wd)` (:322), `activate()` (:354), `toggle()` (:256).
- The Menu tab is head-locked.

**`vrkeyboard.js`**: the headset keyboard.
- `LAYOUTS` (:21): gb us de fr be es it da no sv nl, plus an accents page.
- `layoutFor()` (:40): picks from `navigator.languages`, then the viewer's Language.
- `press()` (:182): sends characters, plus Backspace, Enter and the arrows as keys.

**`vrdepth.js`**: the world-locked depth meshes, one grid per eye.
- `VERT` (:24) reconstructs each point through that eye's window and pose.
- `show()` (:130).
- The far shell is 800 m; the cards sit behind it at 900 m.

### Data flow

1. Each headset frame the page sends a pose: head quaternion and position, both eyes' offsets and tangents, a
   timestamp.
2. The viewer draws the left eye then the right eye. Each eye is a square of the eye's size in the back buffer, and
   is blitted into one side-by-side picture (2 × 1728). Depth is blitted at 1/8.
3. The PBO read is collected next frame, encoded as H.264 (NVENC), and sent with a 200-byte header carrying pose,
   tangents, eyes, depth range and stage stamps. Depth is zlib, log-packed u16.
4. The page decodes it. Each eye becomes a depth mesh drawn for where the eyes are NOW, at 72 Hz.
5. The HUD layer is a separate JPEG stream at up to 30/s: the band plus the panels cut out of it. The window list
   comes as text.
6. Input:
   - The controller ray goes to the viewer, which moves its mouse there.
   - Trigger = left click; B = right click (pie menu).
   - Left stick: walk. Right stick: snap turn.
   - Stick click switches between Big Screen and 3D Mode. Paul loves it; keep it.
   - Double trigger: teleport.
   - The keyboard sends characters.
   - Bar presses go to `wolfvrui`.

## 3. Today's VR commits (`git log`, newest first)

| commit | what it did |
|---|---|
| 4a823353ff | Round 8: the disparity probe takes out the eye windows' offset (stereo proven right with Quest 3 windows); the HUD band stands in the room; shadow maps every second frame |
| 9d76027da7 | Restore on every way out (reconciler); fixed bar order; the HUDs page with the viewer's own HUDs; Sit on the aimed object; Quest 3 starts at 1728; on-device disparity probe |
| 05c67b569a | Enter sends chat (char 13); Wolf screens drawn again after the junk clear; HUDs first, then UI over them; toolbars hidden while connected; camera anchored to the avatar |
| 1ee7c100a7 | Headset floor on the avatar's ground (eye height had been 3.3 m); 3D part of the device line |
| 57b2d8590f | Keyboard with national layouts; ✕ on every panel; bar in the room; allow-list junk clear; VR window hides while connected |
| 7e673d277c | HUD alpha fix (alpha writes back on after HUD attachments; HUD pixels opaque from their colour) |
| 69f74ebd7d | Layer JPEG decoded without imageOrientation and flipped in the canvas |
| d33b18db12 | Device report; layer through a CanvasTexture; hands aim; page reloads itself on a new page id; Quest 3 1440 start |
| 0ce6ae0446 | Merge of the terrain screen into w94-combined |
| c4aa305531 | Right eye reuses the left eye's cull, sort and shadows (WolfVRShareCull) |
| 6618a49d21 | Stage timing; read-back collected at once; no background sleep in VR; eye-size render; Close on the bar; floaters as panels |

Earlier VR history is on w94-vr-pipe / w94-paul-test.

## 4. Paul's feedback and its state

| Paul said (verbatim where we have it) | state |
|---|---|
| "i click [the Menu tab] but then i cant turn off the menu clicking any button on the menu does nothing" | **done**: big Close; bar closes itself after a press; every press answered (tick or reason) |
| "NOTHING apart from the hud menu ... I click map NOTHING" (no layer on the Quest) | **done**: canvas texture, and the HUD alpha fix (section 5) |
| "NO KEYBOARD"; "keys need to match the country the user is in" | **done**: vrkeyboard.js, layouts by `navigator.languages`, Layout key, remembered |
| "there was no easy close on the mini map" | **done**: red ✕ above every panel's corner → `closefloater` |
| "the MENU itself would be better floating in the air than moving with the headset" | **done**: bar world-locked; grip moves it; reopens in front |
| "if i turn on huds and things appear i do get a lot of other junk" (nav and favourites bar, toolbar icons, QR) | **done**: allow-list clear |
| "you can see bits of screens appearing" | **done**: panels leave out what windows in front of them cover |
| "when you are connected the VR mode window needs to dissappear until you disconnect"; QR stays | **done**: hidden (not closed: closing switches VR off), back on every way out |
| "my camera was in the middle of my avatars back instead of up in the air" | **done**: 1.75 m behind, 0.45 m above the avatar's eyes, any posture |
| "i put test in the box with the keybaord and clicked enter but it did not submit it" | **done**: char 13 (section 5) |
| "none of the huds are working - so flight, cars gaming"; "the flight hud is built into the viewer and so are a load of other that are not in the vr menu"; "add it as a seperate menu"; "these are what are built into the viewer NOT attached to me" | **done**: HUDs ▸ page (viewer's own HUDs only), each run through its desktop menu function, ticks from the desktop checks |
| "if i open a floater that is over the top of a hud the hud masks it" | **done in code**: HUDs drawn first, UI over them. Not tested with a real worn HUD |
| "the menu at the bottom of the screen did the same so probably need to hide it" | **done**: toolbars hidden in 3D Mode while connected |
| "when i dropped vr mode stuff didn't come back" | **done**: reconciler |
| "the main vr hud all the buttons are kind of in a funny order" | **done**: three fixed rows, most used first |
| "i couldnt sit on a plane because touching it bought up a menu" | **done**: point at it, then the bar's Sit. The trigger stays the desktop's left click (the plane's script menu is the plane's own) |
| "the resolution was pretty low" → "the sharpness was really nice" | **done**: 1728 px; keep it |
| "the viewer huds should stay static like the main vr hud" | **done** (round 8): HUD band world-locked; grip moves it |
| "still don't look 3D ... at luxor it was incredible but its like flat" | **open**, see section 7 |
| "i did try and fly a plane but it went badly but we can work on that later" | **open**, later |

## 5. Root causes found (don't repeat these)

- **The HUD layer was invisible on the Quest.** `render_hud_attachments` ends in
  `LLPipeline::renderGeomPostDeferred`, which leaves alpha writes OFF (`gGL.setColorMask(true, false)`, pipeline.cpp
  around line 4507), but only when HUD attachments are worn. The test avatar wears none, so the emulator never saw it.
  The layer's coverage was all 0.
  - Fix: alpha writes back on; HUD pixels with colour but no alpha are made opaque at collect.
  - The test switch `WolfVRDebugUIAlphaOff` simulates the masked state.
- **ImageBitmap textures never drew on the Quest.** The layer now goes through a 2D canvas and a `CanvasTexture`, the
  same path as the bar. The flip is done in the canvas, not with `imageOrientation`.
- **Stale page.** The Quest tab kept an old page across a viewer restart. "welcome" now carries a page id (a hash of
  the served files) and the page reloads itself when it changes.
- **Enter did nothing.** The viewer acts on Return only as the CHARACTER 13: `LLViewerWindow::handleKey` lets keys
  under 0x80 fall through to the character handler, and `handleUnicodeChar` "delay[s] processing of return keys". The
  page's Enter was a key only. `applyEvents` now also calls `handleUnicodeChar(13)`.
- **Self-drawn Wolf screens vanished.** `wolf_flight_deck`, `wolf_car_dashboard`, `wolf_helm_hud`, `wolf_game_hud`,
  `wolf_border_banner` and `wolf_loading_notice` are full-window views with no children, so the allow-list clear took
  them. They are now drawn again after the clear. The flight deck draws only while Flight Mode is on.
- **Warm-up step-down.** A "too few pictures a second" quality rule fired in the first seconds and never stepped back
  up. The rule is gone: the PC's drawing sets the picture rate, not the size.
- **Background yield.** The main loop slept `BackgroundYieldTime` (40 ms) every VR frame when the window had no focus,
  which is normal with a headset on. It is skipped while a headset is in VR.
- **Read-back lag.** The PBO ring collected the slot from two frames earlier (138–158 ms). The newest finished slot
  is now collected at the next frame's start (3 ms).
- **Eye height of 3.3 m.** The rear-view 0.75 m lift was added on top of the avatar's centre and then the headset's
  height on top of that. Fixed, then later anchored to the avatar's eyes, because seated residents got a seated
  height.
- **Disparity "259 px everywhere" (round 8).** 259 was the probe's search limit. A Quest 3's mirrored asymmetric eye
  windows put infinity 432 px apart at 1728 px. The probe now removes that offset.
  - With the same windows in the emulator (`window.__asymEyes`): near 32, middle 11, far 0.
  - So stereo delivery is right: halves, routing, and each eye's own frustum on the PC and in the meshes.
- **The bar was unreachable on the Quest.** The left controller's menu button may be kept by the Quest Browser.
  Everything must work with the right trigger alone.
- **LEAP notation.** Integers need `i2`, reals `r2.0`. A bare `2` kills LEAP and wedges the next session.

## 6. Measurements from Paul's real sessions (his log, "headset says" lines)

- **Start of day:**
  - 10–16 pictures/s.
  - Head-pose-to-shown 350–500 ms, PC-to-shown 150–290 ms.
  - Eye render fell to 896–1440 px.
- **d33b18db12, 15:14–15:20Z, 1920 px, step 0:**
  - 7–18/s, pose-to-shown 127–157 ms.
  - Stages: up 4, wait 7, eyeL 34, eyeR 23, readback 8, encode 28, down 21, decode 13, show 11.
- **9d76027da7 at 1152 px:**
  - 24–32/s after warm-up, pose-to-shown 70–90 ms.
  - Stages: eyeL 14–17, eyeR 9, encode 8, decode 7.
- **Round 7 build at 1728 px (19:40–19:51Z):**
  - 12–15/s, pose-to-shown 123–130 ms, PC-to-shown 67–72 ms.
  - Stages: eyeL 28–29, eyeR 15, encode 18, decode 11.
  - Device line: depth on, grid 216×216, eyes drawn 63.4 mm apart (headset 63.4 mm), head 0.87 m above his floor
    (seated), eye windows L −1.6003,0.9657,−1.6643,1.1106 and R −0.9657,1.6003,−1.6643,1.1106.
- **Emulator on this laptop, 1728 px, Paul's graphics settings:**
  - 25–30/s at The Caribbean and at Community Hub (eyes 13–20 / 11–15 ms, encode 17–19 ms).
  - Wolf sky at quality 1 (no volumetric clouds): 33/s.
  - His own 12–15/s has not been reproduced. It may be his scene, a worn HUD or other load.

## 7. Open problems, ranked

1. **"Still flat" (3D).** Delivery is proven right in the emulator with Quest 3 windows. The next headset run's
   device line now reports true disparity ("disparity after the eye windows' own offset … near … middle … far").
   Check it first. If near is about 20–40 px and far about 0, the stereo reaches him and the flatness is perceptual:
   - frame rate 12–15/s at 1728;
   - edge artefacts of the depth meshes (cut triangles show the far shell);
   - the third-person framing.

   Compare with WolfStorm's native WebXR (`~/NetBeansProjects/firestorm/wolfstorm/js/rendering/render_manager.js`,
   `_placeVrRig`). Paul says Luxor there "was incredible": it renders natively at 72–90 Hz, with no stream and no
   reprojection.
2. **Speed at 1728 px** (sharpness stays). The levers left:
   - the Wolf sky's volumetric clouds in VR (ask Paul; 33/s against 26–30/s);
   - zero-copy NVENC from the GL texture (CUDA-GL interop), so there is no PBO and no RGBA copy;
   - a single-pass stereo render (multiview), a big job;
   - centre-sharp encoding, only if the centre stays at least as sharp.
3. **Flying a plane "went badly"** (Paul: later). Not investigated.
4. **Worn HUD attachments in the headset.** The new order is not yet tested with a real worn HUD: LEAP has no attach
   op, and attaching one to Testwolf would prove it.
5. **Transport is still WebSocket over TCP.** WebRTC is the proven low-latency path in the Quest Browser
   (CloudXR.js).
6. **Crash ticket 517** (Paul's viewer, 18:22:47Z, SIGSEGV, 7 s after a test viewer started). Unmapped: the unstripped
   57b2d8590f build is gone. Mapping it needs a relink of that commit in a separate build directory, only while his
   viewer is closed.

## 8. The test rig

- **Test tree:** `~/.cache/skyband/tvr`. Install a build there with
  `strip -o tvr/bin/do-not-directly-run-wolfviewer-bin-avx2 build-linux-x86_64/newview/firestorm-bin` and rsync
  `app_settings/wolfvr` plus `settings.xml`.
- **Sessions:**
  - `~/.cache/skyband/session_vr.sh TAG TREE` runs a Shadow Testwolf session under
    `flock ~/.config/wolf/shadow_testwolf.lock`, with a throwaway profile bind-mounted over `~/.wolfviewer_x64`, a LEAP
    login and a LEAP FIFO (`leap.fifo` / `leap.log`).
  - Launchers that set VR 3D Mode on port 4360 and pass `MORE_KEYS` and `SLURL`: `run_vr3d.sh`, `run_ui.sh`,
    `run_ui2.sh`, `run_s3d.sh`, `run_stages.sh`, `run_par.sh`.
  - **Every launcher calls `wait_paul.sh` first.** It waits until nothing runs from `~/firestorm`.
  - Paul's settings for realism, from his settings.xml (read-only):
    ```
    MORE_KEYS="UIScaleFactor=F32:1.35 RenderFSAAType=U32:2 RenderFSAASamples=U32:3 RenderGLContextCoreProfile=Boolean:1
    RenderQualityPerformance=U32:6 RenderAnisotropic=Boolean:1 FSLimitFramerate=Boolean:0 RenderTerrainPBREnabled=Boolean:1
    RenderLocalLightCount=S32:4096 RenderHeroProbeResolution=S32:2048 RenderVolumeLODFactor=F32:3 RenderAvatarLODFactor=F32:1
    RenderTerrainLODFactor=F32:2 RenderTreeLODFactor=F32:1 RenderAvatarMaxNonImpostors=U32:19"
    ```
    (one line in the shell). His region: `SLURL="secondlife://Community%20Hub/128/128/30"`.
- **e2e scripts** (`scripts/wolfvr/`):
  - `e2e_vr3d.mjs`: stereo, pointer, pie sit, bar sit, teleport, Big Screen. 40 checks. The double-press teleport is
    flaky.
  - `e2e_vrui.mjs`: menu, panels, map close. 21 checks.
  - `e2e_vrui2.mjs`: keyboard layouts, Enter, panel ✕, bar in the room, junk, VR window, toolbars, HUDs page, static
    band. 41 checks.
  - `e2e_stereo3d.mjs`: symmetric vs Quest 3 windows disparity.
  - `e2e_stages.mjs`: phases of settings and quality (`q=N`) with the stage breakdown.
  - `e2e_parallax.mjs` with `parallax_check.py`: lean parallax.
- **Fake headset:** `~/NetBeansProjects/firestorm/wolfstorm/tools/fake_xr.js` plus `fake_xr_controllers.js`
  (Touch Plus layout; `__swapViews`, `__headX`, `__asymEyes`).
- **Output:**
  - `~/.cache/skyband/shots/<TAG>/`: e2e.out, eye renders `eye_*.png` from `__wolfvr.renderEye(0)`, layer pictures.
  - Viewer logs: `~/.cache/skyband/log-<TAG>.log`.
- **Null sink:** the launchers load `module-null-sink` and store the id in `~/.cache/skyband/nullsink.stages`. Unload
  it afterwards with `pactl unload-module $(cat nullsink.stages) && rm nullsink.stages`.
- **Stale processes:** if a LEAP relay dies, a session can hang on its FIFO write. A
  `timeout 15 cat ~/.cache/skyband/leap.fifo >/dev/null` lets it finish. A stale `e2e_vr3d.mjs` from the morning
  (pid 1404346) could not be stopped (classifier).

## 9. Install procedure (the coordinator, Shadow, does installs)

1. Wait until Paul's viewer is closed: no `/proc/*/exe` under `~/firestorm`.
2. Back up every file you replace into `~/viewer-backups/<name>-before-install-<UTC>/` and write `restore.sh` there.
   See `scripts/wolfvr/install_local.sh` for the pattern.
3. Run the gdb start-up check:
   ```
   objcopy --strip-debug build-linux-x86_64/newview/firestorm-bin X
   cd ~/firestorm && LD_LIBRARY_PATH=~/firestorm/lib gdb -batch -ex "break main" -ex run -ex kill X
   ```
   It must print "Breakpoint 1, … in main ()".
4. Strip into the avx2 slot, `~/firestorm/bin/do-not-directly-run-wolfviewer-bin-avx2`, then check with `strings`.
5. rsync `app_settings/wolfvr/` and `app_settings/settings.xml`.
6. The binary must carry release 93 and "Leah's Lanterns": `strings bin | grep -cxF "Leah's Lanterns"`.

**Current build:** `build-linux-x86_64/newview/firestorm-bin`, built 20:00Z, containing 4a823353ff's C++. Its gdb
check passes. Installed by the coordinator so far: 9d76027da7, at 19:38Z.

## 10. Hard rules

- **Never run a test viewer while Paul's runs** (use `wait_paul.sh`). His viewer crashed at 18:22Z (ticket 517) when
  a test viewer started. Builds stay at nice 19 -j2 while his viewer runs, and at most -j8 when it doesn't.
- **Local commits only:** no push, no CI, no release.
- **Shadow Testwolf only,** under its flock, password never on a command line. Never Paul's login or profile; his log
  and settings are read-only.
- **No logging in hot paths.** Summaries only, once per 30 s; debug lines only with `WolfVRDebugStats`.
- **British English** in anything residents see.
- **Simple for older and disabled users:** big targets, no codes, no confirm steps.
- **Never port-scan or probe our servers.**
- **Alarm on any running executable under /tmp or /dev/shm:** stop and tell the user, and don't kill it.
- **No TODOs and no half-done branches.** Declare what's untested.

## 11. Round 9 (2026-11-04, from about 20:45Z): Paul's round-8 test said "flashing" and "doesn't look 3D, just flat"

His round-8 session (Luxor, 20:36–20:38Z, two device lines): 19 pictures/s decoded, pose-to-shown 121–188 ms, eye
1728 px, errors 0; picture disparity after the windows' offset near 32 / middle 0 / far 0 px; depth on; eyes drawn
63.4 mm apart = headset; head 0.92 m above his floor; it was night there (EEP moon below the horizon, real moon 0).

### What was found and changed (uncommitted at the time of writing)
1. **Shadow reuse paired with a probe's matrices (fixed, `pipeline.cpp` wolfVRReuseSunShadow / generateSunShadow,
   `llviewerdisplay.cpp` ~1055).** The main loop runs the two eye passes, then `mReflectionMapManager.update()`
   (llappviewer.cpp ~1857) renders a probe face through `cubeSnapshot` → `display_cube_face` → `generateSunShadow`
   with `gCubeSnapshot`, which overwrites `mShadowModelview/mShadowProjection[0..1]` and stretches `mSunClipPlanes`
   for the probe's camera while its maps go to `mAuxillaryRT`. A mirror's hero probe does the same at the start of the
   left eye's pass. Round 8's `WolfVRShadowEvery=2` reused those live matrices with mMainRT's kept maps. Now
   generateSunShadow keeps copies (`mWolfVRShadow*`) only when a VR eye drew the maps, the reuse restores them (and
   the clip planes) and returns false when nothing is kept, so the caller generates. `WolfVRScreen::stereoEye()` is new.
   - **Not proven to be the flashing.** In the emulator with round 8's binary and `WolfVRShadowEvery=2`
     (scripts/wolfvr/e2e_shadowflicker.mjs, `~/.cache/skyband/run_shadow.sh`), consecutive pictures with the head
     still differ by a median 0.1–0.3 grey levels, neighbours no more than pictures two apart (ratio 0.55–0.63): no
     alternating change. But every run was at night (Community Hub, Luxor's landing point is indoors) with no sun
     or moon shadows; the Midday shortcut sent through LEAP (Ctrl+Shift+Y, keycode 89) did not change the sky in the
     test session, so a sunlit before/after was not obtained. Paul's session was also at night.
2. **Display-side stereo probe (`wolfvr.js` shownProbe, in the device line after the picture probe).** Draws the scene
   with each of three's two XR cameras (their own pose, projection, layer mask) into a 480² target and matches the
   bands as the picture probe does, the cameras' own window offset taken out; also prints the camera layer masks and
   the display viewports the browser gave each view. Emulator: near 54–61, middle 7–18, far 4–7 px; masks 3/5;
   viewports left 0,0 and right 447,0 of 894×796. **Paul's next device line answers whether the Quest shows each eye
   its own picture.** If "shown ... near" is about the picture's near value, the display is stereo and flatness is
   not in the delivery.
3. **Depth gaps checked:** `pictureGrey` hook reports depth / eye windows / meshes per picture; emulator 0 of 40
   without. (His line's depth count rose 524 in a 30 s window decoding ~570: the counter only counts pictures drawn,
   and a picture replaced before the next display frame is never drawn, so that gap is not proof of missing depth.)
4. **Vehicles (Paul: "joysticks should control the vehicle if i am sat on one - i.e. emulate the cursor keys").**
   `wolfvrscreen.cpp`: seated on an object (avatar has a parent) the left stick's x is `LLAgent::moveYaw` (the cursor
   keys' turn, CONTROL_ROT_LEFT/RIGHT for the vehicle's script) instead of a sidestep; the rig heading follows the
   seat's facing each frame (`vehicleHeading`, through the seat's render matrix like avatarEyes) plus the right
   stick's comfort turn as an offset (`gRig.vehOff`, reset on sitting/standing). On foot nothing changed.
   **Untested on a real vehicle** (the emulator has no vehicle to sit on); e2e_vr3d's sit-on-object step ran.
5. Research (two agents, sources in the session): Quest 3 views are parallel (0.0° rotation both eyes, recorded
   runtime data), positions ±IPD/2 of the user's setting, projections strongly asymmetric (L −1.376/0.839,
   B −1.428/T 0.966 raw); three.js r185 routes view index 0 → layer 1 and 1 → layer 2 regardless of XRView.eye
   (eyesOf's eyeLayer corrects for it) and the masks are right only while `xr.cameraAutoUpdate` is true (it is);
   the streaming literature's causes of flat stereo are all "both eyes get effectively the same image"; a Quest
   experimental setting, Positional TimeWarp, is reported to swap/flicker eyes for streamed content (Steam Link
   thread) - worth switching off on Paul's headset.

**Installed for Paul 21:40Z** (binary, page, settings.xml; gdb start-up check passed; e2e_vr3d 39/40 twice, and the
round-8 binary gives the same 39/40 at the same spot: the double-trigger teleport check aims at a moving ship's deck
this evening, which click-teleport refuses). Restore: `~/viewer-backups/vrbigscreen-before-install-20261104T214000Z/restore.sh`.
Test tree `~/.cache/skyband/tvr` holds round 9; round 8's binary is kept there as `.bak-round8-20261104`.

### Open
- The flashing is not reproduced on this laptop. Ask Paul what flashes (the whole world, the HUD band, a panel) and
  whether Settings > Experimental > Positional TimeWarp is on.
- "Flat": wait for the "shown on the display" numbers.
- The test session's Midday key press: find why it did not take (focus on the modal notice, or the Wolf sky).
