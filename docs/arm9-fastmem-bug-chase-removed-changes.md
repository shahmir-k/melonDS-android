# ARM9 Fastmem Bug Chase: Removed Changes

This report is reconstructed from the Codex session log at:

`/Users/shahmir/.codex/sessions/2026/04/20/rollout-2026-04-20T15-36-56-019dac65-5a9e-75e1-9908-ee20823a63f1.jsonl`

It covers the profiler-side changes, optimization-path isolates, and debug-only probes that were introduced during the non-profiled fastmem bug chase and later removed again.

It does not list the fixes that were kept.

## Removed Profiler Improvements

### 1. `LiteProfile.h`: switch `NowNs()` to `CNTVCT` and reinterpret `NsPerFrame()` as ticks

This was an attempt to reduce profiling overhead by moving more profiler timing onto the same raw counter already used by some callsite buckets. It was later removed.

```diff
inline uint64_t NowNs()
{
    if (!kEnabled)
        return 0;
+#if defined(__aarch64__)
+    uint64_t value = 0;
+    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
+    return value;
+#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
+#endif
}

inline double NsPerFrame(const std::atomic<uint64_t>& counter)
{
-    return static_cast<double>(counter.load(std::memory_order_relaxed)) /
-           static_cast<double>(kLogEveryFrames) / 1000000.0;
+    return TicksPerFrameToMs(counter);
}
```

### 2. `ARMJIT.cpp`: remove self-timed helper-body measurements from hot fast-DTCM helpers

This was the measurement-side cleanup pass that stopped timing the tiny fast-DTCM helpers directly and tried to rely on callsite timing instead. It was later backed out.

```diff
template <bool Write, int ConsoleType, int Tag>
void SlowBlockTransfer9FastDTCMProfiled(u32 addr, u64* data, u32 num, ARMv5* cpu)
{
-    const uint64_t totalStart = LITE_PROFILE_NOW_NS();
    LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockTransferCalls);
    if constexpr (Write)
        LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockTransferWrites);
    else
        LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockTransferReads);
-    LITE_PROFILE_SCOPE(timer, LiteProfile::gFrame.ARM9SlowBlockTransferNs);
    NoteSlowBlockSource<Tag>();
-    const uint64_t fastPathStart = LITE_PROFILE_NOW_NS();

    if (TryDirectDTCMBlockTransfer9<Write>(addr, data, num, cpu))
    {
-        const uint64_t totalElapsed = LITE_PROFILE_NOW_NS() - totalStart;
-        const uint64_t elapsed = LITE_PROFILE_NOW_NS() - fastPathStart;
        LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockFastDTCMDirectCalls);
-        LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastDTCMDirectNs, elapsed);
        if constexpr (Tag == SlowBlockProfile_FastStackLoad)
        {
-            LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStackLoadTotalNs, totalElapsed);
-            LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStackLoadNs, elapsed);
-            LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStackLoadDirectNs, elapsed);
            if (num <= 2)
            {
                LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockFastStackLoad_1_2);
-                LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStackLoad_1_2_Ns, elapsed);
            }
            else if (num <= 4)
            {
                LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockFastStackLoad_3_4);
-                LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStackLoad_3_4_Ns, elapsed);
            }
        }
        else if constexpr (Tag == SlowBlockProfile_FastStore)
        {
-            LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStoreTotalNs, totalElapsed);
-            LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStoreNs, elapsed);
-            LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastStoreDirectNs, elapsed);
        }
        return;
    }

    SlowBlockTransfer9Impl<Write, ConsoleType>(addr, data, num, cpu);
-    const uint64_t totalElapsed = LITE_PROFILE_NOW_NS() - totalStart;
-    const uint64_t elapsed = LITE_PROFILE_NOW_NS() - fastPathStart;
    LITE_PROFILE_ADD(LiteProfile::gFrame.ARM9SlowBlockFastDTCMFallbackCalls);
-    LITE_PROFILE_ADD_VALUE(LiteProfile::gFrame.ARM9SlowBlockFastDTCMFallbackNs, elapsed);
}
```

### 3. `LiteProfile.h`: rewrite top-level slowblock reporting to use callsite ticks instead of helper-body timings

This was paired with the helper cleanup above and was also removed.

