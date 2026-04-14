# Optimizations and Commit History

This file summarizes the optimization-related commits currently on this branch
and in the local `melonDS-android-lib` submodule history.

Important context:
- The main repo now keeps instructions and harness commits before the local
  optimization commits, so optimization reverts can be tested without removing
  the workflow scaffolding.
- The main repo currently points at a local `melonDS-android-lib` commit that is
  not recorded upstream.
- Some optimizations were later found to have correctness issues in deeper
  gameplay testing. Those caveats are called out below.

## Timeline (Oldest to Newest)

### `7862b8b0` - `melonDS-android-lib` - liteDS: apply all performance optimisations (OPT-1 through OPT-7)

Type: broad architectural optimization set

What it changed:
- `OPT-1`: ARM7 dedicated thread with spinlock IPC protection and atomic
  scheduler/interrupt updates
- `OPT-3`: scheduler iteration size `64 -> 512`
- `OPT-4`: `NextTarget()` linear scan replaced with `__builtin_ctz` bit scan
- `OPT-5`: batched halt/IRQ checks every `16` interpreter instructions
- `OPT-7`: fast SPU interpolation path
- `LITEV_ARM7_HLE_AUDIO`: ARM7 audio-side HLE framework and idle-loop handling
- `LITEV_AGGRESSIVE_SKIP`: per-frame GPU rasterization skip
- `LITEV_NEON_RENDERER`: NEON hooks in `GPU2D_Soft`
- profiling hooks and future renderer-thread scaffolding

Recorded metrics:
- FPS: not recorded in commit body
- CPU instructions: not recorded in commit body

Notes:
- This is the large initial liteDS optimization drop in the submodule.

### `edf6241` - main repo - Add agent workflow rules

Type: documentation / process

What it changed:
- moved project direction and workflow rules into `AGENTS.md`

Recorded metrics:
- FPS: `N/A`
- CPU instructions: `N/A`

### `9a3e8a95` - `melonDS-android-lib` - Add ARM9 library HLE for hot libc helpers

Type: selective HLE optimization

What it changed:
- added ARM9 library HLE for hot libc-like helpers

Recorded metrics:
- FPS: `54.9 -> 60.4` on the Shrek boot-window sampler
- CPU instructions: `6,045,173,196 -> 6,744,254,649` over `5s`

Notes:
- Commit body records this as `+5.5 FPS` / about `+10.0%`.
- The higher fixed-window instruction count came from advancing more frames in
  the same wall-clock interval.
- Later testing found the global `StrCmp` HLE path unsafe in real gameplay. The
  commit remains in submodule history, but it should not be treated as fully
  validated in its original form.

### `095ff03` - main repo - Tighten big-win performance instructions

Type: documentation / process

What it changed:
- updated `AGENTS.md` to force big-win triage
- explicitly deprioritized repeated low-yield work

Recorded metrics:
- FPS: `N/A`
- CPU instructions: `N/A`

### `5af12b96` - `melonDS-android-lib` - Skip identical OpenGL 3D frames when VRAM is unchanged

Type: GPU3D optimization

What it changed:
- added a real OpenGL `RenderFrameIdentical` fast path
- skips 3D rendering work when the frame is unchanged and texture VRAM /
  tex-palette VRAM are unchanged

Recorded metrics:
- FPS: `66.8 -> 70.0 / 69.17` at `3x` uncapped fast-forward on Shrek
- CPU instructions: `6,734,195,576 -> 6,746,673,279` over `5s`

Notes:
- Commit body records this as roughly `+3.5%` to `+4.8%` FPS, average about
  `+4.2%`.
- The instruction count increase was small and not used as the keep metric.

### `97d508a` - main repo - Add debug emulator input harness

Type: tooling / workflow

What it added:
- debug-only Android broadcast receiver for emulator input automation
- direct button injection, touchscreen injection, savestate load, and
  fast-forward control

Recorded metrics:
- FPS: `N/A`
- CPU instructions: `N/A`

Notes:
- This is the core test harness used instead of unreliable `adb input`
  guessing.

### `c1ec33eb` - `melonDS-android-lib` - GPU3D: prepack GL polygons ahead of VCount215

