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

The gate was checked in both directions before being trusted: two captures of
the same build match on all 296 frames, and a build with `r_skipSpecular 1`
differs on every one of them. A gate that cannot fail is not a gate.

A capture takes about 17 seconds and writes 296 frames at 320x240.

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