```diff
+    const uint64_t arm9SlowBlockCallsiteTicks =
+        CounterValue(gWindow.ARM9SlowBlockCallsiteFastStackPreTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteFastStackWrapTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteFastStackPostTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteFastStorePreTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteFastStoreWrapTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteFastStorePostTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteGenericLoadPreTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteGenericLoadWrapTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteGenericLoadPostTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteGenericStorePreTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteGenericStoreWrapTicks) +
+        CounterValue(gWindow.ARM9SlowBlockCallsiteGenericStorePostTicks);

    Platform::Log(Platform::LogLevel::Info,
        "[LITEV_PROFILE] arm9_jit dispatch=%.3fms/%.1f lookup=%.3fms/%.1f compile=%.3fms/%.1f guest_cycles=%.1f last_hit=%.1f chain=%.1f/%.1f ret_normal=%.1f ret_stop=%.1f ret_idle=%.1f ret_halt=%.1f cache_hit=%.1f cache_miss=%.1f hle=%.1f slowread=%.3fms/%.1f slowwrite=%.3fms/%.1f slowblock=%.3fms/%.1f",
        NsPerFrame(gWindow.ARM9JitDispatchNs),
        CountPerFrame(gWindow.ARM9JitDispatchCalls),
        NsPerFrame(gWindow.ARM9JitLookupNs),
        CountPerFrame(gWindow.ARM9JitLookupCalls),
        NsPerFrame(gWindow.ARM9JitCompileNs),
        CountPerFrame(gWindow.ARM9JitCompileCalls),
        CountPerFrame(gWindow.ARM9JitGuestCycles),
        CountPerFrame(gWindow.ARM9JitLastBlockHits),
        CountPerFrame(gWindow.ARM9JitChainHits),
        CountPerFrame(gWindow.ARM9JitChainAttempts),
        CountPerFrame(gWindow.ARM9JitReturnsNormal),
        CountPerFrame(gWindow.ARM9JitReturnsStop),
        CountPerFrame(gWindow.ARM9JitReturnsIdle),
        CountPerFrame(gWindow.ARM9JitReturnsHalt),
        CountPerFrame(gWindow.ARM9JitBlockCacheHits),
        CountPerFrame(gWindow.ARM9JitBlockCacheMisses),
        CountPerFrame(gWindow.ARM9LibHLEHits),
        NsPerFrame(gWindow.ARM9SlowReadNs),
        CountPerFrame(gWindow.ARM9SlowReadCalls),
        NsPerFrame(gWindow.ARM9SlowWriteNs),
        CountPerFrame(gWindow.ARM9SlowWriteCalls),
-        NsPerFrame(gWindow.ARM9SlowBlockTransferNs),
+        TicksValuePerFrameToMs(arm9SlowBlockCallsiteTicks),
        CountPerFrame(gWindow.ARM9SlowBlockTransferCalls));

    Platform::Log(Platform::LogLevel::Info,
-        "[LITEV_PROFILE] slowblock_source_ns generic_load=%.3fms total=%.3fms generic_store=%.3fms total=%.3fms fast_stack=%.3fms total=%.3fms direct=%.3fms fallback=%.3fms fast_store=%.3fms total=%.3fms direct=%.3fms fallback=%.3fms",
+        "[LITEV_PROFILE] slowblock_source_ns generic_load=%.3fms total=%.3fms generic_store=%.3fms total=%.3fms fast_stack_callsite=%.3fms fast_store_callsite=%.3fms",
        NsPerFrame(gWindow.ARM9SlowBlockGenericLoadNs),
        NsPerFrame(gWindow.ARM9SlowBlockGenericLoadTotalNs),
        NsPerFrame(gWindow.ARM9SlowBlockGenericStoreNs),
        NsPerFrame(gWindow.ARM9SlowBlockGenericStoreTotalNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStackLoadNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStackLoadTotalNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStackLoadDirectNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStackLoadFallbackNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStoreNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStoreTotalNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStoreDirectNs),
-        NsPerFrame(gWindow.ARM9SlowBlockFastStoreFallbackNs));
+        TicksValuePerFrameToMs(fastStackCallsiteTicks),
+        TicksValuePerFrameToMs(fastStoreCallsiteTicks));
```

### 4. `tools/bench/run_android_simpleperf.sh`: harness-driven, gameplay-gated simpleperf workflow

This script change was part of the same “measurement cleanup” lane and was later dropped with that pass.

