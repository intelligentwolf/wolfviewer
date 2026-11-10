# WolfViewer native Vulkan backend: what exists, what it maps to, what is proven

Written 2026-11-08 on branch `vulkan-backend` (worktree `~/NetBeansProjects/wolfviewer-vkbackend`, from W99 `c064cee9a8`).
Plan: `~/wolfviewer-releases/vulkan-plan.md`, option (a), stage 4 "the native Vulkan backend's first real job".
Paul's rule for this work: no shortcuts, complete code paths, every constant from the header or the spec, untested
paths declared. Nothing here is committed; nothing is installed; nothing touches `~/.wolfviewer_x64`.

## 1. In one line

In Vulkan mode the viewer starts, dynamically loads the Vulkan loader, picks the card OpenGL is using, creates the
swapchain on the real window, draws the scene's fullbright batches with the viewer's real fullbright shader (compiled
to SPIR-V at build time from the unchanged GLSL) into a Vulkan render target with the OpenGL frame's depth, and
composites that with the OpenGL world and UI layers into the swapchain; OpenGL still draws everything else; a PC
without Vulkan, or a failed Vulkan start, runs OpenGL as before and says so.

## 2. Files

### 2.1 Build

| File | What |
|---|---|
| `indra/cmake/Vulkan.cmake` | `ll::vulkan_headers`: the vendored Khronos headers (below), `VK_NO_PROTOTYPES`; QuartzCore for the Mac surface helper |
| `indra/llrender/vk/third_party/vulkan-headers/` | Vulkan-Headers 1.4.363 (`vk_platform.h`, `vulkan.h`, `vulkan_core.h`, `vulkan_beta.h`, `vulkan_xlib.h`, `vulkan_wayland.h`, `vulkan_win32.h`, `vulkan_metal.h`, `LICENSE.txt` Apache-2.0 OR MIT). Vendored so Windows, macOS and Linux builds see the same files and every `vulkan_core.h:LINE` citation in the code stays true. No Vulkan SDK is needed to build. |
| `indra/cmake/VulkanShaders.cmake` | target `wolfvk_shaders`: every shader program variant to Vulkan SPIR-V (section 6) |
| `indra/cmake/ShaderCheck.cmake`, `scripts/shader_check.py`, `scripts/shader_check_known.txt`, `.github/scripts/install-glslang.sh` | the shader enumerator and GL compile check from the `vulkan` branch, copied across unchanged except for the Vulkan output hooks |
| `scripts/shader_vulkan.py` | the Vulkan GLSL transform and SPIR-V output (section 6) |

No link-time dependency on Vulkan anywhere: `libvulkan.so.1` / `vulkan-1.dll` / `libvulkan.1.dylib` are opened at run
time (`wolfvkloader.cpp`) and every entry point is fetched through `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`.

### 2.2 Device layer: `indra/llrender/vk/`

| File | Maps to | Notes |
|---|---|---|
| `wolfvkloader.h/.cpp` | (new) | dlopen/LoadLibrary of the loader; X-macro tables of every function with its `vulkan_core.h` line; global/instance/device loading; optional (1.1/1.2) functions with KHR fallbacks |
| `wolfvkallocator.h/.cpp` | (new) | sub-allocating allocator: 64 MiB blocks (an eighth of a small heap), first-fit free list, separate blocks for linear and optimal resources (so `bufferImageGranularity` never applies within a block), dedicated allocations for big resources and for images shared with GL, `maxMemoryAllocationCount` honoured, persistent mapping, `nonCoherentAtomSize` flushes |
| `wolfvkdevice.h/.cpp` | `gGLManager` | instance (validation layer + debug messenger when `WolfVulkanValidation`), physical device choice (setting / GL device UUID / GL vendor / first discrete / first), logical device with one graphics+present queue, features (anisotropy, wireframe, BC, independent blend, depth clamp), per-frame command pools + fences + binary semaphores, timeline semaphore when available, deferred destruction queue (frames-in-flight later), one-time command buffers |
| `wolfvksurface.h/.cpp/_macosx.mm` | `LLWindow` | `VkSurfaceKHR` from `LLWindow::getNativeHandles()`: X11 `vkCreateXlibSurfaceKHR` on a child X window of the viewer's window (8.2s: the viewer's own window carries the GLX drawable), Wayland `vkCreateWaylandSurfaceKHR`, Win32 `vkCreateWin32SurfaceKHR`, macOS `vkCreateMetalSurfaceEXT` on a `CAMetalLayer` the `.mm` attaches to the GL view |
| `wolfvkswapchain.h/.cpp` | `SDL_GL_SwapWindow` | format choice (UNORM 8-bit first, sRGB last), present mode from `RenderVSyncEnable` (FIFO on; MAILBOX or IMMEDIATE off), recreate on resize / out-of-date |
| `wolfvkbuffer.h/.cpp` | `LLVertexBuffer` | `WolfVkBuffer` (device-local + staging, range uploads) and `WolfVkVertexBuffer` with LLVertexBuffer's layout (`calcOffsets`, `sTypeSize`), mapping semantics (`MappedRegion`), attribute formats from `setupVertexBuffer`, bind/draw |
| `wolfvkimage.h/.cpp` | `LLImageGL`, `LLTexUnit` | 2D/cube/array/3D images, the GL -> VkFormat table (section 4), GL-row upload with re-packing and byte swapping, mip generation by blit, read-back, layout transitions; `WolfVkSamplerCache` for `TFO_*` x `TAM_*` |
| `wolfvkrendertarget.h/.cpp` | `LLRenderTarget` | colour attachments (up to four) + depth (`D32_SFLOAT`), four render-pass variants (colour/depth clear or load), framebuffer, viewport in GL's row order (positive height; sub-rectangle for the world view rect), external depth attachment, read-back |
| `wolfvkspirv.h/.cpp` | (new) | SPIR-V reflection: uniform blocks with member offsets/strides, samplers, inputs, outputs (no external library) |
| `wolfvkpipeline.h/.cpp` | `LLGLSLShader` program + `LLRender`/`LLGLState` | `WolfVkProgram` (SPIR-V pair, descriptor set layouts from reflection, pipeline layout) and `WolfVkPipelineCache` keyed on the mapped render state (section 5) |
| `wolfvkshader.h/.cpp` | `LLGLSLShader` uniforms | `WolfVkShaderMirror`: recipe key, SPIR-V loading, std140 uniform images written by every `LLGLSLShader::uniform*()`, per-draw dynamic-offset binding; `WolfVkFrameUniforms` ring buffer + descriptor pools per frame |

### 2.3 Viewer side: `indra/newview/`

| File | What |
|---|---|
| `wolfvkbridge.h/.cpp` | settings, start-up decision and first-run marker, surface/device/swapchain, `present()` in place of `swapBuffers()`, fall-back with notices, About / report fields |
| `wolfvkscene.h/.cpp` | the frame: the GL hand-over (shared layers, or read-backs where sharing is impossible), the fullbright pass with coverage, the composite |
| `wolfvkshare.h/.cpp` | [SHARED IMAGES] `WolfVkSharedImage` (a Vulkan image with exported memory + the GL texture imported over it) and the three layers (world RGBA8, UI RGBA8, depth D32F), the two exported semaphores, the GL copies (blit + depth pass) and the Vulkan acquire/release barriers |
| `wolfvkgbuffer.h/.cpp` | [VULKAN G-BUFFER] the provider behind `LLRenderTarget::sWolfVkProvider`: `mRT->deferredScreen`'s four colour attachments and depth as shared images, the pass semaphores, and the Vulkan render target over them (`WolfVkScene::drawGBufferSimple` draws `PASS_SIMPLE` into it from `LLDrawPoolSimple::renderDeferred` through `WolfVkBridge::drawPassSimple`) |
| `wolfvkmirror.h/.cpp` | Vulkan copies of the GL vertex buffers (`glGetBufferSubData` once, then kept current by `LLVertexBuffer::sWolfVkOnFlush`) and textures (`glGetTexImage` per level, re-made on `LLImageGL::sWolfVkOnTexelsChanged`) |
| `wolffloatervulkan.h/.cpp`, `skins/default/xui/en/floater_wolf_vulkan.xml` | Wolf World > Tools > Vulkan Renderer... |
| `llnavigationbar.cpp`, `skins/default/xui/en/panel_navigation_bar.xml` | the V button next to WE (section 3) |
| `app_settings/shaders/class1/wolfvk/compositeV.glsl`, `compositeF.glsl` | the composite pass (Vulkan GLSL) |

Hooks in existing files (all tagged `[VULKAN BACKEND 2026-11-08]`): `llwindow.h` (`NativeHandles`, `getNativeHandles`),
`llwindowsdl2.cpp`, `llwindowwin32.cpp`, `llwindowmacosx.cpp` (the handles); `llglslshader.h/.cpp` (`mWolfVk`,
`mWolfVkFeatureObjects`, one `wolfVkSet` line in each of the 31 uniform setters); `llshadermgr.h/.cpp`
(`mWolfVkUnitRecipes`, `wolfVkFindShaderClass`); `llvertexbuffer.h/.cpp` (`sWolfVkOnFlush`, `sWolfVkOnDestroy`,
`getGLBuffer`); `llimagegl.h/.cpp` (`sWolfVkOnTexelsChanged`, `sWolfVkOnDelete`); `llviewerdisplay.cpp` (the three
swap points, `captureWorld`, the login-screen marker); `llviewerwindow.cpp` (init after `gGL.init`, resize, shutdown);
`llappviewer.cpp` (`decideBackend`, About, `gDebugInfo`); `llviewerstats.cpp`, `wolfcrashreport.cpp`,
`wolffloaterbugreport.cpp` (report fields); `llviewerfloaterreg.cpp`, `menu_viewer.xml`, `notifications.xml`,
`strings.xml`, `settings.xml`. [VULKAN G-BUFFER]: `llrendertarget.h/.cpp` (`WolfVkProvider`, `setWolfVkShared`: a shared
target takes its textures from the provider and hands them back on release), `lldrawpoolsimple.cpp` (the static
`PASS_SIMPLE` batches go to `WolfVkBridge::drawPassSimple` first).

Debug settings (all off by default, `settings.xml`): `WolfVulkanDebugTint`, `WolfVulkanShowSceneOnly`,
`WolfVulkanDebugNoDepth`, `WolfVulkanDebugNoCull`, `WolfVulkanDebugProbe` (per-second read-backs and batch dumps in the
log), `WolfVulkanDebugNoScenePass`, `WolfVulkanDebugNoShare` (force the read-back hand-over), `WolfVulkanDebugNoGBuffer`
(keep the deferred geometry on GL), `WolfLogFrameRate`.

## 3. Settings, floater, About, reports

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `WolfRenderBackend` | String | `opengl` | `opengl` or `vulkan`; read once at start |
| `WolfVulkanStartOK` | Boolean | TRUE | first-run marker: false while a Vulkan start is under way, true when the login screen has been drawn |
| `WolfVulkanValidation` | Boolean | FALSE | the Khronos validation layer + debug messenger (log tag `VulkanValidation`) |
| `WolfVulkanDevice` | String | empty | part of a device name to prefer |
| `WolfVulkanDebugTint` | Boolean | FALSE | tint what Vulkan drew (shows the hand-over) |
| `WolfVulkanShowSceneOnly` | Boolean | FALSE | hide the GL world layer, show only the Vulkan scene (+ UI) |
| `WolfVulkanShareGBuffer` | String | `on` | (2026-11-09, option A1 of 8.2o) share the G-buffer with Vulkan: `on` (default) always; `off` never; `auto` = yes, except on AMD integrated graphics with Mesa RADV (Vulkan device INTEGRATED, vendor 0x1002, driverID MESA_RADV, and GL on AMD), where OpenGL keeps its own compressed G-buffer and draws the geometry passes. Any other value counts as `on`. Default `on` decided after chain34 (8.2o). Read at Vulkan start; the Vulkan window's status line says so on the automatic AMD choice (`WolfVulkanStatusActiveAMD`); `reportFields()["vulkan_gbuffer"]` gives shared / not shared and why |
| `WolfLogFrameRateOnly` | Boolean | FALSE | with `WolfLogFrameRate`: only the frame-rate line, every timing instrument off (`WolfVkBridge::timingOn`) |

Fall-back: if the device/surface/swapchain cannot be made, or the previous Vulkan start never reached the login
screen, the viewer runs OpenGL, sets `WolfRenderBackend` back to `opengl`, and shows `WolfVulkanFellBack` /
`WolfVulkanFellBackLastStart`; a device lost while running shows `WolfVulkanLost` and the session continues on OpenGL.

Wolf World > **Vulkan Renderer...** (floater): status in plain words, "Renderer: ..." and "Vulkan: <device>, driver
<driver>, Vulkan <api>" (or "not available on this computer"), the OpenGL / Vulkan choice (restart needed, with a
notice), the validation tick box, a line with draws per frame and the hand-over milliseconds.

The **V button** (2026-11-08, Paul: "next to the WE button we add a V button with a 'Use Vulkan shaders' tooltip; when
it's on it goes green like the WE button next to it"): `WolfVulkanBtn` in `panel_navigation_bar.xml`, immediately right
of WE, built like WE (is_toggle, the stock face, tinted `WolfGlassButton` green while on by `tintWolfNavButton` in
`llnavigationbar.cpp`, which WE now uses too). Lit while `WolfRenderBackend` is `vulkan`; a click calls
`WolfVkBridge::chooseBackend` - the same path as the floater's choice (setting, `WolfVulkanStartOK`, the
`WolfVulkanChosen` / `WolfVulkanOpenGLChosen` restart notice; the renderer cannot change while the viewer runs). The
face follows the setting every idle, so the floater and the button agree. Where no Vulkan device was found at start it
is disabled with the tooltip `WolfVulkanButtonUnavailable` ("Vulkan is not available on this computer", strings.xml).
Shown on Wolf Territories (the login grid) only, like the status bar's Wolf buttons. WE keeps its tooltip, which
already begins "Wolf Environment". Layout: the buttons sit in the fixed-width `navigation_buttons_visibility_panel`
(`auto_resize="false"`, `right=`), and the location bar is the auto-resizing panel after it; Paul's first look (18:00Z)
had the location combo over V's right half because the panel was still 223 px (V ends at 234). The panel is now 262
(V's 32 + the 7 px gap every button has), and the four locale overrides that set their own width for longer labels
(de 253, es 222, it 265, pl 255) grew by the same 39. No C++ positions these panels (`mNavigationPanel` /
`mFavoritePanel` are commented out in llnavigationbar.cpp), so the width holds at every window size; the location bar
is what shrinks in a narrow window.

Help > About gains (strings.xml `AboutRenderer`):
```
Renderer: OpenGL: <GL_RENDERER>            or   Renderer: Vulkan: <device name>
Vulkan: <device>, driver <driver>, Vulkan <api>   or   Vulkan: not available on this computer
```

What the grid and the reports receive (`WolfVkBridge::reportFields()`), all strings unless noted:

| Where | Keys |
|---|---|
| `send_viewer_stats` `body.system` (llviewerstats.cpp, sent at login/periodically) | `renderer` (`opengl`/`vulkan`), `vulkan_available` (bool), `vulkan_device`, `vulkan_driver`, `vulkan_api`, `vulkan_loader`, `vulkan_device_count` (int), `vulkan_last_error` |
| crash report (`gDebugInfo["WolfVulkan"]`, wolfcrashreport.cpp fields) | `crashed_renderer`, `crashed_vulkan_device`, `crashed_vulkan_driver`, `crashed_vulkan_api`; and per run `renderer`, `vulkan_device`, `vulkan_driver`, `vulkan_api` |
| Report a Problem (wolffloaterbugreport.cpp) | `renderer`, `vulkan_device`, `vulkan_driver`, `vulkan_api` |

## 4. Format and state mapping

Texture formats (`wolfvkimage.h`, from every GL internal format `llimagegl.cpp` and `llrendertarget.cpp` use):
RGBA8 -> `R8G8B8A8_UNORM`; SRGB8_ALPHA8 -> `R8G8B8A8_SRGB`; RGB8/SRGB8 -> the RGBA form with rows widened on upload
(3-channel 8-bit sampling is optional in Vulkan); LUMINANCE8 -> `R8_UNORM` + swizzle (R,R,R,1); LUMINANCE8_ALPHA8 ->
`R8G8_UNORM` + (R,R,R,G); ALPHA8 -> `R8_UNORM` + (0,0,0,R) (exactly what `setManualImage` does on the core profile);
R8/RG8; BGRA -> `B8G8R8A8_UNORM`; RGB10_A2 -> `A2B10G10R10_UNORM_PACK32`; R16F/RG16F/RGBA16F -> `R16_SFLOAT`...;
RGB16F -> RGBA16F widened; R32F/RGBA32F; DEPTH_COMPONENT24/32F -> `D32_SFLOAT` (RADV has no D24, vkglshare probe);
DEPTH24_STENCIL8 -> `D32_SFLOAT_S8_UINT`; DXT1/3/5 (+sRGB) -> BC1/2/3 blocks; the generic `GL_COMPRESSED_*` requests
stay uncompressed (Vulkan has no "driver picks" compression).

Samplers: `TFO_POINT` nearest/nearest/mip nearest; `TFO_BILINEAR` linear/linear/mip nearest; `TFO_TRILINEAR`
linear/linear/mip linear; `TFO_ANISOTROPIC` + `maxAnisotropy = gGLManager.mMaxAnisotropy`; `TAM_WRAP/MIRROR/CLAMP` ->
REPEAT / MIRRORED_REPEAT / CLAMP_TO_EDGE; shadow compare LESS_OR_EQUAL (pipeline.cpp:1151 GL_LEQUAL).

Pipeline state (`wolfvkpipeline.h`): blend factors `BF_*` -> `VK_BLEND_FACTOR_*` one to one (ADD equation); colour
mask; `LLGLDepthTest` -> depth test/write/compare (GL_NEVER..GL_ALWAYS and VK_COMPARE_OP_NEVER..ALWAYS are in the
same order); `GL_CULL_FACE` + `glCullFace(GL_BACK)` -> BACK/NONE with VK_FRONT_FACE_CLOCKWISE: the targets are written
in GL's row order (row 0 = bottom) with an ordinary positive-height viewport, which mirrors y against Vulkan's y-down window
space and turns GL's CCW into CW (the first cut's negative-height viewport put the Vulkan layer upside down against GL's
depth and layers - nothing passed the depth test); wireframe -> POLYGON_MODE_LINE; `LLRender::eGeomModes` ->
topologies, `LINE_LOOP` drawn as a strip without its closing segment (known limitation; debug drawing only).
Projection: `modelview_projection_matrix` and `projection_matrix` are pre-multiplied by `[1 0 0 0; 0 1 0 0; 0 0 .5 .5; 0 0 0 1]`
so GL's z in [-w, w] becomes Vulkan's [0, w] and the stored depth equals GL's; `inv_proj`/`inv_modelview` are not
corrected (nothing in the fullbright path reads them).

## 5. The frame in Vulkan mode