Type: structural GPU3D optimization

What it changed:
- queued a CPU-only GL polygon/index prepare job at VBlank
- consumed prepared geometry at `VCount215`
- kept a synchronous fallback when the worker missed the frame

Recorded metrics:
- FPS: commit body says the clean `5af12b96` baseline was not benchmarkable on
  the Shrek title path because it rendered a white screen
- CPU instructions: `N/A`

Notes:
- This is an architectural move toward off-thread render preparation.
- The commit body records that the comparison baseline was not stable enough for
  a clean benchmark.

### `3482357` - main repo - Add Android harness runner script

Type: tooling / workflow

What it added:
- `tools/bench/run_android_harness.sh`
- documented harness usage in `AGENTS.md`
- wraps ROM launch, harness input injection, and screenshot capture

Recorded metrics:
- FPS: `N/A`
- CPU instructions: `N/A`

Notes:
- This is the current scripted path for deterministic device-side scene entry.

### `46d92920` - `melonDS-android-lib` - GPU2D: keep accel aux coherent on top screen

Type: correctness fix on top of renderer optimization work

What it changed:
- kept the accelerated top-screen compositor changes
- removed unsafe per-line aux elision that could desynchronize `DrawBG_3D()`
  placeholders from the shader/compositor path

Recorded metrics:
- FPS: `~55 FPS -> ~55 FPS` on the Shrek title scene at `3x`
- CPU instructions: `N/A`

Notes:
- This was a gameplay safety fix, not a throughput win.
- It was added after black-screen issues were found in real 3D gameplay.

### `2e543f7` - main repo - liteDS: add LITEV feature flags and enable optimisation flags in build

Type: build / integration

What it changed:
- added `LITEV_*` options and compile definitions
- enabled optimization-oriented build flags
- enabled feature toggles such as `NEON_RENDERER`, `SCANLINE_BATCH`,
  `AGGRESSIVE_SKIP`, `ARM7_HLE_AUDIO`, and `SPU_FAST_INTERP`
- bumped the `melonDS-android-lib` submodule pointer to the matching liteDS
  optimization commit

Recorded metrics:
- FPS: not recorded in commit body
- CPU instructions: not recorded in commit body

Notes:
- This is the main repo integration point for the original liteDS optimization
  layer.

### `874628ba` - `melonDS-android-lib` - Snapshot pending performance changes

Type: preservation snapshot

What it changed:
- preserved the then-current in-progress submodule work before the parent repo
  history rewrite
- includes pending changes across ARM, JIT, GPU, scheduler, Android renderer,
  and build-side files

Recorded metrics:
- FPS: `N/A`
- CPU instructions: `N/A`

Notes:
- This is a local preservation commit, not a measured final optimization pass.

### `5cccfb4` - main repo - Snapshot pending Android build changes

Type: preservation snapshot

What it changed:
- preserved parent-repo Android build configuration changes
- updated the main repo gitlink to the local submodule snapshot commit

Recorded metrics:
- FPS: `N/A`
- CPU instructions: `N/A`

Notes:
- This is a local preservation commit, not a measured final optimization pass.

## Current Caveats

### Local-only commits

The latest main repo and submodule commits are local preservation/rewrite
history, not upstreamed commits.

### ARM9 `StrCmp` HLE

The `9a3e8a95` selective-HLE commit is historically important, but deeper
gameplay testing found that the global `StrCmp` interception could lead to
blank/incorrect gameplay rendering. Treat that commit as:
- a real optimization experiment,
- a real historical commit,
- but not a fully validated final state in its original form.

## Summary

The most important optimization commits in the local history are:
- `7862b8b0`: initial liteDS architectural optimization batch
- `9a3e8a95`: ARM9 helper HLE pass
- `5af12b96`: OpenGL identical-frame skip
- `c1ec33eb`: GL polygon prepack pipeline
- `46d92920`: top-screen gameplay safety fix for the accelerated compositor

The most important testing/tooling commits are:
- `edf6241`: agent workflow rules
- `095ff03`: big-win performance rules
- `97d508a`: debug emulator input harness
- `3482357`: harness runner script and workflow documentation
