# The regression gate

Phase 1 put an `idRenderBackend` seam under the renderer without changing a
pixel of the output, and Phase 2 built a Metal backend behind that seam and then
deleted the OpenGL one. Both claims need measuring rather than asserting, and
this is what measures them: a recorded render demo, replayed frame by frame,
hashed.

```sh
./regression/gate.sh record            # once, on a known-good build
./regression/gate.sh capture before
#   ... land a change ...
./regression/gate.sh capture after
./regression/gate.sh compare before after
```

`compare` exits non-zero and names the frames whose hashes moved.

## Why a render demo

A demo file carries the render world per frame, so playback renders the same
scene however fast the machine happens to run - unlike a live game, where the
frame you screenshot depends on timing. Two captures of one build come out
byte-identical; that is what makes the hashes worth comparing at all.

The gate is checked in both directions before being trusted: two captures of
the same build match on all 297 frames, and a capture with `r_skipSpecular 1`
differs on every one of them. A gate that cannot fail is not a gate.

A capture takes about twenty seconds and writes 297 frames at 320x240.

**Hold the display awake.** The frame is driven by the display link, which stops
when the panel sleeps (plan.md §5, gap 13) - and a capture that stops half way
looks like a hang. `caffeinate -du` in front of the run is the whole fix, and it
is not optional:

```sh
caffeinate -du ./regression/gate.sh capture after
```

**Compare a build against itself.** Hashes are only comparable within one
machine, one GPU and one renderer - which is why, while the tree still held the
SDL/GL executable next to the eacp port, the two were never compared with each
other. Both wrote the same 320x240 frames of the same tour (`BeginFrame` renders
a tiled screenshot at `com_aviDemoWidth`, so the window's size does not reach
these) and **none of the 297 pairs hashed the same** - which is what two
renderers agreeing to the eye and not to the bit looks like. What the gate
answers is "did this change move anything", which is the question it was built
for.

Both builds were checked in both directions before being trusted, with the same
numbers: 297 identical across two captures, 297 moved with `r_skipSpecular 1`.

**There is one binary now.** Step 5 deleted the SDL/GL one; a build tree holds a
single `dhewm3`, `BUILD` defaults to `cmake-build-release`, and `GAME` has
nothing left to choose. That default is the IDE's own Release tree: it fetches
the demo data itself (`FETCH_DEMO_DATA` is on there), and a Release build hashes
identically to the Debug builds every baseline was captured on - checked once,
297 of 297 against the step 7 baseline, before the port's own `cmake-build-eacp`
was deleted. `gate.sh` refuses a binary that links `libSDL3`, because after
step 5 no honest build of this tree does - such a binary is a stale tree from
before it, and measuring the wrong binary is the one way this harness can lie
quietly. If you still have a `cmake-build-debug` from Phase 1, that is exactly
what the check catches; there is no reason to delete it, but do not point
`BUILD` at it and expect a comparison.

## What steps 4e.5 to 4e.8 learned about the gate

Five things, none of them about the renderer - and a sixth, about what `wait`
buys each build, under "What the tour covers" below.

**`GATE_ARGS` is extra `+set` pairs for the engine**, appended before the
`+exec`. It exists for a cvar that changes every frame - `r_gamma`, say - which
can never match the baseline. While there were two builds, the way to check one
was to capture *both* the same way and compare the pair against each other:

```sh
GATE_ARGS='+set r_gamma 1.5 +set r_brightness 1.2' \
	caffeinate -du ./regression/gate.sh capture eacp-gamma
```

With one build left, `GATE_ARGS` is for capturing a second baseline under the
setting and comparing later runs to that, which is the same trick with one leg.

**The SDL/GL build was not byte-deterministic.** Two captures of the same
unmodified binary differed on frame 99, by two pixels of two out of 255. It
mattered because that frame also moved between captures taken months apart,
which reads like a shared-code regression until a second capture of the same
build shows it moving without one. The eacp build was byte-identical across
captures at every step through step 7, including the whole of step 5 - thirteen
commits, every one of them 297 of 297 against the same baseline - so "identical"
is a claim this build could make and the GL one never could.

**It jitters too, rarely, and by less.** Measured after step 7 while verifying
a tree built against both dependencies' `main`: of eight captures across two
binaries of the same source, three each moved a single frame - 71 by one pixel
of one level, 80 by one pixel of one level, 168 by nineteen pixels of at most
two levels - and the other five matched each other to the byte, the two
binaries included. The three frames are not the same frame and none is the GL
build's 99, so this is the machine's noise rather than a place in the tour. A
compare that names one frame and nothing else is therefore not a regression
until a second capture of the same binary moves it too; a change that moves
sixteen frames, or one frame by a mean anyone can see, still is. Recapture,
then believe the second one.

**A live pinned-camera shot is not an instrument without `com_fixedTic 1`.**
dhewm3 advances game time from the wall clock, so a `screenshot` after a
`setviewpos` lands on a different game tick every run - and the tour's first
stop is looking at a fog light bound to a mover. Two live runs of one build read
30.8 and 35.3 mean there. `com_fixedTic 1` runs one tick per frame and makes the
sequence a function of the command buffer alone, at which point two runs are
byte-identical. The demo playback the gate uses never had the problem.

