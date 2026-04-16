# AGENTS

## Purpose

This project is not trying to remain a pure "optimized melonDS" fork.

The goal is to move toward a faster DS emulator architecture that balances
performance and compatibility, instead of preserving melonDS's default bias
toward accuracy above all else.

The only functionality that must not be compromised is WiFi multiplayer.

## Core Direction

Work should prioritize architectural wins over micro-optimizations.

Small hot-path cleanups are acceptable only when they:
- unlock a larger architectural change,
- remove obvious waste with low risk, or
- produce a clear measured FPS gain.

The project should not spend large amounts of time on minor branch hoists,
small MMIO subsets, or narrow renderer tweaks if they do not materially move
FPS.

When choosing between accuracy and speed outside WiFi-sensitive behavior, this
project should actively favor the better performance architecture instead of
defaulting back to upstream melonDS conservatism.

## What "Closer to DraStic" Means Here

This repo should move toward a performance-first emulator design by doing more
of the following:

1. Reduce dependence on fully emulated ARM9 and ARM7 execution for every task.
2. Use multithreading more aggressively where synchronization boundaries are
   clear.
3. Relax exactness when the result is behaviorally acceptable and does not
   threaten WiFi multiplayer.
4. Redesign rendering around frame-level data reuse instead of repeated
   scanline reconstruction.
5. Reduce JIT dispatch and memory fallback overhead structurally, not just with
   local hot-path edits.

## Architectural Priorities

### 1. Selective HLE

The emulator should investigate high-level handling for work that does not need
to travel through exact ARM9/ARM7 execution.

Candidates include:
- firmware or BIOS service HLE,
- shortcuts for common firmware-facing routines,
- direct subsystem helpers for expensive common behaviors.

Constraint:
- WiFi and local multiplayer critical paths must remain exact.
- Any ARM7 HLE work must exclude WiFi-sensitive behavior unless proven safe.

### 2. Frame-Oriented Renderer

The renderer should move away from repeated per-scanline rebuilding where
possible.

Preferred direction:
- cache BG, OBJ, extpal, and 3D inputs at frame scope,
- reuse stable state across scanlines,
- fall back to exact handling only when mid-frame writes or remaps actually
  require it,
- avoid materializing intermediate planes when a simpler line or frame mode is
  known in advance.

Current 2D rewrite state and direction:
- the current kept renderer stack already has the right upstream building
  blocks:
  - prepared per-frame 2D state,
  - prepared text BG source state,
  - prepared affine source state,
  - prepared sprite state and sprite scanline bins,
  - composed-line replay keyed from exact prepared state
- this means the next 2D wins should come from **expanding the reach of
  composed-line replay**, not adding more downstream line-output caches
- for the current Shrek benchmark, the main remaining structural limiter is
  that composed-line replay is still disabled for large classes of lines:
  - any window usage,
  - BG mosaic,
  - OBJ mosaic,
  - some top-screen 3D compositions

Mandatory planning rule for 2D work:
- before implementing another 2D renderer experiment, explicitly identify:
  - which expensive uncached step it removes,
  - whether it operates **upstream** of final line composition or only caches a
    downstream intermediate/output,
  - what exact ownership key or prepared state it depends on,
  - whether it expands composed-line replay coverage or only optimizes a
    subordinate cache
- if a proposed 2D change does not widen top-level replay coverage or remove a
  major uncached stage in `DrawScanline_BGOBJ()`, it is probably not worth a
  turn
- do not keep “throwing random caches” at the renderer; state the plan first

Current preferred 2D roadmap:
1. cache `WindowMask`/window-state output with an exact key and use it to allow
   composed-line replay on windowed lines
2. after windowed replay works, attack affine/extended/large BG replay from
   prepared source state
3. only then investigate region invalidation on top of the widened replay
   boundary
4. do not prioritize per-line metadata-heavy text BG caches, final sprite-line
   caches, or other downstream line-output caches unless new profiling proves
   they expand top-level replay

### 3. Multithreaded Design

The emulator is currently too single-core limited.

Preferred threading targets:
- renderer preparation,
- GL upload and composition staging,
- frame cache construction,
- other non-timing-critical work that can be moved off the emulator thread.

Avoid naive ARM9/ARM7 parallel execution unless synchronization ownership is
redesigned first.

### 4. JIT and Memory Architecture

