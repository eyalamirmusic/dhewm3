# Porting dhewm3 to eacp

Moving dhewm3 off SDL2 + OpenGL and onto [eacp](https://github.com/eyalamirmusic/eacp):
app lifecycle and message loop first, GPU rendering (Metal / D3D12) as the real work.

**Status: Phase 0 is done and merged. Phase 1 has landed its gate and its seam.
Phase 2 is under way: the app shell, the threading, the engine's own boot and
now the input are in — the engine runs headless on eacp and is driven by a
keyboard and a mouse. The renderer is next.**

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
| `CMakeLists.txt` | Consuming eacp via CPM, including the `EACP_MACOS_PLIST` workaround (gap #3) and `eacp_set_gui_subsystem`. Not `set_default_target_setting` — see §6, step 2b. |

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
   toggle. `r_fullscreen` needs it, and so does Alt-Enter, which
   `sys/events.cpp` handles and `sys/eacp/Input.cpp` therefore does not.
9. **Modifier keys produce no key events** — PureDOOM's gap #2, still open. Doom 3
   binds Ctrl/Shift/Alt as ordinary keys — `_attack`, `_strafe` and `_speed` in
   the stock config. Worked around as PureDOOM does, by diffing
   `Window::getModifiers()`, but **not only once a frame**: step 3 found that a
   tap between two refreshes never differs from the state the poll last saw and
   is dropped whole, so the diff also runs from each key and mouse event's own
   `modifiers`. That fixes the ordering too — the modifier's down now lands
   ahead of the key it modifies rather than a frame behind it. A modifier
   pressed entirely alone is still only as good as the frame poll, which is the
   irreducible part of the gap.

   `ModifierKeys` also does not say which side was pressed, so K_SHIFT and
   K_RIGHT_SHIFT (and the Ctrl and Alt pairs) cannot be told apart. The stock
   config binds the left names, so both keys act as the left one.
10. **Gamepad** — dhewm3 has full SDL gamepad support in `events.cpp`. Either build
    an eacp module or drop controller support for now (see scope cuts).
11. **Texture arrays** — PureDOOM's gap #12. Would collapse per-texture draw
    batching; matters more for Doom 3's draw counts than it did for DOOM's.
13. **Continuous mode stops when the display sleeps** — and so, now, does the
    engine, because `GPUView::update` is what drives `common->Frame()`. Measured
    rather than inferred: with the panel asleep `CGGetActiveDisplayList` reports
    **0 active displays** and `CVDisplayLinkCreateWithActiveCGDisplays` fails
    with `-6661`, so `startContinuous` builds a link that never ticks and the
    window still paints the three frames layout asks for — which is exactly
    enough to look like it is working. `CVDisplayLinkCreateWithCGDisplay(
    CGMainDisplayID())` succeeds in the same process at the same moment, so the
    failure is the *active* display list and not CoreVideo.

    Arguably right — a game whose screen is off has nothing to draw — and it is
    not what `while (1) common->Frame()` does, which keeps stepping the
    simulation, the sound and the network. Worth deciding rather than
    inheriting, and worth knowing before someone spends an hour looking for the
    bug in this port. (Someone did. `caffeinate -du` wakes the panel and every
    measurement here was taken with it held awake.)

14. **The pointer can only be hidden by locking it.** `Window::setMouseLocked`
    hides the cursor, pins it and streams relative motion as one decision,
    which is exactly `GRAB_HIDECURSOR | GRAB_GRABMOUSE | GRAB_RELATIVEMOUSE`
    and covers Doom 3 in game. What has no answer is the menus, which want the
    cursor hidden while the pointer stays free, because Doom 3 draws its own —
    so the system arrow shows through them. Cosmetic, and it needs `MouseCursor`
    to grow a `None` or the window a `setCursorHidden`, not a new mechanism.

15. **Mouse buttons past the third are undifferentiated.** `MouseButton` is
    `Left`, `Right`, `Middle`, `Other`, and every extra button reports `Other`,
    so there is nothing to tell them apart by. Doom 3 binds eight
    (`K_MOUSE1`..`K_MOUSE8`); this build binds three.

16. **`Keyboard::keyCodeToCharacter` folds in live modifier state on Windows and
    not on macOS.** Found by writing the key table (§6, step 3), which resolves
    a printable key by translating its positional code through the layout. The
    macOS implementation calls `UCKeyTranslate` with no modifiers; the Windows
    one calls `ToUnicode` with the live `GetKeyboardState`, so holding Shift
    turns `;` into `:` — and a key that resolves one way on its down and
    another on its up is never released by the engine. The two backends
    disagree and one of them is wrong, so it is a bug rather than a constraint,
    and it has to be fixed before the Windows host (§8) can use this table.
    `ToUnicode` with a live keyboard state can also consume a dead key and
    corrupt the character after it, which is the same fix.

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

### Phase 2 — cut the platform layer and the backend together ← **in progress**

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

#### Step 1 — the app shell — **done**

`neo/sys/eacp/` — `App.h`, `View.h`, `View.cpp`, `Main.cpp` — and a
`dhewm3-eacp` target beside `dhewm3`, built by `cmake -DEACP=ON`. It opens a
1024x768 window titled `ENGINE_VERSION`, clears it, and does nothing else. The
window is `setStencil(true)` and `setContinuous(true)` from the start, because
both are properties the engine will need rather than choices: Doom 3's lighting
is stencil shadow volumes, and a game redraws every refresh whether or not
anything the platform layer can see has changed.

**A second executable, not a switch on the first**, and it stays that way for
the whole of Phase 2. The regression gate measures the SDL/GL binary, and a
port is only measurable while the thing it is measured against still builds and
runs. Both targets build from one configure.

Two things it cost, and both are worth more than the window:

- **The C++ standard was measured, not assumed.** eacp is C++20;
  `neo/CMakeLists.txt:53` pins the tree to C++11 for Dear ImGui and the Doom 3
  code. Rather than guess whether the engine survives C++20, every one of the
  359 translation units this tree builds was recompiled at `-std=c++20`. **126
  failed, and all 126 are in `neo/game` and `neo/d3xp`** — the game code, which
  builds as separate shared libraries and can stay at C++11 indefinitely. The
  engine, idlib, the renderer and `neo/sys` are already C++20-clean, so when
  `dhewm3-eacp` grows to hold `${src_core}` the standard is not what will stop
  it.

  The game code's failure is one identifier, not a language problem: a
  parameter named `requires` in `Game_local.h`, `Mover.h` and `Trigger.h` (and
  their `.cpp`s), doubled because `d3xp` is a copy of `game`. Worth knowing if
  `HARDLINK_GAME` is ever wanted alongside eacp — that option *would* pull the
  game code into the C++20 target, and a rename is the whole fix.

- **`GIT_TAG main` under a CPM source cache does not mean main.** The first
  configure fetched eacp, reported success, and failed on an unknown
  `eacp_set_gui_subsystem` — because CPM keys its cache on the *declaration*, so
  a branch name is cloned once and never looked at again. What it had was eacp
  at `0ba29cf`, from four months earlier, with none of the Phase 0 stencil work
  this port is built on. A branch tag under a source cache is not "we track
  main"; it is "we track whatever main was the first time this machine
  configured it" — neither current nor reproducible. `CMake/Eacp.cmake` now
  pins a SHA, which is at least honest and makes a bump a commit somebody can
  see.

#### Step 2a — threading, off SDL and onto the standard library — **done**

`neo/sys/threads.cpp` rewritten on `std::thread` / `std::recursive_mutex` /
`std::condition_variable_any`, and `xthreadInfo::threadHandle` retyped from
`SDL_Thread *` to an opaque `sysThread_t *` — nothing outside `threads.cpp` does
anything with it but compare it against `NULL`. `tools/debugger/debugger.cpp`,
which was calling `SDL_CreateThread` directly and bypassing the engine's own
API, now goes through `Sys_CreateThread` too.

Done in the *shared* file rather than a new `sys/eacp/` one, because nothing
here needs a window system: both executables use it, which also makes it the
one piece of the platform swap the Phase 1 gate can measure. **297 of 297
frames identical.**

**`Sys_EnterCriticalSection` has to be recursive, and nothing says so.** The
first version used `std::mutex`, compiled without a warning, and hung on the
first capture. `idSoundWorldLocal::ProcessDemoCommand` takes
`CRITICAL_SECTION_ZERO` around `ReadFromSaveGame`, whose first act is
`ClearAllSoundEmitters`, which takes it again (`snd_world.cpp:340` and `:203`).
That was never a bug: both implementations this API has ever had are recursive —
Win32's `CRITICAL_SECTION` by definition, and `SDL_CreateMutex`, which
documents itself as reentrant. The API's own header says neither.

Two things worth keeping from it:

- **The deadlock was on the one path the gate drives and a hand-run of the game
  might not have reached** — replaying a *render demo* is what calls
  `ProcessDemoCommand`. A port that swapped the platform layer wholesale and
  tested by playing would plausibly have shipped this.
- **`condition_variable_any` follows from the recursive mutex**, and brings its
  own constraint: `wait()` unlocks once, so a thread that entered
  `CRITICAL_SECTION_SYS` twice and then waited would release neither. Nothing
  does — `Sys_WaitForEvent` takes the section itself — and the SDL version was
  under the same constraint for the same reason.

What the gate covered, and what it did not. Every run creates two threads:
`AsyncThread` (`Common.cpp:3176`) and `backgroundDownload`
(`FileSystem.cpp:2701`), so create, destroy, the blocking `Sys_WaitForEvent`
and the recursive enter were all exercised across 297 frames.
**`Sys_TriggerEvent`'s signalling branch was not** — it fires from
`BackgroundDownload`, which only runs when `image_useCache` is on, and it
defaults to 0. Re-run with `image_useCache 1` it completes, and the SDL build
and this one produce byte-identical frames under it. (That configuration moves
181 of the 297 frames against the default one — the cvar genuinely changes what
is drawn, which is worth knowing before anyone captures a baseline with it set.)

#### Step 2b — the engine, booted headless — **done**

`dhewm3-eacp` holds `${src_core}` now, and the engine comes up inside
`Apps::run`. What that took:

- **`sys/eacp/Platform.mm`** — `Sys_GetPath`, `Posix_GetExePath` /
  `Posix_GetSavePath`, `Sys_Shutdown`, `Sys_GetSystemRam`. This is
  `sys/osx/DOOMController.mm` with its `SDL_main` removed: that function is a
  second entry point and a second `while (1) common->Frame()`, and the eacp
  target has `Apps::run` for both. The rest of the file was never about SDL.
- **`sys/eacp/GLimp.cpp`** — every `GLimp_*` entry point, all but three of them a
  `FatalError` naming `com_skipRenderer`. The renderer is step 4, so the boot
  runs with `com_skipRenderer 1` and `R_InitOpenGL` is never reached.
- **`sys/eacp/Input.cpp`** — the event queue, with only the console half filled
  in. `sys/events.cpp` pushed terminal lines into *SDL's* queue as
  `SDL_USEREVENT` and read them back in `Sys_GetEvent`; this is the same ring
  without SDL under it, and step 3 adds the keyboard and mouse as further
  producers rather than a second mechanism.
- **`View::update`** starts the engine on the first refresh and calls
  `common->Frame()` on every one after it. Started from the view rather than
  from `main()` because step 4's renderer will want a window, and `update` is
  the first place there certainly is one.

**Measured, both directions.** The eacp build's boot log is line-for-line the
SDL build's under `com_skipRenderer 1`. Everything the diff reports is either a
number that cannot repeat (pid, script compile time, the timer calibration) or
a statement about the platform layer: `SDL video driver: (null)` against
`cocoa`, one eacp line asking for an app icon, and the two GL lines the SDL
build prints on the way out. `com_speeds 1` typed at its console reports
frames 17ms apart, so the engine is running at 60 on a 120Hz panel, which is
what `setMaxFps(60)` is there for. `quit` typed at the same console shuts it
down cleanly. And the SDL/GL build is still **297 of 297 frames identical**
through the gate, which is what the two shared-source changes below needed.

**The frame is paced by the display link, and the engine has to be told.**
`idCommonLocal::Frame` ends by sleeping until the next 60Hz tic *unless* vsync
is on and the display is running at 60 — under vsync the swap already blocked
for that long. Here the display link is the swap, and it calls `update` on the
main thread, so a sleep inside it would be sleeping inside the callback the next
refresh is waiting on. `GLimp_GetSwapInterval` answers 1 and
`GLimp_GetDisplayRefresh` answers 60 for that reason: not a claim about hardware
that isn't there, but the same statement the SDL build makes when vsync is on,
made by the host that is doing the pacing. `setMaxFps(60)` makes the other half
of it true on a panel that isn't 60Hz.

**Four things it cost, and each is worth more than the boot:**

- **`com_fullyInitialized` was set and never cleared, and nothing had ever
  asked.** The engine's own `quit` shuts down and then exits the process from
  inside the frame — and `exit()` runs static destructors, which destroy eacp's
  app, whose view then tried to shut down an engine that had already shut down.
  It died in the file system, as a *recursed* fatal error, which is a fatal
  error with its own reporting already torn down. One line at the end of
  `idCommonLocal::Shutdown` and a `common->IsInitialized()` in the view's
  destructor. The SDL build never noticed because nothing runs after its
  `exit()`.

- **Dear ImGui's `NewFrame` crashes on a null context, and the SDL build has the
  same bug.** `D3::ImGuiHooks::Init` is only ever called from `GLimp_Init`, so
  `com_skipRenderer 1` — or a dedicated server — reaches
  `ImGui_ImplOpenGL2_NewFrame` with no context at all. It *looks* guarded: there
  is an "all windows are closed" early-out right above. That early-out
  deliberately runs two frames before it takes effect, and two frames is enough.
  Fixed in the shared file, because it is not the eacp build's bug.

- **`+set` has to be appended to the command line, not prepended.**
  `ParseCommandLine` starts a new console line at each argument beginning with
  `+` and appends everything else to the line before it. A leading
  `+set com_skipRenderer 1` therefore swallows the user's first argument into
  its own value list if that argument does not itself start with `+`. At the end
  nothing can follow it.

- **`set_default_target_setting` is eacp's warning policy, not a target
  setting.** PureDOOM's template calls it and this target inherited the call.
  It adds `-Wall -Wextra -Wpedantic`, which is right for a 2026 library and
  produces 37 warnings from one Doom 3 translation unit, across some 350 of
  them — enough to bury anything worth reading — and turns on LTO for Release,
  which the SDL/GL target does not have. The bundle plist is the part
  this target came for, so it now sets that property itself.

Also: `DOOMController.mm` makes the bundle's `Resources` directory current and
`Sys_Error`s if it cannot. This bundle has no `Resources`, so that was a fatal
error on an empty question — nothing in the engine reads the working directory
(`Posix_Cwd` has no callers), so all it decides is where a relative path typed
at the console lands. It is now the directory the `.app` sits in, which is
`PATH_BASE`, which is where the game data is.

**macOS only, and deliberately named rather than written.** The eacp target used
to build on Windows too, as a window and nothing else. The engine underneath it
needs the `Sys_*` entry points that `sys/posix/` and `sys/osx/` own, and
Windows' copies of those live in `sys/win32/win_main.cpp` behind its own
`WinMain` — a second entry point, the same conflict `DOOMController.mm` had.
Splitting it is a pass of its own and one nobody on this machine can run, so
`dhewm3-eacp` is `if(EACP AND APPLE)` and says so at configure time.

**What is still SDL, and why it is still linked.** `renderer/qgl.h` includes
`SDL_opengl.h` for its GL types, `idlib/Lib.cpp` uses SDL's endian macros, Dear
ImGui has an SDL backend, and the script debugger uses SDL mutexes. All four go
with the GL backend in step 5. `SDL_Init` is compiled out of `Common.cpp` for
this target — it would open a second video driver, which on macOS means a second
`NSApplication` delegate fighting the one `Apps::run` installed.

#### Step 3 — the events, bridged — **done**

`sys/eacp/Input.h` and a grown `Input.cpp`: the keyboard, the mouse, the wheel,
the modifier keys, the grab and the key names, with the view forwarding eacp's
callbacks and deciding nothing. `GLimp_GrabInput` moved out of `GLimp.cpp` and
into `Input.cpp` — it is the input layer's, and the only reason it lives in
`sys/glimp.cpp` in the SDL build is that SDL's grab calls need the window handle
that file owns.

**A key is identified by the character it prints, not by where it sits — which
is the opposite of what PureDOOM concluded, and the argument does not carry
over.** Doom 3's `keyNum_t` is mostly ASCII: a letter key *is* its lowercase
character, which is why the stock config binds `w` and `a`. `sys/events.cpp`
resolves one by asking SDL what the key prints under the current layout, and
this does the same through `Keyboard::keyCodeToCharacter`, falling back to
`K_SC_*` for a key that prints nothing nameable — with the digit row, the
console key and every named key (`ENTER`, `PGDN`, `KP_HOME`) taken positionally
first, exactly where SDL takes them positionally.

PureDOOM's rule was "never resolve a key by the character the *event* carries",
and its reason was key up, not layout: the Windows backend fills
`KeyEvent::characters` on key down alone, so a down and its up resolve
differently and the engine never clears the key. Nothing here reads the event.
The positional `KeyCode` is translated through the layout, which is a pure
function of the code — the same answer on the down and on the up. What that
buys is a build that behaves like the one it is measured against on every
layout rather than only on US, and a `K_SC_*` half of `keyNum_t` that still
means what it means everywhere else. It also turned up gap 16: the Windows
`keyCodeToCharacter` is *not* that pure function, and has to be fixed before
the Windows host can rely on this.

**`Sys_InitInput` is called from inside `R_InitOpenGL`**
(`renderer/RenderSystem_init.cpp:831`, "input and sound systems need to be tied
to the new window"), so with `com_skipRenderer 1` nothing calls it and the
console-key layout detection would have answered 0 for the whole run. Made lazy
rather than given a second init path in the view, because the dependency is
worth removing rather than working around: what the key between Esc, Tab and 1
prints is a property of the keyboard layout and has nothing to do with whether
a window has a GL context.

**Measured with the engine's own instrument, not with print statements.**
`com_journal 1` (`framework/EventLoop.cpp:86`) records every `sysEvent_t` the
engine consumes into `journal.dat`, which is a complete, unbiased record of
what this layer produced — so the whole step was verified without adding a line
of debug code. Driven by synthetic keyboard and `CGEvent` mouse input against
the real window, with the demo's `demo_mars_city1` spawned so that bindings fire
(`idSessionLocal::ProcessEvent` only reaches `ExecKeyBinding` once a map is up;
before that every event is forced into the console, which is its own useful
half of the test).

What the journal showed, all of it correct: letters and punctuation as `SE_KEY`
plus `SE_CHAR`; the digit row and the keypad apart from each other (`7` against
`K_KP_HOME`); `K_ENTER`, `K_SPACE`, `K_UPARROW`, `K_F6`, `K_BACKSPACE` with its
`SE_CHAR`; the console key as `K_CONSOLE`; `K_SHIFT`/`K_CTRL`/`K_ALT` bracketing
the key they modify; `H` for shift+h and *no* `SE_CHAR` at all for ctrl+h or
alt+h; `K_MOUSE1`/`K_MOUSE2`; `K_MWHEELUP` ×3 for a three-line scroll;
`SE_MOUSE_ABS` at `512,368` — the centre of the 1024×768 view, top-left origin,
which is what confirms eacp's backing views are flipped; and `SE_MOUSE` at
exactly `dx=15 dy=10` per step of a four-step 60×40 drag.

**Three things it cost, and one thing that was found by looking:**

- **A once-a-frame modifier poll drops a tap whole.** PureDOOM diffs
  `getModifiers()` in `update`, and that is enough for a modifier that is
  *held* — which is what DOOM binds. Doom 3 binds `_attack` to Ctrl, and a
  Ctrl pressed and released between two refreshes never differs from the state
  the poll last saw, so the engine sees nothing at all. The diff therefore also
  runs from each key and mouse event's own `modifiers`, which fixes the
  ordering as a side effect: the modifier's down now lands ahead of the key it
  modifies instead of a frame behind it. Visible in the journal as
  `K_CTRL down, 'h' down, 'h' up, K_CTRL up`.

- **Nothing releases a key held across a Cmd-Tab.** macOS delivers key-up to
  the key window alone, so the platform simply never reports it and the player
  walks into a wall until that key is pressed and released again. The window's
  `onActivationChanged` now releases everything this layer believes is held.
  Verified by holding a key with a `CGEvent` down and no up, switching apps,
  and finding the up in the journal.

- **The wheel had no key-up, and that is a bug this build did not inherit.**
  `sys/events.cpp` sends `K_MWHEELUP` as a down and never as an up — a wheel
  has no released position to report. But `idKeyInput` has no notion of a key
  that is only ever pressed, so the wheel stays down for the rest of the
  process. It is agreement rather than divergence, too: the *other* consumer of
  the same event already does it, `idUsercmdGenLocal::Mouse` turning one
  `M_DELTAZ` into `Key(key, true); Key(key, false)`
  (`framework/UsercmdGen.cpp:1265`). Only the queue half was missing its up.

- **`rawDelta`, not `delta`, and the mouse therefore feels different from the
  SDL build.** SDL hands the engine whatever relative motion the system gives
  it, which on macOS has the pointer acceleration curve already applied. That
  curve exists so a cursor can cross a screen and still land on a target;
  applied to a camera it makes an identical flick of the hand turn a different
  amount depending how fast it was made. Doom 3 has `sensitivity` for the
  scaling, so the curve is not wanted twice.

**What was not measured, and why:** that the grab actually engages. Everything
above is a claim about events the journal contains, and the mouse *lock* is not
an event — it is `CGAssociateMouseAndMouseCursorPosition(false)`, a warp and
`[NSCursor hide]`. A synthetic `CGEvent` carrying an absolute position moves the
pointer straight through the association, so the pointer's coordinates cannot
tell a locked window from one that is merely receiving the motion, and the
obvious second probe, `CGCursorIsVisible`, has been unavailable since 10.9. The
wiring is one call deep and the transcribed `handleMouseGrab` decides it, so
this is a hole to close by hand — start a map, confirm the cursor disappears and
the pointer stops leaving the window — rather than an open question about the
code. It is worth closing before step 4, because a grab that never engages is
the difference between playable and not.

The gate is untouched by this step: nothing outside `sys/eacp/` and the eacp
block of `neo/CMakeLists.txt` was edited, so the SDL/GL binary is byte-identical
and its 297/297 still stands without re-running it.

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

1. ~~**Stand up the eacp app shell**~~ — **done**, §6. `neo/sys/eacp/` and the
   `dhewm3-eacp` target; a window that opens and does nothing else.
2. ~~**Boot the engine into it.**~~ — **done**, §6 steps 2a and 2b. Threading on
   `std::thread` at 297/297, then the `Sys_*` entry points and `common->Init`
   off `Apps::run` with `common->Frame()` driven from `GPUView::update()`. The
   engine runs headless: `GLimp_*` is stubbed and `com_skipRenderer 1` is
   appended to the command line until step 4.
3. ~~**Bridge events.**~~ — **done**, §6 step 3. eacp's keyboard, mouse and
   wheel callbacks push into the queue the terminal was already using; the key
   table resolves a printable key through the layout the way `sys/events.cpp`
   does, with `K_SC_*` behind it; the modifier gap (§5, 9) is diffed from both
   the frame and each event; the grab is `Window::setMouseLocked`. Verified
   against `com_journal 1`, the engine's own event recorder, rather than
   against added instrumentation.
4. **`idRenderBackendEacp` beside the GL one** ← **next**, taking the gate's
   frames as the target. The two are selectable while the second is unfinished;
   the GL one goes when it stops being needed.
5. **Delete** `glimp.cpp`, `events.cpp`, `threads.cpp`, `neo/sys/linux/`, SDL,
   and fold `dhewm3-eacp` back into `dhewm3`.

Off to one side of that order, and needed before the port is finished rather
than before the next step: **the Windows host**. `dhewm3-eacp` is macOS-only
from step 2b (§6), because `sys/win32/win_main.cpp` holds Windows' `Sys_*`
entry points behind its own `WinMain`. Splitting that file the way
`DOOMController.mm` was split is the whole of it.

The gate is the whole reason this can be done in that order rather than as one
jump. It is also worth re-reading `regression/README.md` before trusting it on
a backend it has never seen: it was built against a renderer whose output it
already knew, and the first thing it said was wrong.
