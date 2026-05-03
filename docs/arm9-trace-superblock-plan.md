# ARM9 Trace/Superblock JIT Plan

## Goal

Build a trace/superblock execution path for the ARM9 JIT that materially
reduces block-boundary overhead in the Shrek gameplay benchmark by:

- executing more guest instructions per JIT entry,
- removing repeated continuation/helper boundaries on hot same-mode paths,
- deferring guest-state writeback until real exits,
- and preserving correctness by falling back to the existing block model on
  uncommon or unsafe paths.

This is intended as a **major architectural optimization**, not another local
return-helper tweak.

## Why This Is The Right Next Big Bet

Current profiled Shrek runs show the main wall is still ARM9 JIT boundary
overhead, not memory helpers:

- `arm9_jit dispatch ~= 25.8-28.4 ms/frame`
- `execute ~= 10.2-15.5 ms/frame`
- `post ~= 8.0-11.5 ms/frame`
- `slowblock ~= 0.17-0.20 ms/frame`

Representative return pressure is also very high:

- `arm9_ret_pcwrite arm_stack ~= 1.6k-2.3k/frame`
- `arm9_ret_branch arm_reg_lr ~= 0.84k-1.4k/frame`
- `normal_max ~= 1.27k-1.38k/frame`

Interpretation:

- the emulator is already spending much more time on block boundaries than on
  the guest instructions inside those blocks,
- direct helper cleanup helped, but only modestly,
- further narrow return-site tricks will likely keep landing in diminishing
  returns,
- the largest remaining CPU-side win is to **remove the boundary model itself**
  from hot same-mode control-flow chains.

## Core Thesis

The next drastic ARM9 speedup should come from:

1. **trace formation**
2. **direct internal chaining**
3. **deferred state commit**
4. **cold side exits back to the existing JIT/runtime**

The key idea is:

- spend more effort at compile/link time,
- pay much less per hot dynamic edge at runtime.

That avoids the failure mode seen in several recent experiments where extra
runtime fast-path machinery ended up adding its own overhead.

## What This System Should Do

### Current Model

The current hot path is effectively:

1. enter a compiled block
2. execute a limited number of guest instructions
3. write back guest-visible state
4. call/branch through continuation machinery
5. decide the next block
6. re-enter another compiled block
7. repeat thousands of times per frame

### Target Model

The new hot path should become:

1. enter a compiled trace
2. execute several already-linked basic blocks / JIT blocks inside one unit
3. keep hot ARM9 state live in host registers through internal edges
4. do not write back state at each internal block boundary
5. only exit when timing, memory, invalidation, mode, or rare control-flow
   hazards require it

## Non-Negotiable Constraints

The system must preserve:

- WiFi multiplayer correctness
- existing memory invalidation safety
- correct ARM/Thumb mode transitions
- correct IRQ/event/timing behavior
- correct fallback behavior for MMIO and unsafe memory paths

The first implementation should be intentionally narrow:

- ARM9 only
- same-mode ARM traces first
- no ARM7 changes
- no WiFi-specific shortcuts
- no speculative HLE mixed into the first trace pass

## Design Principles

### 1. Keep The Existing JIT As The Safety Net

Do not replace the current block JIT in one shot.

The trace path should:

- reuse the existing compiled blocks as building blocks or inputs,
- reuse the existing invalidation and fallback machinery where possible,
- and bail out cleanly to the normal block continuation path on any uncertainty.

### 2. Compile More, Run Simpler

Recent rejects showed that extra runtime checks and helper layers can erase the
intended win.

So this design should prefer:

- trace assembly work during compile/link,
- patched direct edges,
- and minimal runtime dispatch inside the trace.

### 3. Internal Edges Must Be Cheap

The whole point is to make hot edges closer to:

- raw branch to next compiled code

and farther from:

- helper call
- budget check helper
- lookup helper
- state writeback
- re-entry ceremony

### 4. State Commit Should Be Exit-Oriented

Within a trace, keep these live as long as possible:

- `R15`
- `CPSR`
- cycle counters / timestamp-facing state
- other already-live host-mapped CPU state

Commit them only on:

- side exit
- mode transition
- timing stop
- invalidation-sensitive exit
- cold fallback path

## Proposed Architecture

## Phase 0: Instrumentation Before Runtime Changes

### Objective

Measure whether a trace design would actually collapse the current hottest
dynamic edges.

### Add These Profilers

