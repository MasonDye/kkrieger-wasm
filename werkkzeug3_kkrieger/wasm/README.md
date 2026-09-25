# .kkrieger → WebAssembly / WebGL2

A port of the `player_kkrieger` build to the browser. Everything platform
specific in the original lives in one class (`sSystem_`, `_start.cpp`:
Win32 + Direct3D 9 + DirectSound); this directory replaces that file and the
few x86-only corners of the otherwise portable engine.

## Layout

| file | role |
|---|---|
| `build.sh` | emscripten build (`source ~/emsdk/emsdk_env.sh` first). Incremental; `build.sh clean` rebuilds. Output in `dist/`; `KK_RELEASE=1 bash wasm/build.sh` builds `-O3` without debug info or assertions into `dist_release/` (1.30 MB wasm + 216 KB js + 250 KB data incl. both .kx files). |
| `build_headless.sh` | same game logic for node with GL stubbed and AddressSanitizer; drives the intro → menu → level sequence automatically. `node dist_headless/kk_headless.js`. |
| `serve.py` | dev server on :8766 (`python3 serve.py 8767 dist_release` for the release build) that never caches (`--preload-file` assets are otherwise cached by Chrome). |
| `cdp.js` | headless-chromium driver over CDP (SwiftShader WebGL2): runs a step list (`start`, `key:Return`, `down:w`, `shot:/tmp/x.png`, `log:<regex>`, `evalfile:x.js`) and prints the page log and every WebGL error. |
| `shell.html` | page shell: click-to-start (audio needs a gesture), log tail, `window.__kkLog`. |
| `compat_wasm.hpp` | force-included MSVC/Win32 shim (`__stdcall`, `__int64`, ...). |
| `_start_wasm.cpp` | `sSystem_` on SDL2 + WebGL2: textures, render targets, geometry, render states, materials, sound, input, files, fonts (2D canvas), main loop. |
| `shader_translate.cpp/.hpp` | vs_1_1/ps_1_1 bytecode → GLSL ES 3.00 through the vendored MojoShader (`mojoshader/`), program cache per material setup + sampler types, constant packing. Setups whose vertex token stream is `{ KK04_MARKER \| variant, end }` are hand-written GLSL from `render2004.cpp` instead. |
| `render2004.cpp/.hpp` | the Breakpoint 2004 lighting for beta data: four-light pass and specular pass shaders (transcribed from the traced vs_1_1/ps_1_3 code), their constants, the shadow mask target and its channel quads. The frame logic is `Engine_::Paint2004` in `engine.cpp`. |
| `gen_thunks.py` → `kop_thunks.cpp/.hpp` | typed wrappers for every operator handler; replaces the x86 `CallCode` stack trampoline (`kdoc.cpp`). |
| `mmx_scalar.hpp` | bit-exact scalar `__m64` for the MMX texture generator (`genbitmap.cpp`). |
| `v2_bridge.cpp`, `v2_shim.cpp` | the song/sfx synth: `../v2` C++ core + `v2mconv` instead of `_viruz2a.asm`. |
| `gl_stub.cpp` | generated no-op GL for the headless build. |
| `tools/unpack_beta.py` | unpacks the released beta executable (kkrunchy stub run inside Unicorn, nothing native) and cuts out its player data. |
| `tools/kxread.py` | reader for both `.kx` dialects (header, classes, graph, parameters, animation code, events, splines). |
| `tools/kxconv.py` | converts the 2004 beta data to the dialect the player loads (`data/kkrieger_beta_conv.kx`). |
| `tools/beta_dis.py` | Capstone disassembler for the unpacked 2004 code, handlers labelled from the 2004 handler table. |

## Data

Two exported games are preloaded; the page picks one:

* default: **the released beta** (Breakpoint 2004, `pno0001.exe` from
  `kkrieger-beta.zip` on scene.org). `tools/unpack_beta.py` cuts its data out
  of the executable (`data/kkrieger_beta.kx`, 121,767 bytes) and
  `tools/kxconv.py` rewrites it as `data/kkrieger_beta_conv.kx`.