Two hand-overs exist; `WolfVkShare::available()` picks at start-up and the floater / reports say which
(`vulkan_hand_over`): **shared layers** on Linux and Windows drivers with `GL_EXT_memory_object(_fd|_win32)` +
`GL_EXT_semaphore(_fd|_win32)` and a Vulkan device with the matching export extensions (both test drivers), or the
**read-back** path otherwise (macOS: Apple's GL has none of these).

Shared layers (`wolfvkshare.cpp`, protocol in its header):
1. `render_ui()` after `renderFinalize()`: `captureWorld()` -> GL waits `vk_done` (Vulkan finished with last frame's
   layers), blits the default framebuffer into the shared world layer, runs a full-screen pass that writes
   `deferredScreen`'s depth (GL_DEPTH_COMPONENT24, world-view-rect sized) as `gl_FragDepth` into the shared D32F layer at
   the world view rect's place in the window (a blit cannot convert depth formats), clears GL's colour to (0,0,0,0) so
   the HUD and UI land on a transparent layer.
2. At the swap: the default framebuffer is blitted into the shared UI layer and GL signals `gl_done` (layouts
   COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT). Vulkan's submit waits `gl_done`, acquires the three images from
   `VK_QUEUE_FAMILY_EXTERNAL`, draws the `PASS_FULLBRIGHT` draw infos with `gDeferredFullbrightProgram`'s SPIR-V into a
   window-sized RGBA16F + R8 target whose depth attachment IS the shared depth (viewport = the world view rect, as GL's
   `gGLViewport`), composites world + scene + UI into the swapchain image, releases the images back to EXTERNAL, signals
   `vk_done` and presents. Nothing crosses the bus and nobody waits on the CPU.

Read-back path: the same frame with three `glReadPixels`/`glGetTexImage` into pixel buffer objects mapped straight away
(a GPU wait each), uploaded to Vulkan images, and the depth uploaded into the scene target's own depth.

**Coverage, not alpha.** The Vulkan layer is laid over the GL picture by a second colour attachment, `wolf_coverage`
(R8, 1 wherever a fragment reached the end of `main`), which `scripts/shader_vulkan.py` adds to every fragment stage by
renaming `main` to `wolf_main` and wrapping it. The shader's own alpha cannot be used: GL draws the fullbright pass with
`setColorMask(true, false)` (lldrawpoolsimple.cpp:69), and the fullbright vertex colours carry alpha 0, so the pass
leaves no alpha behind in GL either - found on 2026-11-08 with the `WolfVulkanDebugProbe` read-backs (35 batches drawn,
8,124 pixels with colour, none with alpha).

Known difference: the Vulkan pass draws after GL's post-processing and converts linear -> sRGB without the viewer's
exposure / tone map, so its colours are not yet identical to GL's for the same surfaces (they will be once Vulkan draws
into the shared G-buffer before GL's lighting, section 9).

## 6. Shaders: build-time SPIR-V

`scripts/shader_check.py --vulkan-out DIR` (`VulkanShaders.cmake` runs it into `app_settings/shaders/spv/`, git-ignored,
packaged with the shaders) stitches every program variant exactly as `LLShaderMgr::loadShaderFile` does (the enumerator
from the `vulkan` branch: 300 programs, 2,187 units, 7,246 links across 6 GPU profiles x 35 settings scenarios), then
`shader_vulkan.py` turns each program into Vulkan GLSL mechanically:

- loose non-opaque uniforms of a stage gathered into one `layout(std140, set=0, binding=0|1) uniform WolfUniformsV|F {...}`
  block (the union over the stage's units, same text in every unit; struct definitions carried along; default
  initialisers recorded in the manifest);
- samplers `layout(set=1, binding=k)` by sorted name; the viewer's own `layout(std140)` blocks get set 2 with
  `LLGLSLShader::UniformBlock`'s order as the binding;
- vertex inputs `layout(location = LLVertexBuffer attribute index)`, varyings by sorted union, `frag_color`/`frag_data` at 0;
- each stage compiled with `glslang -V -S <stage> -l -o <key>.<stage>.spv`.

The key is the MD5 of the program's recipe (per unit `stage|file|class found|#version|defines|channels|amd|oldselect`,
attach order), computed identically by the viewer (`WolfVkShaderMirror::create`). A variant without SPIR-V is reported
once with its recipe and skipped. The result of the full run and the remaining blockers are in section 8.

### 6.1 How many programs compile, and what stops the rest (2026-11-08)

Two GL 4.6 profiles x four settings scenarios (the build-time quick set): **932 of 1,310** linked programs compile as
Vulkan SPIR-V. All profiles and scenarios (the full run, `--vulkan-out`): **2,358 of 7,204** with the viewer's own
`#version` lines (872 s), **5,159 of 7,204 (72%)** once every unit is compiled as GLSL 4.50 (spv6, 1,237 s, 18,202 recipe
keys - the set the test tree now carries). Blockers left after spv6 (`spv6.json["vulkan_out"]`):

