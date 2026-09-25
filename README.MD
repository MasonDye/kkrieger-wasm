# kkrieger-wasm

**.kkrieger in the browser** — the 96 KB first-person shooter by
[.theprodukkt](http://www.theprodukkt.com/) (farbrausch), ported to
WebAssembly / WebGL2 and playing the **original Breakpoint 2004 beta** with its
own 2004 lighting.

Port by MasonDye.

**Play it online: https://masondye.github.io/kkrieger-wasm/** (built from
`master` by the `pages` workflow; WebGL2 browser, generation takes a while on
start).

---

## What this is

* The whole game — procedural texture/mesh generation, the V2 synthesizer for
  music and sound, the game logic and the renderer — compiled with Emscripten.
  Everything that was Win32/Direct3D 9/DirectSound specific is replaced by
  `werkkzeug3_kkrieger/wasm/` (SDL2 + WebGL2 + Web Audio); Direct3D shader
  bytecode is translated to GLSL ES at runtime through MojoShader.
* The data is the one release that exists: the Breakpoint 2004 beta. It is
  unpacked from the original executable and converted to the dialect these
  (later) sources read (`wasm/tools/unpack_beta.py`, `wasm/tools/kxconv.py`).
* These werkkzeug3 sources are younger than the beta and render differently.
  For the beta data the port re-implements the 2004 renderer, reconstructed
  from the beta executable and an apitrace of it: four lights per frame, one
  half-resolution shadow mask, one four-light pass per mesh, the 2004 blend
  table. Recorded frames of the original (run under Wine) and of the port
  match stage by stage.

## Play locally

Requirements: [emsdk](https://emscripten.org/docs/getting_started/downloads.html),
Python 3, a WebGL2 browser.

```bash
source ~/emsdk/emsdk_env.sh
cd werkkzeug3_kkrieger
KK_RELEASE=1 bash wasm/build.sh                 # -> wasm/dist_release/
python3 wasm/serve.py 8767 dist_release         # serves wasm/dist_release on 127.0.0.1:8767
# open http://localhost:8767/kkrieger.html
```

`bash wasm/build.sh` (without `KK_RELEASE`) builds the debug version into
`wasm/dist/` (`python3 wasm/serve.py` serves it on port 8766).

The procedural content is generated at start-up; that takes a while, as it
did in 2004.

## Controls

| key | action |
|---|---|
| mouse | look (click the game to capture the mouse) |
| W A S D | move |
| left mouse, right Ctrl | fire |
| left Shift | use (switches, doors) |
| 1 – 9 | weapons |
| Esc | in-game menu |
| arrows, Enter | menus |

On the start page you can pick the **resolution** (1024 × 768 is what the beta
ran at) and **start in fullscreen**; the `fullscreen` button in the corner
works at any time. `?res=WxH` in the URL sets the resolution, `?data=3383`
plays a later development snapshot of the data instead of the beta.
Choosing *exit .kkrieger* returns to the start page. The game's log goes to the
browser console (F12).

## Repository layout

```
werkkzeug3_kkrieger/          the game and tool sources (farbrausch, 2004/2005)
  wasm/                       the WebAssembly port: platform layer, shader
                              translation, 2004 renderer, build scripts, tools
    README.md                 detailed port notes, debugging aids, findings
  data/kkrieger_beta*.kx      the Breakpoint 2004 beta data (original / converted)
v2/                           the V2 synthesizer
kkrunchy/, ktg/, genthree/    other farbrausch tools from fr_public
docs/                         werkkzeug3 documentation
```

Port details, the reverse-engineered 2004 renderer, the Direct3D → GL
conventions that mattered and all debug keys are in
[`werkkzeug3_kkrieger/wasm/README.md`](werkkzeug3_kkrieger/wasm/README.md).

## About .kkrieger & .werkkzeug3

.kkrieger is a first-person shooter created by the German demogroup
.theprodukkt (a former subdivision of farbrausch). The entire game compiles
down to a 96 KB executable. It makes extensive use of procedural generation:
textures are stored as their creation history, meshes are built from basic
solids that are then deformed, and all assets are regenerated during the
loading phase. Music and sounds come from V2, a synthesizer fed with a
continuous stream of MIDI data. .werkkzeug3 is the tool that was used to
create it.

## Credits & license

* .kkrieger, .werkkzeug3, V2, kkrunchy: farbrausch / .theprodukkt — released
  under a BSD license or into the public domain as stated per project
  ([fr_public](https://github.com/farbrausch/fr_public)).
* Earlier repository work on the Windows / Visual Studio 2022 build: Steven
  Schronk, LWP-GH / louis panton (this repository keeps their history; cloned
  from [jaromil/kkrieger-werkkzeug3](https://github.com/jaromil/kkrieger-werkkzeug3)).
* MojoShader (`wasm/mojoshader/`): Ryan C. Gordon, zlib license
  (`wasm/mojoshader/LICENSE.txt`).
* WebAssembly port and the 2004 renderer reconstruction: MasonDye.
