# Porting dhewm3 to eacp

Moving dhewm3 off SDL2 + OpenGL and onto [eacp](https://github.com/eyalamirmusic/eacp):
app lifecycle and message loop first, GPU rendering (Metal / D3D12) as the real work.

**Status: Phase 0 is done and merged. Phase 1 has landed its gate and its seam.
Phase 2 is under way: the app shell, the threading, the boot, the input and the
renderer are in, and the renderer now draws. `dhewm3-eacp` puts **Doom 3's main
menu on screen through Metal**, **loads a level**, **lights it** and
**shadows it** — the world is done, all three of the steps 4d was broken into.
`interaction.vfp` and `shadow.vp` are in the EDSL, the stencil shadow volumes
are counted two-sided in one pass over each volume, and at a pinned camera in
`demo_mars_city1` the two backends draw **the same 71 draws, 1644 triangles and
2376 shadow triangles**, volume for volume. 4e is seven steps in and its basket
is empty: the frame is composed into an app-owned **render target**, that target
is **read back to the CPU** — so the eacp build takes the game's own camera
shots, writes screenshots, and **runs the regression gate**, 297 frames hashed
and identical across two captures — it is **copied into an image**, so
`_currentRender` and `_scratch` are filled in, every 3D view gets **a pass of
its own**, so **mirrors and subviews work**, and now **the Mars sky and the
glass are on screen** — eacp grew **cube textures** (gap 5) and the texgen
variants are three programs on top of them — **the hangar fog is in the frame**
and the blend lights with it, and **`r_gammaInShader` corrects what it corrects
on OpenGL** and nothing else. Every comparison is a hash: over the whole
297-frame tour the eacp build now agrees with the SDL/GL build at a mean of
**0.83 of 255**, from 1.13 before this round, with every camera stop that moved
moving towards it. The over-bright frame 4e.3 carried as a loose defect is closed
too, and was never the port's: it is the level's dropship headlight, drawn by
both builds at the same instant, and what was wrong was a reproducer that
compared two different moments. What is left of Phase 2 is the `newStage`
programs nothing on the gate reaches, and then step 5 — the deletion.**

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
- `RenderPassDescriptor::depthAction` — a pass that keeps the depth and stencil
  it was handed, which is what an interrupted one needs (§5, gap 22)
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
  needed from eacp. **Taken, in step 4e.1**, and it brought two things this entry
  did not predict: the multisampling has to go (gap 20), and the same shape is
  what a *pass per view* needs, not only what `_currentRender` does.
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

**That paragraph counted the wrong textures, and step 4d.2 found out by writing
the program.** The interaction shader samples *five*: the three above plus the
light's projected image and its falloff, and those two are declared by the
**light** material rather than the surface's — so a light that repeats where its
neighbour clamps is a second program, and a projection sampled with the wrong
address mode tiles a light across a level instead of dimming it. The key space
is `4⁵ = 1024`, not 8. What survives is the *decision* below, and it survives
because it was a lazy cache rather than a sized array: the demo's first level
reaches two of the thousand. The shape had to change with the number, though —
1024 slots is a list searched on a packed key, not an array.

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
**None of these blocks Phase 1, which touches no eacp API at all**, and none blocked
*starting* Phase 2 — each degrades the picture or has a workaround, and the list is
better driven by real content than guessed at now.

That last sentence has now been tested. 12, 17, 19, 20, 21 and 22 are what it
turned up — the gaps this port found by walking real content rather than by
reading eacp. 12 and 17 stopped the next step rather than degrading it, and both
are closed, in §6 under step 4b′. 19 does not stop anything today, because it is
D3D12's alone and the eacp host is macOS-only for other reasons; it stops
Windows. 20 is a price already paid, in multisampling, for what step 4e.1
bought. **21 is closed too**, in §6 under step 4e.2, and it is the one that was
stopping something worth having: eacp grew `Texture::read` and `Frame::flush`,
and this build's frames are hashes now rather than screen grabs. **22 is closed
by step 4e.3**, which needed a pass to survive being interrupted — the copy that
fills `_currentRender` has to end the pass, and everything drawn after it has to
be occluded by the depth buffer that pass was writing. **And 5 is closed by step
4e.5**, the one entry on the list that was written before the port began and
survived to be needed exactly as written: the Mars sky and every pane of glass
in the demo's first level sample a cube map.

Numbers are never reused, so a hole is an entry that closed.

### Needed, not blocking

4. **BC/DXT compressed texture formats** — all Doom 3 art ships as DXT1/3/5 in the
   pk4s. Without it: decompress at load, ~4× VRAM, much slower level loads.
5. ~~**Cube textures** — skyboxes and reflections.~~ — **closed**, and kept
   because two of its three users were gone before it was and the third needed
   more than "a texture with six faces".

   Step 4d.2 deleted the normalization cube map, as the entry predicted it
   could, and the `_ambient` map that stands in for it on an ambient light with
   it — that one becoming a uniform, since the whole point of the substitution
   is that the answer does not vary with the lookup. What was left is real cube
   sampling, and step 4e.5 is what needed it.

   `TextureDescriptor::cube` is six square faces of one size and one format,
   taken as **one block of pixels in +X, -X, +Y, -Y, +Z, -Z order, each face's
   row 0 at the top**. That is one convention rather than three: Metal's cube
   slice order, D3D12's array order under a `TEXTURECUBE` view and OpenGL's
   `GL_TEXTURE_CUBE_MAP_POSITIVE_X + i` agree on the order *and* on the
   orientation within a face, which is what lets Doom 3's own cube loader —
   whose `CF_CAMERA` half rotates and flips six camera images into exactly that
   arrangement — be uploaded untouched.

   **Both halves of that are pinned with a test, and the second half is the one
   worth having.** A face in the wrong slot, or flipped inside its own slot,
   still samples and still looks like a picture: a sky is a sky either way
   round, and a reflection of the wrong wall is still a reflection. There is no
   error, no validation message and nothing on screen that says so, and the only
   place it would ever show up is a comparison against another renderer. So
   `Tests/GPU/CubeTextureTests.cpp` holds a six-colour cube read along the six
   axes *and* a 2x2 face whose four corners are four colours, sampled at the
   four directions that hit its texel centres.

   The shader side is `ShaderBuilder::cubeTexture`, `Uniform<TextureCube>` and
   `sample(const TextureCube&, const Float3&)`, and **the interesting part is
   how little of it is new**. The graph records a *kind* per texture slot beside
   the sampling it already recorded, and that kind is read in exactly four
   places — the Metal fragment parameter, the Metal kernel parameter, the HLSL
   render global, the HLSL kernel global — because `t.sample(s, uv)` and
   `t.Sample(s, uv)` read both shapes on both backends, the coordinate's own
   width choosing. So `ExprKind::Sample` never asks what shape a texture is, and
   `RenderPass::setFragmentTexture` binds a cube through the same call on the
   same slot space. The declaration is the whole of the difference, which is why
   there is a codegen case on it.

   **What it costs each backend is not the same, and that is the other half of
   the finding.** Metal has a cube texture descriptor and a slice argument to
   `replaceRegion`, so the change there is a second descriptor and a loop.
   D3D12 has **no cube resource dimension at all** — a cube is a six-slice 2D
   array, and it is the *view* that makes it one, which means the SRV can no
   longer be created from the null description every 2D texture here takes. A
   `TextureCube` declaration reading a `Texture2DArray` view is a dimension
   mismatch: the debug layer says so and a release runtime samples nothing.

   Two shapes are refused rather than half-supported, identically on both
   backends: a cube that is not square, and a cube asked for as a render target
   or a kernel output — there being no way here to say which face a pass or a
   kernel would write. `Texture::update` takes all six faces for the same
   reason, and the region-shaped `update` and `read` are no-ops on a cube rather
   than quietly addressing +X.

   In the working tree of eacp at `e2df82a`, **uncommitted** as of this step —
   the dhewm3 build is pointed at that checkout with `CPM_eacp_SOURCE` until it
   lands on `main` — with the GPU suite at **261 tests, all passing** against
   253 before, clean under `MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1`
   and GPU validation. The D3D12 half is written and unrun, the eacp host being
   macOS-only (§8) — the same standing gap 22's entry carries.
6. **Depth bias / polygon offset** — decals z-fight without it, and step 4d.3
   found the second user: a shadow volume's near cap is the occluder's own
   triangles rebuilt through the extrusion, so it lands within an ulp of the
   depth that surface wrote and Doom 3 biases it a unit away
   (`r_shadowPolygonOffset -1`) to settle which side of the test it falls on.
   Nothing in the frames measured shows the difference, so this is the gap
   growing a user rather than becoming urgent.
7. **Mip filter selection and anisotropy** — currently `Linear|Nearest` ×
   `Clamp|Repeat` only. Doom 3 exposes trilinear and aniso as cvars.
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

18. **A second pass on a frame cannot keep the first one's colour, because
    multisampling resolves and discards it.** Doom 3 renders one *view* per
    pass — a pass clears depth and stencil, which is exactly what
    `RB_BeginDrawingView` asks for — and every view after the first has to find
    the colour the ones before it wrote. `Frame::beginPass` with `clear = false`
    is the call for that, and it is right on a single-sampled target: the
    drawable is attached with `MTLStoreActionStore` and loaded back. With MSAA
    it is not. The MSAA texture is attached instead, with
    `MTLStoreActionMultisampleResolve`, which resolves into the drawable and
    stores nothing — so the next pass's `MTLLoadActionLoad` reads an attachment
    nothing kept. `StoreAndMultisampleResolve` is the shape that would work.

    Read out of `Frame-Apple.mm` rather than measured, because the port took the
    other road: it uses one pass for the whole frame, which clears depth once
    and is therefore right for one 3D view and wrong for two. Step 4d.1 says so
    where it decides it, and it is what made subviews and mirrors 4e's rather
    than a thing that nearly works.

    **Step 4e.1 stopped this binding without closing it.** The frame is composed
    into a texture now, and a texture target is single-sampled and stored — so
    `clear = false` on a second pass over it loads exactly what the first one
    wrote. The entry stays because it is still true of the drawable, and because
    the reason it stopped mattering is that this port gave up multisampling
    (gap 20) rather than that eacp gained `StoreAndMultisampleResolve`.

    **Step 4e.4 is where that was spent.** Every 3D view opens a pass of its
    own now, clearing the depth and stencil planes and loading the colour the
    views before it wrote, which is what makes the mirror in `demo_mars_city1`
    a reflection rather than the wall behind it. So `clear = false` is not only
    honest on this path, it is load-bearing.

19. **A shader can bind four textures on D3D12, and Doom 3's interaction program
    needs five.** `Lib/eacp/GPU/Windows/D3D12Types.h:25` is
    `constexpr int maxTextureSlots = 4`, and the render root signature is built
    out of it: one single-descriptor table per slot, with the storage-buffer
    registers starting immediately above at `RenderPass::bufferRegisterBase`,
    which is 4 and is `static_assert`ed to be no lower. `RenderPass-Windows.cpp`
    drops a `setFragmentTexture` past the fourth without an error. Metal has no
    such limit — 31 slots on every device eacp runs on — so this is one backend's
    ceiling rather than a shape both share.

    The interaction program's five are the bump map, the light's falloff, the
    light's projected image, the diffuse map and the specular map, and none of
    them can be folded into another: two belong to the light and three to the
    material, and every one is sampled at its own coordinate. Two of the ARB
    program's seven *did* fold away — the normalization cube map and the
    specular ramp are arithmetic now — and five is what is left.

    **Found by writing step 4d.2, which is macOS-only until this is raised.**
    That is not a delay: the eacp host has been macOS-only since step 2b for a
    different reason (§8, the Windows host), so this joins the list of things
    that must be true before Windows rather than blocking anything now. Raising
    the constant moves `bufferRegisterBase` with it, which is why it is a change
    to eacp rather than a number to edit.

20. **A texture render target cannot be multisampled.**
    `Frame::beginPass(texture)` is single-sampled by design — its own comment
    says a texture target "has nothing to resolve into", the texture being what
    a resolve would produce — while the drawable path attaches an MSAA texture
    and resolves into the drawable. So the shape is there on one path and absent
    on the other, and an app that moves its composition into a texture (step
    4e.1, and PureDOOM before it) gives up multisampling to do it.

    Measured rather than assumed: 0.8 of 255 on Doom 3's main menu, which is the
    screen with the most edges per pixel in the game, and 0.1 on a level view.
    Worth it for what the target buys, and cheap to close if it ever is not — an
    MSAA texture beside the target, resolved into it, is what the drawable path
    already does.

21. ~~**A texture cannot be read back to the CPU.**~~ — **closed**, and the
    entry is kept because what closing it turned out to need is worth knowing.
    `Buffer::read` existed and `Texture` had nothing like it; the only readback
    in eacp was `GPUView::renderNativeContent`, which renders a *new* frame
    offscreen for a `View` snapshot, and that is no use to an engine whose
    `render` is a whole simulation tick.

    Found by step 4e.1 arriving at the point where it would be used, and closed
    by step 4e.2 — in two calls rather than one, which is the finding.
    `Texture::read` is the obvious half: the blit into a shared buffer that
    `renderNativeContent` was already doing by hand, generalised, with a region
    and a stride. The half that is not obvious is that **it is not enough on its
    own**, because a read-back is asked for from *inside* the frame that drew
    the pixels, and a frame's commands do not reach the GPU until the frame
    ends. So the read would return the frame before it — silently, and on both
    backends. `Frame::flush()` is what makes it true: send what has been
    recorded and carry on recording. The two are separable and each has its own
    reason to exist, so they are two calls; `Tests/GPU/TextureReadTests.cpp`
    pins the pair, and fails without the flush.

22. ~~**A pass could not keep the depth buffer it was handed.**~~ — **closed**,
    and kept because the shape of the answer is the interesting part.
    `Frame::beginPass` cleared depth and stencil unconditionally and Metal
    stored neither, so a pass that ended came back empty. `clear` already said
    what to do with the colour and there was nothing that said it about the
    other two planes.

    Found by step 4e.3 and closed with it. What needs this is a **suspended**
    pass: a texture cannot be sampled by the pass rendering into it, so an app
    that copies the frame it has drawn so far — Doom 3's `_currentRender`, a
    refraction reading what is behind it — has to end its pass, copy, and open
    another over the same attachments, and everything drawn after the copy
    otherwise has nothing to be occluded by.

    `RenderPassDescriptor::depthAction` is `DepthAction::Clear`, `Keep` or
    `Resume`, which is **one enum rather than the two booleans the load and the
    store would be**, because the illegal pairing is the point: loading what the
    pass before was told to discard has no answer, and a three-valued name
    cannot say it. Clear is the default and is what every pass did before.

    Two things worth keeping. **The store is Metal's alone** — a D3D12 depth
    buffer is a committed resource resting in `DEPTH_WRITE`, so what a pass
    wrote is simply still there and only the clear has to be skipped; the
    backends differ in what the enum *costs* rather than in what it means.
    And **the depth half of the test would have passed without the store**:
    putting `MTLStoreActionDontCare` back leaves every depth case in
    `Tests/GPU/DepthActionTests.cpp` green on Apple silicon, the tile memory
    holding the values across the boundary anyway, and fails the stencil one.
    That is `RenderTargetDepthTests`' architecture-dependent silence one step
    along, and it is why that file tests both planes.

    On eacp `main` at `5d15c30`, with the full suite at **1314 tests, all
    passing** and the GPU half clean under `MTL_DEBUG_LAYER=1` with
    `MTL_SHADER_VALIDATION=1` and GPU validation. The D3D12 half is written and
    unrun, the eacp host being macOS-only (§8).

23. **A texture cannot be given a mip chain; eacp builds its own from level 0.**
    Deliberate on eacp's side — one filter shared by both backends, so that
    Metal's `generateMipmaps` and a hand-written D3D12 chain cannot produce two
    pictures (`MipChain.h`) — and `UploadImageLevel` says as much where it
    drops every level below the first. What it loses is `R_MipMap`'s
    `preserveBorder`: a `TR_CLAMP_TO_ZERO` image keeps its zero edge all the
    way down Doom 3's chain, and eacp's averages it away. Measured on
    `lights/headlights`, which is `zeroclamp`, while chasing the over-bright
    frame (§6): the two chains agree to level 4 and diverge below it, Doom 3's
    reaching zero at 2x2 where eacp's stays at the image's 74.8 of 255 average.

    Nothing measured shows it — dropping the chain of every such image moved
    no frame the investigation looked at — so this is the gap growing a name
    rather than becoming urgent, as gap 6 did. Where it would show is a
    projected light seen at a distance, sampling its low levels, spilling past
    the edge of its own image. The shape of a fix is `Texture::update` taking
    a chain the caller built, and eacp's own builder becoming the default
    rather than the only way.

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

#### Step 4a — the seam grows to cover coming up, drawing a view, and images — **done**

Phase 1's seam covered the per-draw state and the draws. It did not cover the
renderer *starting*, a view being drawn, the renderer going away, or a single
byte of texture — all four are plain OpenGL in shared files, so the eacp build
could not have reached any of them without a context. Two commits, both pure
moves on the SDL/GL build, both **297 of 297**.

**The frame.** `Init()` is the API half of `R_InitOpenGL` — the qgl entry
points, the driver strings, `R_CheckPortableExtensions`, the ARB programs —
ending with `glConfig.isInitialized`, because that is the backend saying it is
ready and nothing above it can tell. `Shutdown()` runs before `GLimp_Shutdown`
takes the window away, and is empty on GL: the context owns everything the
backend made, which is the one thing a GL backend gets for free and the one
thing the eacp one does not. `ReleaseTextures()` is the `qglBindTexture(0)` the
command loop ends on, the mirror of `SetDefaultState`.

**`DrawView()` is deliberately the whole of a backend's drawing.** It is
`RB_STD_DrawView` and it is not broken down further, because what Doom 3 does
inside a view — fill depth, add each light's interactions through the stencil,
blend the shader passes, fog — is a sequence of *ideas*, and every one of them
is expressed in fixed-function terms a modern API has no counterpart for. A
second backend reimplements the sequence rather than reimplementing calls
underneath it. That is the difference between this port and a GL emulator.

**The formats stay as GL's names.** The image entry points take `GL_RGBA8`,
`GL_ALPHA8`, `GL_COMPRESSED_RGBA_S3TC_DXT5_EXT`, and that is a decision rather
than an unfinished edge: half of that enum is the **.dds file format's** as much
as the API's, and Doom 3's whole format decision is written in it from
`SelectInternalFormat` down through `BitsForInternalFormat`, the reader and the
precompressed writer. A second backend mapping them onto what it has is much the
smaller job than moving the decision.

**Five image paths were not moved**, because an entry point nothing can call is
worth less than a note saying why. Each is already switched off by something
this port controls: `Generate3DImage` has no callers at all; `GenerateCubeImage`
is behind `cubeMapAvailable`; `UploadCompressedNormalMap` and `SetNormalPalette`
are behind `sharedTexturePaletteAvailable`; `WritePrecompressedImage` is behind
`image_useOfflineCompression`, which is 0 and is a content tool rather than a
rendering path.

One GL detail is now stated where it was duplicated: a cube map's filter state
is its own entry point, because Doom 3 forces clamp on it and applies neither
anisotropy nor LOD bias, and folding it into the 2D one would have been a change
of behaviour hiding inside a move.

#### Step 4b — the renderer comes up on eacp, and draws nothing — **done**

`com_skipRenderer` is gone from the command line. The frontend culls, builds
interactions, extrudes shadow volumes and issues its command list; the images
are on the GPU as Metal textures; `DrawView` is empty. Landing that separately
is the point: everything *around* the drawing is measurable before any of the
drawing exists.

**The two backends are chosen by the linker, not at runtime.** Each is the
answer for exactly one host — the SDL/GL executable has a context and no eacp
device, the eacp one has the reverse — so there is nothing to select between.
`RenderBackend_GL.cpp` left `src_core` and each target names its own.

**`Init()` states what this backend implements, where OpenGL asks a driver**,
and every `false` in it is an open gap from §5. One pays for itself immediately:
`textureCompressionAvailable = false` is what `CheckPrecompressedImage` tests
*before* it opens a `.dds` at all, and the pk4s carry every texture twice —
**3395 `.dds` beside 3771 `.tga`** in the demo — so refusing compression loads
the uncompressed original rather than needing a decompressor on day one. Gap 4
is therefore about load time and memory, not about a missing picture.
`ARBVertexBufferObjectAvailable = false` is the same shape: `idVertexCache` then
keeps its blocks in system memory and hands out plain pointers, which is what
the draws want until it moves onto `GPU::StreamingBuffers` wholesale.

**`common->Frame()` moved out of `View::update` and into `View::render`.** The
engine's frame *is* a frame: `idCommonLocal::Frame` issues its render commands
inside the call and the backend consumes them there, so the eacp `Frame` has to
be open around the whole of it — and eacp hands a `Frame` to `render()` and to
nothing else. One eacp pass per Doom 3 view, which is not an arbitrary mapping:
a pass clears depth and stencil unconditionally and can be told whether to clear
colour, which is exactly what `RB_BeginDrawingView` asks for.

**Measured, and the boot log is the instrument.** From `----- Initializing Game
-----` to the last line, the eacp build's log is **identical to the SDL build's**
on the same data, warning for warning and count for count. Everything before it
is either the platform (SDL's display-mode enumeration and window report against
one eacp line naming the view's size in both pixels and points) or the API (the
extension list and the ARB program loads against `GPU: Apple M5 Max`).
`com_speeds 1` reports 2294 frames at 16–17ms, which is 60 on a 120Hz panel.
`listImages` reports **176 images, 27.9MB**, every one through the new upload
path with no warning raised — so nothing reached the compressed or the
unsupported-source-format branch. The whole run is clean under
`MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1`.

**Three things it cost:**

- **`idVertexCache::Alloc` reads an uninitialised `vbo`, and it is dhewm3's bug
  rather than this port's.** `idBlockAlloc` hands back raw memory. The
  buffer-object path writes `vbo` immediately so nothing ever shows; with
  `r_useVertexBuffers 0` — or on a backend with no buffer objects to generate —
  the field keeps whatever was in that heap block and `Alloc` takes the GL
  branch on garbage. The header's own comment says "only one of vbo / virtMem
  will be set", which is what every reader assumes and what nothing enforced.
  Fixed where it happens, in the shared file.

- **`idRenderSystemLocal::InitOpenGL` called `qglGetError` directly**, and it
  was the last GL call left on the shared boot path. It is `CheckErrors`, which
  was already on the seam.

- **eacp's headers have to come before Doom 3's in any file that mixes them.**
  `idlib/Str.h` does `#define strcmp idStr::Cmp`, and the same for eight other
  `<cstring>` functions, so a standard header pulled in afterwards fails to
  compile on its own `using ::strcmp`. Worth knowing before the next such file.

**Doom 3's own mip chain is dropped in favour of eacp's**, and that is a real
difference rather than a detail. Doom 3 generates its chain and uploads it a
level at a time; eacp builds one on the CPU — deliberately, so Metal's
`generateMipmaps` and a hand-written D3D12 chain cannot produce two different
pictures — and has **no per-level upload entry point at all**. What goes with
Doom 3's chain is `R_MipMap`'s `preserveBorder`, which is what keeps a
`TR_CLAMP_TO_ZERO` image's zero edge intact all the way down, and
`image_colorMipLevels`. The first of those is a real artifact waiting to be
seen — a light projection texture bleeding at distance — and is the thing to
suspect if one turns up.

#### Step 4b′ — back into eacp: the blend equation and the write mask — **done**

Phase 0's pattern, run again mid-Phase-2. Step 4c is the 2D path, 4c's shader is
the generic material stage, and a material stage's blend comes out of a `.mtr`
file — so the first thing that step needs is a blend equation eacp could not
express. Counting the demo's 67 `.mtr` files sized it: of the ambient stages,
446 are `blend blend` and 39 are opaque (both already expressible), 746 are
`(ONE, ONE)`, and **roughly 256 are the `DST_COLOR` and `DST_ALPHA` families,
which had no expression and no shader-side dodge** — the missing operand being
the destination, which a fragment shader cannot read.

Fixed in eacp rather than worked around, which is what Phase 0 did with stencil
and for the same reason: the alternative is a backend that draws most of the
menu and then needs unpicking. eacp `3a0db3c`:

- `BlendFactor`, `BlendOperation` and `BlendState` — the equation in full,
  colour and alpha separately, as an optional on `RenderPipelineDescriptor`.
  Unset, `blendMode` decides and nothing written against that struct changes;
  set, it wins outright rather than merging.
- `blendStateFor(mode)` writes each named mode out in those terms, and **both
  backends build from it** — so a preset's meaning is stated once in the header
  instead of once per backend, which is one fewer place for the two to drift.
- `ColorWriteMask`, which closes gap 12 at the same time. The two were always
  siblings: both are the blend stage being partly exposed.

**1280 of 1280 tests pass**, GPU half clean under `MTL_DEBUG_LAYER=1` with
`MTL_SHADER_VALIDATION=1`, and `Apps/GPU/StencilShadows` drops the workaround it
was built to expose — additive at zero alpha — for a real `ColorWriteMask::none()`,
still putting the shadow over 10.1% of the frame.

Two findings worth keeping:

- **D3D12 rejects the `*Color` factors in the alpha slots and Metal accepts
  them, and that is a spelling rather than a capability.** In the alpha channel
  the alpha component of `SourceColor` *is* `SourceAlpha`, so the two express
  the same arithmetic and D3D12 refuses only the redundant form. The Windows
  backend substitutes, and one `BlendState` means one thing on both. It comes up
  at all because `glBlendFunc` sets one factor pair for colour and alpha
  together, so a material asking for `(DST_COLOR, ZERO)` is asking for it in the
  alpha channel too — which every Doom 3 material that asks for `filter` does.

- **The snapshot read-back un-premultiplies, and a blend test has to allow for
  it.** `GPUView::renderToImage` converts the premultiplied attachment to the
  straight alpha `Graphics::Image` holds, dividing each colour channel by the
  alpha beside it. A case that leaves the destination alpha at 0.5 therefore has
  the very numbers it is checking doubled on the way out, and one that leaves it
  at 0 reads back as transparent black. Six of the thirteen new cases failed on
  exactly that before each hand-built state pinned its alpha half to
  `(Zero, One)`.

And the pin moved with it: `CMake/Eacp.cmake` tracks `main` while the two
repositories are being written together. That needed more than the branch name —
CPM skips the download outright when the source directory exists, which is what
made an earlier `GIT_TAG main` fetch four-month-old eacp and report success — so
a function ahead of `CPMAddPackage` fast-forwards the clone and configure prints
which commit the branch resolved to.

#### Step 4c — 2D, and the menu is on screen — **done**

Everything Doom 3 puts on screen without a world goes through one path, and that
path now runs on Metal. `renderer/RenderProgs_Eacp.{h,cpp}` holds the generic
material-stage program, the two caches a modern API needs where OpenGL needed
none, and the streaming pool; `idRenderBackendEacp::DrawView` holds the walk,
which is `RB_STD_DrawShaderPasses` and `RB_STD_T_RenderShaderPasses` rewritten
rather than ported.

**The whole of a material stage is four lines of EDSL.** A texture sampled at a
transformed coordinate, multiplied by a colour that is part constant and part
per-vertex. What that replaces is a matrix stack, a texture matrix, `glColor`, a
colour array, `GL_COMBINE_ARB` and six `qglTexEnvi` calls — and, for
`SVC_INVERSE_MODULATE` with a non-white constant colour, a *second texture unit
bound to the white image* purely so a combiner had somewhere to put the multiply.

**Two caches, because a pipeline is compiled from all the state at once.** A
program per sampling configuration, because eacp bakes the sampler into the
shader (§4.3), and a pipeline per piece of Doom 3 state under it. Both lazy. The
whole main menu asks for **four programs and five pipelines**, which is a number
this build prints at shutdown rather than a number this plan guessed: §4.3 sized
the sampling half at four and deliberately left the state half to the content.

**Measured at the menu:** 102 draws, 542 triangles, 3 views a frame, clean under
`MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU validation on, and not
one warning from any of the paths this step leaves open — no stage skipped for a
texgen, no image without a texture behind it. The picture is the SDL/GL build's
element for element.

**Five things it cost, and the first is the one that would have cost a day:**

- **OpenGL clips against `-w ≤ z ≤ w` and Metal and D3D12 against `0 ≤ z ≤ w`,
  and 2D is where that difference is total rather than subtle.** The gui model's
  projection is `glOrtho( 0, 640, 480, 0, 0, 1 )` and every vertex it emits is at
  `z = 0`, which the matrix sends to `z_ndc = -1` — *exactly* OpenGL's near plane,
  and just outside everyone else's frustum. Uncorrected, the menu is not dim or
  offset or z-fighting; it is clipped away entirely, and the failure looks
  identical to a backend that never drew. One row of the matrix — `z' = (z + w)/2`
  — is the whole fix, applied on the CPU where the matrix is built, because it is
  a property of the API the matrix is going *to* rather than of the geometry it
  came from.

- **The alpha test in the `GLS_*` bitfield is dead code, and the real one is
  somewhere else.** `GLS_ATEST_EQ_255`, `GLS_ATEST_LT_128` and `GLS_ATEST_GE_128`
  have **zero** call sites in the whole tree; what Doom 3 actually alpha-tests
  with is `shaderStage_t::hasAlphaTest` and a register-valued threshold, applied
  by the depth-fill pass, which is step 4d's. So `setDiscardBelow` is not this
  step's, and the three shader variants that looked mandatory are not. Worth
  finding before building them, and doubly so because `LT_128` — keep where
  alpha is *below* the threshold — is the one of the three that
  `setDiscardBelow` cannot express at all.

- **`CT_FRONT_SIDED` culls `GL_FRONT`**, which reads backwards until you know
  Doom 3's triangles are wound the other way round from OpenGL's default. eacp
  names the convention rather than the call — counter-clockwise **in clip space**
  is the front face, spelled the same on both backends — and OpenGL's default
  front face is that same convention, so the two enums line up directly and
  `CT_FRONT_SIDED` is `CullMode::Front`. The mirror flip stays where `GL_Cull`
  put it.

- **The vertex is uploaded as `idDrawVert`, not repacked into something the
  shader layer likes better.** One `GPU::UNorm8x4` field is what says the colour
  is four bytes read as 0..1 rather than four floats, and with that declaration
  the sixty bytes the engine already holds *are* the vertex layout — no per-draw
  conversion pass, and `vertexInput(&member)` still reads the offsets off the
  struct. A `static_assert` against `idDrawVert`'s size and two of its offsets is
  what keeps the two from drifting into geometry that comes out scrambled.

- **The seam's `DrawIndexed` is real on this backend, and what it draws *with* is
  three fields.** In OpenGL every input to `glDrawElements` is context state,
  left there by whoever last touched it; here the program, the pipeline and the
  vertex buffer are arguments, so the walk sets three members where the OpenGL
  path sets GL's, and the draw reads them. Which means the draws still go through
  `RB_DrawElementsWithCounters`, so `r_showPrimitives` counts the same thing on
  both backends — and it means a draw arriving from a path this backend has not
  written yet finds them null and is a no-op rather than a draw against whatever
  the last one left bound.

**What the menu is missing is not 2D.** The Mars globe is a `renderDef` — a lit
`models/wipes/planet2.lwo` rendered into a sub-rectangle of the screen, which
`r_debugRenderToTexture 1` reports as `3d: 1, 2d: 2` on every frame. What this
build draws in its place is `guis/assets/mainmenu/marshighlight`, the 2D halo the
gui fades in *over* the model — so the dark disc in the screenshot is correct,
and the bright lit sphere the SDL build shows under it is step 4d's.

**Left open on purpose**, each already switched off by something this port
controls: texgens other than `TG_EXPLICIT` (skipped with one warning, and 4e's),
polygon offset (gap 6, and no 2D content asks), `GLS_POLYMODE_LINE` (`r_showTris`
wireframe, which eacp cannot express), and new-style ARB-program stages and soft
particles — both of which the *shared* code already skips on any backend that is
not `BE_ARB2`.

The gate is untouched again: nothing outside `RenderBackend_Eacp.cpp`, the two
new `RenderProgs_Eacp` files and the eacp source list in `neo/CMakeLists.txt` was
edited, so the SDL/GL binary is byte-identical and its 297/297 stands without
re-running.

#### Step 4d.1 — the depth fill, and the world in silhouette — **done**

The first of the world's three. `RB_STD_FillDepthBuffer` and
`RB_T_FillDepthBuffer` rewritten into `idRenderBackendEacp`, `DrawView` no
longer turning back on `viewEntitys`, and the ambient passes it already had now
running over a 3D view as well as a 2D one.

**What it draws is the level in black with its ambient stages on top** — the
light glows, the panel sprites, the sky, the screens — which is the whole of
what Doom 3 puts on a surface before a light touches it. That is not much of a
picture and it is not meant to be: what the step is *for* is the two after it.
An interaction pass can only run at `GLS_DEPTHFUNC_EQUAL` — touching the
fragments that survived and no others — once something has put every opaque and
perforated surface in the depth buffer at exactly its own depth, and a shadow
volume can only be counted against a depth buffer that is already right.

**The depth fill is not a second shader, because it is not a second
expression.** `RB_T_FillDepthBuffer` draws a material's alpha-tested stages
exactly as `RB_STD_T_RenderShaderPasses` draws its ambient ones — same texture,
same texture matrix — with the constant colour set to black and the alpha test
on. So it is `idEacpStageProgram` with two things added: an `alphaTestRef`
uniform, and the discard. The discard cannot be a uniform — a `discard` is a
branch the generated source either has or does not — so it becomes the *second
dimension* of the program cache, which is now `(sampling, alpha test)` and eight
wide worst case. The demo's whole main menu and its first level between them
reach **four of the eight**.

**`setDiscardBelow`'s threshold is a compile-time float and Doom 3's is a shader
register**, so the uniform goes on the other side of the comparison: discarding
below zero on `alpha - ref` is discarding below `ref` on `alpha`. The one
difference from `glAlphaFunc( GL_GREATER, ref )` is at exact equality, which GL
discards and this keeps.

**Both depth hacks survive, and they are one function here rather than three
pieces of context state.** `RB_EnterWeaponDepthHack` and
`RB_EnterModelDepthHack` each rebuild the projection matrix *from the view's* and
set a `glDepthRange` to go with it, so on eacp they are a different
`modelViewProjection` and a different `setViewport` — which takes the range as
two extra arguments and needs no second call. The hack is an argument to
`SetSpace` rather than read off the space, because a surface can refuse the one
its space asks for: a soft particle does (#3878), and its neighbour in the same
space does not.

**Measured against the SDL/GL build drawing exactly what this backend can
draw** — `r_skipInteractions 1`, `r_skipFogLights 1`, `r_skipBlendLights 1` —
at one pinned camera in `demo_mars_city1`:

| | views | draws | tris | shdw |
| --- | --- | --- | --- | --- |
| SDL/GL, lights skipped | 1 | 128 | 13777 | 9736 |
| eacp | 1 | 89 | 4041 | 0 |

**Those are the same numbers, and the first reading of them said the two were
three times apart.** `r_showPrimitives` prints `tris` as
`( c_drawIndexes + c_shadowIndexes ) / 3` and `draws` as
`c_drawElements + c_shadowElements`, so a backend that draws no shadow volumes
is not being compared like for like until the shadow half comes out of the other
one's totals: 13777 − 9736 = **4041**, exactly what the eacp backend drew. The
draw column follows from the same subtraction but cannot be checked against it —
`c_shadowElements` is never printed on its own — so what is measured is the
triangles, and 39 shadow draws is what the draw column *implies* rather than
what it says. Worth knowing before trusting that counter across a port.

**Over the gate's own reference demo, frame for frame, the difference is a
constant 10 triangles with a name.** `textures/decals/p_oppressive` has a
`TG_REFLECT_CUBE` stage, which needs cube maps (gap 5); the backend skips it and
says so once. That holds for as long as the two runs are looking at the same
demo frame, which is the demo's opening while nothing is moving — **and no
further, because `playDemo` is paced by the wall clock.** The two builds run at
different speeds and start sampling different demo frames, after which the
comparison is of two different pictures and says nothing; `com_fixedTic 1` does
not fix it. **A frame-exact eacp gate needs `aviDemo`, `aviDemo` needs the frame
back on the CPU, and that needs step 4e's render target** — so the eacp build
cannot be gated the way the GL one is until 4e lands, and until then a matched
still frame is the sharpest instrument there is.

The run is clean under `MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and
GPU validation, loads the level and quits without an assert, and reports **4
material-stage programs and 23 pipelines** for the level against the menu's 4
and 5.

**Three things it cost:**

- **The game crashes the eacp build at map load, and it is a framebuffer read
  rather than a draw.** `idObjective::Event_CamShot` fires as the level spawns
  and calls `idRenderSystemLocal::CaptureRenderToFile`, which was still plain
  `qglReadPixels` in a shared file — a null function pointer on this host. So
  the seam grew a `ReadPixels`, GL doing what it did and eacp answering `false`,
  and `CaptureRenderToFile` writing nothing rather than crashing. It is the one
  entry point whose answer differs by more than spelling: a modern API has no
  back buffer to read, only a render target somebody kept, so the real answer is
  4e's the same way `_currentRender` is. The screenshot path
  (`R_ReadTiledPixels`) is deliberately *not* moved — its front-buffer read and
  its Wayland special case are GL's own, and the gate's `aviDemo` goes through
  it.

- **One pass per frame, not one per view, and that is now a decision rather than
  an oversight.** A pass clears depth and stencil as it opens, which is exactly
  what `RB_BeginDrawingView` wants for a 3D view — but the colour has to survive
  from one view to the next, and with MSAA on it does not (gap 18). One pass a
  frame is therefore right for one 3D view and wrong for two, which is a subview
  or a mirror, and `BeginDrawingView` warns once when it sees one.

  **The reason expired with step 4e.1 and the code has not caught up yet.** The
  frame is composed into a texture now, and a texture target is single-sampled
  and stored, so a second pass over it with `clear = false` loads what the first
  one wrote. What kept this at one pass a frame is gone; what is left is the
  work of splitting it, which is 4e.4. The warning is still there and still
  honest until then.

- **`RB_STD_LightScale` can never do anything on this backend and is not
  ported.** It is the full-screen multiply that crutches up a backend whose
  blending range is eight bits, and it returns immediately unless
  `backEnd.overBright > 1` — which `RB_DetermineLightScale` only produces when
  the brightest light exceeds `tr.backEndRendererMaxLight`, and that is 999 for
  `BE_EACP` as it is for `BE_ARB2`. `RB_DetermineLightScale` itself is still
  called, for `backEnd.lightScale`, which 4d.2 needs.

**The gate re-run rather than reasoned about**, because this step edited three
shared files — `RenderBackend.h`, `RenderBackend_GL.cpp` and `RenderSystem.cpp`.
**297 of 297 frames identical** to the step 4b baseline.

#### Step 4d.2 — the interaction program, and the world is lit — **done**

`interaction.vfp` ported into the EDSL as `idEacpInteractionProgram`, driven by
`RB_CreateSingleDrawInteractions` through a `DrawInteractions` /
`CreateDrawInteractions` / `DrawInteraction` trio that is
`RB_ARB2_DrawInteractions` and its two companions rewritten. At
`GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK` and
`GLS_DEPTHFUNC_EQUAL` over exactly what 4d.1 filled.

**The measurement is the whole result: 114 draws and 6332 triangles on both
backends, at a pinned camera in `demo_mars_city1`.** The comparison is the
SDL/GL build at `r_shadows 0`, `r_skipFogLights 1` and `r_skipBlendLights 1` —
which is what this backend can draw — and it needs no correction term. That is
worth saying next to 4d.1's, where the same counters read three times apart
until the shadow half was subtracted out of one side: with `r_shadows 0` there
are no shadow volumes on either side, so `c_shadowIndexes` is zero and the two
numbers are the same numbers.

**Two of the ARB program's seven textures do not survive the port, and neither
is a simplification of what is drawn.** Both exist because an ARB fragment
program cannot compute what they hold:

- **the normalization cube map**, sampled to turn the interpolated vector to the
  light into a unit one. `normalize()` does that, and `interaction.vfp` already
  says so — the half-angle half of its own shader takes the arithmetic road,
  with the cube map commented out above it.
- **the specular lookup table**, a 256×1 ramp of `max(0, (d − 0.75) × 4)²`.
  `R_SpecularTableImage`'s comment says it is "the behavior of the hacked up
  fragment programs that can't really do a power function". Two multiplies here,
  and exact rather than quantized to the table's eight bits. The `min(d, 1)` in
  front of it is the sampler's clamp, which the curve needs and does not have.

**The fifth texture is what makes this step macOS-only, and it is eacp gap 19
(§5): `maxTextureSlots` is 4 on D3D12.** Bump, falloff, projection, diffuse and
specular is five, none foldable. Metal has 31.

**An ambient light's direction is a constant, and reproducing it faithfully
meant reproducing a bug.** The ARB path handles ambient lights by swapping one
texture — the normalization cube map becomes `_ambient`, whose every texel is
`tr.ambientLightVector`, so the "direction to the light" comes out the same
everywhere. There is no cube map here (gap 5) and none is needed, the whole
point of the substitution being that the answer does not depend on the lookup;
the constant is a uniform, with `w` selecting it.

What the constant *is*, though, is not `tr.ambientLightVector`.
`R_AmbientNormalImage` writes the vector's x into the channel a compressed
normal map keeps x in, which is alpha — and the fragment program applies that
swizzle to the bump map and not to this one, so what the shader has received
since 2004 is the texel's rgb with 1.0 where x should be. The uniform is
computed by encoding and decoding the same way rather than from the vector
directly, because the game's ambient lighting is what that produces.

**The decomposition is now shared rather than duplicated, and that took a shared
file.** `RB_CreateSingleDrawInteractions` turns one surface under one light into
the sequence of primitive interactions a program can draw — multi-stage lights,
multi-layer surfaces, the nospecular parm, the colour registers — and its own
comment says it "can be used by different draw_\* backends". It could not be:
four `qgl` calls sat in the middle of it, setting the modelview matrix, the
scissor rectangle and the two depth hacks. Those moved out to the two callers,
each saying them in its own terms — `qglLoadMatrixf` and `glDepthRange` on one
side, one `SetSpace` on the other, the depth hack being a modified projection
matrix and a depth range here rather than three pieces of context state.

**Two things this leaves undone and one it cannot do.** There is no stencil
shadow pass, so a light shines through whatever should be shadowing it — 4d.3,
warned once. There is no stencil clear either, which is the same step. And
`r_gammaInShader` has no counterpart: dhewm3 injects `r_brightness` and
`r_gamma` into every ARB fragment program, and no eacp program applies them.
At the defaults both are 1 and the injected code is the identity, so it costs
nothing today and is worth a line in 4e.

**Verified three ways.** The run is clean under `MTL_DEBUG_LAYER=1` with
`MTL_SHADER_VALIDATION=1`, loading the level and quitting without an assert, at
**6 programs and 26 pipelines** against 4d.1's 4 and 23 — so the level's whole
content reaches two of the interaction program's 1024 possible sampling tuples.
The counters match, above. And the picture matches: the same corridor, the same
specular on the panel edges, the same normal-mapped floor.

**That last comparison is by eye and has to be, which is worth stating
precisely.** The eacp build has no screenshot — `R_ReadTiledPixels` reads the
front buffer, and 4e's render target is what a modern API can offer instead — so
its frame can only be captured off the screen, while the GL build's comes out of
the framebuffer. **Those two paths do not agree**: the *same* GL frame reads a
mean RGB of (27.6, 32.3, 27.1) out of the framebuffer and (35.5, 41.1, 36.8) off
the screen, a 29% lift from the display's colour management. Against the eacp
build's own screen grab at (33.7, 39.0, 34.7) that leaves about 5%, over two
runs that are not on the same animation frame and one of which is at 4× MSAA.
Which is to say: the tone comparison says nothing yet, and knowing *why* it says
nothing is the useful part. The counters are the instrument until 4e.

**Step 4d.3 found what was missing and it was neither path's fault.** Compare
two *screen* grabs rather than a screen grab against a framebuffer read, at a
camera where the world holds still, and the two builds land 0.3 of 255 apart —
so the instrument was never the capture path, it was that the two runs were
looking at different animation frames. A framebuffer read is still what a *gate*
needs, and that is gap 21 rather than 4e's render target: the target landed in
4e.1 and there is still nothing in eacp that reads a texture back.

The plan's own stated first result for this step landed too: **the Mars globe on
the main menu**, which is a `renderDef` with a light on it and the one thing on
that screen 4c did not draw. It is there, with its terminator and its surface
relief, and the menu run reports no eacp warnings at all.

#### Step 4d.3 — the stencil shadow pass, and the world is done — **done**

`RB_StencilShadowPass` and `RB_T_Shadow` rewritten into
`StencilShadowPass` / `ShadowSurface`, `shadow.vp` ported into the EDSL as
`idEacpShadowProgram`, and `DrawInteractions` given the four calls in the order
Doom 3 makes them — global shadows, local interactions, local shadows, global
interactions, which is what makes `MF_NOSELFSHADOW` mean anything.

**The two facings are one pipeline, and that deletes most of the original.**
`RB_T_Shadow` is a four-way branch — Carmack's reverse or not, crossed with
`glStencilOpSeparate` being available or not — and the two halves without it
draw the volume two or four times to say what per-face stencil state says once.
eacp has per-face state on both backends (Phase 0, §4.1), so the port is one of
the four: the depth-fail count with the faces separate, in a single draw.
`r_useStencilOpSeparate` therefore has nothing to choose between and is not read;
`r_useCarmacksReverse 0` is not implemented and warns once, its difference being
a third pair of stencil ops for a "preload" nothing has run since the patent
expired in 2019.

**A pass cannot be cleared once it has begun, so the per-light clear is a
draw.** Doom 3 empties the stencil buffer inside each light's scissor rectangle
with `qglClear( GL_STENCIL_BUFFER_BIT )`; on both of eacp's backends the clear
is a property of the attachment being loaded, decided as the pass opens, and
this port has one pass per frame (4d.1). So the clear is a quad with
`StencilOp::Replace` writing the pass's reference value, scissored to the light —
and it goes through the *shadow* program rather than one of its own, because
with an identity transform and the light at the origin that program's extrusion
is the identity too.

**The stencil is a second key on the pipeline cache**, since what OpenGL leaves
in the context between two draws is compiled into an object here. Five
configurations cover Doom 3 — ignore, clear, count depth-fail, count depth-pass,
and the `GL_GEQUAL` mask the interactions are drawn through — with the two
counting ones in mirrored pairs, a mirror being what reverses which winding
faces the viewer. The reference value is *not* part of the key: it is pass state
on both backends, so it is set once per pass, which is right because Doom 3 uses
one value for the whole frame.

**Neither convention needed flipping, and it is worth saying why not.**
eacp's front face is the counter-clockwise winding in clip space,
which is also OpenGL's default, so `GL_FRONT` is `stencilFront`; that Doom 3's
increments look inverted against a textbook depth-fail volume is its own winding,
the same fact behind `CT_FRONT_SIDED` culling `GL_FRONT`. And `GL_GEQUAL` puts
the reference on the left of the comparison, as Metal and D3D12 both do — so
`ES_LIT` keeps the fragments whose count came back down to 128, which is the
ones no volume closed over.

**The measurement needed a camera the *world* holds still under, not just one
the player does.** At `demo_mars_city1`'s first stop the two builds agreed on
every drawing counter — 6332 triangles, 9763 vertices — and disagreed on the
shadow half by one draw, which took an hour to explain and is worth the
paragraph:

- **A shadow volume's triangle count is a silhouette, so it depends on the
  pose; a surface's does not.** Two builds looking at the same animating NPC
  from the same camera draw the same triangles and *different* volumes, and
  `R_CreateShadowVolume` returns nothing at all when no face of a model faces
  the light — so a pose difference can add or remove a whole draw. That is why
  4d.1 and 4d.2 could be measured at a camera with people in it and this could
  not.
- **`g_stopTime 1` freezes the world but not at a reproducible moment.** Printing
  `viewDef->renderView.time` at the shadow draws showed the two builds frozen 44
  seconds apart: `wait N` in a cfg counts command-buffer executions, the buffer
  runs a different number of times per frame in each host, and `com_fixedTic 1`
  does not close that. Two runs of *one* build are identical; two builds are not.
- **So the camera moved to one with no animating entity in view**
  (`setviewpos -3148 -2776 204 180`), where the answer is exact:

| | views | draws | tris | shdw tris | shdw verts |
| --- | --- | --- | --- | --- | --- |
| SDL/GL, fog and blend lights skipped | 1 | 71 | 1644 | 2376 | 5458 |
| eacp | 1 | 71 | 1644 | 2376 | 5458 |

  and the fourteen shadow volumes match **one for one on every field** —
  vertices, indexes drawn, indexes held, and both cap-skipping alternatives —
  which is a stronger statement than the totals, because it says the caps
  decision agreed surface by surface and not merely in sum.

**The picture agrees at 0.3 of 255.** Both builds' windows grabbed off the same
screen at that camera come out at a mean RGB of (62.4, 47.5, 34.9) and
(62.5, 47.6, 35.0), with a mean absolute difference of 0.3 per channel; amplified
sixteen times, what is left is a thin line along every geometric edge, which is
eacp drawing at 4× MSAA against GL's none. That is a much sharper comparison than
4d.2 could make — not because anything improved, but because the scene holds
still: 4d.2 was comparing a framebuffer read against a screen grab, two runs, and
two animation frames.

**And the shadows are visibly there**, which is worth checking rather than
assuming when the instrument is agreement with another backend: the same eacp
frame at `r_shadows 0` differs from itself at `r_shadows 1` by ten times as much
as it differs from the GL build — (3.1, 1.5, 0.5) per channel — and the
difference has a shape, the hard-edged wedge a machine casts across a lit floor.

**Three things it leaves, and one of them is new.**

- **The polygon offset is eacp gap 6, and the shadow pass is its second user
  after the decals.** A volume's near cap is the occluder's own triangles put
  through the extrusion's subtract-and-add rather than copied, so it lands
  within an ulp of the depth that surface wrote and which side of `LessEqual`
  it falls on is decided by rounding. Doom 3 biases it one unit away
  (`r_shadowPolygonOffset -1`) and settles the question. Nothing in the frames
  measured shows it, and the eacp-versus-GL difference image has no speckle in
  it at all, so this is logged rather than worked around.
- **`r_showShadows` is not implemented** — two of its three values draw the
  volumes as lines, and `GLS_POLYMODE_LINE` has no eacp counterpart any more
  than `r_showTris`' does. Warned once.
- **`r_useShadowVertexProgram 0` has no counterpart**: the extrusion is the only
  way this backend projects a volume, and the frontend builds the doubled cache
  for it because `backEndRendererHasVertexPrograms` is true for `BE_EACP`.
  Warned once.

**Verified the same three ways as 4d.2.** Clean under `MTL_DEBUG_LAYER=1` with
`MTL_SHADER_VALIDATION=1` and GPU validation, loading the level and quitting
without an assert, at **8 programs and 32 pipelines**. What the shadow costs is
measured rather than counted off the source: the same run at `r_shadows 0`
compiles **7 and 27**, so it is one program and five pipelines — its stencil
configurations, plus the masked variant of each interaction program the level
reaches. The counters match, above. And the main menu is untouched, which is the
check that the stencil dimension did not disturb the 2D path: no world, no
volumes, and the same picture 4c drew.

**The gate was not re-run, and this time that is a statement rather than an
omission.** The three files this step touched — `RenderBackend_Eacp.cpp`,
`RenderProgs_Eacp.{h,cpp}` — are compiled into `dhewm3-eacp` alone
(`neo/CMakeLists.txt`, `src_renderbackend_eacp`), so the SDL/GL build is byte
for byte the one the last capture was taken from. Unlike 4d.2, which edited
three shared files and re-ran it at 297/297.

#### Step 4e.1 — the frame lives in a render target — **done**

The first piece of 4e, and the one the rest of it is waiting on. `BeginPass`
opens onto an app-owned `GPU::Texture` rather than the drawable, and
`SwapBuffers` closes that pass and opens a second one over the drawable to draw
the texture across it. PureDOOM's `captureTarget` is the same shape (§3).

**Why it has to be a second pass on the same frame rather than a second frame.**
Passes on one frame are ordered by the queue, so a texture written by an earlier
one is legal to sample in a later one and neither backend needs a fence to say
so. What is *not* legal is sampling the texture a pass is rendering into — which
is the whole reason `_currentRender` needs this and why the blit cannot be
folded into the pass above it.

**The target is sized to `glConfig.vidWidth`, not to the window.** Every
viewport, scissor and screen coordinate in the renderer is measured against
those, so a target of any other size would put the picture somewhere the
renderer does not think it is; the blit then maps that rectangle onto whatever
the drawable happens to be. Which is also what makes a window resize scale the
frame rather than corrupt it — `GLimp_SetScreenParms` refuses to resize (gap 8),
so the two really can differ.

**It costs the multisampling, and that is a straight trade rather than an
oversight.** A texture target on eacp is single-sampled — it *is* what a resolve
would produce, so there is nothing to resolve into (§5, gap 20) — so the view is
set to one sample and every pipeline is compiled for one. Measured on the menu,
which is the screen with the most edges per pixel in the game: a mean of 0.8 of
255 against the same frame at 4×, and 0.1 on a level view. What the trade buys is
everything 4e is for, and one thing it settles immediately: `r_multiSamples`
defaults to 0, and until now the eacp build drew at four samples with no way for
the cvar to reach it.

**What it does not cost is the picture.** At the static camera of 4d.3 the
counters are unchanged and the frame still matches the SDL/GL build at a mean of
0.3 of 255 — the same number as before the target existed, which is the point:
the frame went through a texture and came out the same frame. Clean under
`MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU validation, at 9
programs and 32 pipelines against 4d.3's 8 and 31 — the blit being one of each.

**The owning pointers were made to say so while this landed.** The port's
pipelines, programs, textures and passes were held in raw pointers with matching
`delete`s; they are `eacp::OwningPointer` and `eacp::OwnedVector` now, which is
the same thing the containers were already doing for the shader programs with
`std::optional`. Two places kept a bare `new` and both have a reason:
`GPU::RenderPass` has no move constructor, so a new-expression is the only way
to build one from `beginPass`'s prvalue and hold it past the statement; and
`idImage::backendTexture` is a `void *` in a header both backends compile, so
`ReplaceTexture` is where ownership is carried by hand — in and out through
owning pointers, with no `delete` in sight.

**What this unblocks, in the order it is worth doing:**

- ~~**`ReadPixels`, and with it a frame-exact gate.**~~ — **done**, step 4e.2
  below.
- **`_currentRender`**, which is this same blit into an image's texture rather
  than into the drawable.
- **A pass per view.** Gap 18 said a second pass could not keep the first's
  colour because the multisampled drawable resolves and discards it; a texture
  target is stored and loads back, so `clear = false` now means what it says and
  subviews and mirrors stop being blocked by the frame's shape.

#### Step 4e.2 — the frame comes back, and the gate is the eacp build's too — **done**

The keystone 4e.1 was for. `idRenderBackendEacp::ReadPixels` reads the render
target the frame is composed into, so **`dhewm3-eacp` writes its own frames to
disk**: the objective camera shots the game takes for its own UI, the
`screenshot` command, and — the point of the step — `aviDemo`, which is what the
regression gate is built on. The gate has run on this backend at **297 frames,
byte-identical across two captures**, and different on all 297 with
`r_skipSpecular 1`, which is the same both-directions check §6's gate got before
anyone trusted it.

**It needed two things from eacp, not one, and the second was a surprise.**
`Texture::read` is what gap 21 asked for and is the blit
`GPUView::renderNativeContent` was already doing by hand. What that alone gets
you is *the previous frame*: the read happens inside `common->Frame()`, which
runs inside `GPUView::render`, and nothing a frame records has reached the GPU
until the frame ends. So eacp also grew `Frame::flush()` — send what is
recorded, carry on recording — and `ReadPixels` calls `EndPass` then `flush`
before it copies. `EndPass` is the other half and is a different reason: a
texture cannot be read while it is the thing being rendered into.

Both are on eacp `main` at `f6f0034`, with `Tests/GPU/TextureReadTests.cpp`
beside them — which fails when the flush is taken out, that being the only way
to know the test is measuring the thing it is named after. The D3D12 half is
written and unrun: the eacp host is macOS-only (§8), so nothing here has
executed it.

**The seam grew a `presented` flag, and the flag is OpenGL's alone.** Moving
`R_ReadTiledPixels` off `qgl` — it still called `qglReadPixels` directly, which
is why the gate could not see this backend — turned up two callers that want
different buffers. `CaptureRenderToFile` reads what has been drawn and not
shown; `R_ReadTiledPixels` reads what a swap has just put on the screen, and
Doom 3 spells that `glReadBuffer( GL_FRONT )`. On a backend that composes into a
target it owns there is one picture either way, so eacp ignores the flag and the
GL backend is where the two branches — Wayland's included — now live. The GL
build re-ran the gate at **297/297 identical**, which is what makes that a move
rather than a change.

**And it found a bug in the port that had nothing to do with reading pixels.**
The first camshot came back black. `DrawView` returned early when no pass was
open, on the theory that a view arriving then was a draw from outside the frame;
`idObjective::Event_CamShot` disproves it — it renders its camera from the
*game's* think, several commands before the frame that will show it opens its
pass in `SetDrawBuffer`. So a view with no pass now opens one, without the
colour clear, which is exactly the stale back buffer OpenGL draws that view over.
The picture matches the GL build's camshot.

**What the port gets for it.** Every measurement after this is a hash. The
comparisons up to here have been screen grabs agreeing to 0.3 of 255 — good
enough to say a picture is right, useless for saying a change moved nothing —
and `regression/gate.sh` now takes `GAME=dhewm3-eacp` and answers that question
for this backend the way it has answered it for the GL one since Phase 1.

Two things about running it, both in `gate.sh`'s own header now. The display has
to be held awake (`caffeinate -du`), because the engine is driven by the display
link and gap 13 stops it with the panel. And the two builds' hashes are not
comparable with each other — they are two renderers — so each is compared
against itself, which is the rule `regression/README.md` already states for
machines and GPUs.

#### Step 4e.3 — the frame into an image, and `_currentRender` — **done**

`CopyFramebufferToImage` is 4e.1's blit with the destination changed: the same
quad, the same program, into an `idImage`'s own texture instead of onto the
drawable. On OpenGL the frame is in the back buffer and `glCopyTexSubImage2D`
reads it; here it is in a texture this backend owns, and **a texture is copied
by drawing it**. So the entry that has been waiting since 4e.1 is one more
pipeline rather than one more idea.

Its users are `_currentRender` and `_scratch`: the post-process dump inside
`DrawShaderPasses`, which is no longer guarded away, and `RB_CopyRender`, which
is the wipe on a map change and the player-view effects — double vision, berserk
vision, influence vision. The last of those is what this step was measured on,
because it puts **the whole frame through `_scratch` and back on the screen
every frame**, so a copy that is wrong is a screen that is wrong.

**The rectangle arrives upside down and stays that way.** Doom 3 measures from
the bottom left and `glCopyTexImage2D` puts the row at `y` into the
destination's row 0, so what a material samples at `t = 0` is the *bottom* of
the screen — and every caller is written against that, drawing `_scratch` with
`t` running 1 to 0. So the copy maps the source region's bottom edge onto the
destination's first row. Confirmed by reading the texture back: the dump is the
scene inverted, which is what makes the picture upright.

**The power-of-two dance is kept although nothing here needs it**, extra
duplicated edge row and column included. eacp samples a texture of any size, but
`uploadWidth` and `uploadHeight` are what the renderer *above* the seam scales
screen coordinates by, so a backend that sized them differently would be
answering a question the shared code asks in another unit.

**Two things about the destination were found by getting them wrong.** OpenGL
copies as `GL_RGB8` — no alpha channel, so a material samples 1 — and eacp has
no three-channel format, so the blit writes `float4(rgb, 1)` outright; carrying
the frame's own alpha through makes the `blend blend` overlays translucent,
which reads as a picture that is simply too bright. And the sampler state
`glTexParameterf` sets on the GL texture at this point is `TF_LINEAR` /
`TR_CLAMP`, which on this backend is not state on the texture but the choice of
compiled variant, so it is written onto the `idImage` where the variant is read
from.

**It needed a pass to survive being interrupted, which is eacp gap 22** — closed
on eacp `main` at `5d15c30`. The copy has to close the frame's pass, and
`clear = false` only ever said what to do with the *colour*: everything drawn
after the copy came back to an empty depth buffer. `SuspendPass` / `ResumePass`
now bracket it, opening with `DepthAction::Resume` and re-sending the viewport
and the scissor, which are pass state on eacp and which Doom 3 sets once per
view rather than once per draw. `ReadPixels` was interrupting the pass the same
way and not putting it back, and does now.

**Measured.** The gate is **297/297 identical** against 4e.2, which is what says
the change costs the existing path nothing — the tour has no post-process
surface and takes no capture. What it *is* verified by is berserk vision at the
tour's own camera: the eacp build's picture through `_scratch` agrees with the
SDL/GL build's at a mean of **5.7 to 6.7 of 255** over four frames, against
**3.75** for the same camera drawn directly, the extra being what a 1024x512
downsample and a 4x upscale do to two renderers' edges. Clean under
`MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU validation, at 9
programs and 40 pipelines — the capture being one more pipeline on the blit's
one program.

**And it found what it took for a bug, which turned out to be the level.** The
paragraphs below are kept as they were written, because the evidence in them is
real and what it was missing is the lesson — see "The over-bright frame" after
step 4e.7 for what the moment is and why the SDL/GL build looked steady. Driving
the same camera with berserk vision on, the eacp build produces a **~3x
over-bright, flatly-lit frame for about two frames** at one fixed moment in the
level, and the correct picture on either side of it. It is worth being precise
about, because the shape of the evidence is what says it is not this step's:

- The SDL/GL build is flat at 30.2–32.0 over the same twenty-four screenshots;
  the eacp build reads 96.5 and 81.4 at shots 7 and 8 and 26–41 everywhere else.
- **It reproduces on the build before 4e.3**, where the same run has one frame
  at 13.2 against a 4.5–5.6 baseline — the same ratio over a screen that is
  nearly black because nothing had filled `_scratch` in yet.
- Removing the blit from `CopyFramebufferToImage` and leaving only the
  suspend and resume does not stop it. Neither does `r_useScissor 0`, nor
  having `ReadPixels` hand the pass back.
- `r_debugRenderToTexture 1` reports the *identical* command counts on the good
  frames and the bad ones — `3d: 1, 2d: 2, SetBuf: 1, SwpBuf: 1, CpyRenders: 1,
  CpyFrameBuf: 1` — so the frontend is emitting the same frame and the backend
  is drawing it differently.
- It is one-off rather than periodic: twenty-four screenshots at a fixed camera
  put it at 7 and 8 and nowhere else, so it is tied to a moment in the level
  rather than to a count of frames.

What it looks like is the frame composed without its colour clear and drawn over
what was already there, which is what a second view rendered outside the frame's
pass would do — 4e.2's camshot case, one step along. Not proven, and not
chased further here: it is a defect of its own with a reproducer, and 4e.3 is
measurable without fixing it.

*(Chased after 4e.7, and every bullet above survives with a different meaning:
the identical command counts were the same kind of frame around a scene that
had gained a light, "older than 4e.3" because the light was always there, and
"one-off" because a dropship passes once. The SDL/GL build was "steady" because
its twenty-four screenshots were taken forty seconds of level time earlier than
the eacp build's, and the paragraph after step 4e.7 says why.)*

**What is not here.** `TG_SCREEN` and `TG_SCREEN2`, the screen-space texgens a
material samples `_currentRender` *through*, are still skipped with the rest of
the texgen variants — nothing in the demo's content reaches them outside a
`newStage`, so there would be nothing to check them against. They join 4e.5, and
what this step gives them is the copy they need and the `uploadWidth` scale
factor to read it with. `CopyDepthbufferToImage` stays empty for the reason it
always had: `_currentDepth` feeds the soft-particle program, which is behind
`BE_ARB2`.

#### Step 4e.4 — a pass per view, and the mirror — **done**

What gap 18 has been pointing at since step 4d.1 decided it. A Doom 3 3D view
opens with the depth and stencil buffers emptied and the colour left alone, and
on eacp the only thing that can empty either buffer is a pass beginning — so a
3D view that finds a pass another 3D view has already drawn into **ends it and
opens its own**, with `clear = false` and `DepthAction::Keep`. That is the whole
change, and everything else in this step is what it turned out to need.

**The common frame is still one pass.** `SetDrawBuffer` opens the frame's pass
for its debug clear and the first view keeps it — the condition is *another 3D
view has drawn here*, not *a view has arrived* — so one world and the 2D over it
costs exactly what it did. Only a 3D view counts, because only a 3D view writes
depth: `DrawShaderPasses` forces a 2D one to `GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK`,
which is what `RB_BeginDrawingView`'s `glDisable( GL_DEPTH_TEST )` amounts to.
A suspended pass (4e.3) comes back with the buffer it left, so a resume does not
reset the answer either.

**The demo has a subview, and the gate was already standing in front of it.**
`textures/washroom/mirror` carries the `mirror` keyword, which is `sort
SS_SUBVIEW` and nothing else, and `R_GenerateSurfaceSubview` renders that kind
in place — mirrored camera, scissored to the surface, drawn *into the frame*
rather than into a texture. The mirror is at x ≈ −2647 in `demo_mars_city1` and
the tour's ninth stop is `setviewpos -2500 -1116 252 180`, which is 147 units
away looking straight at it. So this is measurable on the gate rather than on a
map written to measure it, and `r_debugRenderToTexture 1` says `3d: 2` there on
both builds.

**Measured.** **16 of 297 frames moved** — 132 to 147, one camera stop exactly —
and those sixteen go from a mean of **3.82 of 255** against the SDL/GL build to
**0.50**, which is better than the 0.8–1.5 the rest of the tour agrees at. The
other 281 are byte-identical, which is what says a frame with one view pays
nothing for this. Two further captures of the new build are identical to the
first. Clean under `MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU
validation, at 9 programs and **57 pipelines** against 42 at the same camera
before — the extra being the mirror's own, every state the washroom is drawn in
compiled a second time with the cull flipped.

**The cull flip, which is the other half of the step.** A mirror reverses the
handedness of the camera, so every triangle presents the winding it did not
before and `CT_FRONT_SIDED` has to cull the other face. `GL_Cull` folds
`backEnd.viewDef->isMirror` in per draw, by choosing the enum it hands
`glCullFace`; here the cull mode is *compiled into a pipeline*, so the flip has
to happen before the pipeline is looked up — which also makes it part of the
cache key, and so the mirrored and unmirrored draws of one material get the two
pipelines they need instead of sharing the first one compiled.
`backEnd.glState.faceCulling` stays what the renderer asked for, because the
shared code sets it to −1 to force the next `SetCull` through. Worth doing
rather than assumed: without the flip the same shot reads **16.5 of 255**
against the GL build where with it it reads 0.33.

**And the mirror clip plane, which is not what it looks like in the source.**
`RB_STD_FillDepthBuffer` binds `_alphaNotch` on a second texture unit and drives
its `s` from an object-linear texgen. Read the generator and the whole thing
collapses: the notch is two texels, alpha 0 then alpha 255, nearest and clamped,
and the texgen plane is the view's with 0.5 added to the distance — so a vertex
in front of the plane samples the second texel, one behind it the first, the
modulate combiner multiplies the fragment's alpha by that 0 or 1, and the alpha
test does the rest. The comparison it amounts to is `dot( plane, vertex ) < 0`,
and that is what the port writes.

Three things about it are worth keeping:

- **It is a uniform, not a third variant of the stage program.** The alpha test
  is a variant because a discard is a branch the source either has or does not;
  the plane changes the *value* being compared and nothing else. `(0, 0, 0, 1)`
  — every vertex a unit in front of it — is what every draw outside a subview's
  depth fill sets.
- **The two discards are one.** eacp's `ShaderGraph` holds a single discard
  node, so a second `setDiscardBelow` would replace the first rather than join
  it. `min( alpha − ref, distance )` is below zero exactly when either of them
  is.
- **It reaches only the perforated draws**, which is not a shortcut but what
  OpenGL does: `RB_T_FillDepthBuffer` enables `GL_ALPHA_TEST` inside the
  perforated branch alone, so on a solid surface the notch modulates an alpha
  nothing tests.

**It changes nothing at this camera, and that took proving rather than
asserting.** The mirror's own frustum already excludes what is behind its plane,
so the shots are identical to two decimal places with the clip in and out. What
says the path is live rather than dead: forcing every vertex in the subview
behind the plane moves the picture by **1.15 of 255 with a worst of 232** — so
the mirror view does contain perforated surfaces, the plane does reach them, and
the real plane simply rejects none of them here.

**What is not here.** The three subview kinds that render into a *texture* —
`DI_REMOTE_RENDER`, `DI_MIRROR_RENDER`, `DI_XRAY_RENDER`, which are
`R_RemoteRender` and friends cropping the view and calling
`CaptureRenderToImage`. Nothing in the demo pk4's two maps uses a material with
a `remoteRenderMap`, `mirrorRenderMap` or `xrayRenderMap` stage — the eight that
declare one are all in art the demo ships and the maps never place — so there
would be nothing to check them against. Both pieces they need are here: the
per-view pass, and the `CopyFramebufferToImage` step 4e.3 built.

#### Step 4e.5 — cube maps, and the texgen variants of the material stage — **done**

`TG_SKYBOX_CUBE`, `TG_WOBBLESKY_CUBE`, `TG_DIFFUSE_CUBE`, `TG_REFLECT_CUBE` in
both of its forms, and `TG_SCREEN` / `TG_SCREEN2`. Everything
`RB_PrepareStageTexturing` does except `TG_GLASSWARP`, which is not really a
texgen — it is a hand-written ARB fragment program that a texgen happens to
select, so it belongs with the `newStage` skip beside it.

**It needed eacp gap 5 closed first, and that is §5.** What the dhewm3 side of
it turned out to need is below.

**Three new programs, not one, and the reason is eacp's rather than a
preference.** A shader's textures are declared by the uniform members it lists —
all of them, whether `define()` samples them or not — and Metal's validation
layer rejects a draw with a declared texture left unbound. So a stage program
that grew a `TextureCube` would declare one on every `TG_EXPLICIT` variant as
well, and every one of those draws would then have to bind a cube it never
reads. The four samplings times the alpha test that step 4c compiled are
therefore untouched, which is also what keeps every gate frame without cube
content byte-identical:

- **`idEacpCubeProgram`**, in three texgen forms. `TG_SKYBOX_CUBE` and
  `TG_WOBBLESKY_CUBE` are **one** of them, because a skybox is a wobblesky whose
  matrix is the identity and `R_LocalPointToGlobal` through an identity 3x3
  returns the vector it was handed, bit for bit. So the sky pays three dot
  products a vertex and the port carries one program fewer.
- **`idEacpBumpyReflectProgram`** — `bumpyEnvironment.vfp`, which is not
  `environment.vfp` with a normal map bolted on: it works in **global** space,
  through the three model-matrix rows the vertex program gets as
  `program.env[6..8]`, where the unbumped one reflects a model-space eye vector
  about a model-space normal and turns with the object.
- **`idEacpScreenProgram`** — and `TG_SCREEN` and `TG_SCREEN2` are one program
  because they are one texgen written out twice: the two branches of
  `RB_PrepareStageTexturing` are identical line for line.

**The coordinate is computed in the shader and the frontend's buffer is
ignored, which is worth saying out loud.** `R_SkyboxTexGen` and
`R_WobbleskyTexGen` still run — they are frontend code this port does not touch —
and still write a three-component texture coordinate per vertex into the vertex
cache every frame. Nothing binds it. The same arithmetic is four instructions in
a vertex shader and costs neither the allocation nor the upload, so
`surf->dynamicTexCoords` is filled in and dead, and someone looking for the bind
should know that before they go looking.

The wobble matrix is the one thing that had to come *out* of the frontend, and
it came out as a function rather than as a copy: `R_WobbleskyTransform` is the
matrix half of `R_WobbleskyTexGen`, lifted whole, with the generator calling it.
Its `floatTime` is a parameter because the two callers sit on opposite sides of
the frontend/backend split. **297/297 on the SDL/GL build**, which is the point
of doing it that way — see the note on frame 99 below.

**Which form a reflection takes is `GetBumpStage() != NULL`, and the surprise is
how often that is true.** `idMaterial` adds an implicit `_flat` bumpmap stage to
any material that has a diffuse or specular stage and no bump of its own
(`Material.cpp:1678`), so most reflective materials take the *bumpy* path with a
normal map that is a constant — which is **not** the same picture as the
unbumped one, the two reflecting in different spaces. Named once each over the
tour, the demo's first level reaches:

| Program | Materials |
| --- | --- |
| `ECT_SKY` | `textures/skies/commoutside` |
| `ECT_REFLECT` (`environment.vfp`) | `textures/glass/glass1`, `glass2` |
| bumpy (`bumpyEnvironment.vfp`) | `textures/sfx/chiglass1blue`, `textures/outside/outfactory_new2`, `models/mapobjects/dropship/dropshipglassns`, `models/monsters/zsecurity/zsgogs2`, `models/characters/male_npc/security/gog`, `textures/decals/p_oppressive`, `textures/sfx/shatterglass01` |

`glass1` and `glass2` are the two with no lighting stage at all — a `maskcolor`
pass and a `cubeMap` pass — so they are the only two that never got an implicit
bump and the only two the unbumped program draws.

**`bumpyEnvironment.vfp` writes no alpha at all, and what to put there was
decided by the content rather than by the specification.** `MOV
result.color.xyz, R0` leaves `w` undefined under ARB_fragment_program, so there
is no number to copy. What there is instead is what the materials the demo's
maps place that reach it do with the channel: `chiglass1blue` and `p_oppressive`
are `blend add`, where the destination alpha becomes `src + dst`, and
`outfactory_new2` is `blend gl_dst_alpha, gl_one`, where it becomes
`src × dst + dst`. A source alpha of **zero** leaves the destination's exactly
as it was under both, so zero is the one value that is invisible on every path
the content actually takes, and it is what the port writes. (The first draft of
this step said the `maskalpha` glasses reached this program too. They do not —
they are the three with no lighting stage, and the unbumped program draws them —
and the conclusion survived the correction because it never depended on them.)

**The texture matrix is not applied to a cube coordinate, and that is a decision
rather than an omission.** GL's texture matrix multiplies the whole coordinate,
and a cube's is a direction: Doom 3's 2x3 mixes s and t into each other and adds
a translation built for the [0, 1] of an image, which on a direction vector is a
rotation about z plus an offset that means nothing. Two materials in the demo
pk4 combine the two — `shaderDemos/cloudySky` and `shaderDemos/skybox`, both
with `rotate` — and neither of the demo's maps places either, so there is
nothing to check a faithful reproduction of the mixture against. It is applied
on the *screen* texgens, where it is well defined and where OpenGL applies it to
the homogeneous (s, t, q) before the divide.

**Two more things the ARB reflect path does not do, and this does not either.**
The bump map is sampled at the surface's raw (s, t) rather than through the
stage's texture matrix — a vertex program bypasses GL's texture matrix outright,
so `scroll` on a reflect stage has never done anything on `BE_ARB2`. And the
colour: `environment.vfp` multiplies by `vertex.color` and the bumpy one
multiplies by nothing, where the three fixed-function cube texgens take the
ordinary `(modulate, add)` pair. A fragment program *replaces* the texture-env
combiner, so the second texture unit the fixed-function path binds the white
image on has no effect on a reflect stage and the stage's constant colour
reaches the shader only where `SVC_IGNORE` put it there with `glColor4fv`. Which
is `(0, c)` and `(1, 0)` — the same two uniforms, computed from a different rule.

**A cube arrives as six upload calls and leaves as one texture.**
`GenerateCubeImage` uploads level 0 of faces 0 through 5 in order and then their
mip chains; eacp takes a cube as one block of six faces in that same order, so
the six are gathered and the texture is created on the sixth. A bit per face
says which have arrived, so a loader that gave up half way through leaves the
image with no texture rather than with a cube whose missing faces hold the last
one's pixels. `UploadScratchImage`'s cube animation — six square faces stacked
into one tall image — needed nothing at all beyond the flag: the bytes it is
handed are already exactly the layout eacp takes, so where OpenGL makes six
`glTexSubImage2D` calls this makes one `Texture::update`.

**The cube's clamp moved rather than disappeared.**
`idRenderBackendGL::SetCubeImageFilterAndRepeat` forces `GL_CLAMP_TO_EDGE`
whatever the material declared — "no other clamp mode makes sense" across a seam
— so a cube declared `repeat` samples clamped. Here the address mode is baked
into a shader, so forcing it is choosing *which compiled variant* the draw goes
through, and `R_EacpSampling` is where it happens. What it costs is one fewer
variant rather than one fewer call.

**And a check that nothing else does.** eacp binds a cube and a 2D texture
through the same call on the same slot, so a shader declaring one and handed the
other is not a bind error — Metal's sampler reads a texture whose type the
shader does not expect and D3D12 reads an SRV of the wrong dimension, and
neither says a word. `Texture::isCube` is the only thing that can tell them
apart, and `TextureForStage` asks. It is reachable rather than theoretical: a
material may declare `cubeMap` and no texgen at all, and a `cubeMap` whose six
files are not all present falls back to the default image, which is 2D.

**Measured.** **115 of 297 gate frames moved** against 4e.4, and they are eight
of the eighteen camera stops exactly — 2, 5, 7, 9, 10, 11, 12 and 14 — with the
other ten byte-identical, which is what says a frame with no cube content pays
nothing for this. Every one of the eight moved *towards* the SDL/GL build, over
a fresh capture of it at the same 297 frames:

| Stop | Before (mean / worst) | After (mean / worst) |
| --- | --- | --- |
| 2 | **4.485** / 115 | **0.430** / 106 |
| 5 | 0.864 / 136 | 0.843 / 136 |
| 7 | 1.474 / 218 | 1.473 / 218 |
| 9 | 0.520 / 147 | 0.515 / 147 |
| 10 | 0.831 / 147 | 0.742 / 147 |
| 11 | 0.933 / 147 | 0.853 / 147 |
| 12 | 2.400 / 113 | 2.400 / 113 |
| 14 | 1.659 / 202 | 1.396 / 181 |
| whole tour | **1.134** | **0.891** |

Stop 2 is the one worth looking at: a tenfold move, and the difference is spread
over the whole lower half of the frame rather than confined to a window, because
what was missing there is the reflection a glass pane adds over everything
behind it. Two captures of the new build are identical on all 297 frames. Clean
under `MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU validation, at
**12 programs and 64 pipelines** against 4e.4's 9 and 57 at the same camera —
three more programs, which is one per row of the table above, and seven more
pipelines.

**The two programs no content reaches were compiled anyway rather than assumed
to work**, by asking for them once from a temporary probe: `ECT_DIFFUSE` and the
screen program both compile and both build a pipeline. That is the whole of what
can be verified about them here, and it is worth saying that the *picture* they
produce is unmeasured — nothing in the demo pk4 declares `texgen normal` or
`texgen screen` outside a `newStage`, and the two `wobblesky` materials the pk4
does carry are never placed. What the screen one has instead of a reference is
the copy step 4e.3 built and that step's own note about `uploadWidth`: the plane
rows would have to be scaled by it if the image sampled here were ever smaller
than the frame, and it is not on this backend, because `CopyFramebufferToImage`
keeps the padded size the shared code expects.

**One thing found that is not this step's.** The gate is **not byte-deterministic
on the SDL/GL build**: two captures of the same unmodified binary differ on
frame 99, by two pixels of two out of 255. It matters because the same frame
moves between a step-4e.2-era GL capture and one taken now, which reads like a
shared-code change until the second capture shows it moving without one. The
eacp build has been byte-identical across captures at every step including this
one. `regression/README.md` carries it now, with the other three things this
step taught the gate.

#### Step 4e.6 — the fog and the blend lights — **done**

`RB_STD_FogAllLights`, `RB_FogPass`, `RB_T_BasicFog`, `RB_BlendLight` and
`RB_T_BlendLight` rewritten, as two programs and one walk each. They go between
the two halves of `DrawShaderPasses`, which is where `RB_STD_DrawView` puts them
and for the reason its own comment gives: a post-process surface reads
`_currentRender`, and the fog has to be in the frame before it does.

**They are not interactions and that is the whole reason they are a step of
their own.** A fog light and a blend light sit in `viewDef->viewLights` beside
the real ones, and `DrawInteractions` has skipped both with a `continue` since
4d.2. Neither lights a surface: they *tint* everything inside a volume. So there
is no bump map, no half-angle vector and no tangent space in either program —
what there is instead is a coordinate that no vertex carries.

**Every coordinate either program samples at is a plane dotted with the vertex.**
That is what `glTexGen` with `GL_OBJECT_LINEAR` means, and on OpenGL the planes
are re-sent whenever `backEnd.currentSpace` changes. Here they are uniforms
rebuilt at exactly that moment — `R_GlobalPlaneToLocal` per space, which is what
`FillDepthBuffer` already does with a subview's clip plane and what
`R_SetDrawInteraction` does with `lightProjectionS/T/Q`. The blend light's
texture matrix is folded into the projection planes by
`RB_BakeTextureMatrixIntoTexgen`, the shared function `R_SetDrawInteraction`
already calls for the same four planes, rather than becoming a matrix in the
shader: the matrix acts on the *generated* coordinate, so it composes with the
planes instead of with the vertex.

**Both fog lookup images stay textures, and that is the opposite of what step
4d.2 decided about the specular ramp.** The reasons are opposite too. That ramp
was a curve an ARB fragment program could not evaluate — the table was the
workaround and `max(0, (d − 0.75) × 4)²` was the intent. These two are not:
`R_FogImage` is `1 − 0.982^d` accumulated over 256 steps, and `R_FogEnterImage`
is `FogFraction`, a piecewise function of two heights with four cases and a
deep-water blend. Reproducing either in the shader would be reproducing a
generator rather than an intent, and keeping them keeps the picture closest to
the GL build's, which is what every step here is measured against. Neither
image can vary: both are generated `TF_LINEAR` / `TR_CLAMP`, so the fog program
has one sampling tuple by construction and needs no variant list. The blend
light's two images *are* the light material's, so it has one keyed on two
sampling indices, the way `InteractionDraw`'s is keyed on five.

**The `_fog` image is two-dimensional and only its middle row is ever sampled,
and the original says so by overwriting its own texgen.** `RB_T_BasicFog`
computes `fogPlanes[1]` — the view's right axis, which would make the lookup a
real two-dimensional distance — and then sets the plane it hands `GL_T` to
`(0, 0, 0, 0.5)` on the next line, with the two lines that would have used the
plane commented out above it. So the second axis was tried and abandoned, and
`0.5` written into the shader is the honest translation. `R_FogImage`'s own
comment agrees: "we calculate distance correctly in two planes, but the third
will still be projection based."

**A blend light's falloff is read at `t = 0` on OpenGL and at `t = 0.5` here,
and the two are the same number everywhere the game reaches.** `RB_BlendLight`
means to set it — it selects texture unit 1 and calls `qglTexCoord2f( 0, 0.5 )`
— but `glTexCoord` addresses unit 0 whatever `glActiveTexture` last said, and
`glMultiTexCoord` is the call that takes a unit and dhewm3 never makes it. So
unit 1 keeps its default coordinate. It changes nothing drawn: every falloff
image a blend light can name is a ramp along `s` alone and constant down `t`
(`lights/squarelight1a` and `lights/xfalloff` are 64×8 with all eight rows
byte-identical), so `0.5` is written here — what the code plainly intends, and
what the interaction program already reads its own falloff at. The one image
where they would disagree is the generated `_noFalloff`, whose row 0 is a black
border, so a blend light falling back to it would draw nothing at all on
OpenGL — and no `blendLight` material in the demo falls back, all six declaring
a `lightFalloffImage`.

**`r_skipFogLights` turns off the blend lights too, and that is reproduced
rather than tidied up.** `RB_STD_FogAllLights` tests it before the loop that
dispatches to either kind, while `r_skipBlendLights` is tested inside
`RB_BlendLight` alone — so `r_skipFogLights 1` is "draw neither" and
`r_skipBlendLights 1` is "draw the fog but not the blend lights", which is not
the pair of switches the two names suggest. A debug cvar that means something
different on the two backends is worse than one that is oddly named on both,
and this one is load-bearing besides: `r_skipFogLights 1` is what makes this
build draw the picture it drew before this step, which is how the step was
measured. `RB_BlendLight`'s other quirk is kept for the same reason: it returns
if `globalInteractions` is empty *without looking at* `localInteractions`, so a
blend light whose only surfaces are the no-self-shadow ones draws nothing. It is
nearly unreachable — a blend light material carries `noShadows`, which sends
every surface to `globalInteractions`.

**`RB_T_BlendLight` has a second vertex path and it is dead.** It falls back to a
surface's `shadowCache` when there is no `ambientCache`, under a comment saying
it "gets used for both blend lights and shadow draws" — a leftover from a shadow
path that no longer calls it. Checked rather than assumed:
`idInteraction::AddActiveInteraction` sets `lightTris->ambientCache` from the
surface's own before linking it into either interaction list, and links shadow
volumes into `globalShadows` and `localShadows` instead. So the second path is
not written here.

**Measured.** The gate moves **17 of 297 frames** against 4e.4 — frames 0 to
16, which is the tour's first camera stop exactly and the only one with a fog
light in view. Two further captures are identical to the first. With
`r_skipFogLights 1` the new build is **byte-identical to 4e.4 on all 297**,
which is what says the path that was already there pays nothing for this.
Those seventeen frames go from **1.90 of 255** against the SDL/GL build to
**0.88**, where the floor — both builds with the fog skipped — is **0.92**. So
the fogged frames now agree as well as the unfogged ones do. Clean under
`MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU validation, at **10
programs and 39 pipelines** at that camera against 9 and 37 with both kinds
skipped: one more program and two more pipelines, which are `RB_FogPass`'s own
two draws — the surfaces at `GLS_DEPTHFUNC_EQUAL` over what the depth fill
wrote, and the light's frustum at `GLS_DEPTHFUNC_LESS` with the cull reversed,
the frustum never having been in the depth buffer at all.

**No map in the demo places a blend light, so one had to be made — and how is
part of the result.** The `blendLight` materials in `materials/fogs.mtr` —
`fogs/glare`, `glare2`, `glare_snd`, `filter`, `pitFog`, `sentest` — are all in
art the demo ships and never places. `spawn light texture fogs/glare …` is the
obvious answer and **does not work on this build**: the entity is created (the
total entity count goes 2056 → 2058 across two spawns) and `Cmd_Spawn_f` is
plainly reached (an odd argument count prints its usage line), but the light
never reaches the frame — a white light of `_color "10 10 10"` and
`light_radius "1000 1000 1000"` on top of the camera moves the picture by 0.3
of 255, which is the animating NPC beside it. `testPointLight 500` prints
"Created new point light" and does the same nothing. **This is not this port:
it reproduces exactly on the SDL/GL build**, and it is written down here because
it is the reason the measurement below takes another road.

That road is to shadow the pk4's `materials/fogs.mtr` with a copy in which
`fogs/basicFog` is a `blendLight` rather than a `fogLight`. `fs_savepath`'s game
directory is first in the engine's search path — the engine prints the order at
startup — and a render demo resolves a light's material *by name* on playback,
so the gate's own hangar light becomes a blend light, deterministically, with
the same content on both builds. **The blend light then moves the same seventeen
frames by 1.61 of 255 on the eacp build and by 1.61 on the SDL/GL one**, and
those frames go from **2.24** against the GL build without it to **0.92** with
it — the floor again, to two decimal places.

The stage's colour had to be written into that material rather than left as
`colored`, and the reason is worth a line: `light_5254`'s shader parms are
`(0.1, 0.03, 0)`, which added over the scene is at most three levels of 255 —
GL's own blend light moved the frame by 0.13 and this backend's by 0.00, both
of them right and neither of them a measurement. A constant colour makes the
same light bright enough that the two renderers have something to disagree
about.

**And it turned up why a live pinned-camera shot is not an instrument here.**
Two runs of the same build at the same `setviewpos` measured a mean RGB of 30.8
and 35.3 at the tour's first stop. dhewm3 advances game time from the wall
clock, so a screenshot lands on a different game tick every run — and this
camera is looking at a fog light **bound to a mover** (`hanger_fog_mover`, moved
UP 320 over five seconds by `map_marscity1.script`), so the picture genuinely
moves. `com_fixedTic 1` runs one tick per frame and makes the sequence a
function of the command buffer alone: two runs then produce **byte-identical**
screenshots. Worth knowing before anyone else compares two live shots and
concludes something about a renderer; the gate's demo playback has never had
this problem, which is why it is the gate.

**What is not here.** `r_showOverDraw`, `backEnd.viewDef->isXraySubview` and the
two `r_skip` cvars are honoured, so every switch the shared code has still
works. The per-light stencil clear that would guarantee no pixel is double
fogged is not, and neither is it in the original: it sits inside an `#if 0`
that _D3XP turned off. `RB_STD_LightScale` and `RB_RenderDebugTools`, the two
other things `RB_STD_DrawView` calls that this backend does not, are named in
`DrawView`'s comment now rather than silently missing — the first can never
fire (`backEnd.overBright` is 1 for `BE_EACP`) and the second is the `r_show*`
visualisations, which have no counterpart yet.

**The merge of 4e.5 and 4e.6, measured once more as one tree.** The two were
written in parallel against 4e.4 and merged without a conflict, and the merged
build moves **132 of 297 frames** against 4e.4 — 17 plus 115, the union of the
two steps' frames and not one more — at **0.833 of 255** over the whole tour
against the SDL/GL build, where 4e.4 stood at 1.134. Two captures identical,
clean under both validation layers.

#### Step 4e.7 — gamma in the shader — **done**

`r_gammaInShader`, which step 4d.2 left as "the identity at the default settings,
and nothing at any other". It is something at the others now, and what it is was
decided by reading how dhewm3 does it rather than by what the cvar's name
suggests.

**On OpenGL it reaches the ARB programs and nothing else.** `R_LoadARBProgram`
splices a `MUL_SAT` and three `POW`s into the *source* of every fragment program
it loads — `interaction.vfp`, the two environment programs, every `newStage`,
the soft-particle one unless it says `nodhewm3gammahack` — and `program.env[21]`
carries `r_brightness` and `1 / r_gamma` into them. A fixed-function stage has
no source to splice into. So on the SDL/GL build, with `r_gammaInShader 1` and
`r_gamma 1.5`, the lit world is corrected and the 2D, the glows, the fog and the
blend lights are not, and that is the picture this build has to match. It does
the same: the interaction and the two reflection programs derive from an
`idEacpGammaProgram` that carries the uniform and the correction, and the generic
stage, the fog and the blend light do not.

**It is a uniform behind a branch, and the branch is the finding.** The
correction is `pow( saturate( rgb × brightness ), 1 / gamma )`, and at the
defaults that is `pow( x, 1.0 )` — which neither shading language promises to
return as exactly `x`. A uniform of `(1, 1, 1, 1)` multiplied out would have
moved every lit pixel by an ulp, which the gate cannot tell from a regression.
So the uniform carries a fourth value, 1 when either setting is away from 1 and
0 otherwise, and the shader branches on it: a branch on a uniform is one every
fragment of a draw takes the same side of, which is the cheap kind, and at the
defaults the pow is never asked for. The saturate is kept although an 8-bit
attachment clamps anyway, because the pow reads the value before the attachment
does and a negative base is what the original's `MUL_SAT` keeps away from `POW`.

The post-process half of the shader passes gets the identity, as
`RB_SetProgramEnvironment( isPostProcess )` gives it "to avoid applying them
twice" — a post-process stage reads a `_currentRender` that was corrected when
it was drawn. `DrawShaderPasses` keeps the flag for the two reflect draws, which
are the only corrected programs it can issue.

**Measured.** At the defaults the gate is **297/297 byte-identical** to the
merged 4e.5 + 4e.6 build, which is what the branch was for. At `r_gamma 1.5`
and `r_brightness 1.2` — through `GATE_ARGS`, which the gate grew for this —
both builds move **every one of the 297 frames**, the eacp build by a mean of
**30.3 of 255** and the SDL/GL build by **29.9**. The two builds at that setting
agree at **1.67 of 255**, against 0.83 at the defaults, and the doubling is
uniform across the eighteen stops rather than concentrated in any: an exponent of
1/1.5 over values scaled by 1.2 turns a difference of one level into a difference
of two, so a renderer that agreed to within a level before agrees to within two
after, on every surface it corrects and on none it does not.

**What is not here.** `r_gammaInShader 0` asks for the display's hardware ramp,
which `GLimp_SetGamma` warns it cannot set; on this host the cvar off means no
gamma at all rather than gamma somewhere else. The `newStage` programs are still
skipped, and would take the same base class when they arrive.

#### The over-bright frame — **closed, and it was never the port's**

The loose defect §8 has carried since step 4e.3 — "the eacp build renders about
two frames of any run three times too bright and flatly lit, at a fixed moment
in the level, where the SDL/GL build is steady" — is `demo_mars_city1`'s own
dropship. Both builds draw it, at the same instant, to within the agreement they
already have everywhere else. What was wrong is the way the two were compared,
and that is the finding worth keeping.

**What the moment is.** `light_5296`, a *projected* light — `light_target`,
`light_right`, `light_up`, `light_start`, `light_end` — carrying
`lights/headlights` and bound to `marscity_ship2_1` at the `ship` joint. It is
the transport's headlight, and it sweeps the airlock as the ship passes at
**55.6 to 56.5 seconds** of level time, nine tenths of a second, fifty-three
frames at 60Hz. In the backend it is one more light in a three-light view: the
view's `viewLights` goes from 3 to 4 and its surface count from 90 to 93, the
light's own colour is `_color 0.99 1 1` times `r_lightScale`, and it reaches the
room through the doorway with nothing between. That is what makes the extra
layer read as flat albedo rather than as a beam — it is a light with no occluder
in front of it, over every surface in view at once.

**Why the SDL/GL build looked steady, and this is the part to keep.** `wait N`
counts command-buffer executions, and `idEventLoop::RunEventLoop` runs the
buffer *once per event it processes* and once more when the queue empties — so N
is one iteration of the event loop rather than one frame, and the two builds do
not drain events at the same rate. Measured rather than reasoned about, with
`com_fixedTic 1` pinning the game clock to one tic per rendered frame and
`backEnd.viewDef->renderView.time` printed from the shared `RB_DrawView`:

| | `wait 3000`–`3150` reaches |
| --- | --- |
| `dhewm3-eacp` | **52.7 s** of level time |
| `dhewm3` | **7.7 s** of level time |

about 6.85 frames a wait against one. So every pair of screenshots the old
reproducer compared was a picture of two different moments, and the SDL build's
twenty-four of them never came within forty seconds of the dropship. The eacp
build is the slow one here for a reason already in the log: gap 13, its frame is
driven by the display link at 60Hz, while the SDL build runs uncapped.

**There is a second trap underneath the first, and it is worse.** The flyby only
happens if the player is left alone until the opening cinematic finishes. Taking
the camera at 50.5 seconds instead of 52.7 — two seconds early — stops the ship
arriving at all: `lights` stays at 3 through 62 seconds and the means over the
same window fall to 26–34. On *either* build. So a reproducer that lands early
does not draw a dimmer flash, it draws no flash, and a sweep of sixty
screenshots across forty seconds of level time finds nothing to report. That is
what the old one did to the SDL build, twice over — once through the wait rate
and once through the takeover.

**Pinned properly, the two agree.** `com_fixedTic 1`, `wait 3150` on the eacp
build and `wait 21960` on the SDL one, both taking the camera at 52.7 s, both
at 640x480:

| t (s) | eacp | SDL/GL | surfs |
| --- | --- | --- | --- |
| 55.53 | 30.47 | 28.71 | 90 |
| 55.60 | 38.37 | 37.17 | 91 |
| 55.67 | 51.93 | 54.37 | 91 |
| 55.73 | 71.04 | 76.12 | 92 |
| 55.87 | 96.12 | 90.75 | 92 |
| 55.93 | **97.57** | **93.58** | 93 |
| 56.07 | 86.23 | 81.17 | 92 |
| 56.13 | 80.86 | 77.52 | 92 |
| 56.40 | 82.76 | 79.46 | 92 |
| 56.53 | 33.51 | 32.52 | 90 |

Same window, same shape, same peak, same surface counts frame for frame. The
mean absolute difference is **2.59 of 255** over the thirty-two frames spanning
the flash and **1.1** on the frames outside it — which is the same three to four
percent the port stands at everywhere at this camera, on a picture three times
as bright rather than a defect of its own.

**What the original evidence really said**, now that the moment has a name. The
identical `r_debugRenderToTexture` counts on good and bad frames were the
frontend emitting the same *kind* of frame — one 3D view, one copy, one swap —
and said nothing about the scene inside it, which had gained a light. It
reproduced before 4e.3 because the light was always there. And berserk vision
was never involved: without it the flash peaks at 97.6 and with it at 97.5, and
the run of twenty-four that found it would have found it either way.

**Four things eliminated on the way, none of which needs re-testing.**

- **The pass plumbing is identical** on the good frames and the bad ones, traced
  call by call: `SetDrawBuffer`, one `BeginPass` with `clearColor = 1`, one
  `DrawView` whose `BeginDrawingView` finds `passHasWorldView` false and keeps
  the pass, `SwapBuffers`, `EndPass`. So 4e.3's hypothesis — the frame composed
  without its colour clear, 4e.2's camshot case one step along — is dead, and so
  is any story about `SuspendPass` / `ResumePass`.
- **`r_shadows 0` does not remove it.** The flash is there with the stencil out
  of the picture entirely, which is what says it is a light arriving rather than
  a shadow going missing.
- **Dropping the mip chain of every `TR_CLAMP_TO_ZERO` image does not either** —
  the one place `R_MipMap`'s `preserveBorder` would have mattered, since
  `UploadImageLevel` hands eacp level 0 and lets it build its own chain, and
  `lights/headlights` is `zeroclamp`. The chains do diverge (Doom 3's goes to
  zero at 2x2 where eacp's stays at the image's 74.8 of 255 average) and it is
  worth knowing — it is gap 23 now — but it is not this.
- Clean under `MTL_DEBUG_LAYER=1` with `MTL_SHADER_VALIDATION=1` and GPU
  validation across the flash, at 9 programs and 40 pipelines.

**What landed.** No renderer code, and the gate says so: **297 of 297 identical**
against 4e.4. What changed is the harness's own account of what a `wait` buys —
`regression/README.md` and `record.cfg`'s header both said "N is roughly 6x the
number of frames it holds", which is the SDL build's number stated as if it were
both builds', and which is what made the comparison look sound. They now give
the measured rate per build, say that a live-game script is not a clock two
builds share, and give the way to pin one: `com_fixedTic 1`, a count calibrated
per build, and the render view's own time printed to check where a run landed.
The gate itself was never exposed to this and that is the argument for it — a
recorded demo replays the render world frame by frame, so both builds draw the
same instants whatever their event rate.

### Shader inventory for Phase 2

Roughly 10–15 EDSL programs, each in the sampling variants §4.3 sizes — 8 worst case
for interaction, 8 for the generic stage now that the alpha test is a variant of it
rather than a program beside it, compiled lazily:

- ~~interaction (bump / diffuse / specular) — the port of `interaction.vfp`~~ —
  **done**, step 4d.2. Its variants are keyed on *five* samplings rather than the
  three §4.3 counted, because a light's projected image and its falloff are
  declared by the light material and vary with it — 1024 combinations, of which
  the demo's first level reaches two. Not an array, therefore, but a list keyed
  on the five indices packed two bits apiece.
- ~~depth fill, with alpha test~~ — **not a program of its own**, which step 4d.1
  found rather than assumed: the depth fill is the generic material stage with the
  colour black and a `setDiscardBelow`, so it is one more *variant* of that one
  and the count below moves from 4 to 8 rather than the list growing an entry
- ~~shadow volume extrude (stencil) — `Apps/GPU/StencilShadows` is the worked
  example~~ — **done**, step 4d.3. `shadow.vp`, which is two instructions and no
  fragment program at all. It is the one program here with *no* sampling
  variants — it reads no texture — and the most pipelines per program, because
  what varies is the stencil rather than the sampler: the count in its two
  forms, the clear, and the mirrored pair of the count.
- **the blit** — **done**, step 4e.1, and the one entry on this list Doom 3 has
  no counterpart for. It puts the render target on the drawable, which is a
  draw here and is nothing at all on OpenGL, where the frame was in the back
  buffer the moment it was drawn. One texture, no transform, six vertices. Step
  4e.3 gave it a second *pipeline* and no second program: `_currentRender` is
  the same quad into an image's texture, and what differs is the attachment.
- ~~generic material stage, in its texgen variants: normal, reflect, skybox,
  wobblesky~~ — **done**, step 4e.5, and it is **three programs beside the
  generic stage rather than variants of it**. eacp declares every texture a
  shader lists whether `define()` samples it or not, and Metal rejects a draw
  that leaves a declared texture unbound, so a cube map inside the stage program
  would have been a cube map declared on every `TG_EXPLICIT` variant too. The
  three are the cube one (skybox and wobblesky folded into a single texgen —
  a skybox is a wobblesky whose matrix is the identity — plus diffuse and
  unbumped reflect, so three texgens times its samplings), `bumpyEnvironment.vfp`
  as a program of its own, and the screen one, which needs no cube at all and
  covers both `TG_SCREEN` and `TG_SCREEN2` because they are one texgen written
  out twice. The demo's first level compiles one variant of each of the first
  two and never reaches the third.
- ~~fog~~ — **done**, step 4e.6. One program with one sampling tuple by
  construction, both of its images being generated `TF_LINEAR` / `TR_CLAMP`, and
  two pipelines: the surfaces at `EQUAL` and the light's frustum at `LESS`.
- ~~light blend~~ — **done**, step 4e.6. The light half of the interaction
  program on its own — projection and falloff, no surface maps — keyed on the
  two samplings the light material declares.
- ~~2D / GUI~~ — never a program of its own: step 4c found that the menus, the
  console and the HUD are the generic material stage over a view with no
  `viewEntitys`, so the entry above covers it.

The EDSL earns its keep here: one source per shader covering Metal and D3D12, against
Doom 3's original two hand-written ARB programs per path.

**All of them exist now, and the list was the right shape at its ends and wrong
in its middle.** What the count missed at the ends: a program the API needs and
the engine does not (the blit), and a program the engine needs and the API makes
unnecessary (the depth fill, which turned out to be a variant rather than an
entry). What it missed in the middle is that "texgen variants" became three
programs for a reason eacp's declaration model imposed, and that gamma is not a
program at all but a base class three of them share (step 4e.7). The
sampling-variant sizing was the part that was wrong, and §4.3 says where. The
demo's first level ends the phase at **12 programs and 64 pipelines** at a
camera with sky and glass in view, 9 and 57 before 4e.5.

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
4. **`idRenderBackendEacp` beside the GL one**, taking the gate's frames as the
   target. The two are the linker's choice rather than a runtime one — each is
   the answer for exactly one host — and the GL one goes when it stops being
   needed.
   - ~~**4a. Grow the seam over init, the view and the images.**~~ — **done**,
     §6. Two pure moves on the SDL/GL build, both 297/297.
   - ~~**4b. The renderer comes up on eacp and draws nothing.**~~ — **done**,
     §6. `com_skipRenderer` is gone, 176 images are Metal textures, the frame
     runs at 60, and the boot log matches the SDL build's from
     `Initializing Game` down.
   - ~~**4b′. The blend equation and the write mask, in eacp.**~~ — **done**,
     §6. What 4c turned out to need before it could start: gaps 12 and 17,
     closed in eacp and the pin moved onto `main`.
   - ~~**4c. 2D.**~~ — **done**, §6. Everything Doom 3 puts on screen without a
     world — the menus, the console, the HUD, the loading screens — goes
     through one path, `RB_STD_DrawShaderPasses` over a `viewDef` with no
     `viewEntitys`, and the generic material stage is the one program that
     draws all of it. The main menu is on screen: four programs, five
     pipelines, 102 draws a frame, clean under the validation layers. It
     exercised the whole draw plumbing at once, which is what it was for —
     the state bitfield translated into a pipeline, the variant cache §4.3
     sizes, geometry streamed through `GPU::StreamingBuffers`, a texture per
     stage.
   - ~~**4d. The world**, in three, because it is three ideas rather than
     one.~~ — **done**, all three.
     - ~~**4d.1. The depth fill.**~~ — **done**, §6. Every opaque and
       perforated surface into the depth buffer at its own depth, and the
       ambient passes now running over a 3D view. What it draws is the level
       in black with its glows on top; what it is for is that the two steps
       after it can test against it. 89 draws and 4041 triangles at a pinned
       camera, which is the SDL/GL build's count exactly once its shadow half
       is subtracted.
     - ~~**4d.2. The interaction program.**~~ — **done**, §6.
       `interaction.vfp` in the EDSL, driven by
       `RB_CreateSingleDrawInteractions` — which had to stop making `qgl` calls
       of its own first — at `GLS_DEPTHFUNC_EQUAL` over what 4d.1 filled. 114
       draws and 6332 triangles at a pinned camera, which is the SDL/GL
       build's count with no correction term. Two of the ARB program's seven
       textures became arithmetic; the five that remain are eacp gap 19 on
       D3D12, which is why this step is macOS-only. The Mars globe on the main
       menu is lit.
     - ~~**4d.3. The stencil shadow pass.**~~ — **done**, §6.
       `RB_StencilShadowPass` and `RB_T_Shadow` rewritten, `shadow.vp` in the
       EDSL, the count taken two-sided in one pass over each volume — which is
       what per-face stencil state buys and what deletes three of the
       original's four branches. The per-light clear is a scissored quad,
       because a pass cannot be cleared once it has begun. 71 draws, 1644
       triangles and 2376 shadow triangles at a camera with nothing animating
       in it, which is the SDL/GL build's count exactly, volume for volume —
       and the picture agrees at 0.3 of 255.
   - **4e. What is left**, and it is a basket rather than a step.
     - ~~**4e.1. The render target.**~~ — **done**, §6. The frame is composed
       into an app-owned texture and the drawable is a blit of it, which is
       what a pass that reads what an earlier one wrote needs — PureDOOM's
       `captureTarget`, §3. Same counters, same picture at 0.3 of 255, one
       multisampling given up (gap 20).
     - ~~**4e.2. `ReadPixels`.**~~ — **done**, §6. eacp grew the texture
       readback gap 21 asked for *and* a `Frame::flush` beside it, because a
       read inside the frame that drew the pixels otherwise returns the frame
       before it. The eacp build's frames are hashes now: the gate runs on it
       at 297/297 identical across two captures and different on all 297 with
       `r_skipSpecular 1`. The objective camshots, the `screenshot` command and
       `R_ReadTiledPixels` came with it — the last of those off `qgl` and onto
       the seam, which cost the GL build nothing (297/297).
     - ~~**4e.3. `_currentRender`.**~~ — **done**, §6. 4e.1's blit with the
       destination changed, so `_currentRender` and `_scratch` are filled in
       and the wipe and the player-view effects draw the frame rather than a
       stale image. It needed eacp gap 22 — a pass that keeps the depth and
       stencil it was handed — because a copy has to interrupt the frame's
       pass and everything after it has to be occluded by the same buffer.
       297/297 on the gate, and the picture through `_scratch` agrees with the
       GL build's at 5.7 of 255 where the same camera drawn directly agrees at
       3.75. It also turned up an over-bright frame that predates it, which is
       written down where it was found.
     - ~~**4e.4. A pass per view.**~~ — **done**, §6. Every 3D view opens its
       own, clearing the depth and stencil planes and loading the colour the
       views before it wrote — which gap 18 said a pass could not do and 4e.1's
       texture target made true. So **mirrors and subviews work**: the washroom
       mirror of `demo_mars_city1` reflects the room, which the gate's ninth
       camera stop happens to be standing in front of. 16 of 297 frames moved,
       from 3.82 of 255 against the SDL/GL build to 0.50, and the other 281 are
       byte-identical. It brought the mirrored cull flip with it, which is part
       of a pipeline here rather than a per-draw enum, and the mirror clip
       plane, which turned out to be `dot( plane, vertex ) < 0` wearing a
       two-texel texture.
     - ~~**4e.5. Cube maps, and the texgen variants.**~~ — **done**, §6. Gap 5
       closed in eacp itself — a `TextureDescriptor::cube`, a `TextureCube` in
       the EDSL, six faces in the one order Metal, D3D12 and OpenGL share, 261
       GPU tests — and the four cube texgens plus the two screen ones ported
       onto it as three programs beside the generic stage. The Mars sky and the
       glass of `demo_mars_city1` are on screen: 115 of 297 frames moved, eight
       camera stops exactly, every one of them towards the SDL/GL build, and
       the tour's mean against it went from 1.13 of 255 to 0.89.
     - ~~**4e.6. Fog and blend lights.**~~ — **done**, §6. `RB_FogPass` and
       `RB_BlendLight` as two programs whose every coordinate is a plane dotted
       with the vertex, between the two halves of the shader passes. The
       hangar fog at the tour's first stop moved 17 frames from 1.90 of 255
       against the SDL/GL build to 0.88, which is the floor; the blend light,
       which no demo map places, was made by shadowing `fogs.mtr` and agrees at
       the floor too. Found on the way: `spawn light` puts no light in the frame
       on either build, and a live pinned-camera shot needs `com_fixedTic 1`.
     - ~~**4e.7. `r_gammaInShader`.**~~ — **done**, §6. What it is on OpenGL —
       a correction spliced into the ARB programs and into nothing
       fixed-function — reproduced as a base class three programs share, behind
       a branch on a uniform so that the defaults stay byte-identical (297/297)
       and `r_gamma 1.5 r_brightness 1.2` moves every frame by the same 30 of
       255 on both builds.
     - **4e.8. What is still skipped, and named.** The `newStage` materials —
       `heatHaze` on `glass1` / `glass2`, `vp1` — which are hand-written ARB
       program pairs and would be a program per newStage; `TG_GLASSWARP`, which
       is one of them wearing a texgen; the soft-particle program behind
       `BE_ARB2`; and `r_showTris` and the other `r_show*` visualisations of
       `RB_RenderDebugTools`. None of them is on the gate's path. Whether any is
       worth a step before step 5 is a question for step 5.
     - ~~**The over-bright frame**, which is not a step of 4e but is loose~~ —
       **closed, and it was never the port's**, §6. The frame is
       `demo_mars_city1`'s dropship headlight sweeping the airlock at 55.6
       seconds of level time, and both builds draw it, at the same instant,
       within 2.59 of 255 of each other. What was wrong was the comparison:
       `wait N` is one event-loop iteration rather than one frame, and the two
       builds drain events at rates seven to one apart, so the reproducer's
       SDL/GL screenshots were forty seconds of level time short of the ship.
       `regression/README.md` says how to pin a live run now.
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
