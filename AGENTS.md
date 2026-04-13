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

Performance work must be evaluated primarily by FPS on device.

Rules:
- FPS is the primary metric.
- CPU instructions and profiles are secondary diagnostics.
- Every optimization pass should be measured with FPS, not just CPU
  instructions.
- Rejected experiments should be recorded when they are architectural enough to
  avoid repeating the same dead end.
- No optimization should be kept just because it looks clever in code.

## Workflow Rules

These instructions are mandatory for work in this repo:

- Focus on big wins first. Prefer architectural shifts over local polish.
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