Big CPU-side wins should come from:
- safer block chaining,
- reduced dispatch/return overhead,
- broader fastmem coverage,
- fewer generic fallback calls into memory and device handlers.

Do not spend time on minor lookup tweaks unless they clearly unlock a larger
dispatch redesign.

## Accuracy Policy

Accuracy is not the top-level priority everywhere.

Accuracy may be relaxed when:
- the performance gain is meaningful,
- common game compatibility remains acceptable,
- the behavior is stable,
- WiFi multiplayer is not affected.

Accuracy must remain strict in:
- WiFi multiplayer behavior,
- timing-sensitive network-related paths,
- any subsystem whose regression would break multiplayer correctness.

## Measurement Policy

Performance work must be evaluated on device, but the primary metric depends on
whether the benchmark is already at full speed.

Rules:
- If the workload is below real-time, FPS is the primary metric.
- If the workload is already saturating real-time or hovering near `60 FPS`,
  fast-forward throughput becomes the primary metric.
- Once a benchmark is effectively capped at `60 FPS`, do not treat tiny
  real-time FPS movement as the main result. Measure how much uncapped speed or
  fast-forward speed the build can actually sustain.
- CPU instructions and profiles are secondary diagnostics.
- Every optimization pass should be measured with the right top-line metric for
  that benchmark state:
  - normal FPS when below full speed,
  - fast-forward throughput when already at or near full speed.
- CPU instructions alone are never enough to justify keeping a change.
- When reporting a kept or rejected pass near the `60 FPS` ceiling, report
  fast-forward performance first and real-time FPS as a guardrail.
- Rejected experiments should be recorded when they are architectural enough to
  avoid repeating the same dead end.
- No optimization should be kept just because it looks clever in code.

Benchmark harness defaults must be safe for new agents:
- launched metric runs should wait for a verified scene before sampling
- gameplay benchmarking should default to the real gameplay scene, not an early
  boot/menu guess
- launch-only metric runs may default to menu because they do not inject input
- do not record benchmark numbers from a state that has not passed the scene
  gate
- for the current Shrek optimization benchmark, launched runs should default to
  a staged flow: wait for menu, run the gameplay-driving input sequence, wait
  for gameplay, run the cutscene-skip input sequence, then wait for gameplay
  again before sampling

## Workflow Rules

These instructions are mandatory for work in this repo:

- Focus on big wins first. Prefer architectural shifts over local polish.
- Do not spend turns on changes that do not have a plausible path to a real
  throughput win. As a working bar, prefer ideas that could credibly move the
  current benchmark by roughly `>= 5%`, not `1-2%` noise.
- Be ambitious about performance changes where WiFi multiplayer is not put at
  risk.
- Do not report CPU instruction deltas alone as the main result of an
  optimization. Report FPS first.
- If a change is functionally meaningful, commit it.
- Make a commit for every functional change that is kept.
- Each commit must include a description/body that records:
  - FPS change for that commit
  - CPU instruction change for that commit
- If a commit is documentation-only or otherwise has no runtime effect, the
  commit description must explicitly say `FPS: N/A` and `CPU instructions:
  N/A`.
- Do not bundle multiple unrelated functional changes into one commit when they
  can be separated and measured independently.

## Android Harness

Do not rely on ad hoc Android UI automation when validating gameplay scenes on
device if the debug harness is available.

The debug build now exposes a direct emulator input harness through:
- receiver action: `me.magnum.melonds.DEBUG_EMULATOR`
- receiver class: `me.magnum.melonds.debug.EmulatorDebugReceiver`
- wrapper script: `tools/bench/run_android_harness.sh`

The harness can:
- launch a ROM through `EmulatorActivity`,
- inject emulator button sequences directly,
- inject DS touchscreen presses,
- toggle fast-forward,
- load a savestate,
- and pull a screenshot after the sequence.

Preferred workflow:
- use `tools/bench/run_android_harness.sh` for scene entry and screenshot capture
  instead of `adb input keyevent`, UI tapping guesses, or temporary controller
  remaps;
- for Shrek specifically, menu-only validation is not enough;
- use a sequence that reaches a non-menu gameplay-relevant scene before judging
  correctness or performance;
- if a title needs repeated confirmation presses, encode that in the harness
  sequence instead of manually repeating shell commands.

Example:
- `tools/bench/run_android_harness.sh --uri 'content://...' --press-a 30`

## Big-Win Triage