* `?data=3383`: `data/kkrieger3383.kx`, a later development snapshot shipped
  with fr_public. It is dimmer (lights toned down, no text-layer glare) and
  predates the final loader as well: no flags word, one-byte class ids;
  `KDoc::Init` detects it.

The converter also moves what the later code expects elsewhere: the 2004
monsters' own shot-event links go into the monster slots of the shot
`Events` operator (`WeaponShot[weapon kind]`), without which ranged monsters
fired invisible shots. Monster speeds stay the 2004 values (3383 retuned the
spider from 1 to 16 for its different AI; borrowing that made them rush in
eight times too fast).

Neither is matched by these sources: fr_public says `werkkzeug3_kkrieger`
was branched more than a year after the release. The beta data is older
still. `kxconv.py` (see its header) handles the layout differences:
the 2004 header, the vanished `e0` world object in front of the old
single-parameter Viewport (now Scene Add + the current Viewport with flag
0x80, "camera from this scene's Camera op", implemented in
`Exec_IPP_Viewport`), Text / Monster / Para / Print parameter blocks, event
intervals, and a Demo root. Parameter blocks that are followed by strings,
splines or the output count must have the *current* length, otherwise those
land in the wrong handler argument (the Viewport freed its output one
reader early; Print read its text pointer as "radius" and bent every glyph
to zero height).

2004 behaviour that differs and is switched on for the beta data only
(`kkBetaData`, set when intro/menu/game share one root):

* Mesh Color's render-slot pass consumed the light slots (the line is
  commented out later); the beta applies it twice in a row on many meshes,
  which otherwise doubled the baked light.
* intro, menu and game are one operator graph switched by `State` / `If`
  operators on switch 1, so a root change doesn't restart music and
  timeline (`mainplayer.cpp`).
* Render target sizes: the 2004 player's table (0x83f278 in the unpacked
  exe) is 1024x512 / 512x256 / 256x128 / 1024x512; later versions made sizes
  0 and 2 16x16. The beta's glow blur and glow mask are size 2, and at 16x16
  they smeared a haze over the level that flickered with every camera move
  and let the teal-tinted dark-area layer of the Mask through almost
  everywhere (walls, gun and shadows came out cyan). `GenOverlayManagerClass`
  uses the 2004 table for the beta.
* The 2004 player ran at 1024x768, so the beta defaults to that resolution
  (its 1024x512 render targets then map 1:1 onto the letterboxed view).
* Resolution: the start overlay offers a list (and `fit window`, in device
  pixels; `?res=WxH` overrides, the choice is kept in localStorage). It
  replaces the game's 640..1280 switch, the 2:1 view is centred on any screen
  shape (the original put it at 1/6..5/6 of a 4:3 screen, the same thing
  there), and the full-size render target grows to cover it (power of two,
  at least 1024x512; the glow targets keep their 2004 sizes so the glow looks
  the same).
* Sound effects were stereo DirectSound buffers without 3d; `PlaySample`
  placed its source once in camera space (pan x/|z+0.2|, volume scaled by
  min(1, halfrange/2/distance)). The beta gets exactly that; the later 3d
  path stays for the 3383 data.
* The first weapon used ammo (the later game made it infinite and printed
  `inf`).
* Monsters (measured in the original with `tools/origprobe.py`): they were
  verlet particles damped like the player on the ground (`DampPlayerGround`,
  0.05), so a spider at speed 1 settles at 2 units/s; the later AI keeps 0.9
  of the last step (1 unit/s). Their hit cells were solid for each other
  (mode 1 in exec_11), so spiders queue up and only the front one bites; the
  later game skips monster cells in `MoveCollider` and only repels them
  softly (`MonsterMagnetAI`, off for the beta). Result, same start: both
  spiders walk the same 2 u/s track, the front one bites 5 every second from
  about 8 s of AI time, the second waits behind it — as in the original.
* Material colours are animated (the intro's text fades, the menu
  highlight). The 2004 engine carried a mesh's animation variables into its
  paint jobs, the later one only runs a material when an effect asks for it,
  so the scene input executes the animated materials under each mesh itself
  (`kkExecMeshMaterialsR` in `genscene.cpp`). Without it every intro title
  stayed at its first key (transparent) and never appeared.

