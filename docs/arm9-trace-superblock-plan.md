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

1. **hot-root promotion**
2. **dedicated trace code objects**
3. **direct internal chaining**
4. **deferred state commit**
5. **cold side exits back to the existing JIT/runtime**

The key idea is:

- spend more effort at compile/link time,
- pay much less per hot dynamic edge at runtime.

That avoids the failure mode seen in several recent experiments where extra
runtime fast-path machinery ended up adding its own overhead.

Important refinement:

- the goal is not "more trace machinery"
- the goal is "more generated trace code, less steady-state runtime
  machinery"
- any approach that improves traceability while adding always-on helper-side or
  compile-path bookkeeping is suspect by default

## What We Learned From The First Implementation Passes

The plan direction was correct, but several early implementation shapes were
not.

What the rejected passes proved:

1. **helper-managed pseudo-traces are not enough**
   - active trace windows, trace-root caches, and helper-side direct-edge
     attempts can raise `arm9_trace_hit`, but they still leave the old
     block-boundary contract intact
   - once the extra metadata checks and activation work are counted, the FPS
     win disappears or reverses

2. **scheduler widening by itself is not the answer**
   - widening the cadence increased trace hits and reduced `normal_max`
   - but overall frame shape got worse and FPS dropped hard
   - this means we cannot “fake” a superblock win by only stretching the old
     dispatch session

3. **root activation caching is lower value than it looks**
   - caching trace metadata on root blocks or via side caches still adds
     bookkeeping on the hot path
   - the hot path cannot afford many extra pointer chases, map lookups, or
     branchy activation checks

4. **the current compiled blocks are too self-contained to reuse naively**
   - each block is compiled as its own unit with its own epilogue/continuation
     expectations
   - helper-side chaining can skip some lookup work, but it does not remove the
     structural cost of block-local state commit and block-local exit handling

5. **the next real step must be generated-code internal-edge execution**
   - not more metadata shells
   - not more helper-side caching
   - not more direct-edge experiments bolted onto the same block epilogue

6. **production builds must not carry profiler-lane trace baggage**
   - recent `LITEV_PROFILE=off` recovery proved that profiler-lane trace
     planning and helper-managed trace runtime can materially drag down the
     production build if they leak into the `off` configuration
   - future trace work must therefore assume:
     - diagnostic-only trace analysis belongs behind `LITEV_PROFILE`
     - runtime trace code must justify itself directly in `off`
     - broad compile-path trace maintenance is not acceptable in production

So the updated interpretation is:

- the profiler still clearly says “superblocks/traces” are the right
  architecture
- but the implementation must move faster toward a **dedicated trace code
  object / trace ABI**, instead of spending more time on runtime metadata
  management around the existing block ABI

### New Hard Rule

Do not add new always-on helper-managed trace layers to the normal JIT path.

If a future trace step requires:

- root activation bookkeeping on every block entry
- trace window checks on every continuation
- per-block trace-map churn in production
- or compile-path trace maintenance for cold/non-promoted blocks

then it is probably the wrong implementation shape.

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

### Correct Mental Model

The target is **not**:

- "make every normal JIT block trace-aware"

The target is:

- "promote a tiny hot set of exact roots into separate trace code objects and
  jump into those directly"

That distinction matters because the rejected pseudo-trace work kept trying to
make the generic block path smarter instead of making the hot promoted path
different.

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

- reuse the existing block JIT as the safety net and source of traceability
  metadata,
- reuse the existing invalidation and fallback machinery where possible,
- and bail out cleanly to the normal block continuation path on any uncertainty.

Important refinement from the rejected passes:

- do **not** assume the existing compiled block entrypoints can simply be
  stitched together into an efficient runtime trace
- they are still valuable as:
  - correctness fallback,
  - invalidation ownership,
  - and trace planning inputs
- but the hot winning path likely needs its **own codegen mode** with a
  dedicated trace ABI

Additional refinement:

- the normal JIT block path should remain simple and generic
- promoted traces should be layered *beside* it, not smeared through it
- a trace root should ideally patch or replace the normal root entry so the hot
  path does not first enter a normal block and only then "discover" that it is
  part of a trace