**The Metal validation layers move the eacp build's frames.** A capture under
`MTL_SHADER_VALIDATION=1` differs from one without on 99 of 297 frames, by a
mean of 0.000 of 255 and a worst pixel of 25 - instrumented shaders and a
different floating-point schedule, not a bug. Run the validation capture for its
log and compare hashes only between captures taken the same way.

**A capture with different *content* cannot go through this script, and that is
deliberate.** Steps 4e.6 and 4e.8 both needed one - a `.mtr` shadowed in
`fs_savepath`'s game directory, so that the demo's own light becomes a blend
light or its own glass draws a different `newStage` program - and `run_engine`
wipes and rebuilds that directory before every run, for the feedback-loop reason
the next section gives. So the way to take one is by hand, with the same
arguments the script uses and `regression/capture.cfg` copied in beside the
shadowed file:

```sh
mkdir -p "$scratch/demo/demos" "$scratch/demo/materials"
cp your-edited.mtr "$scratch/demo/materials/"
cp regression/reference.demo "$scratch/demo/demos/reference.demo"
cp regression/capture.cfg "$scratch/demo/"
"$exe" +set fs_basepath "$build/neo" +set fs_savepath "$scratch" \
       +set fs_configpath "$scratch" +exec capture.cfg
# frames land in $scratch/demo/demos/reference; wait for reference.roqParam
```

`fs_savepath`'s game directory is first in the engine's search path, so the
shadow wins, and demo playback resolves a material *by name* - which is what
made the same edited file drive both builds through the same 297 instants while
there were two of them, and what still makes it drive one build's before and
after.

## Both paths have to be pinned, not just fs_savepath

dhewm3 keeps `dhewm.cfg` on `fs_configpath`, which is separate from
`fs_savepath`. An earlier version of `gate.sh` redirected only the save path,
which left the real config both readable and writable by every run. The
`r_skipSpecular 1` capture above archived that cvar into it, and every capture
afterwards silently read it back - so the gate went on producing stable,
reproducible, byte-identical hashes of a scene that was being rendered wrong,
and reported a difference against a commit that had not changed a thing.

Two lessons worth keeping, because neither is specific to this script:

- **Reproducible is not the same as correct.** Determinism was verified and was
  real; it just said nothing about whether the configuration was the intended
  one. The check that would have caught it is asserting the *value*, not the
  stability, of what the run is configured with.
- **A comparison harness that writes anywhere the program also reads is a
  feedback loop.** `gate.sh` now points `fs_savepath` and `fs_configpath` at
  `regression/work` and gives every run a fresh game directory, so a run cannot
  inherit anything from the run before it.

## What the tour covers

`record.cfg` drives the camera through the eighteen `info_location` entities of
`game/demo_mars_city1` - the named areas the HUD labels - read out of the map
itself. That walks the renderer through most of what the demo pk4 contains:
per-pixel lighting, stencil shadows, animated NPCs, in-world GUI surfaces,
particles and fog.

Two things about the level are worth knowing before editing the tour:

- **It opens on a cinematic** that owns the camera for the first stretch, so the
  script sits through it before taking over. Record without that wait and you
  capture the cinematic instead of the level.
- **`wait N` counts command-buffer executions, not frames, and the rate is a
  property of the host.** `idEventLoop::RunEventLoop` runs the buffer once per
  event it processes and once more when the queue is empty, so what N buys is
  one iteration of *that* loop rather than one frame. Measured with
  `com_fixedTic 1`, which pins the game clock to one tic per rendered frame:
  `wait 3150` reached **52.7 seconds** of level time on the eacp build and
  **7.7 seconds** on the SDL/GL one, because the SDL build drained about 6.85
  events a frame where this one drains one. On this build, `wait N` is N/60
  seconds.

  So **a live-game script is not a clock two hosts share**, and a screenshot
  taken at the same `wait` count in each is a picture of two different moments.
  Pin the level clock instead: `com_fixedTic 1`, a count calibrated on the build
  you are running, and print `backEnd.viewDef->renderView.time` to check where a
  run actually landed. The gate itself is immune and that is the point of it: a
  recorded demo replays the render world frame by frame, whatever the event rate.

  This is not hypothetical. §6 of `plan.md` carried an "over-bright frame" as a
  defect of the eacp backend for two steps on the strength of a comparison made
  this way; pinned properly, both builds drew it.

## The reference demo is an artifact, not an output

`reference.demo` is deliberately gitignored and deliberately not regenerated.
It is the fixed point the whole phase is measured against, so record it once and
leave it alone - re-recording it silently moves the thing you are comparing to.
If it is lost, re-record and re-baseline, and treat comparisons across that line
as meaningless.

Frame hashes are only comparable within one machine and GPU. They are a
before/after check on a change, not a cross-machine conformance suite.

## Requirements

A playable build and the demo data (`FETCH_DEMO_DATA`, on by default). Set
`BUILD=...` if your build tree is not `cmake-build-release`. Everything the gate
writes goes to `regression/work`, which is `fs_savepath` for these runs - your
own config, saves and screenshots are never touched.