## What changed in the shared sources

All under `#if defined(__EMSCRIPTEN__)` unless noted:

* `_types.hpp/.cpp`: x87/MMX intrinsics → portable C; libm for `sFPow` & co.
* `genbitmap.cpp`: MMX blend loops transcribed 1:1 with `mmx_scalar.hpp`.
* `genmesh.cpp`, `genminmesh.cpp`: Perlin FPU rounding made explicit.
* `kdoc.hpp/.cpp`: thunk dispatch, `sVARARGS()` for operators that index past
  their last named parameter (`(&b0)[i]`), old `.kx` layout.
* `genmesh.cpp`, `genscene.cpp`, `kkriegergame.cpp`: other stack-layout tricks
  (`(&tx)[j]`, `(&l0)[i]`) replaced by local arrays.
* `../v2/synth_core.cpp` (unconditional): `V2Synth::init` cleared 4 bytes
  instead of the instance (`sizeof(this)`).
* `mojoshader/profiles/mojoshader_profile_glsl.c` (vendored copy): declares
  `io_5_N` inputs for ps_1_x texture registers in the GLSL ES profiles.

## Status

Beta data (default): the 2004 intro (logo, credits, camera flight with its
glow post-processing), the 2004 menu (`start new game` / `credits` / `exit
.kkrieger`), the level with its glare, HUD and monsters, shooting, item
pickups (sound, particle burst, counters), sound effects, monsters that
move and bite at the original pace, ranged monsters with visible shots. Compared frame by frame
with the original under Wine: same colours, lamp glow and HUD; with the 2004
renderer (below) the lighting, shadows, specular and post-processing of a
recorded corridor frame match the original stage by stage (mean brightness
within ~1-5 %, same sharpness). Not yet played through end to end.

### The 2004 renderer (beta data)

The beta does not light like the werkkzeug3 sources here, so for beta data
`Engine_::Paint` hands over to `Engine_::Paint2004` (`window.__kkNo2004 = 1`
switches back). Reconstructed from the beta executable (Engine paint VA
0x817134, pass loop 0x81566a, constants 0x804b25) and an apitrace of it:

* The frame is lit by at most four lights, picked once per frame (not per
  mesh): importance = range / distance to the camera, 0 beyond 45 units,
  amplify faded between 35 and 45, the newest weapon light always first,
  up to three shadow casters moved to the front. No default light.
* Shadows go into one mask render target at half the view size: cleared per
  shadow count, z-filled by the base passes (which leave their colour in it -
  kept, the mask channels of unshadowed lights read the baked vertex light),
  then per shadow light the volumes stamp the stencil and a quad adds that
  light's channel (a, b, g) where the stencil is 0.
* Every lit mesh gets one additive pass for all four lights: per-vertex
  linear attenuation amplify·(1-d/range), the light directions blended into
  one tangent-space vector for the normal map, colour = Σ colour·attenuation
  ·mask. Specular is a second pass after the texture pass on half vectors,
  (N·H)^8/16/32 by specular power.
* Passes run usage by usage (all z-fill, all light, all texture, then
  envmaps / single-pass / specular), not render pass by render pass.
* Material blend modes use the 2004 table (`genmaterial.cpp`): mode 6 was
  ONE/ONE add (later: subtract), 7 and 8 were the dest/source-alpha adds.

3383 data: intro → menu (`[start new game]` / credits / options / exit) → level,
WASD movement, HUD (`life:` / `ammo:`), music and sound effects. ~24 fps in
headless SwiftShader, no GL errors.

Known gaps:

* (Fixed 2026-09-26) Stencil shadows never showed because `CmpGL` maps D3D
  `EQUAL` to `GL_LEQUAL` for the z-equal passes and the stencil used the same
  mapping: "0 <= stencil" passed everywhere. The stencil now has its own
  `StencilCmpGL`.
* (Fixed 2026-09-26) The picture was soft: `MakeProjectionMatrix` carries
  D3D9's half-pixel shift, which in GL put every full-screen post-processing
  copy half a texel off (bilinear 2×2 average, ~10 copies per frame). The GPU
  copy of the projection drops it (`MakeGLProjectionMatrix`).

