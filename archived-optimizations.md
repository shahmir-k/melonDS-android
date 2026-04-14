# Archived Optimizations

This file preserves optimization work that is not part of the current clean
split stack.

The active optimization base is the `split-opt-bundle` branch in this worktree.
It keeps only the measured slices we decided to carry forward.

## Current Kept Stack

Main repo commits:
- `a98417d` Enable release-style native build flags
- `08fcaaf` Enable NEON 2D conversion path
- `6ee6b19` Point submodule at approved core slices
- `d4fa6a6` Point submodule at scheduler window slice
- `95d6be1` Point submodule at renderer slices
- `7622e75` Point submodule at JIT cache slice
- `609b630` Point submodule at timer deadline slice

Submodule commits:
- `e3ac7516` Add optional fast SPU interpolation path
- `6f440e45` Add optional NEON 2D conversion path
- `9ba4ab83` Increase scheduler iteration window
- `6f353c54` Skip identical OpenGL 3D frames when VRAM is unchanged
- `3a65dcc4` GPU3D: prepack GL polygons ahead of VCount215
- `0f1d84ed` Add NonStupidBitField Any helper
- `8c0787be` Add per-CPU last-block JIT cache
- `ea5d963e` Cache next timer overflow deadlines

## Archived Optimization Commits

These commits are not part of the clean split stack and should be treated as
archive/reference material, not the working optimization baseline.

Main repo:
- `0ecb562` `liteDS: add LITEV feature flags and enable optimisation flags in build`
  - old broad integration point for the initial liteDS bundle
- `b27b69b` `Snapshot pending Android build changes`
  - preservation snapshot, not a kept optimization step
- `d54823d` `Rebuild optimization summary after history rewrite`
  - documentation only

Submodule:
- `7862b8b0` `liteDS: apply all performance optimisations (OPT-1 through OPT-7)`
  - broad mixed bundle; too many risky changes in one commit
- `9a3e8a95` `Add ARM9 library HLE for hot libc helpers`
  - historical win, but not safe enough in its original global form
- `46d92920` `GPU2D: keep accel aux coherent on top screen`
  - correctness fix for a GPU2D stack that has not been cleanly reintroduced
- `874628ba` `Snapshot pending performance changes`
  - large preservation snapshot, not a measured clean commit

## Extracted Slices From Archived Work

These were pulled out, tested, and intentionally not carried forward.

- `NextTarget()` ctz bit-scan
  - regressed throughput
- interpreter halt/IRQ batching
  - unstable / not worth keeping
- ARM7 dedicated worker thread
  - render-visible desync and severe FPS regression
- ARM7 audio HLE path
  - intentionally skipped from the current direction
- `AGGRESSIVE_SKIP`
  - not worth keeping; unsafe direction for this benchmark
- broad ARM9 slowmem + MMIO + div/sqrt bundle
  - slightly worse in practice
- narrowed ARM9 slowmem / block-transfer-only retry
  - also worse in practice

## What Remains Unported From Archived Work

The main remaining area from old work is GPU2D top-screen composition changes
that were never cleanly split into a safe commit chain.

If we revisit archived work, that should be the first place to mine rather than
reopening the rejected ARM7 or slowmem slices above.
