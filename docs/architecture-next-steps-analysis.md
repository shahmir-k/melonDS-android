# Architecture Next Steps Analysis

This document summarizes the current `quickmelonDS` architecture, the main
remaining bottlenecks, what recent experiments imply, and what the next
high-value architectural steps should be.

Relevant code:

- [NDS.cpp](/Users/shahmir/Documents/GitHub/quickmelonDS/melonDS-android-lib/src/NDS.cpp)
- [GPU.cpp](/Users/shahmir/Documents/GitHub/quickmelonDS/melonDS-android-lib/src/GPU.cpp)
- [GPU2D_Soft.cpp](/Users/shahmir/Documents/GitHub/quickmelonDS/melonDS-android-lib/src/GPU2D_Soft.cpp)
- [GPU3D_OpenGL.cpp](/Users/shahmir/Documents/GitHub/quickmelonDS/melonDS-android-lib/src/GPU3D_OpenGL.cpp)
- [drastic-like-performance-priority-list.md](/Users/shahmir/Documents/GitHub/quickmelonDS/docs/drastic-like-performance-priority-list.md)
- [performance-optimization-plan.md](/Users/shahmir/Documents/GitHub/quickmelonDS/docs/performance-optimization-plan.md)

## Current Architecture

melonDS is still fundamentally an event-driven emulator with one main
performance-critical thread.

At a high level:

1. `NDS::RunFrame()` starts a frame and loops until `GPU.TotalScanlines` is
   set.
2. Each scheduler slice advances ARM9 to the next target, runs timers, advances
   GPU3D, advances ARM7 to the same target, then dispatches scheduled events.
3. `GPU::StartFrame()` begins scanline scheduling.
4. On every HBlank, both 2D engines render the current line and prerender the
   next line's sprites.
5. The software 2D renderer still composes around `DrawScanline()` and
   `DrawScanline_BGOBJ()`.
6. The GL 3D path still finishes through `GLRenderer::RenderFrame()`.

This means the emulator still pays for:

- frequent CPU scheduler rendezvous,
- repeated 2D scanline reconstruction,
- repeated sprite/state decoding,
- VRAM coherency derivation and flat-view syncing,
- JIT dispatch/block-boundary overhead,
- and fallback traffic through generic memory/device handlers.

## What Makes It Slow

The primary issue is not a single bad loop. The problem is that too much work
remains serialized on the emulator thread.

### 1. Fine-grained scheduler exits

ARM9, ARM7, DMA, timers, IRQs, LCD timing, and GPU work constantly meet at
short scheduler boundaries. This preserves accuracy, but it increases the
frequency of JIT exits and dispatcher overhead.

### 2. ARM9 and ARM7 are still not meaningfully parallel

Even when ARM7 threading exists, the core loop still waits before system-event
processing. This is not a clean two-core execution model.

### 3. 2D rendering is still scanline-oriented

The 2D renderer has improved with caches, but the architecture is still built
around reconstructing effective BG/OBJ/window/blend state line by line.

### 4. Sprite work remains expensive

Sprite ordering, mosaic, OBJ window behavior, and per-line visibility all add
branch-heavy work on a hot path.

### 5. JIT cost is not just instruction execution

The remaining CPU-side overhead is not just "run faster code". It includes:

- block lookup,
- dispatch returns,
- memory fallback calls,
- invalidation handling,
- exact timing exits,
- and generic MMIO/device transitions.

### 6. GL is not the real main bottleneck

The GL path still has overhead, but the failed GL prep-thread expansion and the
current hot symbols both indicate that the main remaining cost is still the
emulator-thread-side 2D and CPU architecture.

## What Recent Experiments Imply

Recent experiments in this repo give a clear directional signal.

### Confirmed useful

- sprite scanline binning
- existing composed-line caching
- existing stable text BG caching
- existing decoded text tile-row caching

These show that reusable 2D prepared state can pay off.

### Confirmed bad

- full text BG surface cache
- adaptive full text BG surface cache
- disabling full-line BG cache writes

These show that "cache more" is not enough. Cache shape, invalidation scope,
and rebuild cost matter more than cache size.

### Near-noise or not credible

- GL prep-thread expansion

This suggests that small renderer-side staging changes are not enough unless
they remove substantial emulator-thread work.

### Not worth keeping

- JIT background-analysis groundwork

This added compile-path work without a finished background worker or another
structural consumer. It did not change the actual dispatch architecture enough
to justify the cost.

## Priority Order For Next Steps

The best next steps, in order, are:

1. frame-oriented 2D renderer, but region-based and invalidation-driven
2. JIT dispatch reduction and safe block chaining
3. broader ARM9 fastmem/fallback collapse
4. selective non-WiFi HLE only at coarse subsystem boundaries
5. multithreading that serves the renderer architecture
6. NEON after the structural work, not before it