* The level is still darker than videos of the released game (no lamp
  glare, weaker light pools). As far as it could be traced this is the data:
  `kkrieger3383.kx` matches `kkrieger3383.k`, its corridor lights are weak
  (amplify 0.125..0.5) and the lamp glow is baked vertex light only; no op
  in this export reads the glare/brightness switches.
* Mouse look uses pointer lock: requested again on the first click inside the
  canvas, because browsers only grant it from a user gesture. Headless
  Chromium refuses pointer lock outright, so this path is untested.
* A hidden tab clamps timers to one second each, and generation yields to the
  browser between operators — `Progress()` yields ten times less often while
  `document.hidden`, but loading in a background tab is still slow.

## Debugging aids

* During an F9 trace every post-processing stage (and both inputs of each
  merge / mask, colour and alpha) is kept as an image; the cdp.js step
  `shots:<dir>` saves them as PNGs. That is how the text-layer and render
  target bugs below were found.

* `F9` dumps the next frame's GL calls (`[kk] T ...` lines: viewport, clear,
  setup, instance, draw plus a 10x6 read-back probe after each).
* Debug keys: `F2` shadow volumes stamp the stencil, `F3` drop one test at a
  time from the lighting passes (scissor / depth / stencil), `F4` draw the
  shadow volumes, `F5` invert the shadow stencil test, `F6` base-only or
  lighting-only passes, `F7` shadows / no shadows / no lights, `F8` depth
  nudge for the z-equal passes, `F11` portal visibility off. All of them
  reset the state cache where needed, so they can be toggled live.
* `F10` dumps one frame of operator execution (`exec op=`, `sceneinput`,
  `engine:`/`engineS:` job counts, `print page=`, `addev`); with the 2004
  renderer also its four lights and the shadow volumes drawn per light.
* 2004 renderer: `window.__kk04Shots = 1` saves the next frame's stages
  (mask, z-fill, light, texture, each render pass of the last usage) for
  `shots:<dir>` (render targets come out upside down);
  `window.__kkNo2004 = 1` falls back to the later renderer.
* `window.__kkTeleport = [x, y, z, yaw, pitch]` moves the player (the pose
  logs of the original line up with it). `cdp.js --window 1100,1000` shows a
  1024x768 canvas 1:1; the default window scales it and hides blur.
* `KK_VERBOSE 1` in `compat_wasm.hpp` adds the per-operator generation log and
  emmalloc heap validation after every operator.
* `node wasm/cdp.js --steps "wait:2,start,wait:20,focus,key:Return,wait:8,shot:/tmp/x.png"`
  reproduces a run end to end without a browser window.
* More keys: `G` logs the next frame's base paint jobs (world boxes, cull
  result) and every submitted light, `H` engine frustum culling off, `U`
  draw one mesh material index at a time, `J` culling off / inverted, `K`
  alpha test off, `F1`/`C` swap red and blue, `V`/`B`/`N` light pass boost,
  forced specular alpha and term views.
* `O` puts the player next to the next collectable that is still there,
  facing it (walk forward to pick it up). With `window.__kkTracePickup` set
  the frame just after a pickup is traced like `F9`; particle draws are kept
  as stage images too.
* `P` puts the player 10 units from the next ranged monster, facing it.
* `window.__kkFindMtrl = <material operator index>`, then F11 (paint all
  sectors) for a moment: the engine logs every place a mesh with that
  material stands, and `L` walks through them.
* `window.__kkShowZEqual = 1` paints what the EQUAL emulation drops magenta
  instead of dropping it; `window.__kkNoZEqual = 1` turns it off (A/B).
* `window.__kkMonLog = 1` logs the active monsters (collider position, melee
  timer) and the player's life every 5 game ticks, and every melee hit;
  `tools/origprobe.py` samples the same from the original under Wine (it
  reads the process memory), so the two can be lined up tick by tick.
* `window.__kkDumpSamples = 1` (before start) keeps every rendered sound
  effect as a WAV for `shots:`.
