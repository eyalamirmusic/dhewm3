# Porting dhewm3 to eacp

Moving dhewm3 off SDL2 + OpenGL and onto [eacp](https://github.com/eyalamirmusic/eacp):
app lifecycle and message loop first, GPU rendering (Metal / D3D12) as the real work.

**Status: Phase 0 is done and merged. Phase 1 has landed its gate and its seam.
Phase 2 is next, and is where eacp starts being used.**

Reference implementation for almost everything on the platform side:
`~/Code/PureDOOM/examples/EACP` — a complete engine hosted on eacp, with its own
`CLAUDE.md` carrying an eacp gap log worth reading before starting.

---

## 1. Where dhewm3 stands today

### The platform layer is small and already contained

SDL lives in essentially four files:

| File | `SDL_` refs | Responsibility |
| --- | --- | --- |
| `neo/sys/glimp.cpp` | 314 | window + GL context creation, swap, gamma, video modes |
| `neo/sys/events.cpp` | 285 | keyboard, mouse, grab, gamepad |
| `neo/sys/threads.cpp` | 59 | mutex / cond / thread |
| `neo/framework/Common.cpp` | 34 | timing, misc |

The main loop is `neo/sys/linux/main.cpp:449`:

```cpp
while (1) { common->Frame(); }
```

Sound is OpenAL (`neo/sound/*`), independent of SDL — untouched by this port.
Threading can go straight to `std::thread` / `std::mutex`.

### The renderer is the project

2040 `qgl` call sites across 131 distinct entry points. It is **not** GLSL —
`qglCreateShader` appears zero times. It is GL 1.4-era fixed function plus ARB
assembly programs:

```
qglMatrixMode              42     qglProgramEnvParameter4fvARB  42
qglBegin                   28     qglBindProgramARB             21
qglTexEnvi                 19     qglProgramStringARB            4
qglEnableClientState       15     qglCreateShader                0
```

Matrix stack, texture-env combiners, client arrays, `glBegin`, `glAlphaFunc` have
no Metal/D3D12 counterpart. They get rewritten as shaders, not translated.

Doom 3's shape is more cooperative than 2040 suggests, though — the seams are
already there:

- **State is already an abstract bitfield** — `GLS_*` constants, `neo/renderer/tr_local.h:1015`
- **All geometry funnels through two functions** — `RB_DrawElementsWithCounters`
  (`neo/renderer/tr_render.cpp:79`, 13 call sites) and
  `RB_DrawShadowElementsWithCounters` (`:118`, 15 sites — the stencil path)
- **Binding funnels through** `GL_SelectTexture` / `idImage::Bind`
- **The frontend already emits a command list** consumed by
  `RB_ExecuteBackEndCommands` (`neo/renderer/tr_backend.cpp:644`) — five command
  types: `RC_NOP`, `RC_DRAW_VIEW`, `RC_SET_BUFFER`, `RC_COPY_RENDER`, `RC_SWAP_BUFFERS`

---

## 2. What eacp gives us

Verified against `main` at `be7a749`. Several of these were added *by* the PureDOOM
port and the rest by Phase 0 of this one, which is why both gap logs are the right
place to start reading.

**Input — solved.**
- `Window::setMouseLocked(bool)` / `isMouseLocked()` — FPS mouse lock
- `MouseEvent::rawDelta` — the *device's* unaccelerated movement, added specifically
  because the pointer acceleration curve makes an identical flick of the hand turn a
  camera a different amount depending how fast it was made
- The cursor-warp-reported-as-user-motion bug is fixed (was measured at −222px in a
  single event)
- `Graphics::KeyCode` is **positional** (macOS virtual key values, translated on
  Windows) — which is what a movement binding needs

**Loop / lifecycle — solved.**
- `Apps::run<T>()`, `Threads::EventLoop`, `Timer`, `DisplayLink`
- `GPUView::update(FrameTime)` at vsync, `setMaxFps`, `setFramesInFlight`
- `Graphics::primaryDisplay()` — frame, work area and backing scale in points, for
  picking an initial window size

**GPU.**
- `GPUView` with MSAA, depth and **stencil**, scissor, viewport, render targets
- **Stencil**: per-face ops and compare on `RenderPipelineDescriptor`, shared
  read/write masks, `RenderPass::setStencilReference`,
  `RenderPassDescriptor::clearStencil`, `TextureDescriptor::stencil`
- `RenderPipelineDescriptor::cullMode` with the clip-space winding convention pinned
  on both backends (counter-clockwise = front, glTF's convention)
- `RenderPass::bind(program, vertices)` — a program drawn over geometry the app owns
- `GPU::StreamingBuffers` — per-frame allocations out of a pool, handed back as
  sub-ranges. **This is `idVertexCache`'s exact shape**, and it already exists.
- `Buffer::update` — dynamic geometry re-uploaded in place
- `ShaderProgram::setDiscardBelow` — alpha test in the EDSL
- `RenderPass::targetWidth()` / `targetHeight()` — finding I6, §4
- The shader EDSL itself: one C++ source emitting both MSL and HLSL, so the two
  backends cannot drift on a shader we write once

**A worked example of the hard part.** `Apps/GPU/StencilShadows` runs the real
two-sided depth-fail shadow-volume algorithm — the one Doom 3's lighting is built
out of — over a spinning cube, in three panels: no stencil, the volume drawn
translucently, and the shadowed result. It exists because the state that expresses
an algorithm is not proof the algorithm composes, and this port should not find that
out inside a renderer rewrite.

---

## 3. What PureDOOM hands us as a template

`~/Code/PureDOOM/examples/EACP/` — the app-framework half is now transcription,
not design.

| File | What to lift |
| --- | --- |
| `App.h` | `Window` + `GPUView` content view, `view.focus()`. ~15 lines. |
| `View.h` / `View.cpp` | The loop shape: `update(FrameTime)` steps the engine when *its own clock* says a tic is due; `render(Frame&)` draws. Mouse lock engaged on click, `rawDelta` accumulated into aim, flushed once per tic. |
| `Input.h` | The positional `KeyCode` → engine-key table, wholesale. Read its header comment on why positional must win even where a character is available. |
| `CMakeLists.txt` | Consuming eacp via CPM, including the `EACP_MACOS_PLIST` workaround (gap #3), `eacp_set_gui_subsystem`, `set_default_target_setting`. |

Two patterns worth stealing beyond the code:

- **`captureTarget`** — PureDOOM keeps the whole composited frame in an app-owned
  render target rather than copying back from the drawable. That is the answer for
  Doom 3's `_currentRender` (heat haze, mirrors), so no drawable→texture blit is
  needed from eacp.
- **Demo replay as the safety net.** PureDOOM's regression gate is deterministic
  demo playback. dhewm3 has the same facility built in: `recordDemo` / `playDemo` /
  `timeDemo` (`neo/framework/Session.cpp:643`, `:681`, `:692`).

---

## 4. Phase 0 — closed

Three blockers, all answered, merged to eacp `main` as `be7a749` with Windows
checked. The full eacp suite is **1247 tests, all passing**, and the GPU half runs
clean under `MTL_DEBUG_LAYER=1`.

Kept rather than deleted, because each carries something that outlived the gap —
the convention PureDOOM's own log follows.

### 1. Stencil — built

Doom 3's entire lighting model is stencil shadow volumes (`qglStencilOpSeparate`
×14, `qglStencilOp` ×15, `qglStencilFunc` ×12, two-sided depth-fail). eacp had
none. It now has:

- `GPUView::setStencil(bool)` / `hasStencil()`, and `TextureDescriptor::stencil` /
  `Texture::hasStencil()` for a texture target. Both **imply depth**: the two planes
  are one attachment of one combined format on both APIs (`Depth32Float_Stencil8` /
  `D32_FLOAT_S8X24_UINT`), so a stencil-only buffer would allocate the depth plane
  anyway and then have to explain itself.
- `RenderPipelineDescriptor::stencil`, `stencilFront`, `stencilBack` (`StencilFace`:
  a compare and the three outcome ops), and `stencilReadMask` / `stencilWriteMask`.
  **The masks are shared by both faces**, which is D3D12's shape rather than
  Metal's — offering per-face masks would offer something one backend cannot honour.
- `RenderPass::setStencilReference`, `RenderPassDescriptor::clearStencil`.
- `DepthCompare` is now `CompareFunction`, both APIs using one comparison enum for
  depth and stencil alike. The old name stays as an alias.

Twelve cases in `Tests/GPU/StencilTests.cpp`, each with its mirror — the cull-mode
lesson, that a test which only looks for something *missing* passes when nothing
drew at all.

**Three things worth keeping:**

- **The validation layer's silence was measured, not assumed.** Nulling the stencil
  attachment makes Metal abort at `setRenderPipelineState` — *"For stencil
  attachment, the renderPipelineState pixelFormat must be MTLPixelFormatInvalid, as
  no texture is set."* So the quiet run is evidence. Metal catches this earlier than
  it caught the depth equivalent, which was silent on Apple silicon.
- **The suite could not run under the validation layer at all** until a pre-existing
  test was fixed: `TextureUpdateTests` uploaded with `bytesPerRow = 10` for an RGBA8
  texture, and Metal asserts on a stride that is not a whole number of pixels. Green
  without the layer, `SIGABRT` with it.
- **One cross-backend divergence found and closed.** The stencil reference is
  *encoder* state on Metal, reset per pass, and *command-list* state on D3D12, where
  one list carries several passes — so a pass setting a reference would have lent it
  to the next pass on that backend only. `Frame::beginPass` resets it, with a test
  that needs two passes to see it.

### 2. `RenderPass::bind(program, vertices)` — carried over

Gap I1, answered on eacp's `puredoom` branch and never on `main`. `ad3fa16`
cherry-picked cleanly; `drawInstanced(program, …)` now routes through `bind` as the
commit intended, and four cases in `Tests/GPU/ExternalGeometryTests.cpp` came with
it. dhewm3's `VertexCache` owns all geometry and issues sub-ranges, so this was
needed on day one.

### 3. Per-draw sampler state — decided, and it cost eacp nothing

The question was whether Doom 3's per-material-stage clamp-vs-repeat forces N
program variants or a runtime sampler selection back into eacp — where sampling is
fixed at shader compile time, deliberately, with a Windows-on-Arm driver bug behind
it (eacp's `SAMPLERS.md`, PureDOOM's finding I3).

**Neither. Doom 3 asks the sampler for less than its enum suggests.** Its four
repeat modes collapse to two at the hardware sampler
(`idImage::SetImageFilterAndRepeat`, `neo/renderer/Image_load.cpp:406`):

| Doom 3 mode | What the sampler is set to |
| --- | --- |
| `TR_REPEAT` | `GL_REPEAT` |
| `TR_CLAMP` | `GL_CLAMP_TO_EDGE` |
| `TR_CLAMP_TO_ZERO` | `GL_CLAMP_TO_EDGE` |
| `TR_CLAMP_TO_ZERO_ALPHA` | `GL_CLAMP_TO_EDGE` |
| `TR_CLAMP_TO_BORDER` | `GL_CLAMP_TO_BORDER` |

The two `_TO_ZERO` modes are **not** a border colour. The zero edge is written into
the texel data at upload — `R_SetBorderTexels` (`neo/renderer/Image_process.cpp:141`),
called from `idImage::GenerateImage` (`neo/renderer/Image_load.cpp:609`) — and the
sampler is plain clamp-to-edge. The *data* carries the border, so any backend that
can clamp reproduces it exactly.

`TR_CLAMP_TO_BORDER` is the one real border mode and no material can reach it: the
parser accepts `clamp`, `noclamp`, `zeroclamp`, `alphazeroclamp` and nothing else
(`neo/renderer/Material.cpp:980`), and its single user is the generated
`_borderClamp` image (`neo/renderer/Image_init.cpp:369`), whose edge texels the
generator has already zeroed. Its own declaration says it *should* have replaced the
`_TO_ZERO` pair and was left alone out of caution.

Filters are the same story: `TF_DEFAULT`, `TF_LINEAR`, `TF_NEAREST` — mip-filtering
and anisotropy ride on `TF_DEFAULT` as cvars (gap 7), leaving linear-vs-nearest as
the only sampler-visible choice.

**So Doom 3's whole sampler need is `{Nearest, Linear} × {Clamp, Repeat}` — exactly
the four configurations eacp already has.**

That leaves only the shape question, and the demo's own 67 `.mtr` files (5617
stages) size it:

| Stage kind | `clamp` | `zeroclamp` | `linear` | `nearest` |
| --- | --- | --- | --- | --- |
| `map` (generic stage) | 273 | 377 | 11 | 1 |
| `diffusemap` | 24 | — | — | — |
| `bumpmap` | 15 | 2 | — | — |
| `specularmap` | 11 | 3 | — | — |
| `videomap` | — | — | 67 | — |

plus 244 material-level `clamp` and 20 material-level `nearest`, which apply to
every stage of the material they head.

The **interaction** program has three material-controlled textures and no stage in
the demo sets a filter on any of them, so its variants are `2³ = 8` worst case and
in content overwhelmingly the two uniform ones, a material-level `clamp` clamping
all three together. The **generic material stage** is one texture, so `2 × 2 = 4`.
Everything else is fixed at author time.

**Decision: a lazy variant cache keyed on the tuple of samplings a draw needs,
built on eacp's four configurations as they stand.** Compiled on first use, so we
pay for the combinations the content contains rather than the sixteen it could.
`Sprites::SpriteRenderer`'s `Array<std::optional<Shader>, samplingConfigurations>`
is the pattern, one dimension wider.

### Interface finding I6 — a pass never said how big its target was

Not a missing feature; a shape that made this port write something it should not
have had to. PureDOOM's `I1`–`I5` are the precedent.

`RenderPass` has held `targetWidth`/`targetHeight` privately since scissor clamping
needed them. So a caller dividing the target between viewports — which
`setViewport`'s own documentation offers as the reason it exists — had to
reconstruct the number from the view's bounds times `GPUView::backingScale()`.

That derivation is wrong wherever the pass is not the drawable: a snapshot renders
at whatever scale `renderToImage` was given, and a viewport outside the target is
deliberately a no-op. The stencil example's three panels silently collapsed into one
under `--check` while looking correct on screen.

**Closed**: `RenderPass::targetWidth()` / `targetHeight()`, with a case in
`Tests/GPU/ViewportTests.cpp` that renders one view at two scales — one scale cannot
tell the pass's size apart from the bounds it was given.

---

## 5. eacp gaps still open

Re-verified against `main` at `be7a749` rather than carried forward on trust.
**None of these blocks Phase 1, which touches no eacp API at all**, and none blocks
*starting* Phase 2 — each degrades the picture or has a workaround, and the list is
better driven by real content than guessed at now.

Numbers are never reused, so a hole is an entry that closed.

### Needed, not blocking

4. **BC/DXT compressed texture formats** — all Doom 3 art ships as DXT1/3/5 in the
   pk4s. Without it: decompress at load, ~4× VRAM, much slower level loads.
5. **Cube textures** — skyboxes and reflections. (The normalization cubemap can be
   deleted outright; `normalize()` is free now.)
6. **Depth bias / polygon offset** — decals z-fight without it.
7. **Mip filter selection and anisotropy** — currently `Linear|Nearest` ×
   `Clamp|Repeat` only. Doom 3 exposes trilinear and aniso as cvars.
12. **No colour write mask** — found by building the stencil example, which needs one
    and has to fake it. `RenderPipelineDescriptor` has a blend mode and no write
    mask, so a pass that must update depth or stencil while leaving colour alone has
    no way to say so.

    Doom 3 wants this **per channel**: `GLS_REDMASK`, `GLS_GREENMASK`,
    `GLS_BLUEMASK`, `GLS_ALPHAMASK` (`neo/renderer/tr_local.h:1036`) are four
    independent bits, set from the `maskRed` / `maskColor` / `maskAlpha` material
    keywords (`neo/renderer/Material.cpp:1414`) as well as by the shadow and depth
    passes.

    Not blocking, because there is an exact workaround and the example uses it:
    `BlendMode::Additive` is `(SRC_ALPHA, ONE)`, so a fragment written at zero alpha
    contributes nothing and the destination survives untouched. **That covers "write
    no colour at all" and nothing else** — masking *some* channels, which `maskRed`
    asks for, has no workaround. This is the only open gap that does not.

### Platform-side, smaller

8. **`Window::setFullscreen(bool)`** — the public `Window` API has no fullscreen
   toggle. `r_fullscreen` needs it.
9. **Modifier keys produce no key events** — PureDOOM's gap #2, still open. Doom 3
   binds Ctrl/Shift/Alt as ordinary keys. Workaround is PureDOOM's: diff
   `Keyboard::getModifiers(window)` once per frame into synthetic down/up events.
10. **Gamepad** — dhewm3 has full SDL gamepad support in `events.cpp`. Either build
    an eacp module or drop controller support for now (see scope cuts).
11. **Texture arrays** — PureDOOM's gap #12. Would collapse per-texture draw
    batching; matters more for Doom 3's draw counts than it did for DOOM's.

### Checked, and *not* gaps

- **`GPU::StreamingBuffers` already exists** (`Lib/eacp/GPU/Buffer/`) and is
  `idVertexCache`'s exact shape. An earlier draft of this plan had it as work to do.
- **Doom 3's 3D textures are dead code.** `idImage::Generate3DImage`
  (`neo/renderer/Image_load.cpp:736`) is defined and never called, so `TT_3D` never
  happens and eacp needs no 3D texture. Worth knowing before anyone builds one.

---

## 6. Plan

### Phase 0 — eacp — **done**, see §4

### Phase 1 — dhewm3 under SDL/GL: the backend seam — **done**

`idRenderBackend` (`neo/renderer/RenderBackend.h`) over the choke points that
already existed — the `GLS_*` bitfield, the two `*WithCounters` draw functions,
`GL_SelectTexture`, the draw buffer, the swap — with the existing bodies moved
unchanged into `RenderBackend_GL.cpp`. The free functions stay as one-line
forwarders, so the ~2000 qgl call sites above them were never touched.

`idImage::Bind` is deliberately *not* behind it. The plan listed it as a choke
point and it is one, but putting it behind the interface means moving texture
creation and upload, which is Phase 2's work. Same for `backEnd.glState`: the
cache stays where it is because `idImage::Bind` and the tools read it from
outside the backend.

**297 of 297 frames identical**, against a canary that moves all 297.

#### The gate — `regression/`, and read its README

`regression/gate.sh record | capture LABEL | compare A B`. A recorded render
demo replayed frame-by-frame through `aviDemo` and hashed: playback is
frame-exact, so two captures of one build are byte-identical. About twenty
seconds a capture.

Three things it cost to get there, all written up in `regression/README.md`:

- **Playing a render demo crashed outright on demo data.**
  `StartPlayingRenderDemo` dereferences `guis/map/loading.gui` unconditionally
  and `demo00.pk4` does not ship one. Four sites in `Session.cpp` already
  treated `guiLoading` as nullable and five did not. Fixed.
- **Recording had to be scriptable, and gameplay is not.** The tour drives the
  camera through the map's own eighteen `info_location` entities with
  `setviewpos`. Two traps: the level opens on a cinematic that owns the camera,
  and `wait N` counts command-buffer executions rather than frames — the buffer
  runs several times per frame, so N is about six times the frames it holds.
- **The first answer the gate gave was wrong, and was perfectly reproducible.**
  `gate.sh` redirected `fs_savepath` but not `fs_configpath`, which dhewm3 keeps
  separately, so the `r_skipSpecular 1` canary archived that cvar into the real
  config and every capture afterwards read it back. Determinism was verified and
  was real; it said nothing about whether the configuration was the intended one.
  **Reproducible is not correct**, and a comparison harness that writes anywhere
  the program also reads is a feedback loop.

The demo data is already unpacked at `cmake-build-debug/neo/demo/demo00.pk4`
(`CMake/DemoData.cmake` fetches it at configure time), so the gate needs no
retail install.

### Phase 2 — cut the platform layer and the backend together ← **next**

Both at once, on one branch:

- **Platform**: delete `glimp.cpp`, `events.cpp`, `threads.cpp`. Bring up `App.h` /
  `View.h` / `Input.h` from PureDOOM. Bridge eacp's push-callback events into
  dhewm3's polled `Sys_GetEvent` via a ring buffer — dhewm3 already does exactly this
  for console events (`PushConsoleEvent`, `neo/sys/events.cpp:909`). Drive
  `common->Frame()` from `GPUView::update()`.
- **Renderer**: implement `idRenderBackend` on `eacp::GPU`.

*Why not a throwaway GL view in eacp first?* It was considered — a legacy-GL-backed
`View` would let the platform swap land independently with the game never breaking.
Dropped, because with PureDOOM's files to transcribe the platform swap is a day or
two, so the stepping stone only shortens a phase that is already cheap, and we would
pay for legacy GL plumbing we then throw away. The demo-replay gate from Phase 1 is
the better answer to the same worry.

### Shader inventory for Phase 2

Roughly 10–15 EDSL programs, each in the sampling variants §4.3 sizes — 8 worst case
for interaction, 4 for the generic stage, compiled lazily:

- interaction (bump / diffuse / specular) — the port of `interaction.vfp`
- depth fill, with alpha test (`setDiscardBelow`)
- shadow volume extrude (stencil) — `Apps/GPU/StencilShadows` is the worked example
- generic material stage, in its texgen variants: normal, reflect, skybox, wobblesky
- fog
- light blend
- 2D / GUI

The EDSL earns its keep here: one source per shader covering Metal and D3D12, against
Doom 3's original two hand-written ARB programs per path.

---

## 7. Scope cuts

Explicitly **not** ported:

- `neo/tools/` — radiant (220 qgl calls), CamWnd (107), guied, dmap. Windows-only,
  gated behind `ID_ALLOW_TOOLS`, and a large fraction of the total qgl count.
- `neo/renderer/tr_rendertools.cpp` — 345 qgl calls, all debug visualisation. Stub it,
  restore selectively if a specific view proves useful during the port.
- Gamepad support, initially (see gap 10).
- Depth bounds test (10 refs) — an optimisation, drop it.

**Linux — dropped. Decided, not discovered.** eacp's README is not stale:
`Lib/eacp/CMakeLists.txt:5` gates the entire graphics stack — `Graphics`,
**`GPU`**, `Text`, `Sprites`, `UI`, `SVG` — behind `(APPLE OR WIN32)`. Only
`Core`, `SIMD` and `Network` build on Linux, which is why CI builds Linux and
there is still no window there to put a frame in.

Three ways out were on the table: keep the SDL/GL backend alive behind the
Phase 1 seam and build it on Linux only; add Linux windowing and a Vulkan
backend to eacp (there is a `vulkan-backend` branch); or accept macOS + Windows.
**The third is the answer** — so this port ends dhewm3's Linux support, which
dhewm3 has today.

What that settles, and it is the reason the question had to be answered before
Phase 2: **the GL backend behind the seam is throwaway.** It exists to keep the
game running and measurable while the eacp one is written beside it, and it is
deleted when the eacp one passes the gate. No second backend to keep alive, no
GL path to keep honest, and no reason for `idRenderBackend` to stay expressible
in fixed-function terms a moment longer than the port needs.

Nothing has been deleted yet. `neo/sys/linux/`, the SDL layer and the POSIX
files still build, and there is no reason to remove them before Phase 2 wants
their call sites — but nothing is owed to them either.

---

## 8. Next steps

Phase 1's three steps are done: the gate is recorded and verified in both
directions, the seam is landed at 297/297, and Linux is decided (§7 — dropped,
which makes the GL backend throwaway).

Phase 2 is the first work that compiles against eacp. In rough order:

1. **Stand up the eacp app shell** — `App.h` / `View.h` / `Input.h` transcribed
   from `~/Code/PureDOOM/examples/EACP`, CPM in `neo/CMakeLists.txt`, a window
   that opens and does nothing else.
2. **Bridge events**, push-callback to dhewm3's polled `Sys_GetEvent`, through a
   ring buffer — `PushConsoleEvent` (`neo/sys/events.cpp:909`) is the pattern
   already in the tree. Drive `common->Frame()` from `GPUView::update()`.
3. **`idRenderBackendEacp` beside the GL one**, taking the gate's frames as the
   target. The two are selectable while the second is unfinished; the GL one
   goes when it stops being needed.
4. **Delete** `glimp.cpp`, `events.cpp`, `threads.cpp`, `neo/sys/linux/`, SDL.

The gate is the whole reason this can be done in that order rather than as one
jump. It is also worth re-reading `regression/README.md` before trusting it on
a backend it has never seen: it was built against a renderer whose output it
already knew, and the first thing it said was wrong.
