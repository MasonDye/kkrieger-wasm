# kkrieger-wasm

**.kkrieger in the browser** — the 96 KB first-person shooter by
[.theprodukkt](http://www.theprodukkt.com/) (farbrausch), ported to
WebAssembly / WebGL2 and playing the **original Breakpoint 2004 beta** with its
own 2004 lighting.

**浏览器里的 .kkrieger**：把 .theprodukkt（farbrausch）那款 96 KB 的第一人称射击游戏移植到
WebAssembly / WebGL2，运行的是 **2004 年 Breakpoint 发布的原版 beta 数据**，并按原版的 2004
渲染方式还原了光照与阴影。

Port by MasonDye.

---

## What this is / 这是什么

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

中文简介：

* 整个游戏（程序化生成贴图与模型、V2 合成器音乐音效、游戏逻辑、渲染器）都用 Emscripten
  编译。原来依赖 Win32 / Direct3D 9 / DirectSound 的部分由 `werkkzeug3_kkrieger/wasm/`
  （SDL2 + WebGL2 + Web Audio）替换，D3D 着色器字节码在运行时经 MojoShader 转成 GLSL ES。
* 数据是唯一公开发布的版本：2004 Breakpoint beta，从原版 exe 中解包并转换格式。
* 这套 werkkzeug3 源码比 beta 更新，渲染方式不同。对 beta 数据，移植版根据原版 exe
  反汇编和 apitrace 抓帧，重写了 2004 年的渲染流程（每帧 4 盏灯、半分辨率阴影遮罩、
  每个网格一次四灯光照、2004 年的混合模式表），与 Wine 下运行的原版逐阶段对比一致。

## Play locally / 本地运行

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

## Controls / 操作

| key | action |
|---|---|
| mouse | look (click the game to capture the mouse) / 视角（点击画面锁定鼠标） |
| W A S D | move / 移动 |
| left mouse, right Ctrl | fire / 开火 |
| left Shift | use (switches, doors) / 使用 |
| 1 – 9 | weapons / 切换武器 |
| Esc | in-game menu / 游戏内菜单 |
| arrows, Enter | menus / 菜单选择 |

On the start page you can pick the **resolution** (1024 × 768 is what the beta
ran at) and **start in fullscreen**; the `fullscreen` button in the corner
works at any time. `?res=WxH` in the URL sets the resolution, `?data=3383`
plays a later development snapshot of the data instead of the beta.
Choosing *exit .kkrieger* returns to the start page.

开始页可以选择**分辨率**（原版为 1024 × 768）和**全屏启动**，右上角的 fullscreen
按钮随时可用。URL 参数 `?res=宽x高` 指定分辨率；`?data=3383` 改为加载较晚的开发版数据。
菜单里选 *exit .kkrieger* 会回到开始页。游戏日志只输出到浏览器控制台（F12）。

## Repository layout / 目录

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