1. **Edge-pair profiling**
   - count `(block_start_pc -> successor_block_start_pc)` frequency
   - separate by exit class:
     - `arm_pc_stack`
     - `arm_reg_lr`
     - `arm_reg_other`
     - `arm_imm`
     - `normal_max`

2. **Chain-length profiling**
   - how many current JIT blocks execute before returning to outer dispatch
   - histogram buckets:
     - `1`
     - `2`
     - `3-4`
     - `5-8`
     - `9-16`
     - `17+`

3. **Traceability profiling**
   - classify why a hot edge cannot remain inside a same-mode trace:
     - mode switch
     - budget stop
     - target mismatch
     - MMIO / unsafe memory barrier
     - invalidation-sensitive boundary
     - dynamic branch miss

4. **Commit pressure profiling**
   - count how many state commits are currently paid per frame
   - split by:
     - normal block exit
     - computed-PC return
     - max-block fallthrough
     - mode transition

### Success Criteria

Proceed only if the data confirms:

- a small number of edge pairs dominate dynamic control flow
- many dispatch returns are made up of chains that could have been kept inside a
  superblock
- mode-switch and invalidation barriers are the minority on the hot benchmark

## Phase 1: Trace Metadata Layer

### Objective

Add a trace planning layer without yet changing runtime behavior.

### Required Data Structures

1. **Trace node**
   - source JIT block id / PC
   - mode
   - region ownership
   - entry code pointer
   - exit classification

2. **Trace edge**
   - source block
   - destination block
   - exit class
   - hit count
   - safety flags
   - whether direct same-mode chaining is allowed

3. **Trace plan**
   - ordered block list
   - internal direct-link edges
   - side exits
   - commit points
   - maximum guest-cycle budget

### Planning Rules

The first trace planner should only include edges that are:

- ARM9
- ARM mode to ARM mode
- not known mode-switch exits
- not usermode/CPSR-restore block-transfer exits
- not invalidation-fragile MMIO paths

### Important Scope Limit

Do not try to solve arbitrary dynamic CFG formation in v1.

v1 should be **hot linear traces**, not a full hyperblock system.

## Phase 2: Runtime Skeleton With No Real Win Expected Yet

### Objective

Create the trace execution shell and side-exit contract before doing serious
optimization.

### Implementation Shape

1. enter trace with current ARM9 live-state register mapping
2. execute constituent blocks in sequence
3. when an internal edge is taken, branch directly inside the trace
4. when a side exit is hit:
   - materialize guest state
   - return to existing continuation/runtime path

### Keep It Narrow

The first runnable version should support:

- hot same-mode fallthrough
- traced same-mode immediate branches
- traced same-mode exact return edges already proven hot

It should explicitly exclude:

- mode-switch exits
- generic computed-target misses
- uncommon memory-assisted `PC` writes
- Thumb-mode trace formation

### Validation Goal

This phase is a correctness scaffold. It does not need to win yet.

It must:

- boot
- pass the normal Shrek gameplay harness
- preserve profiler visibility
- build in both `LITEV_PROFILE=on` and `off`

## Phase 3: Deferred State Commit

### Objective

Remove repeated state materialization at internal trace edges.

### What To Keep Live

Keep these live across internal edges:

- guest `R15`
- `CPSR`
- cycle accumulator / local cycle delta
- hot ARM state already mapped in host registers

### What To Materialize Only On Exit

- canonical CPU memory copy of guest registers
- visible `CPSR`
- timing-facing values needed by the outer scheduler
- `LastJitBlock*` update if required by the fallback contract

### Risks

This is one of the highest-risk layers because recent broken continuation
rewrites failed on state ownership mismatches.

So phase 3 must be introduced incrementally:

1. only defer `R15/CPSR/cycle` materialization first
2. keep all other state behavior unchanged
3. validate against known hot return sites before broadening

### Success Signal

This is where the profiler should start showing a meaningful `post` reduction.

## Phase 4: Direct Internal Edge Patching

### Objective

Stop treating hot internal trace edges as helper-driven continuation events.

### Mechanism

For trace-internal edges:

- emit direct host branches to the next compiled segment
- patch them once the target is available
- invalidate them when the underlying code cache is invalidated

### Preferred Initial Edge Classes

1. hot max-block fallthrough
2. same-mode immediate branches
3. exact-site same-mode `BX LR`
4. exact-site same-mode hot stack `PC` returns already validated by profiler

### Why This Order

- max-block and immediate edges are easier than generic computed-target edges
- exact-site return edges already proved valuable in the current direct-return
  work
