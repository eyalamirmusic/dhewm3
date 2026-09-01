# The Phase 1 regression gate

Phase 1 puts an `idRenderBackend` seam under the renderer without changing a
pixel of the output. That claim needs measuring, not asserting, and this is what
measures it: a recorded render demo, replayed frame by frame, hashed.

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

## The eacp build too, since step 4e.2

`GAME` picks the binary, because a build tree holds both of them:

```sh
GAME=dhewm3-eacp BUILD=$PWD/cmake-build-eacp \
	caffeinate -du ./regression/gate.sh capture eacp-after
```

Two things are different about that run and neither is optional.

**Hold the display awake.** The eacp build's frame is driven by the display
link, which stops when the panel sleeps (plan.md §5, gap 13) - and a capture
that stops half way looks like a hang. `caffeinate -du` is the whole fix.

**Compare each build against itself.** `dhewm3` and `dhewm3-eacp` are two
renderers, so their hashes have nothing to say to each other - the same reason
the hashes are only comparable within one machine and GPU. What the gate answers
for either build is "did this change move anything", which is the question it
was built for.

The eacp build was checked in both directions the same way the GL one was, and
the numbers are the same: 297 identical across two captures, 297 moved with
`r_skipSpecular 1`. Both builds write the same 320x240 frames of the same tour -
`BeginFrame` renders a tiled screenshot at `com_aviDemoWidth`, so the window's
size does not reach these - and **none of the 297 pairs hash the same**, which
is what two renderers agreeing to the eye and not to the bit looks like.

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
- **`wait N` counts command-buffer executions, not frames.** The buffer is run
  several times per frame, so N is roughly six times the number of frames it
  actually holds.

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
`BUILD=...` if your build tree is not `cmake-build-debug`. Everything the gate
writes goes to `regression/work`, which is `fs_savepath` for these runs - your
own config, saves and screenshots are never touched.
