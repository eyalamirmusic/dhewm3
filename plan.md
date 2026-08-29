# Porting dhewm3 to eacp

Moving dhewm3 off SDL2 + OpenGL and onto [eacp](https://github.com/eyalamirmusic/eacp):
app lifecycle and message loop first, GPU rendering (Metal / D3D12) as the real work.

**Status: Phase 0 is done and merged. Phase 1 has landed its gate and its seam.
Phase 2 is under way: the app shell, the threading, the boot, the input and the
renderer are in, and the renderer now draws. `dhewm3-eacp` puts **Doom 3's main
menu on screen through Metal**, **loads a level**, **lights it** and now
**shadows it** — the world is done, all three of the steps 4d was broken into.
`interaction.vfp` and `shadow.vp` are in the EDSL, the stencil shadow volumes
are counted two-sided in one pass over each volume, and at a pinned camera in
`demo_mars_city1` the two backends draw **the same 71 draws, 1644 triangles and
2376 shadow triangles**, volume for volume. What is left is 4e: fog and blend
lights, the texgen variants, subviews, and the render target `_currentRender`
and a frame-exact gate both need.**

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

That last sentence has now been tested. 12, 17 and 19 are what it turned up —
the gaps this port found by walking real content rather than by reading eacp.
12 and 17 stopped the next step rather than degrading it, and both are closed,
in §6 under step 4b′. 19 does not stop anything today, because it is D3D12's
alone and the eacp host is macOS-only for other reasons; it stops Windows.

Numbers are never reused, so a hole is an entry that closed.

### Needed, not blocking

4. **BC/DXT compressed texture formats** — all Doom 3 art ships as DXT1/3/5 in the
   pk4s. Without it: decompress at load, ~4× VRAM, much slower level loads.
5. **Cube textures** — skyboxes and reflections. Two of the three users are
   gone rather than pending: step 4d.2 deleted the normalization cube map, as
   this entry predicted it could, and the `_ambient` map that stands in for it
   on an ambient light with it — that one becoming a uniform, since the whole
   point of the substitution is that the answer does not vary with the lookup.
   What is left needs real cube sampling.
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
    where it decides it, and it is what makes subviews and mirrors 4e's rather
    than a thing that nearly works. Worth knowing that the eacp build draws at
    4x MSAA either way — `GPUView`'s default, which `r_multiSamples` does not
    reach.

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
   - **4e. What is left.** ← **next.** Fog and blend lights, the texgen variants
     (reflect, skybox, wobblesky), subviews and mirrors, `r_gammaInShader`
     (step 4d.2 — the identity at the default settings, and nothing at any
     other), and `_currentRender` — which needs the composited frame to live in
     an app-owned render target that the drawable is blitted from, because a
     texture cannot be sampled by the pass rendering into it. PureDOOM's
     `captureTarget`, §3. That render target is also what gives the eacp build a
     `ReadPixels` and so a frame the gate could hash; two screen grabs of a
     scene that holds still get within 0.3 of 255 of each other (step 4d.3),
     which is enough to compare a picture by and not enough to hash one.
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