* Reference: the original beta runs under Wine (`WINEPREFIX=<scratch>
  wine pno0001.exe` in a fresh prefix, `WINEDLLOVERRIDES="mscoree,mshtml="`;
  wined3d renders it faithfully except fonts: Wine has no Times New Roman, so
  the HUD shows a sans face there — real 2004 screenshots show serif).
  `import -window $(xdotool search --name '^kk$')` grabs frames; `xdotool
  key Return` drives the menu once the window is focused.
* `window.__kkDumpOps = [op indices]` (set before start) saves those bitmap
  operators' results as stage images; `window.__kkDumpSetups = [ids]` dumps
  the translated GLSL and constants of those material setups during a trace.
* `kkBitmapLog = 1` in `kdoc.cpp` logs every bitmap operator (average colour,
  parameters, texture handle) and every mesh operator (face count, bounding
  box), which is how the cube bug below was found.

## Things this data file needed

`data/kkrieger3383.kx` was exported by an older werkkzeug than these sources,
so several operators have fewer parameters than the handlers expect:

* `KOp::Calc` / `KOp::Exec` zero the parameter block, so missing trailing
  parameters read as 0 instead of stack garbage (the Viewport operator gained
  stereo-3d and sub-rectangle parameters later, and they were landing in
  `fx0..fy1`, pushing the whole viewport off screen).
* `GenOverlayManagerClass::Alloc` treats a missing output count as 1; with 0
  the IPP chain handed a render target out as its own source (WebGL rejects
  the feedback loop and drops the draw).
* `Exec_Effect_Print` remaps the flag layout: x-alignment sat in bits 0..1
  before it moved to bits 8..9.

* The IPP Viewport operator has one input instead of two (no spline yet):
  `KOp::Call` pushes a null spline so `Init_IPP_Viewport` doesn't call
  `Release()` on the size parameter.

Compiler flags the code depends on (all in `build.sh`):

* `-fno-delete-null-pointer-checks`: the code calls methods on null pointers
  by design (`KInstanceMem::DeleteChain`).
* `-fno-strict-aliasing`: MSVC never did type-based alias analysis and the
  code indexes struct members as arrays (`(&vs.x)[j]`). With clang's
  struct-path TBAA, `Mesh_Cube`'s tessellation loop kept the previous
  axis's selection box, so the z extrusion never happened and every cube
  with z tessellation came out 1/tz as long: skirting boards, lower walls,
  cornices and ceiling parts were missing and the level showed the green
  clear colour in their place.
* `-fwrapv`: the fixed-point generator code assumes x86 wrap-around.

## Other things worth knowing

* `sINTRO` is 0 in `player_kkrieger/kkrieger_config.hpp` (it ships as 1). With
  1 the font-page branch of `Bitmap_Text` is compiled out, so `Letters[]` stays
  empty and neither the menu nor the HUD draws any text.
* `sSystem_::InitScreens()` resizes the canvas: the game picks its resolution
  from `Switches[KGS_RESOLUTION]` (800x600 by default) and the master viewport,
  render targets and clear rectangles all follow `ConfigX/Y`.
* `LastViewProject` must stay world→clip; the materials build their own
  world-view-projection as `ModelSpace * LastViewProject`.
* `sMaterialEnv::MakeProjectionMatrix` stays in D3D conventions because the
  engine derives its frustum and portal boxes from it; only the copy that goes
  to the GPU (`MakeGLProjectionMatrix`) gets the z-range fix and the render
  target y flip.
* D3D clears only the viewport rectangle, GL clears everything — `Clear()`
  scissors itself.
* `sSystem_::SetScissor` takes its rectangle in **clip space** (-1..1, y up),
  not pixels — the d3d layer turned it into a viewport-relative pixel rect.
  Treating it as pixels scissored every per-pixel lighting pass down to a
  2x2 corner, which left the level flat-textured and made stencil shadows
  look broken.