### 2. Compile More, Run Simpler

Recent rejects showed that extra runtime checks and helper layers can erase the
intended win.

So this design should prefer:

- trace assembly work during compile/link,
- patched direct edges,
- and minimal runtime dispatch inside the trace.

But "compile more" also needs a hard limit:

- do not do broad trace-prep work for every block compile
- do not persist recipes or maintain trace plans universally in production
- promotion/build work should happen only for hot roots that already crossed a
  threshold

### 3. Internal Edges Must Be Cheap

The whole point is to make hot edges closer to:

- raw branch to next compiled code

and farther from:

- helper call
- budget check helper
- lookup helper
- state writeback
- re-entry ceremony

Additional concrete rule from the first failed passes:

- if an internal edge still requires:
  - trace-map lookup,
  - vector walk,
  - root activation cache setup,
  - extra epilogue-specific guards,
  - or helper re-entry,
  then it is probably still too expensive

Practical rule:

- the trace root may pay a one-time dispatch cost
- internal edges may not
- if an internal edge is not close to a plain generated branch, the design is
  off course

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

## Promotion-First Model

The high-level structure should be:

1. **normal block JIT remains the default**
2. **profiling identifies exact hot trace roots**
3. **only those roots are promoted into dedicated trace objects**
4. **root lookup resolves directly to the promoted trace entry**
5. **internal trace edges stay inside generated code**
6. **all uncommon behavior side-exits back to the normal block JIT**

This should replace the earlier instinct to make all ordinary blocks carry
trace-runtime awareness.

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

5. **Promotion candidacy profiling**
   - identify exact hot roots that are both:
     - stable enough to justify promotion
     - narrow enough to avoid dragging in mixed-mode or dynamic-exit baggage
   - keep this rooted in real exit instruction shape, not just block start PCs

### Success Criteria

Proceed only if the data confirms:

- a small number of edge pairs dominate dynamic control flow
- many dispatch returns are made up of chains that could have been kept inside a
  superblock
- mode-switch and invalidation barriers are the minority on the hot benchmark

## Phase 1: Profile-Only Planning Layer

### Objective

Add a trace planning layer without yet changing runtime behavior.

This phase is explicitly **profile-only infrastructure**:

- allowed in `LITEV_PROFILE=on`
- compiled out in `LITEV_PROFILE=off` unless some piece later becomes part of
  the actual promoted runtime path

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

4. **Promotion record**
   - exact root PC
   - hotness / stability score
   - chosen trace family
   - reason selected over nearby alternatives

### Planning Rules

The first trace planner should only include edges that are:

- ARM9
- ARM mode to ARM mode
- not known mode-switch exits
- not usermode/CPSR-restore block-transfer exits
- not invalidation-fragile MMIO paths

### Important Scope Limit

Do not try to solve arbitrary dynamic CFG formation in v1.

v1 should be **hot linear promoted traces**, not a full hyperblock system.

Also:

- no production runtime dependency on this metadata layer yet
- this phase is allowed to be rich only because it can stay out of `off`
  builds

## Phase 2: Promoted Trace Code Object Skeleton

### Objective

Create the promoted trace execution shell and side-exit contract before doing
serious optimization.

### Implementation Shape

1. promote one exact hot root into a **dedicated compiled trace object**
2. enter that trace with current ARM9 live-state register mapping
3. the hot root must resolve directly to trace entry
4. execute multiple guest blocks inside that one trace object
5. internal edges must stay inside the same generated code object
6. when a side exit is hit:
   - materialize guest state
   - return to existing continuation/runtime path

### Important Correction To The Original Plan

The first implementation attempts treated this phase as:

- helper-side trace metadata
- runtime activation caches
- direct continuation shortcuts around existing block entrypoints

That shape is now considered a **proven low-yield sub-lane**.

Phase 2 should now explicitly mean:

- add a separate trace codegen mode or trace wrapper code object
- resolve promoted roots directly to trace entries
- stop trying to get a superblock win from helper-managed pseudo-traces alone

### Keep It Narrow

The first runnable version should support:

- one narrow hot family only for the first execution pass
- preferably the max/fallthrough family rooted at proven sites like
  `02068440`, because it is structurally cleaner than generic computed-PC exits