| count | glslang message | meaning / fix |
|---|---|---|
| 1,218 | non-opaque uniforms outside a block | uniform declarations the transform did not hoist (#ifdef'd, struct, array-of-struct forms; e.g. PBR Glow Shader) |
| 967 + 426 | fragment / vertex stage: Missing entry point | the stage's object list has no `main` - to be examined per program |
| 879 | syntax error, unexpected IDENTIFIER | the uniform-hoisting regex meets a declaration form it does not parse (e.g. Deferred Alpha Shader) |
| 191 | No function definition (body) found | a function declared in one object and defined in an object the stage list lacks |
| 60 | 'location' : SPIR-V requires location for user input/output | an in/out declaration form the IO regex misses (PBR Terrain) |
| 28 | overlapping use of location N | two outputs given the same location (WolfTk Cloud Shape) |
| 16 | transform: stage geom is not handled | geometry-shader programs, by design (section 9) |

Blockers of the FIRST full run (spv5, before the version change), for the record:

| count | glslang message | meaning / fix |
|---|---|---|
| 4,348 + 4,086 | fragment / vertex stage: Missing entry point | the stage's object list has no `main` - to be examined per program (likely programs whose stage is attached as library objects only, or feature objects supplying main) |
| 3,892 (+3,892 "compilation terminated") | 'binding' : not supported for this version | `#version 150/330` units; fixed after the run by compiling every unit as `#version 450` (shader_vulkan.py) - re-run pending |
| 873 | syntax error, unexpected IDENTIFIER | the uniform-hoisting regex meets a declaration form it does not parse (per-case review) |
| 562 | non-opaque uniforms outside a block | uniform declarations the transform did not hoist (#ifdef'd, struct, array-of-struct forms) |
| 308 | 'location qualifier on output' : not supported for this version | same `#version` cause |
| 16 | transform: stage geom is not handled | geometry-shader programs, by design (section 9) |

## 7. Platform coverage

| Path | Linux (this laptop) | Windows | macOS |
|---|---|---|---|
| loader | `libvulkan.so.1` (dlopen) | `vulkan-1.dll` (LoadLibrary) | `libvulkan.1.dylib` / `libMoltenVK.dylib` (bundle not wired) |
| surface | X11 tested (through XWayland, both GPUs; child window from W103, 8.2s); Wayland written, RADV only (8.2q); pure NVIDIA X11 reported failing in W101/W102 (8.2s) | Win32 written: child HWND for the surface from W103 (8.2t), first run W102 = black world (8.2t) | Metal layer written (`wolfvksurface_macosx.mm`) |
| build | built here | CMake paths complete, not compiled here | CMake paths complete, not compiled here |
| MoltenVK quirks | - | - | `VK_KHR_portability_subset` enabled when offered; triangle fans not converted (declared) |

## 8. Proof (test runs of 2026-11-08)

All runs: this laptop, Linux 7.2.15 (omarchy), X11 window 2522x1520, The Caribbean at 2597/4759/88 looking down the quay,
draw distance 4096, graphics level 6, Shadow Testwolf, a throwaway profile. Two GPUs:

| | OpenGL renderer string | Vulkan device line (Help > About / floater) |
|---|---|---|
| NVIDIA | NVIDIA GeForce RTX 5090 Laptop GPU | Vulkan: NVIDIA GeForce RTX 5090 Laptop GPU, driver 615.78.08, Vulkan 1.4.351 |
| AMD | AMD Radeon 890M Graphics (radeonsi, strix1) | Vulkan: AMD Radeon 890M Graphics (RADV STRIX1), driver radv Mesa 26.2.5-arch1.1, Vulkan 1.4.354 |

What was proven on both drivers, from the logs of the runs (`WolfVulkan` log tag):

1. `libvulkan.so.1` loaded at run time, instance 1.4 created, the Khronos validation layer loaded when ticked.
2. The physical device chosen by the GL device UUID (`GL_EXT_memory_object`), so Vulkan and GL are on the same GPU.
3. X11 surface, swapchain `B8G8R8A8_UNORM` (vsync off in the test profile): NVIDIA 3 images, IMMEDIATE; AMD 4 images, MAILBOX.
4. The login screen and the in-world frame presented through the Vulkan swapchain every frame (GL's swap skipped).
5. `Deferred Fullbright Shader` loaded as SPIR-V (recipe key 9118f74fa2bd8b88330816ffec6e4f43 from the build-time
   set): 29 vertex + 37 fragment uniforms mirrored, 4 samplers, attribute mask 0x2045; 35 fullbright batches a frame
   drawn by Vulkan from mirrored vertex buffers and textures, depth-tested against GL's depth buffer.
6. The composite pass (world layer, Vulkan scene layer, premultiplied UI layer) into the swapchain image.
7. Clean shutdown and logout. Automatic OpenGL fall-back exercised for real (12:52Z): with `VK_ICD_FILENAMES` pointing at nothing the loader offers
no `VK_KHR_surface`; the log shows "Falling back to OpenGL: The Vulkan loader offers no VK_KHR_surface (no window output
possible)", the WolfVulkanFellBack alert appeared on the login screen (shots/nv-fallback/login.png) and the viewer ran and
quit cleanly on OpenGL.

### 8.1 What is Vulkan-drawn and what is OpenGL in the pictures

Exactly: everything in the world (terrain, water, sky, avatars, shadows, lighting, the deferred pipeline) is still
drawn by OpenGL. Vulkan draws (a) the `PASS_FULLBRIGHT` batches - the fullbright (unlit) objects of the scene, 35 of
them in the test spot - with the real fullbright shader compiled to SPIR-V, into its own RGBA16F + D32 target whose
depth is loaded from GL's depth buffer so they sit correctly behind and in front of the GL world, and (b) the final
composite of the three layers and the presentation of every frame. The UI is drawn by GL into a transparent layer and
composited by Vulkan. The pictures named `...-only-the-vulkan-drawn-part.png` show layer (a) alone on a dark background
(`WolfVulkanShowSceneOnly`); `...-tinted-where-vulkan-drew.png` tints the Vulkan-drawn pixels (`WolfVulkanDebugTint`).

### 8.2 Frame rate, 45 s windows (frames presented / wall-clock seconds; frame ms = mean of the per-second means)

Round 1 (12:03-12:34Z, before the alpha-mask fix: the Vulkan batches were drawn but invisible):

| run | fps | frame ms |
|---|---|---|
| NVIDIA OpenGL | 14.1 | 71.9 (a SPIR-V compile ran alongside; re-measured in round 2) |
| NVIDIA Vulkan, validation on | 8.0 | 126.2 |
| NVIDIA Vulkan, validation off | 9.2 | 108.5 |
| AMD OpenGL | 18.3 | 55.5 |
| AMD Vulkan, validation on | 8.8 | 115.8 |

Round 2 (12:37-12:53Z, after the alpha-mask, R32_SINT, semaphore and destroy-order fixes; same spot, nothing else running):

| run | fps | frame ms (mean / median) |
|---|---|---|
| NVIDIA OpenGL | 17.7 | 57.2 / 56.9 |
| NVIDIA Vulkan | 9.0 | 111.3 / 107.8 |
| NVIDIA Vulkan, validation on | 8.8 | 113.6 / 110.0 |
| AMD Vulkan | 9.2 | 109.2 / 103.6 |
| AMD OpenGL (round 1) | 18.3 | 55.5 / 52.4 |

Round 3 (14:11-14:24Z, shared layers, coverage attachment, no visible Vulkan geometry yet - the orientation bug):
NVIDIA OpenGL 18.2 / 55.0; shared layers without the scene pass 17.4 / 55.9; shared + scene pass + validation + probe
16.4 / 61.4; read-back + coverage 15.5 / 65.0.

Round 5 (14:49-15:05Z, the final hybrid of this stage: shared layers, orientation fixed, coverage, depth bias; no
validation, no probe; the same spot and sky; the Vulkan runs' window had a 1-minute syntax check running alongside):

| run | fps | frame ms (mean / median) |
|---|---|---|
| NVIDIA Vulkan (shared layers + fullbright pass) | 17.1 | 59.1 / 56.0 |
| NVIDIA OpenGL | 16.3 | 62.8 / 60.9 |
| AMD Vulkan (shared layers + fullbright pass) | 15.8 | 63.4 / 61.7 |
| AMD OpenGL | 19.7 | 51.8 / 50.3 |

Round 9 (17:11-17:35Z, the fixed binary: one renderer per batch, staging ring, device-loss guard; `PASS_SIMPLE` drawn
by Vulkan into the shared G-buffer, fullbright overlay off; no validation, no probe; the same spot, nothing else
running; the pictures of this round still have the wrong normals of 8.2g - the cost of the pass does not depend on the
normal values, the look does):

| run | fps | frame ms (mean / median) |
|---|---|---|
| NVIDIA Vulkan (shared layers + G-buffer simple pass) | 21.2 | 47.2 / 47.0 |
| NVIDIA OpenGL | 19.7 | 50.0 / 49.7 |
| AMD Vulkan (shared layers + G-buffer simple pass) | 16.2 | 62.4 / 61.1 |
| AMD OpenGL | 19.6 | 51.5 / 51.1 |

Zero validation messages, no device loss and no fall-back in any round-9 log; 757 G-buffer draws a frame, 0 skipped,
0 buffers remade (nv-vk9).

With the shared layers the hand-over costs nothing measurable on NVIDIA (the two runs are within run-to-run noise of
each other, the order even reversed) and about 4 fps on the AMD integrated GPU, where the extra pass and composite
compete for the same memory bandwidth as GL. In the first hybrid (round 1 and 2 below) the read-backs halved the frame rate.

Round 1 and 2, the read-back hybrid, for the record: Vulkan mode was slower by design: every frame GL rendered the whole world as before, then the
colour and depth of the default framebuffer and the UI layer are read back to the CPU through pixel buffer objects
(2522x1520, three layers), uploaded to Vulkan images and composited - the floater's "handing the picture over" number
(16-27 ms a frame) is that cost. It goes away in the next step (section 9.1: shared GL/Vulkan images, no read-back).
The point of this stage is the proof that the real pipeline - device, swapchain, SPIR-V from the viewer's own
shaders, mirrored buffers and textures, state mapping, depth sharing - works end to end on both drivers.

### 8.1a The Vulkan-drawn part, made unmistakable (14:49Z, Wolf Territories Sandbox)

At the Caribbean quay the fullbright pass covers ~70 pixels (lamp bulbs), so the proof was moved to the Sandbox, whose
large fullbright signs ("WOLF-GRID.COM" sales board, the region control panel, "JOIN NOW") are drawn by Vulkan:
`sandbox-signs-nvidia-vulkan.png` (the frame as the resident sees it), `sandbox-signs-nvidia-vulkan-only-the-vulkan-drawn-part.png`
(`WolfVulkanShowSceneOnly`: the signs alone, right place, right depth, textures from the Vulkan copies) and
`sandbox-signs-nvidia-vulkan-tinted-where-vulkan-drew.png` (`WolfVulkanDebugTint`: exactly those surfaces tinted). The
session also rezzed a Full Bright cube through the Build floater (campaign.py `rez` mode); it did not land in view and
is not needed for the proof. Two things the pictures showed and that were fixed straight after: hatching on the signs in
the Vulkan layer (the pass re-draws surfaces GL has already put in the depth buffer and the two depth computations
z-fight; a polygon offset of -2/-1 towards the camera on that pass, `WolfVkPipelineState::mDepthBias*`), and, found on
the way to them, the orientation bug of section 4 (nothing passed the depth test until the Vulkan layer was written in
GL's row order).

### 8.2a The one hang (2026-11-08 13:35Z) and what is known about it

The first run of the shared-layer build (nv-vk3, NVIDIA) froze at the first frame that drew the fullbright pass:
the main thread sat in `WolfVkDevice::beginFrame` -> `vkWaitSemaphores` inside the NVIDIA driver (hang-bt-nv-vk3.txt),
i.e. the previous frame's work never completed. Evidence against the usual suspects: the semaphore protocol is one
wait per signal in both directions in that binary too (traced frame by frame from the code), no validation run of
that binary exists, and the kernel log was not readable from the test account. One functional difference separates
that binary from the next (nv-vk4a/b/c, which ran the same path three times without a hang, with the 8 s hang guard
armed and never fired): the UI capture (`WolfVkShare::glCaptureUIAndSignal`) did not wait for `vk_done`, so on the
login screen - where `captureWorld` does not run - GL blitted into the shared UI layer every frame while Vulkan's
previous composite could still be sampling it, with no semaphore between the two. Writing an image another queue is
reading is undefined behaviour on the shared memory and a plausible way to wedge a channel; it is the most likely
cause, not a proven one. Should the hang recur, the hang guard now turns it into a fall-back to OpenGL with a
notice instead of a frozen viewer, and the log line "The graphics card did not finish a Vulkan frame" marks it.

### 8.2b The crash of 15:37Z (first G-buffer run) - found and fixed

With the G-buffer pass the frame slot is opened in the MIDDLE of the GL frame (the pass's mid-frame submit). An external
window resize then had `present()` recreate the swapchain while that slot's command buffer was still recording;
`WolfVkSwapchain::recreate` -> `WolfVkDevice::waitIdleAndFlushDeferred` reset every command pool, which put the open
buffer back to its initial state, and the frame's next command landed on an unbegun buffer
(VUID-vkCmdPipelineBarrier-commandBuffer-recording, then an error exit). Fix: `waitIdleAndFlushDeferred` leaves the
recording slot's pool and deferred list alone, and the one-time pool while a one-time buffer is open
(`mOneTimeOpen`). Before the G-buffer stage the frame was only ever opened inside `present()`, after the recreate, so
the path was never taken.

### 8.2c "Textures flashing" (Paul, 15:50Z) - the investigation

Paul, watching the Vulkan test sessions: textures flashing, "pretty much all the time", more while loading and when
floaters open. Read from the code before any guess, two mechanisms in the hybrid of round 8 draw the same surfaces
twice at equal depth, which z-fights and flickers frame to frame as the two depth computations trade places:

1. The G-buffer pass's "incomplete -> GL draws the whole pass as well" fallback. Any batch Vulkan skipped (a vertex
   buffer rewritten this frame and still uploading; a buffer lacking an attribute the shader reads) made GL redraw
   EVERY static PASS_SIMPLE batch over Vulkan's. Streaming rewrites buffers all the time, so the pass was incomplete
   almost every frame: double draws of most of the scene, every frame. (Probe lines of round 8 show "30 draws (6
   skipped)" for the overlay pass; the simple pass had the same skips.)
2. The fullbright overlay: Vulkan re-drawing the unlit surfaces GL had already drawn, on its own layer, with a skip
   whenever a buffer was pending - the surface then alternated between GL's look and Vulkan's (no tone map) look.

Fixes (2026-11-08 16:00Z, built as round 9):
- **One renderer per batch.** `WolfVkScene::drawPass` records every draw info it drew; `LLRenderPass::pushBatches`
  skips exactly those (`WolfVkBridge::batchDrawnByVulkan`). `LLDrawPoolSimple::renderDeferred` runs Vulkan's pass
  first, then GL's `pushBatches` draws only the leftovers. Nothing is drawn twice, whatever Vulkan managed.
- **Passes are complete.** `WolfVkScene::preparePass` mirrors every buffer and texture the pass needs and records
  the uploads BEFORE the render pass begins, so "copy still uploading" no longer skips anything.
- **Overlay off by default** (`WolfVulkanOverlayFullbright`): the fullbright overlay was the proof stage and draws
  without the viewer's tone map; the G-buffer pass is the real routing.
- **Staging race closed.** `LLVertexBuffer::flush_vbo`'s hook wrote straight into the staging memory that last
  frame's `vkCmdCopyBuffer` could still be reading on the GPU. `WolfVkStagingRing` (per-frame transient host memory,
  reset after the slot's fence) now receives a copy of each dirty range and the GPU copies from there; the shadow
  is CPU-only. A full ring falls back to the direct copy with a one-time warning.
- **Evidence line.** With `WolfLogFrameRate`, once a second: `gbuffer draws N, overlay draws N, skipped N; mirrors:
  buffers made / KB written, textures made / remade / dropped`.

Evidence plan (chain22): 20-frame bursts with the camera still at the quay, twice during the cold-load minute and
once after the 45 s window, for the round-8 binary (nv-flash-old), the fixed binary (nv-flash-new), the fixed binary
with the hand-over only (nv-flash-nopass: WolfVulkanDebugNoScenePass + NoGBuffer) and OpenGL (nv-flash-gl);
`~/.cache/vkbackend/flashdiff.py A B` prints the changed fraction per step and a 16x16 map of where. The OpenGL
control alone changes 8-13% of its pixels between frames at this spot (water, sky, flags, idle animation), so the
test is the EXTRA change, and where it is.

Round-8 frame rates (G-buffer pass on, overlay on, both at 2522x1520 then shrunk to 1250x749 by an external resize
during the window): NVIDIA Vulkan 20.9 fps / 48.4 ms, NVIDIA OpenGL 20.3 / 49.3.

### 8.2d First run of the fixed binary (nv-flash-new, 16:25Z): two more findings

1. **Flashing worse, not gone, and now explained exactly.** The early 20-frame burst (camera still, scene loading)
   changed 30-58% of the pixels between frames against OpenGL's 8-13%. Cause: the "GL skips what Vulkan drew" rule
   was applied in `LLRenderPass::pushBatches` everywhere - and the same draw infos go through `pushBatches` again for
   the sun shadow maps (`renderShadow`), the reflection probes and the impostors. Those passes therefore left out
   every batch Vulkan had drawn, and as the set of draw infos changes with every rebuild the shadows and probes
   flickered over the whole quay. Fix: the skip is active only inside `WolfVkBridge::ScopedSkipDrawn`, which
   `LLDrawPoolSimple::renderDeferred` opens around GL's own draw of that pass; the shadow, probe and impostor passes
   draw every batch as always.
2. **VK_ERROR_DEVICE_LOST at 16:27:53Z** (the fall-back worked: OpenGL took over with the notice, the viewer went on).
   Cause found in the mirror: `LLVertexBuffer::allocateBuffer` re-sizes a buffer in place (same object, new GL buffer,
   new counts) and the Vulkan copy kept the old size, so draws fetched past its end - an out-of-range fetch on the
   card is a device loss. Fix: `WolfVkMirror::vertexBuffer` remakes a copy whose vertex/index counts or sizes no
   longer match GL's (the old one goes with its frames); `mirrorBuffer` refuses a copy of the wrong shape; `drawPass`
   never issues a draw whose index range or vertex range exceeds the copy. The evidence line counts "remade" buffers.

Round-8 binary (nv-flash-old) and the fixed binary (nv-flash-new2) are measured next with the same bursts; the fixed
binary must come out at OpenGL's level of frame-to-frame change.

### 8.2e Burst evidence so far (20 grabs ~150 ms apart, camera still, threshold 24/255)

| binary | early_a | early_b | after the window | static buildings region |
|---|---|---|---|---|
| round 8 (nv-flash-old; double-draw fallback + overlay) | 5.70% | 6.13% | 5.36% | 0.03% per step |
| OpenGL control (nv-gl8, 10 grabs 100 ms apart) | - | - | 8.1-13.3% | 0.02-0.07% per step |
| fixed binary, first run (nv-flash-new, with the shadow-skip bug) | 30-58% | - | - | - |
| fixed binary (nv-flash-new2: scoped skip, device-loss guard) | 4.95% | 2.63% (window shrunk) | 3.8-6.8% before the resize, 0.4-0.7% after | 0.036-0.061% per step (before the external resize; 1.5% -> 0.04% as textures re-streamed after it) |

**Four-way result, identical grab spacing (20 grabs, camera still, 16:25-17:01Z):**

| binary / mode | early_a | early_b | after the 45 s window |
|---|---|---|---|
| round 8 (double-draw fallback + overlay) | 5.70% | 6.13% | **5.36%** |
| fixed (one renderer per batch, complete passes, overlay off, staging ring, device-loss guard) | 4.95% | 2.63% | **2.34%** |
| fixed, hand-over only (no Vulkan geometry) | 5.04% | 4.93% | 2.59% |
| OpenGL | 5.10% | 4.76% | **2.00%** |

After loading, the round-8 binary changed 5.36% of the frame per step against OpenGL's 2.00% - that extra 3.4 points
of frame-to-frame change, spread over the scene rather than in any one region, is the flashing Paul saw. The fixed
binary sits at 2.34%, within a third of a point of OpenGL, and the hand-over-only run at 2.59%: the double draws
were the cause and are gone. (The final-burst "buildings" figures of the fixed and OpenGL runs, 5.1% and 8.1%, are an
external window resize that landed inside both bursts; before it they read 0.04-0.06%.) Frame rates in these runs:
fixed 18.5 fps, OpenGL 18.2, hand-over only 24.1.

So at the quay, once loaded, both the round-8 binary's and the fixed binary's static textured surfaces are as stable
as OpenGL's by this metric (0.03-0.06% of the building pixels change per step in all three); the fixed binary ran
581-591 G-buffer draws a frame with 0 skipped, 0 buffers remade, no device loss and no validation message, and
survived five external window resizes in one session;
the whole-frame figures are dominated by water, sky and idle animation and depend on the grab spacing (the GL control
used 100 ms). Paul saw the flashing on his screen while the sessions ran; the grabs sample one frame in about three
at 20 fps, so a one-frame flash can slip between them. The fixed binary's own figures (nv-flash-new2, same spacing),
the hand-over-only run and an OpenGL run with the same spacing follow in chain24.

### 8.2f Validation of the fixed binary (nv-vkval11, 17:01Z)

Khronos validation layer on, probe on, 4 min in world at the quay with the G-buffer pass drawing 535 batches a frame:
**zero validation messages**, no device loss, no fall-back, 0 skipped batches, 0 buffers remade, 5,886 buffers and
580 textures mirrored (318 dropped with their GL textures). 13.0 fps with the layer and the per-second read-backs on.

### 8.2g "Weird blue dotted lines" (Paul, 17:15Z) - the G-buffer pass wrote wrong normals

Paul's screenshot of the fixed binary at the quay: the ship hulls and the shop (the large `PASS_SIMPLE` surfaces, i.e.
exactly what the Vulkan G-buffer pass draws) black with short blue/white dashes; cobbles, lamp posts, the avatar and the
alpha/fullbright parts (GL-drawn) normal. The per-attachment compare of the two dumps already taken at the same spot
(nv-flash-new2, Vulkan drew the pass, against nv-gbuf-glonly, `WolfVulkanDebugGBufferGLOnly`) placed it:

| attachment | texels differing | mean abs diff | what the dumps show |
|---|---|---|---|
| 0 diffuse RGBA8 | 18.4 % | 0.021 | the same wood hull in both (edges and streaming only) - the textures were right |
| 1 ORM RGBA8 | 2.6 % | 0.009 | agree |
| 2 normal RGBA16 | 19.8 % | 0.041 | GL hull green, Vulkan hull pink: **the encoded normal is wrong** |
| 3 emissive RGBA16F | 36.4 % | 0.028 | both zero on the hulls (sky differences only) |
| 4 depth D32F | 4.8 % | 0.0002 | agree (the ships bob) |

Decoding attachment 2 at ten hull texels (deferredUtil.glsl `decode_normal`): GL n = (-1.00, -0.04, 0.02) ... (-0.88,
-0.46, 0.11) - a hull seen nearly edge-on, facing left; Vulkan n = (0.000, -0.999, 0.03) ... (0.001, -0.96, 0.28): the
x component exactly zero at every texel, the normal pointing straight down. The lighting pass then lights the hull from
below (black) with the sky's specular where the garbage normal happens to face it - the dashes.

Cause, proven on the host without a session: `cmake/00-Common.cmake:37` builds the viewer with
`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES=1`, so `glm::vec3` is 16 bytes and `glm::mat3` is 48 (three 16-byte columns).
`WolfVkScene::drawPass` built `glm::mat3 normal_matrix(transpose(inverse(modelview)))` and handed
`glm::value_ptr(normal_matrix)` to `WolfVkShaderMirror::set(Kind::M3)`, which reads 9 contiguous floats. A test program
with the viewer's GLM flags prints `value_ptr(mat3)` as `c0.x c0.y c0.z PAD c1.x c1.y c1.z PAD c2.x c2.y c2.z PAD`, so
the shader's std140 block (`normal_matrix` at offset 128, MatrixStride 16, ColMajor - spirv-dis of
`b849c2b36a1e66e88fdad9874f2d8357.vert.spv`) received columns (c0.x, c0.y, c0.z), (PAD, c1.x, c1.y), (c1.z, PAD, c2.x);
the hull normals are mostly +-y in the ship's own frame, so the result was +-(PAD, c1.x, c1.y) with PAD a denormal:
x = 0. The OpenGL side never had this: `LLRender::syncMatrices` (llrender.cpp:1068-1077) picks norm[0..2], [4..6],
[8..10] out of a `glm::mat4`, whose columns are 16 bytes with or without the flag. Fix: `drawPass` packs the nine floats
the same way from the `mat4` and never passes a `glm::mat3` to the mirror. Audit: this was the only `glm::mat3` (and
the only non-mat4 matrix) handed to `set()` anywhere in `llrender/vk` and `newview/wolfvk*`; `Kind::F3` callers pass
plain float arrays.

The "thin green bar along the right-hand window edge" in the same screenshot is Hyprland's active-window border: its
pixels are RGB (64, 155, 84), the compositor's `general:col.active_border` is `ff3e9c53` = (62, 156, 83), 2 px wide
(`general:border_size`), and the smeared strip beyond it is the compositor's blur of what lies beside the window. Not
the viewer.

### 8.2h The same-session compare found the pass running for avatar impostors (18:02Z)

nv-vbtn took two G-buffer dumps ten seconds apart in one session (Vulkan drew `PASS_SIMPLE`, then GL drew it with
`WolfVulkanDebugGBufferGLOnly`), camera still (cross-correlation shift 0, 0). They did not match: the "Vulkan" dump held
palm trees seen from below and a partial scene (160,511 texels with depth, against GL's 694,424). The dump - and the
Vulkan pass - had run during an avatar impostor render: `LLPipeline::generateImpostor` (pipeline.cpp:12291) goes
through `renderGeomDeferred` with `mRT` still `&mMainRT`, so the old guard (`mRT == &mMainRT && !gCubeSnapshot`) let
Vulkan take the impostor camera's `PASS_SIMPLE` batches into the shared main G-buffer, and the impostors lost them.
Guard now (`mainGBufferBound`, wolfvkbridge.cpp): main render target, no cube snapshot, not `sImpostorRender`, not
`sShadowRender`, and `mRT->deferredScreen` is the bound target (llviewerdisplay.cpp:1274 binds it for the main
camera, also inside a VR eye's offscreen target). The dump obeys the same guard and waits for the next main pass.

### 8.2i VR in Vulkan mode: what the headset reads

- **3D Mode (the eyes):** `WolfVRScreen::captureWorld` (llviewerdisplay.cpp:1815) copies each eye's finished world
  from its offscreen eye target (or the back buffer) before `WolfVkBridge::captureWorld` runs. The G-buffer pass's
  geometry is in that picture, because GL lights the shared G-buffer: nothing Vulkan draws lives only in the swapchain.
- **Big Screen (the whole window):** `WolfVRScreen::captureFrame` reads `GL_BACK` at swap, which in Vulkan mode holds
  the UI on a cleared layer (the world went to the shared world layer at `captureWorld`), so the headset gets no world.
  A first fix (skip the split when the debug overlay is off) was built in chain28 and **reverted** the same evening:
  it also skipped `WolfVkShare::resize`, which lives in that capture, so the shared layers stopped following window
  resizes; and the chain28 session that ran it hung the NVIDIA GPU (Xid 109 at 18:28:00Z, 8.2j) - not proven to be
  this change, but it is not kept while unproven. Big Screen in Vulkan mode is therefore still open.

### 8.2j The GPU hang of 18:28Z (nv-fix3, chain28 build)

Xid 109 "CTX SWITCH TIMEOUT" from the nv-fix3 viewer (pid 1145918, channel 0xd) at 18:28:00Z, repeating every ~4.5 s;
the viewer's log stops at 18:27:55 (frame 5136), then `VK_ERROR_DEVICE_LOST` at 18:30:10 and the NVIDIA full-chip reset
at 18:34; GLX contexts fail from then until the GPU is reset. What the log shows just before: no Vulkan warning at all
(0 VUID, 680-700 G-buffer draws a frame, 0 skipped, 0 remade); at 18:27:55 "Viewer was backgrounded for 60s, freeing up
video memory" (LLViewerTexture::updateClass: discard bias to the maximum, every texture re-created smaller) - the first
time in any Vulkan session (0 in nv-vbtn, nv-vbtn2, nv-normfix, nv-vk9, nv-flash-new2); earlier, at 18:24:29-31, the
window was resized 2522 -> 1224 -> 2514 wide (the G-buffer re-shared each time; the shared world/UI layers did not, a
defect of the reverted whole-frame change). Cause not proven; investigation in the hand-back.

### 8.2k After the hang: hand-over holes closed, every CPU wait bounded (code only, 2026-11-08 ~19:00Z, NOT RUN)

What the 18:28 hang looked like, from `hang-bt-nv-fix3.txt`: the main thread was not in any Vulkan wait but inside the
NVIDIA GL driver, drawing terrain (`LLDrawPoolTerrain::renderDeferred`, pipeline.cpp:4390) - the CPU blocked because
GL's channel had stopped, which fits a queue-side semaphore wait that never completes. The 8 s guard could not fire: it
lives in `beginFrame`, and the CPU never got there. So the work was (1) make every queue-side wait complete by
construction, (2) bound every CPU-side wait with the same 8 s limit and the same hung flag / fall-back.

**1. The hand-over holes (queue-side waits have no timeout in either API, so they must be balanced by construction):**
- `WolfVkGBuffer::glHandToVulkan` returned silently (`!mAvailable`, target build failure) without signalling
  `gl_pass_done`, and `drawPassSimple` / `dumpGBufferIfAsked` still recorded the acquire from `VK_QUEUE_FAMILY_EXTERNAL`
  (no GL release: VUID-vkQueueSubmit-pSubmits-02207) and drew into images GL could still be writing. It now returns
  true only for a signal GL accepted (GL errors checked around the call alone); on false the mid-frame command buffer is
  discarded unsubmitted (`WolfVkDevice::discardMidFrame`) and GL draws the pass. `glTakeBack` checks its wait the same way.
- `WolfVkShare::glCaptureUIAndSignal` set `mGlSignalled` even when `glSignalSemaphoreEXT` raised a GL error, so the frame's
  submit waited on a signal that never happened (VUID-vkQueueSubmit-pWaitSemaphores-03238 - nothing would ever complete
  it). Now set only on success; the layers are acquired/released only when GL really released them
  (`WolfVkScene::mLayersAcquired`); a failed GL wait/signal marks the share `broken()`, and `present()` falls back
  (`WolfVulkanLost`) before submitting anything that waits on it.
- The G-buffer dump path's failed submit only logged, leaving `vk_pass_done` marked signalled: the next `glTakeBack`
  would have made GL wait on the card for ever. It now falls back like `drawPassSimple`; `recordDump` releases the images
  on its own failure path; the unused standalone `WolfVkGBuffer::dump()` (same hole) is removed.
- The "no swapchain image" path (`abandonFrame` + a fence-less semaphore submit whose failure was only logged) is
  replaced by `closeFrameWithoutImage`: the slot's own command buffer and fence are submitted with the hand-over
  semaphores, failure falls back. The abandon path also left a slot marked submitted (by the mid-frame G-buffer pass)
  whose fence was reset and never submitted, so its next `beginFrame` waited 8 s and declared a hang that was none
  (`Frame::mFenceSubmitted` now). The same close runs when the swapchain cannot be rebuilt after a G-buffer pass opened
  the slot (one slot per frame always).
- A shared-layer resize that failed inside `captureWorld` was logged once and the invalid share kept (GL neither waited
  nor signalled while Vulkan still acquired and sampled); it now drops to the read-back hand-over in that same frame, as
  `onSwapchainChanged` already did.

**2. Every CPU wait bounded by `FRAME_WAIT_LIMIT_NS` (8 s) and routed to the existing guard** (`WolfVkDevice::declareHung`
-> `hung()` -> `fallBack(..., "WolfVulkanLost")`, Vulkan objects abandoned): the one-time submits and `submitSync`
(were `UINT64_MAX`); `vkAcquireNextImageKHR` (was `UINT64_MAX`, which VUID-vkAcquireNextImageKHR-surface-07783 forbids
without guaranteed forward progress); `waitIdle()` now waits the timeline value of the last submit and every submitted
fence with the limit before it calls `vkDeviceWaitIdle` (kept for the swapchain destroy, VUID-vkDestroySwapchainKHR-01282:
at that point only presents of already-signalled images remain); `waitIdleAndFlushDeferred`, `destroy()`, the swapchain,
pipeline-cache and uniform-pool destroys use it; every submit is now trackable (`submitSync` signals the timeline;
devices without a timeline get a fence per mid-frame submit, waited in `beginFrame`); timeline values are taken only
after a successful submit. GL side: `glFinish` in the share/G-buffer resize and destroy paths is now
`WolfVkShare::glFinishBounded` (fence sync + `glClientWaitSync` with the same limit). `ensureFrameBegun` and `present`
check `hung()` first, so a timeout anywhere falls back at the next frame without another wait. `llimagegl.cpp:1767`
(`glClientWaitSync(..., GL_TIMEOUT_IGNORED)`) is the stock threaded-texture path, off by default, not part of the Vulkan
path, and left as it is.

**3. Resize:** the swapchain recreate, the shared world/UI/depth layers (`WolfVkShare::resize`) and the shared G-buffer
(`colorTexture` / `depthTexture` / `released`) now all idle BOTH APIs (bounded) before the old images go - the G-buffer
paths waited for Vulkan only before. Old images, views and framebuffers are destroyed through the frame-slot deferral;
the scene target that uses the shared depth is dropped before the share is remade. If an idle wait runs out, nothing is
destroyed and the G-buffer provider stops sharing (`mAvailable = false`), so old- and new-size attachments are never mixed.

**4. Texture purge (`TextureDiscardBackgroundedTime`):** read through - `LLImageGL::deleteTextures` drops the copy
before the GL name can be reused; `WolfVkImage::destroy` defers to the frame slot, whose descriptor pools are reset before
its deferred list runs; `scaleDown` / re-creation mark copies stale and they are re-made. Changes: a copy whose
layout transition submit fails or times out is dropped (it was kept, noted as SHADER_READ_ONLY without being there); hooks
that fire off the main thread (only with `RenderGLMultiThreadedTextures/Media`, both default off) are queued under a
mutex and applied on the main thread before the next lookup.

**Unproven until a run:** all of it. Build + shader check (2 known Sky Dome failures only) + `-Wconversion` pass is all
that was done. Not shown: that the 18:28 hang came from one of these holes (no GL error was logged before it, which
argues against the GL-error paths; the abandon/fence and resize paths are the stronger candidates), that the guard now
turns that hang into a fall-back, and the cost of the bounded waits (none per frame: they replace waits that already
existed).

**Not recoverable, by design of the two APIs:** once the Vulkan queue is genuinely stuck, a `glWaitSemaphoreEXT` that GL
has already queued on a Vulkan semaphore (vk_done, vk_pass_done) blocks GL's channel, and the CPU then blocks inside the
GL driver at its next GL call that needs the channel - as at 18:28 (terrain draw). No timeout exists for that wait and the
guard cannot fire inside a GL call; only the queue-side construction above (every wait has its signal submitted first)
prevents it. `glFinishBounded` protects the waits we start ourselves, not GL's own blocking.

### 8.2l Run nv-hang1 (19:03-19:10Z, the 8.2k build; coordinator's session) and the fixes after it

Shadow Testwolf, NVIDIA, validation on, `TextureDiscardBackgroundedTime` 5, unfocused by `unfocus_side.py`
(`hl.dsp.focus`). The resizes did not apply (the window was fullscreen). No kernel Xid.
- 19:07:22Z: 10 x SYNC-HAZARD-WRITE-AFTER-WRITE on `vkCmdCopyBuffer` into vertex buffers - an earlier frame's copy into
  the same buffer was not ordered before this one. Fixed in `WolfVkBuffer::recordUploads` (a TRANSFER|dst -> TRANSFER,
  TRANSFER_WRITE -> TRANSFER_WRITE barrier before the copies).
- 19:09:09Z, the second of the purge ("backgrounded for 5s"): VUID-VkImageCreateInfo-mipLevels-00958 (an 8x8 copy made
  with 6 levels) and VUID-vkCmdCopyBufferToImage-imageSubresource-07971 (16 wide into a 1x1 level), 2 each. **Cause:** the
  purge re-specifies a texture's level 0 smaller and GL keeps the older, larger levels above it; `mirrorTexture` counted
  levels by "width > 0" and so took them as part of the chain. The copy wrote past the level on the card.
- 19:09:10Z: `VK_ERROR_DEVICE_LOST` from that copy's one-time fence wait. Nothing marked the device; the next texture's
  `vkQueueSubmit` went to the lost device and the validation layer crashed (SIGSEGV, `endOneTimeAndWait` <-
  `mirrorTexture` <- `WolfVkMirror::texture` <- `preparePass`). No fall-back happened.

Fixes: the level count stops at the first level that is not max(1, w0 >> i) x max(1, h0 >> i) and at 1x1;
`WolfVkImage::createInternal` refuses more levels than the full chain (00958), `WolfVkImage::upload` refuses a level,
layer or extent outside the image (07967 / 07968 / 07971, block alignment 00207 / 00208) instead of recording it; a copy
with any level missing is dropped. `WolfVkDevice::lost()` turns `VK_ERROR_DEVICE_LOST` from every submit, fence/timeline
wait, acquire and present into the hung flag, and `endFrame` / `submitMidFrame` / `submitSync` / `endOneTimeAndWait` /
`beginOneTime` refuse on a hung device, so nothing submits again and the next frame falls back. `mirrorTexture` refuses
on a hung device and does not retry a texture that failed in the same frame. Review fixes in the same round: the share and
G-buffer destroys decide "hung" from their own waits (they read it before waiting); `fallBack` / `shutdown` re-check
`hung()` between teardown steps and abandon the rest from the first step that finds it (no surface destroy under a
swapchain that could not be destroyed, VUID-vkDestroySurfaceKHR-surface-01266); `WolfVkScene::destroy` abandons when its
wait fails (it reset descriptor pools anyway); `waitIdle` no longer ends in an unbounded `vkDeviceWaitIdle` (the destroys
need only the batches, which are tracked and waited); an acquire timeout (now 100 ms) skips the frame instead of falling
back, since a compositor may withhold images from a hidden window under FIFO - a stopped GPU is still caught by
`beginFrame`'s bounded fence wait; `recordFrame` returns false (no submit, fall-back) when the shared layers were not
acquired. Item 8 of the review (`mirrorTexture`'s byte count never set) was checked and is not a bug: `bytes +=
data.size()` per level, `texture()` adds `entry.mBytes` to `sTextureBytes`. Still to be shown by a run (nv-hang2).

### 8.2m Run nv-hang2 (19:24-19:26Z) and present fences for the swapchain recreate

nv-hang2 (coordinator's session, fullscreen unset by `unfocus_side2.py`, 4 resizes, 4 texture purges): no device loss,
no Xid, no SYNC-HAZARD, no mip-level VUIDs - the 8.2l fixes hold. What was left: 20 x
VUID-vkDestroySwapchainKHR-swapchain-01282 ("in use by VkQueue") and 20 x VUID-vkDestroySemaphore-semaphore-05149, one
pair per swapchain recreate (three at start-up, then one per resize). Cause: the recreate destroyed the old swapchain and
its render-done semaphores as soon as our command buffers were finished; a present is not a command buffer, and without
the unbounded `vkDeviceWaitIdle` (dropped in 8.2l) nothing waited for the presents that still held them.

Fix (`wolfvkswapchain.h/.cpp`, `wolfvkdevice.cpp`): the instance enables VK_KHR_surface_maintenance1 (else the EXT) with
VK_KHR_get_surface_capabilities2, the device enables the matching VK_KHR/EXT_swapchain_maintenance1 when its
`swapchainMaintenance1` feature is there (vulkaninfo: both the RTX 5090 and the Radeon 890M offer both, feature true).
Every present then carries a fence (`VkSwapchainPresentFenceInfoKHR`). The old swapchain is passed as `oldSwapchain` and
kept, with its semaphores and its presents' fences, as "retired"; it is destroyed once its last present fence has
signalled (checked without waiting after each present; present fences signal in present order). Fences are recycled
only after they signalled. Shutdown, and more than 4 retired swapchains, wait for them with the 8 s bound; running out is
logged and the objects are left (a held present is the compositor's, not a GPU hang, so it does not fall back).

Without the extension (not on either test GPU, so not exercised): no API reports a present's completion. The old
swapchain is destroyed when the first image presented on its successor has been acquired again - ANGLE's rule for the
same case (chromium `third_party/angle/.../SurfaceVk.cpp:1521-1523`, on this machine) - and at shutdown or past the cap
after `vkQueueWaitIdle`, which takes no timeout: the one CPU wait of the backend that cannot be bounded. The spec text
behind both rules is not on this machine; ANGLE's source is the reference used.

Not yet built or run (the coordinator's OpenGL control session was running): to be confirmed by a build and a session
with validation (expected: no 01282 / 05149 at recreates; check that no new present-fence VUID appears).

### 8.2n "Still got that weird blue on the right" (Paul, 19:50Z): the G-buffer depth was 1 ulp off GL's

What is seen: short blue/white (sky and water colours) horizontal dashes along the plank lines of the hulls and on the
shop's dark wood, gold trim and glass, Vulkan mode only (session nv-ab1: Vulkan and GL drawing the same pass seconds apart,
`shots/nv-ab1/ab{0,1,2}_{vk,gl}.png`, `bluedash/ab0_crop_vk_left_gl_right.png`). In the GL picture the same plank lines
are thin dark seams.

Evidence (no session, from the same-session dumps of nv-hang2 and nv-hang3, Vulkan vs GL drawing the simple pass): on the
static texels where both drew the same surface (equal normals, depth within 1e-4), the diffuse, ORM, normal and emissive
attachments agree, and the simple pass leaves no holes - but the stored depth is bit-identical in only 17-24% of them;
36-46% are 1 ulp nearer and 36-40% 1 ulp farther (median |difference| = 1 ulp). Cause, read from the code: GL stores
z_w = ((f - n)/2) z_d + (n + f)/2 from the viewport transform (OpenGL 4.6 13.8.1), while Vulkan received
modelview_projection_matrix pre-multiplied by z' = 0.5 z + 0.5 w (`WolfVkShaderMirror::applyProjectionFix`,
wolfvkshader.cpp:299-312) and divided that by w: the same value by a different sequence of float roundings. Every GL pass
drawn after the Vulkan simple pass that tests against that depth on the same or coplanar surfaces (LLGLDepthTest LEQUAL)
then passes or fails per texel depending on the sign of that ulp - a z-fight, which on the near-horizontal iso-depth lines
of a hull or wall shows as short horizontal dashes; where the later pass fails, what is left is what lies behind it. Which
later GL pass it is (the seam faces, an alpha-mask or material pass, the water or sky) is not yet identified, and the
dump cannot show it (it is taken right after the simple pass). Checked and ruled out in the code: the matrices
(LLRender::loadMatrix/multMatrix/syncMatrices llrender.cpp:1015-1100, 1238-1254 compute view*model and proj*mv with the
same glm calls `drawPass` uses), the viewport (both 0, 0, the target's size: llrendertarget.cpp:481 glViewport in bindTarget,
wolfvkrendertarget.cpp:353-364), the depth bias (only the fullbright overlay has one, wolfvkscene.cpp:849).

Fix (root): `VK_EXT_depth_clip_control` (vk.xml marks it specialuse "glemulation"; vulkaninfo: both the RTX 5090 and the
Radeon 890M offer it, `depthClipControl` true) is enabled with its feature, every pipeline chains
`VkPipelineViewportDepthClipControlCreateInfoEXT{ negativeOneToOne = VK_TRUE }` (wolfvkpipeline.cpp), and
`applyProjectionFix` no longer touches the matrices on such a device: Vulkan gets GL's matrices unchanged and the depth
comes from the same viewport transform as GL's. A device without the extension keeps the old correction (and the 1-ulp
mismatch; logged by the device line not naming the extension).

Proof plan (build r24, not yet run): `campaign.py` mode `ab` now also dumps the G-buffer in both states at the same view
(`gbuf-<TAG>/gbufvk_*`, `gbufgl_*`), and `~/.cache/vkbackend/abcheck.py <TAG>` prints the share of bit-identical depth on
static texels (expected near 100% after the fix, against 17-24% before) and the final-frame difference in the shop crop
with side-by-side crops `shots/<TAG>/ab<k>_crop_vk_top_gl_bottom.png`. Baseline nv-ab1 (old build): shop crop mean
difference 35-50/255, 44-63% of pixels over 24/255 (that crop also holds part of a bobbing ship, so it never reaches 0).
Until that run, "the dashes are this z-fight" is the best-supported explanation, not a proven one.

**nv-ab2 (r24, coordinator's session).** The device line names VK_EXT_depth_clip_control. Split by region at the `ab`
view (`abcheck.py`, now printing the split): **buildings (x < 1650): 43,642 texels, depth identical 100.00%**; boats (x >=
1650): 2.19% (the two dumps are seconds apart and the boats rock between them, so that number measures the rocking as much
as the renderers - `ab2_depthmap` shows whole hulls uniformly nearer or farther, a pose shift, not texel noise). So the
depth path is now identical to GL's for static geometry. Paul, watching: the houses are fine; the boats still show the blue
tearing and some bits missing. The six final-frame grabs timed out (grim gets no frame from a window that is not visible);
`campaign.py`'s grab now falls back to the viewer's own snapshot (LEAP LLViewerWindow saveSnapshot, BMP).

**The boats (open).** They are rocked by the viewer (wolfboatrock.cpp: an offset applied in LLDrawable::updateXform,
lldrawable.cpp:653/747, `markMoved` -> `updateMove` -> `makeActive`, lldrawable.cpp:822), so their batches carry a model
matrix (LLDrawInfo::mModelMatrix, applied by LLRenderPass::applyModelMatrix, lldrawpool.cpp:559-574). Read and found the
same on both sides: the matrix maths (drawPass's `view * model`, `projection * modelview` against LLRender::multMatrix and
syncMatrices), the order (the rocking and every updateMove run in idle before display, llappviewer.cpp:6335/6372; the
mesh rebuilds and LLVertexBuffer::flushBuffers end in postSort, pipeline.cpp:4134, before renderGeomDeferred). Not yet
shown either way within ONE frame, which is what matters (the dumps are seconds apart). Build r25 adds a debug check for
exactly that, `WolfVulkanDebugPoseCheck` (off by default): the Vulkan pass records each model matrix's values, the view and
each drawn vertex/index range; OpenGL's later `applyModelMatrix` calls in the main G-buffer phase are compared with them;
at captureWorld the Vulkan copies' positions and indices are compared with the GL buffers' bytes (glGetBufferSubData, once
a second); GL flushes into a buffer after the Vulkan pass drew from it in the same frame are counted and the first ten
logged. One log line a second, tag `WolfVulkanPose`. A non-zero count on the boats names the cause; zeros everywhere rule
the pose out and point back at depth/rasterisation of moving objects.

**nv-ab3 (r25, pose check on).** WolfVulkanPose all zeros over 2.6M batches (model matrix changed after the Vulkan pass 0,
view 0, sampled positions 0, indices 0, late GL flushes 0): within one frame Vulkan and GL use the same matrices and vertex
data. The crops show the dashes on what looked like a static waterfront shop; the G-buffer dumps of the same session say
otherwise: every building texel is depth-identical (green in the depth map), while the object at the right edge of the
`ab` view (gold trim, dark planks, windows - x > 2280 in the G-buffer, where the shop crop sits) is uniformly 1+ ulp
nearer in the Vulkan dump than in the GL dump taken seconds later, i.e. it moved between them: it is rocked like the
ships (a stern gallery, or a waterfront object WolfBoatRock rocks). So, on the evidence so far, the dashes appear only on
rocked objects, and a same-frame matrix/vertex mismatch is ruled out. Next: `campaign.py` mode `abbisect` grabs the vk/gl
pair with each water/reflection pass switched both ways (RenderScreenSpaceReflections, RenderMirrors,
RenderTransparentWater, WolfViewerWaterFFT, WolfViewerWaterWake, WolfViewerWaterShoreField, WolfViewerWolfWaterPrims,
WolfViewerHorizonSea - names and live reads cited in the script).

**nv-bis1 (coordinator's session).** Not one pass: the dashes are in every Vulkan grab from the start until the first
graphics change that actually changes a value, and in none after it, whatever the setting. RenderScreenSpaceReflections
and RenderMirrors run `handleReflectionProbeDetailChanged` (llviewercontrol.cpp:587-600, registered :1391-1392):
reflection and hero probe managers reset, `releaseGLBuffers` + `createGLBuffers` (every render target remade, the shared
G-buffer with them through the provider) and `setShaders` (every GL program reloaded, so every Vulkan shader mirror is
deleted, llglslshader.cpp unloadInternal, and made again at its next Vulkan draw). It does not touch the vertex buffers
(pipeline.cpp:1311/1437). Build r26 adds to the pose check (a) every attribute array, not only positions, with the batches
that have a model matrix first, and (b) once a second the Vulkan mirror's own copy of the simple pass's uniforms
(`WolfVkShaderMirror::describeUniforms`, log tag WolfVulkanPose); `campaign.py` mode `abreload` takes the vk/gl pair at the
start, after one reload (RenderScreenSpaceReflections True) and after a second (False). Comparing the uniform lines and
the attribute check before and after the reload is the next evidence.

**nv-rl1 (r26) and what it really showed.** The reload result was an artefact: the log has `no SPIR-V for "Deferred
Diffuse Shader" (key 5a3d2113...)` at 21:00:26 and `gbuffer draws 0` from then on. With screen-space reflections on, the
reloaded program is a variant the build-time SPIR-V set does not have, and `WolfVkScene::mirrorFor` kept ONE flag for the
whole scene (`mWarnedNoShader`), so after that no shader got a Vulkan mirror again - not even the original variant after
SSR went back off. Every "clean after the reload" grab of nv-bis1 and nv-rl1 was OpenGL drawing the whole pass. Fixed in
r27: a missing variant is remembered per shader and per GL program object (`mNoSpirv`), so a reload is tried again. The
dashes therefore come from the Vulkan simple pass whenever it draws, on rocked objects. Also checked: the simple pass's
SPIR-V writes no gl_ClipDistance (`b849c2b3...vert.spv`: gl_PerVertex declared, only gl_Position stored, capability Shader
only), and the fragment shader's `mirrorClip` discards only when `mirror_flag > 0` - the Vulkan copy holds mirror_flag 0
with clipPlane NaN, so the NaN plane is not used. Clipping is ruled out. Within one frame the pose check (all attributes,
49,369 model-matrix ranges) found no difference in matrices or vertex data. Next evidence: `campaign.py` mode `abfreeze`
(rocking on, then off with G-buffer dumps while the boats stand still - comparable texel by texel - then the dry-hull
water carving off, then both back on).

**nv-fz1 (r27).** Dashes with the rocking off and with the dry hulls off too; the still-boat dumps (Vulkan's view of the
attachments) match OpenGL's texel for texel in depth, ORM, normal and emissive. So whatever makes the dashes is not in
what Vulkan wrote, as Vulkan reads it back. Two things in r28: (1) a real synchronisation error found while reading the
hand-back: the release barriers of the shared depth (WolfVkGBuffer::release, WolfVkShare::release) named only
LATE_FRAGMENT_TESTS as the source stage, while depth attachment writes happen in EARLY and LATE fragment tests (vk.xml
syncaccess VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT); early-test writes were outside the release's first scope, so
nothing made them available to GL. Both barriers now name both stages. (2) The decisive check whether GL sees what Vulkan
wrote: every `WolfVulkanDebugDumpGBuffer` now also writes the attachments as OpenGL reads them in the same frame
(`gbufglview_<vkpass|glpass>_*`, glGetTexImage on the imported textures after glTakeBack), and `viewcheck.py <TAG>`
compares them byte for byte with the Vulkan-side dump. Not yet run.

**nv-fz2 (r28).** The first grab still has the dashes; `viewcheck.py`: OpenGL's reads of the five attachments equal
Vulkan's byte for byte in both states. So the data are right and only something OpenGL derives from them can differ. The
semaphore layouts match on both sides (GL signal/wait: GL_LAYOUT_COLOR_ATTACHMENT_EXT / GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT,
glext.h:8065-8066; Vulkan release/acquire: COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL); the
EXT_external_objects spec text itself is not on this machine, so what the driver may assume beyond that cannot be cited
here. Experiment in r29: `WolfVulkanDebugGLDepthRewrite` makes OpenGL copy the shared depth and draw it back into the
bound G-buffer (depth test ALWAYS, gl_FragDepth, colour writes off) right after the hand-back, so OpenGL's depth unit has
written every texel itself before its later passes; `campaign.py` mode `abdepth` takes the pair before, with and after it.
Dashes gone with it = OpenGL's own depth-test state (hierarchical Z / compression) was stale after Vulkan's writes.

**Proven (nv-dz1, coordinator's session).** First grab with dashes, the same view with the GL depth rewrite clean on hulls
and stern, dashes back with it off; GL drawing the pass clean throughout (sheet
`/tmp/claude-1000/-home-paulc-NetBeansProjects-firestorm/fc37926a-505d-48c1-a311-dd201f112325/scratchpad/dz1/sheet.png`).
OpenGL READS the shared depth correctly (nv-fz2 viewcheck: byte-identical), but its depth TEST state for that buffer is stale
after Vulkan has written it, so later GL passes let the water and the sky through Vulkan-drawn surfaces in short runs.
EXT_external_objects §4.2.3 (registry.khronos.org, quoted by the coordinator): "Since texture layout state is managed
internally by the GL, but may have been modified by an external API, the current layout of the textures must be specified
to initialize internal GL state prior to using the textures after an external access" - the layout on the wait is the only
mechanism, and ours already matches (Table 4.4: GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT <-> VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL).

**Shipping fix (r30, `WolfVulkanGLDepthRewrite`, on by default).** Right after the hand-back, `WolfVkGBuffer::glRewriteDepth`:
`glCopyImageSubData` of the shared D32F depth to a scratch texture, then one full-screen triangle into the bound G-buffer,
depth test ALWAYS, `gl_FragDepth` = the copy (texelFetch), colour writes off, scissor and culling off; all GL state put back.
The values do not change; GL's depth unit has written every texel itself. Its GPU time is measured with GL_TIME_ELAPSED
queries (read a frame later, never waited for) and logged once a second in the `WolfVulkanFrame` line ("GL depth rewrite
N ms (GPU)"). The cleaner options: VK_EXT_image_compression_control is offered by RADV only (vulkaninfo:
imageCompressionControl true on the Radeon 890M; the extension is absent on the RTX 5090), so it cannot be the fix for
NVIDIA; handing the depth back in GENERAL (`WolfVulkanDepthHandBackGeneral`, Table 4.4 GL_LAYOUT_GENERAL_EXT) is built as a
test option. `campaign.py abdepth` now takes: the defaults (rewrite on - the first grab must be clean), rewrite off, rewrite
off + GENERAL, defaults again. Not yet run: the proof on NVIDIA and AMD, the cost in ms and fps against GL mode.

**Results (nv-dz2 / amd-dz2, r30; coordinator's sessions; contact sheet `~/.cache/vkbackend/bluedash/dz2all.png`).**
NVIDIA: the first Vulkan grab (rewrite on, the default) clean; rewrite off: dashes; rewrite off with the depth handed back in
GENERAL: dashes (the layout alone does not reset NVIDIA's GL depth state); default again: clean. AMD (RADV + radeonsi): clean
in every state - that driver pair never had the stale state. GPU time of the rewrite (GL_TIME_ELAPSED, mean): NVIDIA
0.131 ms a frame over 121 s, AMD 0.819 ms. Decision: the rewrite stays ON for every GPU (Windows and Intel drivers are not
tested; the cost is small against a 50-70 ms frame). The GENERAL-layout option was removed in r31 (it does not fix NVIDIA
and is not shipped as a dead switch); `WolfVulkanGLDepthRewrite` stays as an on-by-default setting so the A/B
(`campaign.py abdepth`) can still show the difference. Still unproven: Windows NVIDIA/AMD/Intel GL+Vulkan interop, where the
same stale state may or may not occur (the rewrite covers it either way), and whether a driver update makes it unnecessary.

### 8.2o AMD: Vulkan mode ~11 ms a frame slower than OpenGL (cause half found; options paper for Paul)

Measured by the coordinator (alternating, Caribbean 4096 m, performance profile): AMD Radeon 890M Vulkan 23.0 / 22.7 fps
(43.7 ms) against OpenGL 30.1 / 31.0 (32.8 ms); NVIDIA Vulkan 33.8 / 33.6 against OpenGL 32.3 / 32.6 (+4%). The depth
rewrite of 8.2n costs 0.33 ms of it on AMD. Instrumentation in build r35 (only with `WolfLogFrameRate`): a second
`WolfVulkanFrame` line a second, "breakdown per frame", with
- CPU ms of each Vulkan-mode step (steady_clock): hand (glSignalSemaphoreEXT), pass record (acquire + mirror uploads +
  draws + release), pass submit, take back (glWaitSemaphoreEXT), depth rewrite, slot wait (the frame slot's fence /
  timeline wait in beginFrame), world capture, acquire, frame record (incl. OpenGL's UI capture + gl_done), frame submit,
  present; the one-time submits (count and CPU ms, WolfVkDevice::takeOneTimeStats);
- GPU ms: the Vulkan G-buffer pass and the Vulkan frame (vkCmdWriteTimestamp in their command buffers, read without waiting
  after the slot's fence; ticks x timestampPeriod), OpenGL's world capture and UI capture (GL_TIME_ELAPSED);
- the swapchain's present mode, image count, size and format.
Result (coordinator's sessions amd-pb-vk / nv-pb-vk, window 1224x1520): AMD CPU sum of all Vulkan steps 1.67-1.81 ms a
frame (pass record 0.85-0.91 the largest), one-time submits 0, GPU: Vulkan pass 0.81 ms, Vulkan frame 0.21, GL world
capture 0.38, GL UI capture 0.09, swapchain MAILBOX 4 images; NVIDIA CPU sum 1.05-1.25, Vulkan pass 0.22, IMMEDIATE 3
images. The ~11 ms is in none of the Vulkan steps, so it must be in OpenGL's own work in Vulkan mode. r36 adds an OpenGL
GL_TIMESTAMP at every swap/present in both renderers (`WolfVkBridge::glFrameStamp`, llviewerdisplay.cpp swap()): one
"GL GPU frame interval" line a second, labelled OpenGL / Vulkan with the shared G-buffer / Vulkan without it. The three
runs that separate the causes: OpenGL; Vulkan with `WolfVulkanDebugGBufferGLOnly` (shared images kept, Vulkan pass off);
Vulkan with `WolfVulkanDebugNoGBuffer` (read at start: swapchain, layers and composite kept, G-buffer not shared).
**Mode runs (r36, coordinator, window 1224x1520).** AMD: OpenGL 31.9 fps, GL GPU frame ~31 ms; Vulkan without the shared
G-buffer 27.0 fps, ~37-38 ms; Vulkan with the shared G-buffer but OpenGL drawing the pass 25.2 fps, ~40 ms; full Vulkan
25.4 fps, ~40 ms. NVIDIA: OpenGL 42.2 fps (~24 ms) against Vulkan 40.2 (~25 ms) - about 5% slower at this window size
(the +4% earlier was at 2514x1520). So the Vulkan pass costs nothing measurable; OpenGL's own GPU time grows, ~6 ms on
AMD from the layers/swapchain path and ~3 ms more from the shared G-buffer. Why is not known yet. The driver sources
(radv, radeonsi) are not on this machine, so what the drivers do with imported memory (compression, placement) cannot be
read here; it has to be measured. r37 adds three measurements: (1) every image shared with OpenGL logs its size, format,
usage, create flags, the memory types the driver allows (memoryTypeBits) and the one taken (`Shared image ...` lines,
wolfvkimage.cpp); (2) `GL hand-over span` in the breakdown line - OpenGL's GPU time from its gl_pass_done signal to the end
of its vk_pass_done wait, i.e. the Vulkan pass plus the cost of switching APIs on the GPU; (3) two testing settings:
`WolfVulkanPresentMode` (immediate / mailbox / fifo instead of the vsync rule; the 890M picked MAILBOX, NVIDIA IMMEDIATE;
read when the swapchain is made) and `WolfDebugGLDepth32F` (OpenGL mode with its own depth buffers in the shared
G-buffer's D32F format instead of D24, to price that format alone; read when the targets are made).
Observation, not a finding: the AMD device lists DEVICE_LOCAL types 0, 1, 3, 4 and 7 (7 = DEVICE_COHERENT_AMD |
DEVICE_UNCACHED_AMD); `WolfVkAllocator::findMemoryType` takes the first DEVICE_LOCAL type the image allows (no preference
asked), which is type 0 unless an image's memoryTypeBits exclude it - worth logging per shared image if the runs point there.
**r37 runs (coordinator, logs `log-{amd,nv}-gy-*.log`).** AMD: OpenGL 28.6 fps (GL GPU 36-40 ms), Vulkan MAILBOX 21.9
(49-50 ms), Vulkan IMMEDIATE 21.7 (45-46 ms), Vulkan without the shared G-buffer 22.9 (43.5-44 ms); NVIDIA OpenGL 30.4,
Vulkan 30.8. So the present mode is not the cause. Every shared image on AMD: memory type 0 (memoryTypeBits 0x7ad),
dedicated, OPTIMAL, usage 0x15 colour / 0x25 depth, create flags 0 - not the uncached type 7. NVIDIA: type 1 (bits 0x3).
Window size: these runs were meant at full width, but every log shows the window going 2522x1520 -> 1224x1520 one
second after start (`Handling a resize event`, llwindowsdl2.cpp:2559) and ~300 breakdown lines at swapchain 1224x1520
against one at 2522x1520, so they are at 1224x1520 like r36. Two faults found: `WolfDebugGLDepth32F` stopped the viewer at
start-up (`LLImageGL::dataFormatBits` had no `GL_DEPTH_COMPONENT32F` case: LL_ERRS "Unknown format: 8cac"), and the
`GL hand-over span` read 0.00 ms on AMD (0.48-0.53 on NVIDIA) with a 0.81 ms Vulkan pass between its two ends: radeonsi
does not count the semaphore wait as GL_TIME_ELAPSED time. (The field was there; the values carry no "ms" suffix, as all
GPU fields of that line, which is why a grep for "span [0-9.]* ms" found nothing.)
**r38.** (1) `dataFormatBits` knows `GL_DEPTH_COMPONENT32F` (32 bits; `dataFormatBytes` derives from it,
`dataFormatComponents` is only asked for pixel formats of uploads and never sees a depth format); the VR depth copy
renderbuffer (wolfvrscreen.cpp) follows the same setting, as a depth blit needs matching formats. (2) The hand-over span is
two GL_TIMESTAMP counters (glQueryCounter, recorded once all earlier commands are done, OpenGL 4.6 4.3), the second issued
after glWaitSemaphoreEXT, so the wait is inside it on any driver; the field now reads `GL hand-over span X ms (GL
timestamps)`; a failed hand-over drops its sample. (3) `WolfVulkanDebugCompressionQuery` (default off, read when the device
is made): enables VK_EXT_image_compression_control (RADV 26.2.5 offers it, vulkaninfo; NVIDIA 615.78.08 does not) with its
feature, and every Vulkan attachment image - shared or Vulkan-only - logs `Compression of shared|Vulkan-only image ...:
imageCompressionFlags 0x...` from vkGetImageSubresourceLayout2 + VkImageCompressionPropertiesEXT, with the subresource and
allocation sizes. That extension only tells fixed-rate (lossy) compression from none: DEFAULT means "no fixed-rate
compression" (reference page), so the lossless DCC / HTILE question is answered by the drivers' own image dumps instead:
`RADV_DEBUG=img` ("Print image info") for every Vulkan image and `AMD_DEBUG=tex` ("Print texture info") for radeonsi's
textures, imported or its own (Mesa environment-variable documentation, docs.mesa3d.org/envvars.html), written to the
viewer's stderr (the session's `run-<tag>.out`). The pricing run beside it: OpenGL mode with `AMD_DEBUG=nodcc,nohyperz`
(DCC and Hyper-Z off in radeonsi, same page) - if pure OpenGL then falls to the Vulkan-mode frame time, lost compression
on the imported images is the cause.
**r38 runs (chain30, coordinator, AMD, window 1224x1520 again).** OpenGL 28.6 fps (GL GPU frame 35-37 ms, mean of the
per-second lines 35.4); OpenGL with D32F depth 28.4 (35.1) - **D32F is not the cause**; OpenGL with
`AMD_DEBUG=nodcc,nohyperz` 24.5 (40.2; 42-43 settled) - turning off radeonsi's colour and depth compression costs pure
OpenGL ~6 ms; Vulkan 20.9 (45.5; 47-49 settled). GL hand-over span (timestamps) median 1.17 ms (0.98-1.09 typical) around
a 0.71 ms Vulkan pass: the API switch itself costs ~0.4 ms a hand-over on AMD (NVIDIA r37: ~0.5 span around 0.24).
Compression: `WolfVulkanDebugCompressionQuery` - every shared image (RGBA8 layers and G-buffer, RGBA16, RGBA16F, D32F, both
window sizes) reports imageCompressionFlags 0x4 DISABLED. The driver dumps agree: `RADV_DEBUG=img` prints a DCC block for
the swapchain images (modifier `GFX11,64KB_R_X,...,DCC`) and none for any shared image; `AMD_DEBUG=tex` shows radeonsi's
own D32F/D24S8 targets with an HTile block and the imported D32F images (flags 0x45020000) without one. (One thing not
understood: radeonsi's dump lists a DCC block, at an offset past the Vulkan allocation's end, on the RGBA8 textures that
match the imported ones by count, size, format and flags; RADV laid them out without DCC. No GPU fault or wrong picture
was seen; what radeonsi really does with it needs the Mesa source, which is not on this machine.) No `Vulkan-only image`
line was logged because no Vulkan-only attachment exists in a default Vulkan frame: the only one, the overlay's scene
target (`WolfVkScene::ensureSceneTarget`, RGBA16F + R8), is made only with `WolfVulkanOverlayFullbright` on
(wolfvkscene.cpp:820-827, off by default); the RADV dump has no RGBA16F image beyond the shared ones either.
**Conclusion.** On RADV + radeonsi, images shared between the two APIs run without DCC (colour) and HTILE (depth). That
explains the shared-G-buffer part of the gap (r37: Vulkan 49-50 ms against Vulkan without the shared G-buffer 43.5-44):
OpenGL draws all the deferred geometry into those images and the lighting reads them back several times, all at full
bandwidth on an iGPU. It does NOT explain the other ~6 ms (OpenGL 36-40 against Vulkan without the shared G-buffer
43.5-44, r37; r36 31 against 37-38): there OpenGL only blits once into each shared layer (GL world + UI capture 0.47 ms
GPU together), so that part lies elsewhere in the layers/present path and is still open. NVIDIA: Vulkan equal to OpenGL
at 1224x1520 in r37 (30.8 against 30.4), ~5% slower in r36 at the same size, ~4% faster at 2514x1520 earlier. The
choices (fewer shared images, compression across the boundary, the whole G-buffer phase in Vulkan, OpenGL on AMD iGPUs)
are written up for Paul in `~/wolfviewer-releases/vulkan-amd-compression-options.md`; nothing is changed until he decides.
**The second half: XWayland ruled out; the radeonsi dumps show no second cause (2026-11-09).** Coordinator's runs (AMD,
1224x1520): OpenGL on X11 with `AMD_DEBUG=tex` (amd-gltex) 31.8 fps, GL GPU frame 32-34 ms; OpenGL on native Wayland
(amd-wl-gl, "SDL video driver: wayland") 30.8 fps, 35-36 ms; Vulkan on native Wayland (amd-wl-vk, "Window surface created
(Wayland)", RADV, MAILBOX 4 images) 22.9 fps, 43-45 ms - the same gap as through XWayland, so XWayland is not the cost.
amd-wl-vk is also the first native-Wayland Vulkan session (wolfvksurface.cpp's Wayland path) on AMD: it started, ran and
quit without falling back.
Texture dumps compared (`AMD_DEBUG=tex`, every texture radeonsi creates, ~90-97 thousand per session; tabulated by size,
format, swizzle mode, flags and metadata for everything of 0.5 Mpixel or more, `texdump.py` in the scratchpad):
- *Same in both modes:* every texture and render target the viewer itself allocates other than the G-buffer - the
  reflection probes, shadow maps (2522x2522, 2048x2048 z24x8 with HTile), `mRT->screen` and the post-processing chain
  (RGBA16F with DCC), the exposure/luminance mips (r16g16_float, DCC). That includes the window-wide *linear* BGRA8 and
  RGBA8 images seen earlier (1224x1350, 1224x1501, 1224x1520, swmode 0, flags 0x10000): 25 of them in OpenGL mode against
  22 in Vulkan mode. They come in pairs with a tiled image of the same size, so they are upload or read-back staging and
  not render targets. Nothing is linear in Vulkan mode that is tiled in OpenGL mode.
- *Only in OpenGL mode:* 14 window back buffers at 1224x1520 (BGRA8, DCC modifier), allocated as the drawable resizes. In
  Vulkan mode GL's swap is skipped (llviewerdisplay.cpp:1914) and no back buffer is ever allocated at 1224x1520. Only the
  one from the first second at 2522x1520 exists. So GL keeps drawing its last pass and the UI into a 2522-wide, DCC
  back buffer through a 1224-wide viewport. That costs no extra traffic, except that clears without a scissor cover the
  whole buffer, and DCC fast-clears make those cheap.
- *Only in Vulkan mode, and all accounted for:*
  - **G-buffer.** GL's own deferredScreen attachments (pipeline.cpp:981-982 `allocate` + `addDeferredAttachments`: 2
    RGBA8 DCC, RGBA16 DCC, RGB16F DCC, z24 HTile) are replaced by the imports (wolfvkgbuffer.cpp:299 and the colour
    images through `WolfVkImage::createExternal`, wolfvkshare.cpp:375): flags 0x5010000/0x45020000, no HTile, no DCC on
    the 16-bit images. This is the first half.
  - **Shared layers.** World RGBA8, UI RGBA8 and depth D32F at window size.
  - **Depth rewrite scratch.** A native GL D32F with HTile, one per size (wolfvkgbuffer.cpp:491-493, glRewriteDepth).
  - **G-buffer dump staging.** Two each of linear RGBA16F, RGBA16, R32F and RGBA8 at 1224x1501, from
    `writeGLViewDump`'s glGetTexImage (wolfvkgbuffer.cpp:412-436; this session dumped to `gbuf-amd-r38-vkcomp/`). These
    happen once and are not per frame.
Per-frame traffic of everything Vulkan mode adds without the shared G-buffer (estimate, 1224x1520 at 4 bytes a pixel =
7.4 MB a full-window image):
- world blit, read + write: 15 MB;
- depth pass, read D24 + write D32F + clear: 22 MB;
- UI blit: 15 MB;
- Vulkan composite, read world + UI and write the swapchain image: 22 MB.
That is about 75 MB a frame, roughly 0.9 ms at the ~80 GB/s the world capture achieves; measured GPU 0.64 ms (GL
captures 0.43 + Vulkan frame 0.21). **So the dumps rule out layout and compression as the second ~6 ms.** What is left,
untested:
- (a) the GPU idles between the two APIs' submits (GL's world capture waits vk_done of the previous frame,
  wolfvkshare.cpp:688; the UI capture waits it too if no world capture did, :752; Vulkan's submit waits gl_done; radeonsi flushes at every glSignalSemaphoreEXT), so the frame
  interval holds idle time, not work;
- (b) the APU runs the GPU at a lower clock in Vulkan mode, because CPU and GPU share one power budget and Vulkan mode
  adds CPU work (the mirrors, a second driver);
- (c) GL work that grows in Vulkan mode.
The measurement that separates them is `~/.cache/vkbackend/gpusample.sh TAG` (read-only: card2's `gpu_busy_percent`,
`freq1_input` sclk and `power1_average`, plus the viewer's CPU from /proc, every 250 ms) beside alternating gl /
vk-noshare / vk sessions. Busy clearly under ~95% in Vulkan mode against ~100% in OpenGL means (a); the same busy at a
lower sclk means (b); the same busy and clock means (c), to be bisected with the existing switches
(`WolfVulkanDebugNoScenePass`, `WolfVulkanDebugNoGBuffer`).
**gpusample runs (coordinator, AMD, 1224x1520, means over each 45 s timing window).** OpenGL: busy 91.2% (min 86),
sclk 2387 MHz, APU 40.0 W, 31.3 fps. Vulkan without the shared G-buffer: busy 77.3% (min 73), 2386 MHz, 40.0 W, 24.0 fps.
Vulkan: busy 78.6% (min 74), 2479 MHz, 40.0 W, 23.1 fps. The viewer-CPU column was empty: the sampler's process match
missed the `-avx2` executable; it now matches on /proc/PID/exe. Same clock, same power, so (b) is out. Busy x frame time
gives the GPU's working time a frame: OpenGL ~29.1 ms of 32.0; Vulkan without the shared G-buffer ~32.2 ms of 41.7;
Vulkan ~34.0 ms of 43.3. The GPU therefore does ~3 ms (no shared G-buffer) to ~5 ms (shared) more work, and sits idle
~6.6 ms more a frame than in OpenGL. That is why the second half shows as idle time.
**Why the idle is not yet shown to be the vk_done wait.** In the frame protocol, GL's world capture for frame N waits on
the vk_done of Vulkan's frame N-1 composite (wolfvkshare.cpp:688). That composite is submitted at present N-1, right
after GL's gl_done N-1 signal (wolfvkshare.cpp:777), and its GPU work is 0.21 ms. GL's frame N commands that come before
the capture have no dependency, so while the CPU is ahead of the GPU, the GPU always has frame N's GL work to run, and the
wait has long been satisfied by the time frame N's capture comes up. On this protocol the GPU can only run dry when the
CPU is late with frame N's commands, i.e. when the processor side, not a wait, sets the frame rate. Then double-buffered
layers would change nothing. Our own CPU steps without the shared G-buffer total 0.46-0.50 ms a frame and its mirrors
are idle (0 buffers/textures), so a CPU cost, if that is what it is, lies outside the steps we time: in the GL driver
when GL never swaps, or elsewhere on the main thread. Before double-buffering (a second set of window-sized layers,
~45 MB at 2522x1520, and per-buffer semaphores), r39 measures which it is:
- `GL world capture span ... ms (GL timestamps, incl. the vk_done wait)` in the breakdown line (wolfvkbridge.cpp,
  `prof::sGlCaptureSpan` around `sScene->captureWorld()`). Next to `GL world capture` (GL_TIME_ELAPSED, which on radeonsi
  does not count semaphore waits) the difference is the GPU time the vk_done wait held GL back each frame. About 6 ms
  means (a) and the double-buffering fix; about 0 means it is not the wait.
- `gpusample.sh` now records the main thread's CPU (/proc/PID/task/PID/stat) beside the whole process. At or near 100%
  in Vulkan mode while the GPU idles means the processor side sets the frame rate.
- `chain31.sh` runs gl / vk without the shared G-buffer / vk on AMD with the sampler and 20 s of `perf record -t
  <main thread>` once in world (perf-TAG.txt: user-space samples by library and symbol), which shows where the main
  thread's time goes in each mode.
**chain31 (r39, coordinator, AMD, 1224x1520).**

| Run | fps | GPU busy | sclk | Viewer CPU | Main thread | GL world capture (elapsed / span) |
|---|---|---|---|---|---|---|
| OpenGL | 31.0 | 92.5% | 2388 MHz | 130% | 93% | - |
| Vulkan, G-buffer not shared | 24.6 | 78.0% | 2463 MHz | 113% | 83% | 0.33 / 0.33 ms |
| Vulkan | 23.0 | 79.7% | 2468 MHz | 109% | 80% | 0.38 / 0.38 ms |

- **The vk_done wait costs nothing**, so double-buffering the layers is not the fix and was not built.
- **The CPU profiles have the same shape in both modes** (perf, main thread, user space): OpenGL 60.2% viewer, 31.7%
  libgallium, 1.5% libGL; Vulkan 59.2%, 30.5%, 1.4% libvulkan_radeon, 1.2% libGL.
- **Both the main thread and the GPU idle about 13 points more** in Vulkan mode. The main thread is blocked or asleep
  for ~7 ms a frame (OpenGL ~2 ms), outside every step we time.
- **Code read for a sleep that keys off the swap: none found.**
  - `LLAppViewer::doFrame`'s sleep block (llappviewer.cpp:1921-2046) has `YieldTime` (off at -1), the background yield
    (`BackgroundYieldTime` 40 ms, only without focus or visibility - the same in both modes), the FS frame limiter
    (`FSLimitFramerate` on, `FramePerSecondLimit` 60: it sleeps only below 16.7 ms a frame, so not at 24 fps) and the LFS
    back-off.
  - Nothing in wolfvk*.cpp, llrender/vk or llwindowsdl2.cpp sleeps.
- **r40 measures where the main thread blocks.**
  - A "main loop per frame" line a second (WolfLogFrameRate, both renderers; `WolfVkBridge::LoopTimer` /
    `loopFrameEnd`, wolfvkbridge.cpp; timers in llappviewer.cpp doFrame and llviewerdisplay.cpp swap()). For each stage -
    input (native events, gatherInput, coroutines), idle, display (without the swap), swap/present, sleep block, other,
    frame - it gives the wall time and the blocked time: wall minus the main thread's own CPU time
    (`clock_gettime(CLOCK_THREAD_CPUTIME_ID)`; Windows `GetThreadTimes`, tick resolution).
  - `wchansample.py` (read-only, 200 Hz) gives a histogram of the main thread's state and kernel wait function
    (/proc/PID/task/PID/wchan; /proc/PID/syscall is not readable here under ptrace_scope 1). A fence wait (dma_fence /
    drm_syncobj / amdgpu), a futex (another thread) or a timer sleep each name a different culprit.
  - `chain32.sh` runs gl / vknog / vk on AMD with both.
**chain32 and the environment tests (coordinator, AMD, r40).**
- wchan: OpenGL's main thread is R 93.6%, S `drm_syncobj` 6.0% (the swap's throttle). In both Vulkan modes it is R 83%,
  S `__futex_wait` 14.8-15.4%, `drm_syncobj` 0.4%.
- Loop timers: the blocked time is in display (without the swap): OpenGL 0.9-1.9 ms, Vulkan without the shared
  G-buffer 5.7-6.0, Vulkan 7.0-8.0. Our own steps inside present stay ~0.2 ms, so the glSignalSemaphoreEXT at the frame's
  end is not where it waits.
- Environment tests:

| Run | fps | Display blocked |
|---|---|---|
| Vulkan, `mesa_glthread=false` | 23.1 | 7.46 ms (no change, so not glthread) |
| OpenGL, `mesa_glthread=false` | 29.0 | 2.99 ms |
| Vulkan, `GALLIUM_THREAD=0` (no radeonsi threaded context) | 26.2 (+14%) | 0.41 ms |

  So the futex is the gallium threaded context's sync, where the main thread waits for the driver thread.
**Found in the Mesa source (tag mesa-26.2.5, gitlab.freedesktop.org/mesa/mesa; downloaded and read, not on the system):**
- `glGetQueryObject*(GL_QUERY_RESULT_AVAILABLE)` -> `_mesa_check_query` (src/mesa/main/queryobj.c:401-406,
  :1180-1183) -> `get_query_result(pipe, q, false)` -> `tc_get_query_result`
  (src/gallium/auxiliary/util/u_threaded_context.c:1300-1310). That calls `tc_sync_msg(tc, "nowait")` whenever the
  query is not yet marked `flushed`: wait for the driver thread's batch, then run the unflushed calls on the calling
  thread (`_tc_sync`, :716-793).
- A query is marked flushed only by `tc_flush_queries` (:3625-3637). That runs from a context flush through the
  threaded context: `tc_call_flush` (:3652-3662, the asynchronous flush on the driver thread) or the synchronous path
  of `tc_flush` (:3724-3728).
- OpenGL mode makes that flush once a frame in swapBuffers: frontends/dri/dri_drawable.c:584/594 `st_context_flush`
  -> state_tracker/st_manager.c:787-809 -> `st_flush` -> `pipe->flush`.
- Vulkan mode never swaps (llviewerdisplay.cpp swap(), the present replaces swapBuffers). `glSignalSemaphoreEXT` goes
  `server_signal_semaphore` (main/externalobjects.c:697-731) -> `tc_fence_server_signal` (u_threaded_context.c:3531-3540),
  which syncs once but hands the signal straight to the driver, never through tc_flush. So no query is ever marked
  flushed.
- Every occlusion query the viewer polls - `LLOcclusionCullingGroup::checkOcclusion` (llvieweroctree.cpp:1134),
  `LLReflectionMap::doOcclusion` (llreflectionmap.cpp:381), the scene monitor - then makes the main thread wait for the
  driver thread. A few thousand octree queries run per frame (stage-3 count, 8.2).
- No run-time switch prints the sync reasons: `tc_printf` exists only when Mesa is built with TC_DEBUG >= 2
  (u_threaded_context.h:218 `#define TC_DEBUG 0`, u_threaded_context.c:46-55). The HUD's `API-thread-num-syncs`
  (hud_context.c:1326) draws over the picture, so it was not used.
**The fix (r41).**
- `WolfVkBridge::present()` issues `glFlush()` once on every way out (a scope guard; not on a hung device). That is
  OpenGL's end-of-frame flush, which the skipped swapBuffers used to make.
- `_mesa_flush` (main/context.c:1617-1622) -> `st_glFlush` -> `st_flush` (state_tracker/st_cb_flush.c:51-63, 90-99) ->
  `tc_flush`, so the frame's queries are marked flushed before the next frame polls them.
- With externally shared images `_mesa_flush` takes the synchronous path (`async = !HasExternallySharedImages`); that
  is one sync right after the gl_done signal's own, when the driver thread has just been drained.
- The flush only submits GL's queued work. The GL/Vulkan semaphore order, waits and signals are unchanged.
- The depth rewrite's GL_TIME_ELAPSED query (wolfvkgbuffer.cpp `glRewriteDepth`), which ran every frame although only
  the log reads it, now runs only with the timing instruments on.
- `WolfLogFrameRateOnly` (new, off) keeps just the frame-rate line, with every instrument off (`WolfVkBridge::timingOn`),
  so fps runs measure no instrument cost.
- NOT RUN yet. Proof: `chain33.sh` (diagnosis runs with wchan/gpusample, then AMD gl/vk alternating twice and NVIDIA
  gl/vk twice with `WolfLogFrameRateOnly`, GL once with `GALLIUM_THREAD=0` for the record), after an AMD validation
  session; 'ab' and abdepth pairs per GPU.
**r41 results (coordinator).**
- AMD validation (GPU-AV + sync + resizes + unfocus): 0 VUIDs, 0 hazards, no device loss.
- Diagnosis runs: `__futex_wait` 0.2-0.8% (was ~15%), GPU busy 94.8% (was 78-80%).
- fps with `WolfLogFrameRateOnly` (the scene was heavier than the night before, so compare only within one chain):

| GPU | OpenGL | Vulkan |
|---|---|---|
| AMD | 20.6 / 19.9 (`GALLIUM_THREAD=0`: 20.5) | 24.6 (vk2 invalid: the window lost focus) |
| NVIDIA | 24.4 / 23.8 | 26.1 / 24.7 |

- abdepth on NVIDIA confirms the depth rewrite. The NVIDIA 'ab' pair was invalid because the camera moved between its
  grabs; `abcam` (camera locked and pose-checked) replaces it.
**chain34 (r42, AMD, the one valid round).** Vulkan with the G-buffer shared 25.6 fps (38.9 ms), not shared 26.9 (36.9),
OpenGL 26.6 (37.5). With the end-of-frame flush, Vulkan matches OpenGL on the 890M either way; not sharing is within noise
of sharing and would leave Vulkan drawing none of the world. Decision (coordinator, for Paul's "whichever is best"):
`WolfVulkanShareGBuffer` defaults to `on` everywhere; `auto` (the AMD-RADV rule) and `off` stay as choices. The first
round was invalid (the window was unfocused for 67-264 s while Paul used the laptop). A chain34 bug labelled its reruns
clean: `session()` overwrote a global TAG. Fixed in the chain scripts (local variables), and the focus check now counts
only seconds inside the campaign's timing window.

### 8.2p VR Big Screen in Vulkan mode (r43, 2026-11-09)

8.2i left Big Screen open in Vulkan mode: at swap the back buffer holds only the interface, on a cleared layer, because
`WolfVkBridge::captureWorld` has already moved the finished world into the shared world layer. The reverted 11-08
attempt changed the hand-over itself, and with it lost the shared layers' resize. The fix does not touch the shared
layers, their semaphores or the frame protocol at all.
- `WolfVRScreen::keepWorldForBigScreen()` (wolfvrscreen.cpp, called in llviewerdisplay.cpp render_ui between
  `WolfVRScreen::captureWorld` and `WolfVkBridge::captureWorld`) makes, only in Vulkan mode while Big Screen streams,
  the same copy `WolfVkShare::glCaptureWorld` makes: default framebuffer, GL_BACK, the window's rectangle from 0,0.
  The copy goes into GL's own RGBA8 texture, before the clear.
- At swap, `captureInto(false)` calls `composeVkBigScreen()`. It copies the back buffer (the interface) 1:1 into a
  second texture, then draws the composite of `wolfvk/compositeF.glsl` into a third:
  `ui.rgb + world.rgb * (1 - ui.a)`, texel for texel (texelFetch; the Vulkan composite samples each layer at its texel
  centres). The usual Big Screen copy (scaled, flipped, letterboxed) then reads that instead of the back buffer.
- The headset therefore gets the picture the Vulkan window shows. The login screen (no world captured this frame) reads
  the back buffer as before, which is what Vulkan composites there too.
- No CPU or GPU wait is added, and no semaphore.
- The three textures follow the window's size every frame, so a resize needs nothing else.
- They exist only while Big Screen streams in Vulkan mode (~3 x 7.4 MB at 1224x1520) and are freed with the rest of the
  capture (`WolfVRScreen::release`).
- Not reproduced: the debug views `WolfVulkanDebugTint` / `WolfVulkanShowSceneOnly` and the debug overlay
  `WolfVulkanOverlayFullbright` (all off by default; they draw in Vulkan only).
- Test (chain35 part 3): `scripts/wolfvr/e2e_vrbig.mjs`, run by `~/.cache/skyband/campaign_vrbig.py`.
  - Fake headset, Big Screen. The world must be in the picture: under 30% of the window's area exactly black.
  - The same check is repeated after two window resizes.
  - Vulkan with validation and OpenGL run on both GPUs, then `vrbigcheck.py` compares Vulkan against OpenGL.
  - Paul tests on the Quest 3.

### 8.2q NVIDIA on native Wayland: the swapchain failed and the viewer quit (Paul, r44, 2026-11-09 12:06Z)

**What Paul's log shows** (`WolfViewer.old`): native Wayland ("SDL video driver: wayland", NVIDIA by PRIME render
offload), then "Window surface created (Wayland)" on the RTX 5090, "Device created", then
`Falling back to OpenGL: vkCreateSwapchainKHR: VK_ERROR_SURFACE_LOST_KHR`. OpenGL went on (Wayland EGL 1.5, the window
2560x1600 / 4096x2560 pixels), and about a second later came "User requested quit", the clean shutdown and "quitCursors:
mWindow already gone". The next start's `WolfVulkanStartOK` check then chose OpenGL, so Vulkan never ran for him.

**Cause, from the code read.**
- SDL 2.28.0 (the viewer's SDL, packages SDL_version.h:60-62) gives every OpenGL window on Wayland a `wl_egl_window` and
  an EGL window surface on the window's own `wl_surface` when it creates it (src/video/wayland/SDL_waylandwindow.c:
  2082-2087). The Vulkan surface (wolfvksurface.cpp, `vkCreateWaylandSurfaceKHR` on the same `wl_surface` from
  `SDL_GetWindowWMInfo`) therefore asks a second presentation API to own a surface EGL already owns. The Vulkan spec
  makes a native window in use by another API an error of `vkCreateSwapchainKHR` (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
  NVIDIA answered VK_ERROR_SURFACE_LOST_KHR.
- In SDL 2.28 a Wayland viewer is only told to quit by a broken connection (SDL_waylandevents.c:372-385: a failed
  `wl_display_dispatch_pending`, then `Wayland_VideoReconnect`, which is compiled out, SDL_waylandvideo.c:1118, so
  `SDL_SendQuit`) or by a window close. So the Vulkan attempt very likely broke the Wayland connection (a protocol error
  from the compositor ends it). That is inferred, not yet seen: chain36's nv-wl-repro records the protocol traffic
  (WAYLAND_DEBUG=client). Once the connection is gone no window can carry on, which is why the fall-back could not
  keep the viewer running.
- Mesa's RADV shared the surface without trouble (amd-wl-vk, 8.2o); every NVIDIA Vulkan test before ran through
  XWayland, where GLX and the Vulkan Xlib surface share an X window.

**Fix (r45).**
- A start that tries Vulkan never makes a native Wayland window. `WolfVkBridge::decideBackend` (before the window
  exists) sets `LLWindow::sWolfAvoidNativeWayland`. `LLWindowSDL::createContext` then does not ask SDL for Wayland,
  logs "Native Wayland not used this start: the Vulkan renderer is chosen, and it shares the window with OpenGL through
  XWayland", and the window runs through XWayland - the proven path on both GPUs. A start in OpenGL keeps native
  Wayland as before.
- If the window is native Wayland anyway (SDL_VIDEODRIVER=wayland set by the user), `initAfterGL` checks, before any
  Vulkan surface is made, which driver the OpenGL device's Vulkan twin uses (`deviceUUID` = GL_DEVICE_UUID_EXT,
  `driverID`). Only VK_DRIVER_ID_MESA_RADV goes on; any other driver falls back to OpenGL with `WolfVulkanFellBack`
  ("on a native Wayland window this graphics driver cannot show Vulkan and OpenGL in the same window ..."). The window
  and its connection are untouched, so OpenGL carries on in it.
- Test switches (hidden, off): `WolfVulkanDebugFallBack` takes the run-time fall-back at the next frame (as a lost
  device does) and switches itself off; `WolfVulkanDebugWaylandAnyDriver` bypasses the driver check, to reproduce the
  failure.
- Cost for a Wayland desktop: Vulkan mode runs through XWayland, so scaling follows the compositor's XWayland rules
  rather than native fractional scaling.
- **Untested until chain36 runs.**

**Proven (chain39 nv-paul-repro, 13:07Z).** The session used Paul's own window and graphics settings, native Wayland
on the RTX 5090 (`SDL_VIDEODRIVER=wayland` and `__NV_PRIME_RENDER_OFFLOAD=1`, which LLWindowSDL itself sets on his
desktop) and `WAYLAND_DEBUG=client`, with the driver check bypassed.
- The protocol trace shows `wl_display#1.error(nil, 0, "Surface already has a fifo")` at 13:07:09.445, then
  `vkCreateSwapchainKHR: VK_ERROR_SURFACE_LOST_KHR` (run-nv-paul-repro.out:3076-3078). NVIDIA's EGL window surface had
  already taken the window's wp_fifo_v1 object and the Vulkan swapchain asked for a second. A protocol error ends the
  client's Wayland connection.
- SDL then spun for ever in `Wayland_ShowWindow`, waiting for the first configure on the dead connection
  (SDL_waylandwindow.c:1422-1426; perf of the main thread: pthread_mutex_lock / libc and wl_display_dispatch under
  Wayland_ShowWindow, state R). On Paul's start the window was shown before the swapchain, so SDL quit instead
  (SDL_waylandevents.c:372-385).
- Mesa's EGL takes no fifo object (src/egl/drivers/dri2/platform_wayland.c has none), which is why RADV shares the
  surface.
- Why the earlier repros did not reproduce it: chain36's nv-wl-repro and nv-wl-forced set SDL_VIDEODRIVER, and that
  skips LLWindowSDL's whole native-Wayland block, the PRIME request included. OpenGL, and so the Vulkan device chosen by
  its UUID, was then the Radeon (log: "Device chosen by the OpenGL device UUID: AMD Radeon 890M"). The driver check
  correctly let RADV through, so it did not "fail to fire".

**Run-time failures: stop and quit (r46, Paul's decision 2026-11-09: "we force the user to relog if they switch to
opengl").**
- Background: the in-session Vulkan-to-OpenGL hand-over flashed on a native Wayland window (chain36 amd-wl-forced,
  12:55Z; Paul saw it). A diagnosis repeat (amd-wlfb-diag, 13:01Z, bursts before and after) did not reproduce it,
  so its cause is not proven - but the path is gone.
- What happens now: any failure after the first Vulkan frame calls `stopAtRunTime` (wolfvkbridge.cpp). That covers a
  device lost or hung, a broken hand-over, a swapchain failure, and the `WolfVulkanDebugFallBack` test switch. It:
  - tears Vulkan down;
  - sets and saves `WolfRenderBackend=opengl` (and `WolfVulkanStartOK`) at once;
  - shows one operating-system message box (`OSMessageBox`, llwindow.cpp:72, no renderer needed; strings.xml
    `WolfVulkanStoppedRestart`: "WolfViewer's Vulkan renderer stopped working on your graphics card ([REASON]).
    WolfViewer will use OpenGL from now on. Please restart WolfViewer to carry on.");
  - quits by the normal path (`LLAppViewer::requestQuit`) after OK.
  - The reason is saved in `WolfVulkanStoppedReason` (hidden) with the rest, BEFORE the box, and cleared once the box
    has been seen. The one-button box answers `OSBTN_CANCEL` only when it could not be shown (`OSMessageBoxSDL` when
    `SDL_ShowMessageBox` fails; `OSMessageBoxWin32` when `MessageBoxW` fails). So when no box could be shown, or the
    viewer never came back from it, the next start (`decideBackend`) clears it and shows the `WolfVulkanStoppedLastRun`
    notice on the login screen once (r48-r50).
- From the failure on, `present()` reports the frame as shown, so OpenGL never swaps into the window (State::STOPPED).
  Nothing switches presenters in a live window any more.
- The `WolfVulkanLost` notice is removed. Start-up fall-backs are unchanged: OpenGL in the same start, and the
  `WolfVulkanFellBack` notice.

**The message box on Linux (r48-r50, chain40-43).**
- chain40 amd-r47-wlstop hung in SDL's `waitpid` on zenity. chain42 amd-r49-wlstop showed why the rig could not
  answer it: on a native Wayland window the blocked main thread stops answering the compositor, Hyprland opens its
  "Application Not Responding" dialog (Terminate / Wait), and that dialog took the focus. Return and a pointer click
  did not reach the box. A resident can still click OK. Only a test can reach this case: Vulkan always uses XWayland
  (section 8.2q) unless `SDL_VIDEODRIVER=wayland` is forced from outside.
- chain41/42 showed a second fault: a box that was shown and answered was still reported as failed. r49 logged
  `SDL_ShowMessageBox failed: Waiting on zenity failed: No child processes` (nv-r49-stop; the zenity wrapper logged
  `zenity ended rc 1` at the same second). The cause: `LLVivoxVoiceClient`'s constructor sets SIGCHLD to SIG_IGN
  (llvoicevivox.cpp, "Ignoring SIGCHLD should prevent zombies"). The kernel then reaps zenity itself, so SDL's
  `waitpid` fails (SigIgn mask of the running viewer: 0x11000 = SIGPIPE + SIGCHLD). This affects every OSMessageBox
  on Linux once voice has started, not only Vulkan's.
- Fix (r50, llwindowsdl2.cpp `OSMessageBoxSDL`): SIGCHLD goes back to its default action only while the box is up,
  and the previous action is restored afterwards. `SDL_GetError` is logged whenever the box fails.
- chain43 (r50, XWayland, Return on the box): nv-r50-stop and amd-r50-stop. zenity ended rc 1 at the Return; there
  was no `SDL_ShowMessageBox failed`; the reason was cleared (absent from the saved file); the viewer quit by itself
  within a second; no leftover box or process.
- nv-r48-next (the next start with a saved reason): the `WolfVulkanStoppedLastRun` notice was on the login screen,
  the reason was cleared, and the session reached the world in OpenGL.
- amd-r48-nobox (no X display, zenity failing): no box. The viewer quit within a second with OpenGL and the reason
  saved.

**Native Wayland across monitors of different scale (found, not a Vulkan fault; parked for after the release).**
Paul (2026-11-09): "this has been an issue for a long time, I normally just move the viewer to my laptop screen". It
happens in OpenGL mode with WolfNativeWayland on, which is his normal start. Vulkan mode always uses XWayland.
- Reproduced every time: nv-paul50-gl and nv-wlscale-gl (chain44/45, r50). The window is made at 2560x1600 for the
  1.6-scale panel and placed by Hyprland on a scale-1 monitor (DP-2). The picture filled 0.63 x 0.63 of the window
  (fillcheck.py: 1201x676 of 1920x1080, 589x644 of 941x1030, 876x563 of 1400x900, 688x438 of 1100x700) and the rest
  was edge streaks. This was after the first placement, after both resizes and after re-tiling.
- The viewer side is right. Every resize reached `LLWindowSDL` with matching drawable pixels ("Handling a resize event:
  1400x900 (1400x900 pixels)"), and the GL viewport followed (sky dome viewport 1897x1004 etc.).
- Protocol trace (WAYLAND_DEBUG=client, run-nv-wlscale-gl.out):
  - At the 1.6 start, SDL 2.28.0 made a viewport, `wp_viewport#43.set_source(0,0,4096,2560)` and
    `set_destination(2560,1600)` (SDL_waylandwindow.c ConfigureWindowGeometry, NeedViewport: fractional scale +
    SDL_WINDOW_ALLOW_HIGHDPI).
  - On `preferred_scale(120)` (1.0) it took the integer-scale branch: `wp_viewport#43.destroy()` and buffer scale 1.
  - From then on the compositor drew the buffer as if that old viewport were still there. A 4096-wide source mapped to
    2560 is 0.625; 1920 buffer pixels show as 1200; beyond the buffer the source is clamped, which gives the streaks.
- So the viewport state outlives `wp_viewport.destroy` on this Hyprland (0.56.2). That is a compositor-side cause,
  inferred from the trace and not yet confirmed in Hyprland's source.
- Workarounds to weigh later: keep the viewport and reset it (`set_source(-1,...)` / `set_destination(-1,-1)`)
  instead of destroying it, which needs SDL patched; or start the window on the monitor it will land on.
- XWayland (Vulkan mode) is not affected on the same-scale checks (chain46 below).

**chain44/46 (r50, 2026-11-09 14:51-15:21Z): Paul's start path and the picture filling the window.** Paul's launcher
(tvk/wolfviewer, diffed identical to ~/firestorm/wolfviewer) with a read-only COPY of his settings.xml (mkprof
PROFBASE) and his window keys; nothing forced.
| Run | Result |
|---|---|
| nv-xw-vk / amd-xw-vk (Vulkan) | "Native Wayland not used this start", SDL x11, Vulkan active (RTX 5090 / Radeon 890M, surface X11), in world. Swapchain followed every size. fillcheck 1.00 x 1.00 on every grab: Hyprland's first placement (fullscreen 1920x1080), fullscreen unset (941x1030), resized 1400x900 and 1100x700 (floating), tiled again, moved to the 1.6-scale panel (NV 2522x1520 pixels / AMD 1250x1520) and back to the scale-1 monitor |
| nv-paul50-vk / amd-paul50-vk (Vulkan) | same start lines, in world; fill 1.00 (that run's resizes were refused because the window was fullscreen; fixed in resizefill.py for chain46) |
| nv-paul50-gl (OpenGL, Paul's normal start) | "Wayland GPU: NVIDIA driver present, requesting it (PRIME render offload)", SDL wayland, GL on the RTX 5090, in world, clean quit - as before; picture 0.63 x 0.63 on the scale-1 monitor = the parked fractional-scale issue above |

**chain39 (r46, 2026-11-09 13:07-13:58Z, results).**

| Run | Window | Result |
|---|---|---|
| nv-paul-repro | native Wayland, RTX 5090, check bypassed | the failure reproduced and proven (above); session closed by hand |
| nv-paul-guard | native Wayland, RTX 5090 | the driver check fell back before any Vulkan surface; OpenGL on NVIDIA in the same native Wayland window, into the world, clean quit |
| nv-paul-fix | Paul's own settings, nothing forced | "Native Wayland not used this start", SDL x11, Vulkan active on the RTX 5090, into the world; the window moved by Hyprland 2560x1600 -> 1920x1080 and the picture fills it (shots/nv-paul-fix/world.png) |
| nv-/amd-r46-stop | XWayland | `stopAtRunTime`; the message box blocked the main thread (46 s / 107 s, until OK was pressed on the screen - the rig's box finder missed it, fixed for chain40), then requestQuit and a clean quit |
| amd-/nv-r46-val | XWayland, validation + sync + GPU-AV, resizes + unfocus | 0 VUID, 0 SYNC-HAZARD, no device loss |
| amd-/nv-r46-play | XWayland | settled bursts max 5.1% / 6.2% of pixels per step, all in the moving water/sky/rigging area: no flashing |
| nv-r46-noicd | no Vulkan driver | "Falling back to OpenGL: The Vulkan loader offers no VK_KHR_surface", the WolfVulkanFellBack notice on the login screen, into the world. (The chain's saved-file check printed nothing: its `grep -A4` stops before the `Value` line. `fallBack` sets `opengl`, which is not the default `vulkan`, so it is saved; the stop's saved file was read with a longer grep after chain40: `opengl`.) |
| VR Big Screen | Vulkan with validation and OpenGL, both GPUs | e2e_vrbig PASSED all 4 (world in the picture before and after two resizes); Vulkan against OpenGL 14-18/255 mean (separate sessions: pose and foliage motion; same scene by eye) |

**r47.** macOS CI run 42614243966 (snapshot 433e2208bd) failed at link on `_glCopyImageSubData` (GL 4.3) and
`_glGetNamedBufferSubData` (GL 4.5); Apple's GL is 4.1.
- `WolfVkGBuffer::glRewriteDepth` is not built on LL_DARWIN. Nothing reaches it there: no GL/Vulkan sharing on macOS.
  `mRewriteTexLoc` is reset in destroy so no field goes unused there.
- The pose-check probe reads the vertex through `GL_COPY_READ_BUFFER` and `glGetBufferSubData` (GL 3.1 / 1.5) on every
  platform.
- A grep of all new code for other GL 4.2+ entry points finds only `glTexStorage2D`, inside the excluded
  glRewriteDepth.

### 8.2r VR 3D Mode frame slots, and a worn HUD (r51, 2026-11-09)

**VR 3D Mode crash (release blocker).** Found by the VR agent and reproduced twice at Community Hub; it was also in r50.
- With the desktop paused (WolfVRScreen::displayFrame, WolfVRPauseDesktop), swap() never reached
  `WolfVkBridge::present()`. The frame slot that the eye passes opened (drawPassSimple -> ensureFrameBegun) was
  therefore never submitted, and its descriptor pool (8192 sets, wolfvkshader.cpp) was never reset.
- About 3 s later `VK_ERROR_OUT_OF_POOL_MEMORY` came. `WolfVkScene::drawComposite` then wrote to and bound the failed
  (null) uniform set, and the NVIDIA driver crashed with SIGSEGV.
- Fix:
  - `WolfVkBridge::endUnshownFrame()` is called once per main-loop frame, right after `WolfVRScreen::displayFrame()`
    (llappviewer.cpp). A slot still open there is closed the way a frame without a swapchain image is
    (`closeFrameWithoutImage`: GL semaphores balanced, frame fence handed to the queue). Every frame, shown or not,
    now recycles its slot: pools, uniform ring, deferred destructions. This covers the paused desktop, the left eye
    and a snapshot's render.
  - The composite draws only when both of its sets were allocated; otherwise the draw is skipped. The scene passes
    already skipped on a failed bind (wolfvkscene.cpp drawPass).

**A worn HUD in Vulkan mode.** The world layer used to be taken before the HUD attachments, so the HUD was drawn onto
the cleared UI layer:
- `renderGeomPostDeferred` turns alpha writes off (`setColorMask(true, false)`).
- Opaque HUD faces carry their shininess as alpha (llface.cpp, SHININESS_TO_ALPHA).
- So the composite `ui.rgb + world.rgb * (1 - ui.a)` added the HUD onto the world instead of covering it.

Now, in Vulkan mode, `WolfVkBridge::captureWorld` (and `keepWorldForBigScreen`) run after `render_hud_attachments`
(llviewerdisplay.cpp render_ui). OpenGL draws the HUD straight over the world in the default framebuffer, exactly as
in OpenGL mode, and only the 2D/3D UI goes onto the UI layer, still with the VR agent's coverage rule
(`wolf_set_ui_coverage`). Passes that skip the UI (the left eye, a paused desktop) take the world layer in their own
branch. The one exception is the Vulkan scene layer, which the composite lays between world and UI: it is the
debug-only `WolfVulkanOverlayFullbright` (off by default) and could cover a HUD.

**chain47 (r51, 2026-11-09 18:11-19:04Z).** The fake headset (e2e_vrsoak.mjs) was held in VR for 10 min per run,
Vulkan with validation (sync + GPU-AV):

| Run | Result |
|---|---|
| nv-r51-vr3d (VR 3D, desktop paused) | PASSED: 19125 pictures, ~30 pictures a second throughout; 0 VUID, 0 SYNC-HAZARD, 0 OUT_OF_POOL, no stop, no crash, clean quit; viewer VR 3D stereo 29.6 frames/s |
| amd-r51-vr3d | PASSED: pictures every 30 s (445; the headless browser's intake on that run, the viewer logged 41 stereo frames/s); 0 VUID / SYNC / OUT_OF_POOL, clean quit |
| nv-r51-vrbig (Big Screen) | PASSED: 7980 pictures; 0 VUID / SYNC / OUT_OF_POOL, clean quit. The desktop rate fell from ~21 to ~4 frames/s after 7 min (swap/present wait 30 -> 80 ms and display CPU time doubled together); cause not proven (laptop at Tctl 87 C afterwards, GPU-AV on) |
| amd-r51-vrbig | PASSED: 17096 pictures, steady ~28 frames/s; 0 VUID / SYNC / OUT_OF_POOL, clean quit |
| nv-r51-hud-vulkan / -opengl | Shadow Testwolf's own worn HUD (top right, "HUD objects count: 1"): Vulkan against OpenGL over that HUD's area, mean difference 0.5/255. The extra cube the campaign tried to rez and attach did not appear in either run (no rez seen) |

### 8.2s Pure NVIDIA X11: `vkCreateSwapchainKHR` refused with VK_ERROR_INITIALIZATION_FAILED (ticket 724, 2026-11-10)

**The report** (Shy Morrigan, W101 7.2.4.101, Linux Mint 22.2 XFCE, X11 session, RTX 4060 Ti 16 GB, driver 580.178.04,
Vulkan 1.4.312). Instance, device by GL UUID, X11 surface (`wolfvksurface.cpp:154`), allocator, logical device (queue
family 0, two frames in flight, timeline semaphores) all succeed; then `fallBack: vkCreateSwapchainKHR:
VK_ERROR_INITIALIZATION_FAILED` and OpenGL carries on. `vkcube` works on the same desktop with the system loader AND
with the viewer's bundled loader (`LD_LIBRARY_PATH=/opt/wolfviewer/lib vkcube`). Her full `vulkaninfo` and the log lines
before the fall-back were asked for on the ticket; not yet received when this was written.

**What is different between vkcube and the viewer on that machine, from the code.**
- The viewer's X window is made with `SDL_WINDOW_OPENGL` (llwindowsdl2.cpp:848) and the GL context is created and made
  current on it (`SDL_GL_CreateContext(mWindow)` :935, `SDL_GL_MakeCurrent(mWindow, ...)` :3575/:3612): the window
  carries a GLX drawable. `WolfVkSurface::create` then made the Vulkan surface on THAT window (`handles.mWindow` from
  `SDL_GetWindowWMInfo`, llwindowsdl2.cpp:3345), and the swapchain on it. vkcube's window is a plain X window of its
  own (Vulkan-Tools cube.c:3006-3007 `xcb_create_window(... XCB_COPY_FROM_PARENT ... XCB_WINDOW_CLASS_INPUT_OUTPUT,
  screen->root_visual ...)`, mapped at :3031, swapchain after it at :5025) with no GLX drawable.
- The Vulkan specification (vkCreateSwapchainKHR refpage, docs.vulkan.org): "If the native window referred to by
  pCreateInfo->surface is already associated with a non-Vulkan graphics API surface, VK_ERROR_NATIVE_WINDOW_IN_USE_KHR
  must be returned", and "The native window ... must not become associated with a non-Vulkan graphics API surface
  before all associated Vulkan swapchains have been destroyed". A GLX drawable is such a surface. Mesa's X11 WSI keeps
  no such book (it shares happily: every run through XWayland on the Radeon, and the NVIDIA runs through XWayland +
  PRIME render offload on this laptop, section 8). NVIDIA on native Wayland refused the same sharing with its EGL
  window surface (8.2q, proven by the protocol trace). On a pure X11 session the NVIDIA X driver and the NVIDIA GLX
  server module own the window; the error code is the driver's choice (the Vulkan-Docs issue 828 thread records
  NVIDIA answering VK_ERROR_INITIALIZATION_FAILED from `vkCreateSwapchainKHR` for conditions the spec words
  differently).
- Nothing in the request itself is outside the capabilities: the format is one of the surface's own with
  SRGB_NONLINEAR (wolfvkswapchain.cpp build), the present mode comes from the surface's list (FIFO otherwise),
  minImageCount is clamped to maxImageCount, usage adds only bits in supportedUsageFlags, compositeAlpha opaque only
  when offered, preTransform identity only when offered, pNext is empty (the present fence goes on vkQueuePresentKHR,
  and only when `swapchainMaintenance1` was enabled), and the extent is the surface's currentExtent on X11.
- Not reproducible here: this laptop runs Hyprland (Wayland); its X windows are XWayland's and NVIDIA runs by PRIME
  render offload, the path that works. Xephyr/Xvfb would not have the NVIDIA GLX path. A pure NVIDIA X11 session was
  not available. So the cause is the strongest reading of the code and the spec, not a proven one; the diagnostics
  below are there so the next report settles it.

**What W103 does.**
- `WolfVkSurface::create` on X11 makes a child X window of the viewer's window (`XCreateWindow`, depth and visual
  CopyFromParent, InputOutput, background black like vkcube's, NoEventMask), maps it, and makes the Vulkan surface on
  it. GLX never touches that window. Pointer events in the child propagate to the viewer's window, which selected them
  (SDL_x11window.c:674), with coordinates relative to it; the keyboard goes to the focused top-level as before; the
  cursor is inherited. SDL selects StructureNotify, not SubstructureNotify, on its window, so the child's own Map/
  ConfigureNotify never reach SDL. OpenGL keeps drawing into the viewer's window's own back buffer (never swapped in
  Vulkan mode) and the UI / read-back layers are taken from GL_BACK as before (wolfvkshare.cpp:767-772,
  wolfvkscene.cpp:453/543). The one thing the child changes for OpenGL is that the parent's pixels under it are no
  longer the parent's on screen; what `glReadPixels(GL_BACK)` returns there is OpenGL's pixel-ownership question. The
  back buffer is the GLX window's own on both drivers in use (NVIDIA's shared-back-buffer mode, UBB, exists only on the
  professional RTX/Quadro boards, README "Flipping and UBB": "When UBB is enabled, all windows share the same back,
  stencil and depth buffers"; Mesa's DRI3 drawables carry their own buffers), so the layers are expected unchanged;
  the runs below are the check (a wrong UI layer would show at once).
- `WolfVkSurface::resize(w, h)` (XMoveResizeWindow + XSync) runs in `createSwapchain` before every build, so the
  child's size, and with it `currentExtent`, is the viewer's drawable size at the moment of the build.
- If the child cannot be made (an X error, caught synchronously by a temporary error handler), the surface goes on
  the viewer's window as before, with a WARNING.
- `WolfVulkanDebugSharedWindow` (hidden, off): the W101/W102 way, for comparison on a reporting machine.
- `WolfVkSwapchain::build` logs ONE WARNING block when `vkCreateSwapchainKHR` fails: every field asked for (count,
  format, colour space, extent, layers, usage, sharing, preTransform, compositeAlpha, present mode, clipped,
  oldSwapchain, pNext), the window size given, and the surface's capabilities, formats and present modes; the bridge
  adds the X windows' depth, size and map state (`WolfVkSurface::describe`).
- The W102 fall-back guard is unchanged: a refused swapchain still means OpenGL for that start, with the notice.

**Tested here (XWayland, the same code path; chain-t724, 2026-11-10 11:08Z onwards).** Build from the W102 tree, clang
macOS-flag check and -Wconversion clean on the three changed files, gdb break-at-main start-up check passed.
- nv-t724-child (RTX 5090 by PRIME offload, Vulkan): "Window surface created (X11): viewer's window 0xe0000a depth 24
  2522x1520 unmapped; Vulkan child window 0xe000f2 depth 24 2522x1520 mapped, parent unmapped" (the viewer's window is
  still hidden under the splash at that point, as always), "Vulkan renderer active", the login screen drawn and
  presented at ~114 frames/s, the swapchain followed Hyprland's placement (2522x1520 -> 618x366, 3 images,
  B8G8R8A8_UNORM, immediate), no fall-back, no X protocol error. The login, the in-world shots and the resize/fill
  checks (resizefill.py + fillcheck.py), then amd-t724-child (Radeon 890M, RADV) and the nv-t724-shared control
  (WolfVulkanDebugSharedWindow=1) were still running when this was written: results in ~/.cache/vkbackend/chain.log,
  log-*-t724-*.log and shots/*-t724-*/ (to be copied here).
- Not tested anywhere: a pure NVIDIA X11 session (the reporting machine). The child window is the spec-backed change;
  whether that driver accepts it is for the ticket's next run, and the WARNING block tells the rest if it does not.

### 8.3 Validation layer

Round 1 (NVIDIA + AMD, Khronos validation layer): VUID-vkQueueSubmit-pSignalSemaphores-00067 (one render-done
semaphore reused across swapchain images), VUID-VkGraphicsPipelineCreateInfo-Input-08733 (texture_index attribute
R32_UINT against `in int`), VUID-vkDestroyImageView-imageView-01026 and VUID-vkDestroyBuffer-buffer-00922 (objects
destroyed while a submitted command buffer still read them), VUID-vkDestroyDevice-device-05137 (5 objects leaked at
shutdown: the fullbright program's two descriptor set layouts, pipeline layout, two shader modules). All five fixed in
source; round 2 (NVIDIA, validation on, 4 min in world) was clean of 00067, 08733 and 05137 and still showed 20 x 00922
and 18 x 01026 during the first minute after login. Cause found by reading `WolfVkDevice::deferDestroy`: `endFrame`
advances the frame slot, so a destroy raised between two frames - where every destroy raised by the GL side of the
frame lands (`LLVertexBuffer`/`LLImageGL` hooks) - was filed against the NEXT slot, whose fence belongs to the frame
before last, not the frame just submitted. Fixed: deferred work is filed against the last submitted frame's slot (the
current one while recording); `abandonFrame` closes an unsubmitted frame. To be re-measured in the next validation run.


### 8.2t Windows: the first Vulkan run ever, and the world was black (W102, 2026-11-10)

**The report** (Omniwright Mooncastle, Windows, AVX2 installer, W102 "Vulcan's Forge" e702c1c258 - the first time the
Vulkan renderer ran on Windows at all). With Vulkan on, the whole 3D view is black at every region; the UI, the mini-map
and the title bar draw; one white dot in the middle of the window. Cache clear, relog, other regions: black. V off and
a restart: the world is back. The viewer noticed nothing, so no safety net fired, and W102 had just switched every
W101 OpenGL fall-back back to Vulkan once. No Windows log had been received when this was written. Screenshot in the
coordinator's session (10.png). Branch `winvk-black` (worktree `wolfviewer-winvk`), on top of T724's commit (8.2s).

**What was ruled out, with evidence.**
- *The SPIR-V keys differ on Windows (CRLF, backslashes, a different stitch).* No. The Windows AVX2 installer of W102
  and the Linux bundle were unpacked side by side: 18766 `manifest.json` keys each, the same set (0 only-Windows, 0
  only-Linux); 12043 `.spv` files each with identical names; 5122 complete vert+frag pairs each, the same pairs. The
  CI logs of run 42769827477 say "5207 of 7254 ... 18766 recipe keys" on both the Windows and the Linux job. A key is
  the MD5 of recipe strings `stage|file|class|#version|defines|channels|amd|oldselect` (scripts/shader_vulkan.py:87-95
  = wolfvkshader.cpp:159-193) built from C++ string literals ("deferred/diffuseV.glsl"), never from file contents or
  paths; `.gitattributes` has `* text eol=lf` besides. And a program without SPIR-V only makes Vulkan draw nothing of
  it: OpenGL draws every batch itself (lldrawpoolsimple.cpp:109-113 ignores drawPassSimple's result; pushBatches skips
  only batches in mDrawnByVulkan), so the world would still be there.
- *The composite, the swapchain, the surface, the loader, the plain RGBA8 import.* All proven on that PC by the UI
  itself: the UI layer is the same mechanism as the world layer (a blit from GL's back buffer into a shared RGBA8
  image imported with GL_EXT_memory_object_win32, sampled by the composite, presented through the swapchain), and
  the login screen goes the same way.

**What is left, ranked** (none provable from the source - the Windows interop paths had never run, 8.2n "Still
unproven: Windows NVIDIA/AMD/Intel GL+Vulkan interop"):
1. **The shared G-buffer.** In Vulkan mode `LLRenderTarget` takes deferredScreen's four colour attachments and the depth
   from Vulkan memory (wolfvkgbuffer.cpp colorTexture/depthTexture -> WolfVkSharedImage::create: glImportMemoryWin32HandleEXT
   + glTexStorageMem2DEXT, OPAQUE_WIN32 handles, dedicated allocation). OpenGL then draws the WHOLE deferred frame
   into those (RGBA8 x2, RGBA16, RGBA16F, D32F, depth-tested), Vulkan draws PASS_SIMPLE into them mid-frame, and OpenGL
   rewrites the depth (glCopyImageSubData + a gl_FragDepth pass). The lighting pass samples that G-buffer; in the PBR
   deferred path the sky is in it too. A G-buffer OpenGL cannot read back after writing gives exactly "everything black
   including the sky, UI fine". The world differs from the UI only in rendering INTO imported attachments, the other
   formats, the Vulkan pass and the depth rewrite.
2. **One HWND for WGL and the Vulkan swapchain.** The Win32 surface was made on the viewer's own window, which carries
   WGL's pixel format and context - the same spec violation as 8.2s on X11 (vkCreateSwapchainKHR: a native window
   "already associated with a non-Vulkan graphics API surface"). On Windows the swapchain was accepted (the UI shows
   through it), so the question is what two presenters on one HWND do to GL's back buffer; the UI being right argues
   against this being the black world's cause, but it is wrong by the spec either way.
3. The world layer's hand-over itself (same as the working UI layer; the only extra is the depth copy pass, which
   writes no colour). The white dot is unexplained; nothing here guesses at it.

**Found by running the W102 Windows binary under Wine (winevulkan + Wine's opengl32 on the NVIDIA Linux driver;
login screen only): a HALF-shared G-buffer in vintage mode.** The run itself: Vulkan started, Win32 surface, swapchain,
the shared layers (Wine's opengl32 exposes GL_EXT_memory_object_win32 and the import raised no GL error), the login
screen presented - so the Windows build's own paths (loader, keys, the composite SPIR-V, the Win32 import calls) work
as code. But its GPU benchmark under Wine gave Class1 -> graphics level 1 -> `RenderDisableVintageMode 0`
(featuretable.txt:140/185/411-459 for Low/LowMid/Mid) -> `handleDisableVintageMode` sets `RenderHDREnabled` and
`RenderEnableEmissiveBuffer` off (llviewercontrol.cpp:366-369) -> `addDeferredAttachments` (pipeline.cpp:394-420) asks for a
`GL_RGB10_A2` normal attachment and no emissive one. W102's provider knew only `GL_RGBA16` at index 2 and logged
"G-buffer attachment 2 asked for GL format 0x8059, not the one this provider shares; left to GL": diffuse, orm and the
depth over Vulkan memory, the normal OpenGL's own, and no Vulkan pass ever (`prepare()` wanted four). Every resident on
graphics quality Low, LowMid or Mid is in that state, on every platform; no Linux test ran below level 6. Whether this
is Omniwright's exact fault depends on his graphics level (unknown until his log); it is real either way. W103
([VINTAGE G-BUFFER], wolfvkgbuffer.cpp): the slot format follows the request (RGBA8 / RGBA16 / RGB10_A2 ->
`A2B10G10R10_UNORM_PACK32` / RGBA16F, all five checked exportable at init), the Vulkan target takes three or four colour
attachments (`colorCount()`), the pass semaphores and dumps name only the attachments that exist, and a format the
provider cannot share ends the sharing for the session instead of leaving a hole. The self-test decodes the packed word.

**What W103 does (all shipped whatever the cause turns out to be).**
- *Win32 child window for the surface* ([WIN32 CHILD], wolfvksurface.cpp, the twin of 8.2s): a `WS_CHILD | WS_VISIBLE`
  window of class `WolfVulkanSurface` over the viewer's client area, no pixel format, no GL context; its window
  procedure answers WM_NCHITTEST with HTTRANSPARENT (the mouse goes to the viewer's window beneath, same thread, as
  without the child), WM_ERASEBKGND 1 and WM_PAINT validate-only (nothing of GDI's). It is created, moved and
  destroyed on the viewer's window thread (`NativeHandles::mPostToWindowThread` -> `LLWindowWin32Thread::Post`), the
  main thread waiting at most 5 s for the creation; `WM_SIZE` on the parent sizes the child on that thread
  (llwindowwin32.cpp, found by class name) before the main thread hears of the resize, and `WolfVkSurface::resize`
  posts the size again before every swapchain build. If the child cannot be made the surface goes on the viewer's
  window as W102 did. `WolfVulkanDebugSharedWindow` (hidden) keeps the W102 way for comparison. Known risks, untested
  here: OLE drag-and-drop onto the 3D view (the drop target is the parent; WindowFromPoint skips HTTRANSPARENT
  windows by the documented behaviour, unverified here), DPI changes, and `recreateWindow` (fullscreen/vsync
  switches), which destroys the parent and so the child - the surface was lost there before this change too.
- *Shared-image round-trip self-test* ([IMPORT SELF-TEST], `WolfVulkanShareSelfTest`, default on): every image shared
  with OpenGL makes one round trip before it is used. OpenGL clears the imported texture to a known value (colour
  0.25/0.5/0.75/1.0, depth 0.25) through a framebuffer of its own and signals; Vulkan waits, acquires the image from
  VK_QUEUE_FAMILY_EXTERNAL, copies the 4x4 block at its centre to a host buffer, releases it and signals; OpenGL
  waits; the texels are decoded by format (UNORM8, UNORM16, half, float) and compared. The world and depth layers are
  tested at `WolfVkShare::init` (a failure = the read-back hand-over for the session, and then the G-buffer is not
  shared either); each G-buffer attachment is tested as it is created (a failure ends the sharing: that image goes,
  the ones already handed to the target stay until it releases them, no Vulkan pass runs). Log lines "Shared layer
  self-test: ..." / "G-buffer self-test: ...", "... FAILED ..." on failure. OpenGL's writes not reaching Vulkan is
  the fault class candidate 1 belongs to; this test fires before any black frame.
- *World check* ([BLACK WORLD], `WolfVulkanBlackWorldCheck`, default on; wolfvkscene.cpp WorldProbe + wolfvkbridge.cpp
  blackCheck): once a second for the first 90 s in-world, OpenGL blits the world view rect of its finished frame
  (at captureWorld, before the UI) to 32x32 and reads it back through a PBO two probes later (no GPU wait), and with
  the shared hand-over Vulkan blits its view of the shared world layer in the same frame's command buffer (after
  the acquire) to 32x32 and copies it to a per-slot host buffer read when the slot comes round. "Black" = the
  largest R/G/B of all 1024 points is exactly 0. Decisions: (A) OpenGL lit, Vulkan black, three probes running -> the
  import does not carry the picture -> `stopAtRunTime` ("Vulkan drew nothing on this computer: the picture OpenGL
  drew did not reach Vulkan through the shared image"). (B) OpenGL black three probes running -> stage 1: OpenGL draws
  the simple pass itself and no depth rewrite runs (`sGBufferGLOnly`); lit three running -> stop ("its pass into the
  G-buffer shared with OpenGL blanked the picture"); still black -> stage 2: the G-buffer is given back to OpenGL
  (`unshareGBuffer`: the target remade with its own textures at the next frame, the provider deleted once the
  target released the images); lit three running -> stop ("the G-buffer shared between OpenGL and Vulkan came back
  black"); still black -> a dark scene: the normal mode is restored (`shareGBuffer` again) and nothing fires. Ten lit
  probes running end the check early ("passed"). The stop is the existing run-time stop (8.2q: message box, OpenGL
  saved for the next start, quit). A black scene with the G-buffer not shared is not judged at all (nothing of
  Vulkan's is in OpenGL's frame then). Log tag `WolfVulkanCheck`: "World check started", "World probe N: GL max R/G/B
  .. (.. of 1024 points lit), Vulkan's view max ..", "World check passed/ended/FAILED: ...".
- *Logging*: "Sharing extensions: ..." (every GL/Vulkan sharing extension yes/NO + the GL renderer), "Shared image
  imported into OpenGL: texture N over memory object M (...)" per import, "SPIR-V store: N .spv files in <dir>,
  manifest.json <bytes>" at start, and "Vulkan session summary (teardown/shutdown): SPIR-V <made/missing>; hand-over
  ...; G-buffer ...; world check ..." at the end. `reportFields()` gains `vulkan_self_test`, `vulkan_world_check`,
  `vulkan_spirv` (viewer stats, crash and bug reports).

**Tested here.** Linux build (reflinked W102 tree), clang macOS-flag check and -Wconversion on the changed files, gdb
start-up check, and a Linux session (NVIDIA, shared layers + shared G-buffer) proving the self-tests pass and the world
check ends "passed" without firing. The Windows binary was run under Wine (winevulkan + Wine's opengl32 on the NVIDIA
Linux driver): that exercises the Windows build's own code paths (keys, paths, the Win32 child window, the read-back
hand-over where Wine's GL lacks GL_EXT_memory_object_win32) but not a Windows driver. The real Windows test is the TEST
installer (/downloads/wolfviewer/test/WolfViewer-vulkan-test-windows64-AVX2-Setup.exe) on a Windows PC with a real GPU
and Vulkan driver (a GPU-less VM never starts Vulkan and proves nothing); the results per machine are the log lines above.

## 8.5 Design for the next pass: `PASS_SIMPLE` into the G-buffer (stage 4 proper)

Decided 2026-11-08 after the shared layers worked; the pieces, in the order they are built:

1. **The G-buffer shared.** `mRT->deferredScreen` (pipeline.cpp allocateScreenBufferInternal: `GL_RGBA` + the three of
   `addDeferredAttachments`: `GL_RGBA` orm, `GL_RGBA16` normal, `GL_RGB16F` emissive; depth `GL_DEPTH_COMPONENT24`) gets
   its textures from Vulkan images imported into GL, through a provider hook in `LLRenderTarget`
   (`LLRenderTarget::sWolfVkProvider`, used by `allocate` / `addColorAttachment` / `allocateDepth` / `release` when
   the target is marked shared). Two format changes for sharing, both invisible to the viewer's shaders: the depth
   becomes `GL_DEPTH_COMPONENT32F` (= `D32_SFLOAT`; RADV has no D24, vkglshare) and the emissive buffer
   `GL_RGBA16F` (three-channel 16-bit images are not exportable; the alpha is unused). `mRT->screen` shares that depth
   as before (`shareDepthBuffer`).
2. **Several submits per frame.** `WolfVkDevice::Frame` gets mid-frame command buffers from the slot's pool
   (`beginMidFrame` / `submitMidFrame`): each mid-frame submit signals the timeline with the next value, so
   `beginFrame`'s wait on the slot's last value still covers everything the slot did.
3. **The pass.** `LLDrawPoolSimple::renderDeferred` (lldrawpoolsimple.cpp:99-111: `LLGLDisable blend`,
   `gDeferredDiffuseProgram`, `pushBatches(PASS_SIMPLE, true, true)`) asks `WolfVkBridge::renderPassSimple()` when the
   Vulkan renderer is active: GL signals `gl_pass_done` (textures: the five G-buffer images, layouts
   COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT), Vulkan acquires them from VK_QUEUE_FAMILY_EXTERNAL, draws the
   `PASS_SIMPLE` draw infos with `gDeferredDiffuseProgram`'s SPIR-V (`diffuseV.glsl` + `diffuseIndexedF.glsl`, four
   `frag_data` outputs at locations 0-3; the `wolf_coverage` output at location 4 has no attachment and is discarded)
   into a `WolfVkRenderTarget` over the shared G-buffer (LOAD everything: GL cleared it), with GL's state for that pass
   (blend off, cull back, depth LEQUAL + write, colour mask all channels, `setColorMask(true, true)` at
   pipeline.cpp:4352), releases the images back and signals `vk_pass_done`; GL waits `vk_pass_done` on its queue and
   carries on with the next pool. Nothing waits on the CPU; the rigged half (`PASS_SIMPLE_RIGGED`) stays on GL in
   this step.
4. **Picture check.** The same harness (`WolfVulkanDebugTint` tints what Vulkan drew; `WolfVulkanShowSceneOnly`
   shows the Vulkan layer alone), and for the G-buffer a per-attachment compare: `WolfVulkanDebugDumpGBuffer` writes
   the five shared attachments after the simple pass (`gbuf_<n>_<w>x<h>_<bpt>.raw` in the logs folder; the test
   sessions keep them in `~/.cache/vkbackend/gbuf-<tag>/`), once with Vulkan drawing the pass and once with
   `WolfVulkanDebugGBufferGLOnly` (GL draws it into the same shared buffers); `~/.cache/vkbackend/gbufdiff.py A B`
   prints, per attachment, the fraction of texels differing and writes difference images - the normal buffer first
   (any winding or matrix slip shows there first).

## 9a. Release readiness (r43, 2026-11-09)

What a resident gets, and what is proven, item by item.

**Settings.**
- Defaults: `WolfRenderBackend` opengl, `WolfVulkanShareGBuffer` on, `WolfVulkanGLDepthRewrite` on, every debug or
  test switch off.
- Hidden from the Debug Settings editor (`HideFromEditor`, llcontrol.cpp:1140) are 17 settings: `WolfVulkanStartOK`
  (internal), every `WolfVulkanDebug*`, `WolfVulkanShowSceneOnly`, `WolfVulkanOverlayFullbright`,
  `WolfVulkanPresentMode`, `WolfLogFrameRateOnly`, `WolfDebugGLDepth32F`.
- Still shown, as support switches: `WolfRenderBackend`, `WolfVulkanValidation` (also in the floater), `WolfVulkanDevice`,
  `WolfVulkanGLDepthRewrite`, `WolfVulkanShareGBuffer`, `WolfLogFrameRate`.
- The test rig still sets hidden settings through LEAP and the profile.

**Logging.**
- With the defaults the Vulkan code logs only at start, at fall-back and when images are made (window resizes); nothing
  per frame.
- Every per-second line needs `WolfLogFrameRate`. With `WolfLogFrameRateOnly` only the frame-rate line remains (r43: the
  per-second Vulkan lines and the breakdown are now gated by `WolfVkBridge::timingOn`).
- Not ours: a test-profile session also logs ~1,200 `WolfEnvTrace` lines (wolfenvironmentrender.cpp:2006, the sky's own
  trace).

**Falling back to OpenGL.**
- At start: a missing or failed instance/device/surface/swapchain, or a previous Vulkan start that never reached the login
  screen, starts the viewer on OpenGL, sets `WolfRenderBackend` back to opengl and shows `WolfVulkanFellBack` /
  `WolfVulkanFellBackLastStart`.
- While running: device lost, a broken hand-over, or a bounded wait running out shows `WolfVulkanLost`, and the session
  carries on in OpenGL (8.2k-8.2m).
- Proof run: chain35 part 4, no Vulkan driver (`VK_ICD_FILENAMES` / `VK_DRIVER_FILES` -> /nonexistent).

**Texts.** All in British English and plain words: the notices (notifications.xml `WolfVulkan*`), the window status
lines (strings.xml `WolfVulkanStatus*`, including `WolfVulkanStatusActiveAMD` for `WolfVulkanShareGBuffer` auto) and the V
button's tooltips.

**Shader check (r43).** 7,246 programs linked, 0 failed; only the two known Sky Dome entries (samplerCubeArray below
GL 4.0).

**-Wconversion / -Wsign-conversion / -Wshadow=local.**
- All 16 Vulkan files (llrender/vk, newview/wolfvk*) are clean.
- Every line this branch changed in the other files is clean too; the warnings left there are upstream lines this branch
  did not change.
- That covers the MSVC C4267 class of fault (W87) as far as GCC can see it; MSVC itself only runs in CI.

**Windows and macOS: not compiled here.**
- `.github/workflows/wolfviewer.yml` builds all platforms on a push to `wolfviewer`, or one platform by
  `workflow_dispatch` (platform windows64 / macos64 / linux...).
- A compile check of this branch needs it committed and pushed to a branch, then
  `gh workflow run wolfviewer.yml --ref <branch> -f platform=windows64` (and macos64). That needs Paul's go (no commit or
  push without it).
- Windows-only code in this branch: the win32 handle import (wolfvkshare.cpp, wolfvkdevice exportMemory) and
  `GetThreadTimes` (wolfvkbridge.cpp loop timers). macOS: wolfvksurface_macosx.mm and the read-back hand-over.
- **First CI check (Paul's OK, 2026-11-09).** A snapshot (3544adab0c) was pushed to branch vulkan-ci-check: Windows run
  42595764440 and macOS run 42595767793 both failed.
  - Windows: `LLWindow::NativeHandles::Kind` had an enumerator named `WIN32`, which is a predefined macro under MSVC
    (C2143). The enumerators are now `KIND_NONE/X11/WAYLAND/WIN32/COCOA` (llwindow.h, llwindowsdl2.cpp,
    llwindowwin32.cpp, llwindowmacosx.cpp, wolfvksurface.cpp).
  - macOS: the vendored Vulkan-Headers lacked `include/vk_video/`, which vulkan_core.h includes (:9805). All twelve
    files from tag v1.4.363 (the vendored vulkan_core.h's version; byte-identical to that tag) now sit in
    `third_party/vulkan-headers/include/vk_video/`. `include/` is already the include directory (Vulkan.cmake).
- **Macro sweep for the next CI run.** Every identifier in this branch's added code (comments and strings removed)
  was matched against the 157,708 macro names `#define`d in Wine's Windows headers (/usr/include/wine/windows,
  msvcrt) and a list of known macOS names.
  - Hits: `INT` (wintab.h `#define INT(x)`), in `WolfVkSpirv::BaseType`. Its enumerators became
    `Unknown/Bool/Int/...`, which broke Linux CI (run 42603921168: Xlib.h `#define Bool int`); the coordinator made them
    `kUnknown/kBool/kInt/kUInt/kFloat/kDouble`; `GetCurrentThread`, deliberately the Win32 call; `min`/`max` (NOMINMAX is
    defined, 00-Common.cmake:90); standard names (NULL, bool, true/false, UINT32_MAX, isfinite).
  - No `near`/`far`/`interface`/`ERROR`/`TRANSPARENT`/`OPAQUE` in code.
  - Objective-C `id`, `nil`, `YES` appear only in .cpp parameter names (`id`, not compiled as Objective-C) and in the
    .mm file where they are meant.
- **Second sweep, against the real Linux header set.** `g++ -E -dM` of X11/Xlib.h, Xutil.h, extensions/Xrandr.h,
  SDL2/SDL.h + SDL_syswm.h (X11 on), GL/gl.h, glext.h, glx.h, dlfcn.h, time.h and unistd.h gives 15,043 macros. The
  identifiers in this branch's code that match them are only those meant: `CLOCK_THREAD_CPUTIME_ID`, `RTLD_NOW`,
  `RTLD_LOCAL`, `NULL`, `UINT32_MAX`.
  - Still prefixed, for safety: `WolfVkBridge::LoopStage` (`INPUT, IDLE, DISPLAY, SWAP, SLEEP, COUNT`; `COUNT` is a
    macro in X11/extensions/XI.h, and the others are common platform names) is now
    `kInput/kIdle/kDisplay/kSwap/kSleep/kCount`.
  - macOS: no SDK on this machine. Names checked by hand: `check`/`verify`/`require` (AssertMacros.h) occur only in
    comments; `nil`/`YES` only in wolfvksurface_macosx.mm, where they are meant; `id` only as a parameter name in C++
    files.
- **MSVC builds with /W3 /WX** (00-Common.cmake:101/128), so level-3 warnings such as C4267 and C4244 are errors.
  GCC -Wconversion/-Wsign-conversion is clean on all 16 Vulkan files, which covers those conversions as far as GCC
  sees them. MSVC itself is only proved by the CI re-run.

**Memory, leaks, quitting.**
- chain35 part 5: 10 minutes in world in Vulkan per GPU with `memsample.sh` (RSS; whole-card VRAM/GTT on the Radeon,
  whole-card memory on NVIDIA), then quit and start again.
- With validation on, any object left at `vkDestroyDevice` is reported by the layer and counted ("not destroyed").
- The viewer cannot log out to the login screen and back (no relog in this viewer), so "relog" means quit and restart,
  and the profile is fresh for each session.

**Proven before r43:**
- AMD validation (GPU-AV + sync + resizes + unfocus): 0 VUIDs.
- NVIDIA tour with validation: 0 VUIDs, no device loss.
- abcam on both GPUs: identical pictures (8.2o).
- The r41 flush fix.

**Untested until chain35 runs:**
- Big Screen in Vulkan mode.
- The start-up fall-back without a driver.
- The soak and the relog.
- The r43 fps pair.

## 9. What is next (in order)

1. **Vulkan into the G-buffer** (stage 4 proper): the shared-layer machinery of `wolfvkshare.cpp` applied to
   `mRT->deferredScreen`'s colour attachments and depth (D32F for the shared target), so Vulkan draws BEFORE GL's lighting
   and post-processing and its surfaces get the viewer's exposure / tone map exactly as GL's do (the one declared
   difference left in section 5).
2. **More of the geometry on Vulkan**: the opaque deferred pools (`PASS_SIMPLE`, materials, PBR, terrain, avatars)
   into the shared G-buffer, with the same mirror -> shared-resource path for vertex buffers and textures.
3. **Culling on the card** (stage 3/5): when the Vulkan path takes over culling, treat the reflection probes as a
   first-class cost: the stage-3 measurements (2026-11-08) showed `LLReflectionMap::doOcclusion` issues about 250-300
   occlusion queries per frame (one per probe per frame) on top of the octree groups' ~3,800.
4. **Windows and macOS builds** of this branch on the CI runners (the paths are written, not compiled here), MoltenVK
   bundling and signing, triangle fans converted to lists for Metal.
5. **Geometry-shader programs** in the Vulkan transform (one stage pair more in `shader_vulkan.py`), `inv_proj` /
   `inv_modelview` corrected for the Vulkan clip space when a shader that reads them moves over.