## 1. Frame-Oriented 2D Renderer

This remains the highest-value remaining lever.

The right version is not a giant whole-surface cache. The failed full text BG
surface-cache pass already showed that broad rebuilds and invalidation spikes
can be worse than line reconstruction.

The right design is:

- snapshot 2D inputs at frame or stable-region scope,
- build reusable prepared state,
- replay cached composed regions or lines,
- invalidate only touched regions,
- and fall back to exact handling only where needed.

### Recommended shape

Introduce a design centered on something like:

- `Frame2DSnapshot`
- per-engine prepared BG state
- per-engine prepared OBJ/sprite bins
- precomputed window/blend applicability where stable
- region validity masks
- targeted invalidation from OAM/VRAM/register writes

### Why this is the best next step

It matches:

- the current hot 2D composition costs,
- the success of sprite scanline binning,
- the success of decoded tile-row reuse,
- and the failure of overly large coarse caches.

The goal is to stop treating every scanline as a fresh composition problem.

## 2. JIT Dispatch Reduction and Safe Block Chaining

This is still the best remaining CPU-side architectural lever.

The next real win is not more compile-time analysis by itself. The next real
win is reducing how often the CPU has to return to generic dispatch.

### Correct direction

- safe static-exit linking
- safe same-window block chaining
- reduced dispatcher round-trips
- exact exits when timers, IRQs, DMA, or WiFi-sensitive timing require it

### Wrong direction

- more analysis objects without a strong consumer
- generic dispatch loop rewrites
- looser chaining that ignores timing boundaries

The failure of the JIT analysis groundwork in this session is a warning that
compile-path work with no real execution-path change is not enough.

## 3. Broader ARM9 Fastmem / Fallback Collapse

This is still a strong CPU-side opportunity.

The right version is:

- more stable-memory direct paths,
- fewer generic slowmem/device/MMIO fallbacks for common ARM9 traffic,
- tighter specialization where the memory behavior is stable.

The wrong version is:

- speculative ITCM fastmem work without airtight invalidation,
- broad fragile remap assumptions,
- MMIO widening without proof that the covered traffic is hot enough.

The goal is fewer expensive fallback calls, not just more special-case code.

## 4. Selective HLE

Selective HLE is still valid, but only at coarse boundaries.

The repo history already shows:

- BIOS-level HLE is not the big win,
- narrow helper interception is unreliable,
- service-init loop replacement was not enough,
- and WiFi-sensitive paths are off-limits.

That means the only HLE worth more time is:

- a coarse non-WiFi subsystem cut,
- with strong measured call density,
- with a clear behavioral contract,
- and with no WiFi timing risk.

Anything smaller is likely to waste time.

## 5. Multithreading

Multithreading is still useful, but not as the immediate top lever.

The safe threading targets are:

- 2D cache construction,
- region/frame preparation,
- GL upload staging for already-finalized data.

The unsafe target is still naive ARM9/ARM7 parallel execution. The timing
ownership is too tightly coupled for that to be the next step.

So threading should support the frame-oriented renderer, not replace it.

## 6. NEON

NEON can help, but it is not the main architectural answer.

There is already NEON support in:

- [GPU2D_NEON.cpp](/Users/shahmir/Documents/GitHub/quickmelonDS/melonDS-android-lib/src/GPU2D_NEON.cpp)
- [GPU2D_NEON.h](/Users/shahmir/Documents/GitHub/quickmelonDS/melonDS-android-lib/src/GPU2D_NEON.h)

### Good NEON targets

- RGB555 conversion
- brightness and blend passes
- bulk line post-processing
- regular no-window/no-special-effect inner loops
- data-parallel line transforms with stable layout

### Bad NEON targets

- scheduler logic
- JIT dispatch
- MMIO/fallback architecture
- branch-heavy sprite/state decode

So NEON is worth extending later, but it will not solve the main bottlenecks by
itself.

## Recommended Next Concrete Implementation

If only one major next step should be taken, it should be:

## Build a region-based frame-oriented 2D preparation layer on top of the
## existing line-cache and sprite-binning work

Concretely:

1. create a per-frame/per-engine prepared snapshot object
2. split cache validity by stable regions, not one giant full-frame surface
3. feed sprite bins, text tile-row reuse, and composed-line replay from that
   snapshot
4. invalidate only touched regions on OAM/VRAM/register changes
5. keep exact fallback for the invalidated regions

This direction is the best fit for:

- the current hot symbols,
- the experiments that worked,
- the experiments that failed,
- the project's performance-first direction,
- and the requirement to preserve WiFi multiplayer behavior.

## Second-Best Alternative

If renderer work is paused, the next best architectural target is:

- safe JIT block chaining with tight scheduler-boundary control

But the renderer architecture still offers the clearest remaining large win.