- optionally a second exact-root family only if the first one is already sound

It should explicitly exclude:

- mode-switch exits
- generic computed-target misses
- uncommon memory-assisted `PC` writes
- Thumb-mode trace formation
- any "all blocks are trace-capable" runtime shell

### Validation Goal

This phase is a correctness scaffold, but it still has one performance rule:

- a non-promoted block must not get slower just because the trace system exists

It must:

- boot
- pass the normal Shrek gameplay harness
- preserve profiler visibility
- build in both `LITEV_PROFILE=on` and `off`

And it should avoid repeating already-rejected shapes:

- no more root-trace metadata caches as the main optimization
- no more scheduler widening as a surrogate for trace execution
- no more exact-root direct-edge hacks attached to the ordinary block epilogue

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

This is also likely the first place a real superblock win appears. The earlier
runtime experiments mostly failed because they preserved eager block-local state
commit and only optimized around it.

So phase 3 must be introduced incrementally:

1. only defer `R15/CPSR/cycle` materialization first
2. keep all other state behavior unchanged
3. validate against known hot return sites before broadening
4. keep the non-promoted path untouched

### Success Signal

This is where the profiler should start showing a meaningful `post` reduction.

## Phase 4: Direct Internal Edge Patching

### Objective

Stop treating hot internal trace edges as helper-driven continuation events.

### Mechanism

For trace-internal edges inside the dedicated trace object:

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

Important update:

- this phase should now be treated as the **real start** of the winning runtime
  path, not as a later polish step after helper-managed traces
- helper-managed traces were good enough for measurement, but not good enough
  for the intended speedup
- if phase 2 does not already put the root directly into generated trace code,
  phase 4 should not proceed

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

## Phase 6: Trace Selection And Promotion Policy

### Objective

Ensure compile-time overhead does not become its own slowdown.

### Policy

Only form traces when:

- source blocks are hot enough
- successor edge is hot enough
- edge stability is high enough
- expected chain length is large enough
- predicted promoted-path win is high enough to justify extra compile work

### Initial Heuristics

1. trace only from top hot exit sites
2. trace only the dominant successor edge first
3. cap trace length tightly in v1
4. abandon trace growth when side exits become too frequent
5. do not promote neighboring sites just because they are similar

### Why This Matters

This repo has already seen “good-looking machinery” lose because its own
bookkeeping cost outweighed the benefit.

Trace formation must be selective, not universal.

Recent lesson:

- exact-root promotion beat broad smart-runtime ideas
- over-broad site expansion and broad recipe persistence lost
- promotion policy should therefore bias toward a very small hot set

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

Additional explicit rejection rule from the current attempts:

- if a trace experiment mainly improves `arm9_trace_hit` counts or reduces
  `normal_max`, but does so through more helper-side/runtime metadata work
  rather than fewer real internal boundaries, do not treat that as progress on
  its own

## Immediate Next Steps

### Step 1

Freeze the current lesson set:

- no more helper-managed pseudo-trace work in production
- no more broad compile-path trace-prep work in production
- no more runtime trace activation shells for non-promoted blocks

### Step 2

Use the existing profiler data to choose **one** exact promoted root family:

- first choice remains hot ARM same-mode max/fallthrough roots around
  `02068440`

### Step 3

Move planning/promotion selection behind `LITEV_PROFILE` unless it is directly
part of the promoted runtime path.

### Step 4

Implement a correctness-first promoted trace root that:

- replaces normal root entry on hit
- executes multiple internal edges in generated code
- side-exits cleanly to the existing block JIT

### Step 5

Only after the first promoted root family is sound:

- add deferred state commit
- then broaden to the next exact-root family

## Final Recommendation

This should be treated as the next serious ARM9 architectural lane.

The profiler is no longer asking for another tiny helper tweak. It is saying:

- too much time is spent crossing JIT boundaries,
- too much state is being committed too often,
- and the real win is to execute larger chunks of guest work without returning
  to generic continuation machinery.

That makes a selective ARM9 trace/superblock system the strongest high-upside
next move currently available in this codebase.