* `ZFUNC EQUAL` is emulated. The engine z-fills and then shades with EQUAL;
  D3D guarantees the same depth from different vertex programs, GL does not
  (a true `GL_EQUAL` still leaves patches of walls unshaded, even with
  `invariant gl_Position`), so the hardware test is `GL_LEQUAL`. That alone
  let the light and texture passes of alpha-tested z-fills paint through the
  holes onto whatever lies behind — the spike strips on the wooden doors
  showed as a translucent film. Before the first EQUAL draw after depth was
  written, `kkZSnapPrepare` blits the depth buffer into a texture (unit 7),
  and EQUAL draws drop fragments more than 2e-5 in front of it
  (`kkZEqual` in every fragment shader).
* `DEPTHBIAS` / `SLOPESCALEDEPTHBIAS` map to `glPolygonOffset` (D3D biases in
  normalised depth, GL in units of the smallest resolvable difference).
* The back buffer needs alpha (lighting keeps specular in destination alpha),
  but D3D presented it opaque. The frame loop clears alpha to 1 before the
  browser composites the canvas (`kkOpaqueBackbuffer`); the level used to
  end its frames with alpha 0 everywhere, which GPU compositors are free to
  show darkened or discoloured.
* `Engine_::InsertLightJob` counted past `MAXLIGHT` and rendered a fifth,
  out-of-bounds light every frame; the count is clamped now.
* IPP render targets are ordinary textures drawn into through an FBO, not
  `sTIF_RENDERTARGET` textures, so the sampler asked them for mipmaps they
  never had; an incomplete texture samples black and every render target
  copied to the screen was black. Filtering now follows whether a mip chain
  was actually uploaded (`sGLTex::Mips`).
* `Exec_IPP_Select` gets no `count` (its convention has no output count);
  on x86 it read stack garbage, here it read 0 and released its output one
  reader early. It uses the operator's output count now.
* Sound effects were never played: `SampleAdd` / `SamplePlay` / the 3d calls
  were stubs. `_start_wasm.cpp` now mixes them in the audio callback with the
  DirectSound bookkeeping of `_start.cpp` (buffers per handle used
  round-robin, use counter, linear volume, pan in 1/100 dB, 3d rolloff
  (min/dist)^rolloff and a left/right balance; no doppler).
* ...and they were silent anyway: the VFX table stores *sample* offsets into
  the effects tune and the 2004 `CV2MPlayer::Play` took samples (0x807f11
  compares its argument straight to the sample counter), while the later
  `V2MPlayer::Play` takes ticks, 1000 per second by default. Every effect was
  cut from silence 44 times further into the tune. `v2_bridge.cpp` initialises
  the player with 44100 ticks per second.
* Fragment shaders are `highp`: MojoShader asks for `mediump`, which some
  drivers really run at fp16 (ANGLE on NVIDIA's GLES driver reports a 10 bit
  mantissa) — tiled texture coordinates then quantise to 1/256.
* Mouse buttons arrive as keys (`sKEY_MOUSEL` / `MOUSER` / `MOUSEM`, with
  `sKEYQ_BREAK` on release), which is how the game reads the fire button;
  without them there was no way to shoot.
* Effects (every particle system, shot and impact) sorted into light slot 0,
  i.e. right after the first light and before the texture pass and the
  specular add. Their quads write alpha 1, and `GENOVER_ADDDESTALPHA` turned
  that into solid white squares — the item pickup burst covered half the
  screen. `Engine_::BuildPaintJobs` now gives effects from `ENGU_SHADOW` up
  the last light slot, like single-pass meshes.
* `material11.cpp` dropped `sMCA_INVERTA` on combiner alpha sources (only
  the alpha combiner honoured it). Only the IPP "alpha" merge sets it, so
  the text layer was shown where transparent and hidden where opaque.
* Render states are deduplicated against a shadow copy of what GL actually
  has: the material state blocks are absolute and nearly identical from draw
  to draw, and ~97% of the calls are no-ops (the per-frame counter line
  reports `states applied=` / `skipped=`). Anything that changes GL state
  outside `SetState` has to keep that shadow in sync.
* The back buffer needs an alpha channel (`SDL_GL_ALPHA_SIZE`): the lighting
  accumulates specular in destination alpha and adds it back with
  `GENOVER_ADDDESTALPHA`; without it every frame ended up white.