- generic register-target prediction should not be phase 1 of this system

## Phase 5: Trace Budgeting And Event Awareness

### Objective

Replace frequent tiny boundary checks with trace-level timing control.

### Design

Instead of checking budget at each block continuation:

- compute a local trace run allowance
- let the trace execute until:
  - local cycle budget exhausted
  - event/IRQ boundary crossed
  - unsafe side exit

### Important Constraint

Do not blindly relax timing cadence.

The trace-level run allowance must still respect:

- ARM9 scheduler deadlines
- event queue ownership
- interrupt visibility
- WiFi-sensitive timing behavior

### Safe v1 Rule

Use a conservative trace budget first.

The first timing win should come from:

- fewer continuation boundaries

not from:

- aggressively stretching timing windows

## Phase 6: Trace Selection Policy

### Objective

Ensure compile-time overhead does not become its own slowdown.

### Policy

Only form traces when:

- source blocks are hot enough
- successor edge is hot enough
- edge stability is high enough
- expected chain length is large enough

### Initial Heuristics

1. trace only from top hot exit sites
2. trace only the dominant successor edge first
3. cap trace length tightly in v1
4. abandon trace growth when side exits become too frequent

### Why This Matters

This repo has already seen “good-looking machinery” lose because its own
bookkeeping cost outweighed the benefit.

Trace formation must be selective, not universal.

## Phase 7: Invalidation And Safety Model

### Objective

Make trace invalidation no less safe than the current JIT.

### Requirements

1. any base block invalidation invalidates owning traces
2. trace-internal patched edges must never jump into stale code
3. `LastJitBlockAddr` / `LastJitBlockEntry` safety assumptions must remain
   correct
4. side exits after invalidation must cleanly fall back to ordinary lookup

### Strong Recommendation

Reuse existing cache/invalidation ownership as much as possible.

Do not invent a separate fragile invalidation system if the current block cache
can be extended to own traces.

## Phase 8: Expand Coverage Only After A Real Win

### Objective

Broaden traceable control flow only after the narrow ARM same-mode path produces
repeatable FPS movement.

### Expansion Order

1. ARM same-mode hot returns and fallthrough chains
2. broader ARM register-target chains
3. Thumb same-mode traces
4. mixed ARM/Thumb traces only if clearly worth it

Do not start with mixed-mode complexity.

## Measurement Plan

Every phase that changes runtime behavior must be tested with:

1. `LITEV_PROFILE=true`
2. Android device harness
3. real gameplay scene gate
4. matched baseline vs candidate runs
5. `LITEV_PROFILE=false` build+boot validation when shared JIT/runtime code is
   touched

### Primary Success Metric

For current Shrek:

- FPS first

### Secondary Diagnostics

- CPU instructions
- profiler movement
- specifically:
  - `dispatch`
  - `post`
  - return-family counts
  - chain-length growth
  - side-exit reasons

### What A Real Win Should Look Like

The signature of success is:

- meaningful repeatable FPS increase
- substantial `dispatch` drop
- substantial `post` drop
- smaller `execute` movement than `dispatch/post`
- little or no regression in `slowblock`

If `execute` barely changes but `dispatch/post` fall sharply, that is the right
  result.

## Rejection Criteria

Reject or revert a trace pass if it:

- lowers FPS on repeat
- increases compile-time overhead enough to offset runtime gains
- introduces white-screen / crash / menu-gate failures
- depends on profiler-on behavior that does not hold with `LITEV_PROFILE=off`
- adds a lot of runtime bookkeeping without reducing `dispatch/post`

## Immediate Next Steps

### Step 1

Implement Phase 0 instrumentation:

- edge-pair profiling
- chain-length histogram
- traceability/miss-reason split
- commit-pressure counts

### Step 2

Use the measured edge data to identify the first narrow trace family:

- likely same-mode ARM traces rooted at the existing hot return/fallthrough
  sites

### Step 3

Add a trace metadata/planning layer without changing runtime behavior.

### Step 4

Implement a correctness-first narrow trace execution shell for one hot trace
family.

## Final Recommendation

This should be treated as the next serious ARM9 architectural lane.

The profiler is no longer asking for another tiny helper tweak. It is saying:

- too much time is spent crossing JIT boundaries,
- too much state is being committed too often,
- and the real win is to execute larger chunks of guest work without returning
  to generic continuation machinery.

That makes a selective ARM9 trace/superblock system the strongest high-upside
next move currently available in this codebase.