Before implementing an optimization, ask whether it attacks one of the current
large structural costs:
- top-screen 2D composition / `DrawScanline_BGOBJ()` architecture,
- frame-stable renderer caching,
- JIT dispatch / block-boundary overhead,
- fastmem / fallback collapse for common ARM9 memory traffic,
- multithreaded staging that removes work from the emulator thread.

If it does not clearly target one of those, it is probably not worth a turn.

## Dead-End Avoidance

Do not keep probing the same low-yield area once it has already failed in
multiple structural forms on the current benchmark.

Current explicit dead-end warning:
- selective HLE at the traced Shrek service-helper/service-init boundary has
  already failed in multiple forms and should not get more speculative passes
  unless new evidence shows a materially different, higher-level subsystem cut.

Current rejected Shrek performance experiments that should not be retried
blindly:
- full text BG surface cache:
  - measured `38.397 FPS` on one pass and `36.901 FPS` on the adaptive variant,
    versus a `39.693 FPS` baseline
  - when mixed into the later renderer stack it also correlated with major
    scene-dependent drops into the `20-25 FPS` range
  - why it lost: it cached and rebuilt very large decoded text BG surfaces, so
    invalidation events turned into heavy full-surface memory traffic and
    rebuild spikes; the broad cache shape was too expensive relative to the
    useful reuse
- disabling full-line BG cache writes on top of tile-row caching:
  - measured `35.995 FPS` versus the same `39.693 FPS` baseline
  - why it lost: the composed-line cache was still paying for itself, and
    turning off writes removed profitable full-line replay opportunities while
    leaving the rest of the renderer work in place
- GL prep-thread expansion:
  - measured `37.008 FPS` versus the same `39.693 FPS` baseline on the first
    isolated pass
  - later direct A/B on a different renderer stack was near-noise rather than a
    clear win, so this path is still not a credible big-yield target
  - why it lost: it moved too little meaningful work off the emulator thread
    while adding extra staging, copies, and bookkeeping; the ownership boundary
    was not strong enough to beat the added overhead
- JIT block-analysis refactor groundwork:
  - latest direct pair was `30.187 FPS` with the analysis refactor versus
    `30.697 FPS` without it, and live gameplay smoothness was reported as worse
    with the refactor enabled
  - earlier one-off harness runs were contradictory, so this area is noisy and
    should not be judged from a single sample
  - why it lost: it added analysis and container-copy overhead on the compile
    path without a finished background worker or another structural consumer, so
    the extra compile-time work could outweigh any restore-side benefit on this
    workload
- exact per-line text BG frame-cache ownership:
  - measured `39.552 FPS` against a kept renderer build at `42.872 FPS`
  - why it lost: it added too much per-line metadata and memory traffic to a
    subordinate cache without expanding top-level composed-line replay
- final sprite-line output cache:
  - measured `40.921 FPS` from a manually verified gameplay frame, below the
    kept renderer build at `42.872 FPS`
  - why it lost: it cached a large downstream sprite output buffer instead of
    increasing composed-line replay coverage, so it duplicated line data and
    bookkeeping without removing enough upstream work

Current 2D dead-end pattern:
- upstream replay and prepared-state ownership have been the winning direction
- downstream line-output caches and per-line metadata-heavy caches have been the
  losing direction
- when choosing the next 2D experiment, bias toward the strongest replay
  boundary, not the easiest local cache
- combined restore stack that re-enabled full text BG surface cache, GL
  prep-thread expansion, sprite scanline binning, and the JIT analysis
  groundwork:
  - rough measured result `32.312 FPS`
  - why it lost: the full text BG surface cache dominated the interaction and
    dragged the whole stack down; treat that combined stack as poisoned by the
    surface-cache design rather than evidence against sprite binning itself

When an area is showing repeated rejects, pivot to another architectural lever
instead of refining the same idea.

## What Counts as a Good Change

A good performance change in this repo is one that does at least one of these:
- raises measured FPS in a repeatable way,
- removes a recurring architectural bottleneck,
- reduces emulator-thread work without increasing synchronization or GL call
  overhead elsewhere,
- creates a path toward selective HLE or frame-oriented rendering.

## What to Avoid

Avoid spending cycles on:
- tiny renderer branch rearrangements,
- speculative GL upload tricks that increase call count,
- generic dispatch-loop rewrites without tight control over timing boundaries,
- changes that preserve exactness everywhere but do not materially improve FPS.

## Non-Negotiable Constraint

Do not break WiFi multiplayer.
