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