```diff
-# Run an Android ROM launch under simpleperf and save stat/report artifacts.
+# Reach a harness-validated Android scene, then run simpleperf and save stat/report artifacts.

 PACKAGE="${MELONDS_PACKAGE:-}"
 ACTIVITY="${MELONDS_ACTIVITY:-me.magnum.melonds.ui.emulator.EmulatorActivity}"
 URI=""
 DURATION=10
 FREQ=1000
 OUT_DIR=""
 EVENTS="${SIMPLEPERF_EVENTS:-task-clock,cpu-cycles,instructions}"
 ADB="${ADB:-adb}"
 RUN_RECORD=1
+RUN_LABEL=""
+REQUIRE_PROFILE_BUILD="${HARNESS_EXPECT_PROFILE:-any}"
+LAUNCH_ONLY=0
+declare -a HARNESS_ARGS=()
```

## Removed Optimization-Path Isolates

### 5. `ARMJIT_A64/ARMJIT_LoadStore.cpp`: disable block-transfer fastmem only in non-profiled builds

This isolate forced ARM9 block transfers to avoid the fast patched path when `LITEV_PROFILE=off`. It was temporary and later removed.

```diff
bool compileFastPath = fastMemoryEnabled
    && !usermode
    && loadStoreShapeAllowed
    && condCompatible;
+#if !LITEV_PROFILE
+    if (Num == 0)
+        compileFastPath = false;
+#endif
```

### 6. `ARMJIT_A64/ARMJIT_LoadStore.cpp`: disable the tiny fixed fast-DTCM helper specialization

This isolate turned off the `1-4`-word fixed helper call path while debugging the off-build crash. It was later removed.

```diff
-    const bool fixedFastDTCMHelper = Num == 0
-        && compileFastPath
-        && expectedTarget == ARMJIT_Memory::memregion_DTCM
-        && regsCount <= 4;
+    const bool fixedFastDTCMHelper = false;
```

### 7. `ARMJIT.h`: global fastmem kill switch

This was the blunt isolate that disabled all JIT fastmem emission. It was removed once the failure was narrowed further.

```diff
-    bool FastMemoryEnabled() const noexcept { return FastMemory; }
+    bool FastMemoryEnabled() const noexcept { return false; }
```

### 8. `ARMJIT_A64/ARMJIT_LoadStore.cpp`: disable only the single-op fastmem patch path

This isolate restored block fastmem while disabling the ordinary single-op `Comp_MemAccess()` fastmem patch path. It was later removed.

```diff
-    if (NDS.JIT.FastMemoryEnabled() && ((!Thumb && CurInstr.Cond() != 0xE) || NDS.JIT.Memory.IsFastmemCompatible(expectedTarget)))
+    if (false && NDS.JIT.FastMemoryEnabled() && ((!Thumb && CurInstr.Cond() != 0xE) || NDS.JIT.Memory.IsFastmemCompatible(expectedTarget)))
    {
```

### 9. `ARMJIT_A64/ARMJIT_LoadStore.cpp`: temporary combination patch used during split isolation

This patch was the combined “single-op off, block fastmem back on, fixed helper back on” state. All of it was temporary and later removed.

```diff
-    if (false && NDS.JIT.FastMemoryEnabled() && ((!Thumb && CurInstr.Cond() != 0xE) || NDS.JIT.Memory.IsFastmemCompatible(expectedTarget)))
+    if (NDS.JIT.FastMemoryEnabled() && ((!Thumb && CurInstr.Cond() != 0xE) || NDS.JIT.Memory.IsFastmemCompatible(expectedTarget)))
@@
-    bool compileFastPath = fastMemoryEnabled
+    bool compileFastPath = fastMemoryEnabled
         && !usermode
         && loadStoreShapeAllowed
         && condCompatible;
-    compileFastPath = false;
@@
-    const bool fixedFastDTCMHelper = false;
+    const bool fixedFastDTCMHelper = Num == 0
+        && compileFastPath
+        && expectedTarget == ARMJIT_Memory::memregion_DTCM
+        && regsCount <= 4;
```

## Removed Debug-Only Instrumentation

### 10. `ARMJIT_A64/ARMJIT_LoadStore.cpp`: rewrite-boundary logging

This was added to identify which JIT patch site was being rewritten before failure. It was later removed.

```diff
if (it != LoadStorePatches.end())
{
    LoadStorePatch patch = it->second;
    LoadStorePatches.erase(it);

+    Log(LogLevel::Debug,
+        "RewriteMemAccess pc=%p off=%td patch_off=%d patch_size=%u func=%p\n",
+        pc, pcOffset, patch.PatchOffset, patch.PatchSize, patch.PatchFunc);
+
    ptrdiff_t curCodeOffset = GetCodeOffset();
```

### 11. `ARMJIT_Memory.cpp`: JIT fault logging

This was added to show whether a fault was being rewritten to the slow path and what memory state it saw. It was later removed.

```diff
    if (memStatus[faultDesc.EmulatedFaultAddr >> PageShift] == memstate_Unmapped)
        rewriteToSlowPath = !nds.JIT.Memory.MapAtAddress(faultDesc.EmulatedFaultAddr);

+    Log(LogLevel::Debug,
+        "JIT fault pc=%p emu_addr=%08x mem_state=%u rewrite=%d cpu=%u\n",
+        faultDesc.FaultPC,
+        faultDesc.EmulatedFaultAddr,
+        memStatus[faultDesc.EmulatedFaultAddr >> PageShift],
+        rewriteToSlowPath,
+        nds.CurCPU);

    if (rewriteToSlowPath)
```

### 12. `ARMJIT_A64/ARMJIT_Compiler.cpp`: patched-stub identity logging

This logged the generated load/store fastmem stub addresses so rewrite logs could be mapped back to specific stub signatures. It was later removed.

```diff
                    PatchedStoreFuncs[consoleType][num][size][reg] = GetRXPtr();
+                    Log(LogLevel::Debug,
+                        "PatchedStore func=%p console=%d cpu=%d size=%d reg=%d\n",
+                        PatchedStoreFuncs[consoleType][num][size][reg], consoleType, num, 8 << size, reg);
                    if (num == 0)
                    {
```

```diff
                        PatchedLoadFuncs[consoleType][num][size][signextend][reg] = GetRXPtr();
+                        Log(LogLevel::Debug,
+                            "PatchedLoad func=%p console=%d cpu=%d size=%d sign=%d reg=%d\n",
+                            PatchedLoadFuncs[consoleType][num][size][signextend][reg],
+                            consoleType, num, 8 << size, signextend, reg);
                        if (num == 0)
                            MOV(X1, RCPU);
```

## Short Summary

The removed changes fell into three buckets:

1. Profiler cleanup experiments that tried to stop the profiler from self-measuring tiny helpers.
2. Narrow fastmem isolates that selectively disabled pieces of the ARM9 fastmem path to localize the non-profiled crash/white-screen behavior.
3. Temporary logging added to shared rewrite/fault boundaries and prebuilt fastmem stub generation.

The important practical point is that these were not all “real optimizations” being abandoned permanently. Most were either temporary measurement cleanups or temporary isolates/probes used to localize the off-build fastmem bug.

## Best Candidates To Reintroduce

These are the removed changes most worth bringing back in a controlled form.

### A. Harness-driven `simpleperf` workflow

Source item: `#4`

Status: reintroduced as `tools/bench/run_android_simpleperf.sh`.

Why it is a good candidate:
- it improves production-build measurement without touching emulation behavior
- it matches the repo goal of understanding production hot paths in a gameplay-valid scene
- it is operationally useful even if no profiler redesign lands immediately

Reintroduction guidance:
- restore the harness-first, gameplay-gated `simpleperf` flow
- keep it as a measurement tool, separate from optimization code changes

### B. Remove direct timing from tiny fast-DTCM helpers

Source items: `#2` and `#3`

Why it is a good candidate:
- it addresses the real profiler pathology found during this bug chase: tiny hot helpers were heavily measuring profiler overhead
- it keeps the useful attribution signal while moving timing responsibility to the lighter-weight callsite buckets
- it is directly aligned with the goal of making profiled optimization data better match production behavior

Reintroduction guidance:
- do not reintroduce it as an ad hoc patch
- bring it back as an intentional profiler redesign
- after reintroducing it, establish a new profiled baseline and compare future candidates against that new baseline only

### C. Tick-based timing API, but only as a redesign

Source item: `#1`

Why it is only a partial candidate:
- the underlying idea is valid: `CNTVCT` is cheaper than repeated `std::chrono` calls in hot paths
- the exact removed patch was too blunt because it silently changed the meaning of existing `Ns` helpers

Reintroduction guidance:
- if this comes back, it should come back as a separate explicit tick-based API
- do not globally redefine `NowNs()` / `NsPerFrame()` semantics under existing nanosecond names
- keep unit meaning explicit in counter names and print helpers

## Not Good Reintroduction Candidates

These should stay out unless needed again for one-off debugging:

- `#5` through `#9`: they were temporary fastmem isolate edits, not product changes
- `#10` through `#12`: they were debug-only logging probes and should only return behind an explicit debug flag when needed
