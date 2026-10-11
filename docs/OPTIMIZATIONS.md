# What SereneDS changes on top of melonDS

SereneDS is a fork of [melonDS Android](https://github.com/rafaelvcaetano/melonDS-android) (Rafael
Caetano) running a fork of the [melonDS](https://melonds.kuribo64.net/) core
([shahmir-k/quickmelonDS](https://github.com/shahmir-k/quickmelonDS), branch `liteDS-main`, based on
upstream melonDS `10a173b5`, 2026-06-07). This page lists everything the fork adds, per area, one line
per item.

Every core optimisation is a compile-time `LITEV_*` option. All are OFF by default in the core's
`CMakeLists.txt`; the app turns on the shipping set in `app/build.gradle.kts`. The per-option write-ups
are in the core's `docs/LITEV-OPTIMIZATIONS.md`. Only options that are ON in the app build are listed as
shipped. Section 13 lists the ones that are not shipped.

## How to read this page

- **Exactness classes.** **A** = exact: the emulated console's memory, registers and cycle timing are
  byte-identical to the same build with the option off. **B** = deterministic timing approximation:
  every run and every device gives the same result, but emulated timing differs from upstream (see
  "What we don't change" below). **R** = emulation identical; only the picture or sound you get differs.
- **Numbers** come from our development measurements, mostly on the **Anbernic RG DS** (RK3566, 4x
  Cortex-A55, Android 14). "Emu CPU" = emulator-thread CPU time per frame. Scenes: Pokémon White (PW)
  town (frame 6500 of a recorded play session), gift box (f10100), overworld (f17000), title (f1300);
  Shrek Smash n' Crash Racing (Shrek) single-player race; Mario Kart DS (MKDS). "Headless" = the core's
  command-line benchmark. Older numbers were taken on earlier builds, and gains do not simply add up. If a
  lever has no measurement, it says so.
- **vs DraStic** notes appear only where our docs compare the technique with DraStic's design (we studied
  a decompilation of it). They describe where an idea came from. They do not claim parity.

## What we don't change (accuracy policy)

- Emulation stays **deterministic**: the same inputs give the same result on every run and every device,
  so Netplay and savestates stay in sync. Nothing depends on host timing or thread scheduling.
- Speed comes first. The fork accepts approximations that are deterministic and **version-locked**
  (class B: fixed cycle estimates, batched events). They change emulated timing compared with upstream
  melonDS, but every SereneDS build of the same version agrees with every other. Multiplayer peers must
  run the same SereneDS version.
- Pervasive cycle-timing changes are off-limits. `RELAXED_ARM9_TIMING` measured −27% and stays OFF.
- Every class-B lever must pass Netplay and LAN multiplayer checks on two devices before it ships.
  Renderer changes must match stock melonDS on screenshots. The fast software 3D renderer is the one
  documented exception (see section 5b).

## Headline results (RG DS)

- All HLE on vs off: emu CPU −19% (town) to −28% (overworld); town uncapped 78 → 89 fps.
- Build levers (PGO/CSPGO, ThinLTO, hot ordering, A55 tuning, plus `JIT_EXIT_PROTO`): PW emu CPU ~11.0 → 9.5 ms (−14%).
- JIT code quality for the in-order A55: emu CPU −4.4% to −5.5% (PW scenes, Shrek race; Shrek 97 → 101 fps uncapped).
- Render-thread pinning alone: 28 → 40.6 fps (software renderer, July build); the largest single lever.
- Full 19,297-frame PW play session replays at an average of 60.00 fps (normal speed), 0 crashes.
- 2-player Shrek Netplay on two RG DS: 56.1/55.7 fps in 3-minute races. 3-player Hosted Netplay: ~60/59/59.

---

## 1. CPU / JIT (ARM64 backend)

### Dispatch and block linking
- `JIT_DISPATCH` + `LINK_UNCOND/COND/FALLTHROUGH` (A): the block dispatcher runs in generated code and static block exits jump directly to the next block. Alone: flat to small (headless A55 menu −1.4%/+2.8%). Needed by the levers below.
- `JIT_ICACHE` (A): a 2-way guest-PC → host-code cache at each dynamic exit site. 96.6% hit rate; no isolated fps measurement.
- `JIT_DIRECTPATCH` (A): exit sites that always go to one target become guarded direct branches. Flat on device; kept because it costs nothing.
- `JIT_RAS` + link fixes (A): guest function returns use a return-address stack. Full dispatcher lookups ~2175 → ~185 per frame; dispatch share of the emu thread 5.8% → 0.94% (PW overworld).
- `JIT_REGION_CACHE` (A): the dispatcher keeps the last 4 code regions per CPU instead of returning to C++. PW overworld −3.4% cycles (headless on device).
- `JIT_IRQMASK_CONT` (A): the dispatcher keeps running past interrupts the guest has masked. PW overworld −0.87% cycles.
- `JIT_BUDGET_REG` (A): the time-slice budget stays in a register across block hops. PW overworld −1.7%, Shrek −2.7% cycles; app 63.3 → 63.8 fps.
- `JIT_EXIT_PROTO` (A): leaner linked hops (no PC store, stop check folded into the budget compare, conditions on host flags). PW −0.5% cycles, Shrek flat. vs DraStic: its exit protocol.
- `JIT_DP_REVERTLIST` (A): invalidating a block reverts only the promoted exits instead of scanning 2 MB of slots. PW scene-transition hitches 46-74 ms → 19-25 ms.
- `JIT_CYCLE_BATCH` (A): per-instruction cycle adds are merged until something reads the cycle count. −0.9% cycles, −1.8% L1I misses.

### Registers and flags
- `JIT_CONDFOLD` (A): compound guest conditions are evaluated with native NZCV (5 → 2 host instructions). Neutral on fps.
- `JIT_FIXEDREG` (A): guest flags stay in host flags on the ALU hot path. Host ARM9 time −2.3%; not measured on the device.
- `JIT_GLOBALREG` (A): the hottest unbanked guest registers stay pinned to host registers across blocks. ARM9 execution −2.6..−4.9% on device; fps flat.
- `JIT_LAZYFLAGS` (A): no reserved CPSR register; NZCV is computed lazily, which frees a register for pinning. JIT code −5.7%, L1I misses −3.2%. vs DraStic: its lazy flags.
- `JIT_FLAGMERGE`, `JIT_FLAGS_BFXIL`, `JIT_NZ_BRANCH` (A): cheaper flag merges (8 → 3 instructions); branches on N/Z already in host flags. Neutral to small.

### Memory access and fastmem
- `MEM_DTCM_FASTMEM` (A): DTCM (the guest stack, 84-99% of slow-helper calls) is mapped into the fastmem window. 55.9 → 57.8 fps (software renderer).
- `MEM_DTCM_BLOCK` + `MEM_MAINRAM_LOAD` (A): LDM/STM to DTCM and MainRAM word loads are emitted inline. Block-transfer helper calls −98%; headless +4.4% (menu) / +6.1% (race).
- `JIT_LDMSTM`, `JIT_LDM_FASTMEM`, `JIT_BLOCKXFER_FAST`, `JIT_BLOCKXFER_MAINRAM` (A): block transfers as LDP/STP pairs with fastmem for loads, and a fast path in the slow helper. −0.5% instructions where measured. vs DraStic: paired block copies.
- `JIT_USERSTM_FASTMEM` (A): the NitroSDK thread-context save (`STM ^`) uses fastmem. Slow block stores 180k → 0 per test run.
- `JIT_COND_MEMGUESS` + `JIT_STORE_REPROMOTE` (A): fewer one-time fastmem faults (~70% fewer store faults); a store that was demoted to the slow path is promoted back later.
- `JIT_CP15_INLINE` (A): no-op cache-maintenance MCRs and the DTCM MRC run inline instead of in the interpreter. Fallbacks 1330 → 220 per frame; overworld emu CPU 7.24 → 7.05 ms, uncapped 88.9 → 90.7 fps.
- `JIT_VWRAM_LOAD` (A): ARM7 loads from VRAM mapped as ARM7 memory are inline. ~1.5% (multiplayer remote consoles).
- `JIT_ARM7_CPUARG` + the ARM9 equivalent (A): slow-memory helpers get the CPU as an argument instead of a thread_local lookup. 34.7 → 35.3 fps; TLS dTLB misses 6.5% → 0.3%.
- `JIT_MEMBASE_PIN`, `JIT_LDR_ALIGNCHK` (A): the fastmem base is set once per slice; aligned loads go straight to fastmem. Part of the code-quality set below.
- `IO_DISPATCH_TABLE` (A): ARM9 32-bit I/O goes through a dense jump table. IOWrite32 self time 0.42 → 0.27%.

### Code quality for the in-order Cortex-A55
- `JIT_EXIT_TAIL`, `JIT_COLD_EXITS`, `JIT_MEMBASE_PIN`, `JIT_FLAGS_BFXIL`, `JIT_R15_ELIDE`, `JIT_LDR_ALIGNCHK`, `JIT_CODECYCLES_DEAD`, `JIT_NZ_BRANCH`, `JIT_MOV_ELIDE` (A, 9 levers): exit stubs and cold branch edges move to the block tail, and dead stores and moves are dropped. Host instructions per guest instruction 7.3 → 6.2; emu CPU −5.5% overworld, −5.2% town, −4.4% gift box, −5.2% Shrek race.

### JIT compile bursts (scene-entry hitches)
- `JIT_REGALLOC_SUFFIX`, `JIT_IMMLOGICAL_CACHE`, `JIT_PATCHMAP_RESERVE` (A): cheaper compiling. Town hitch frames −4 ms.
- `JIT_PATCHMAP_FLAT` (A): no memory allocation per compiled memory access. Burst frames 33/23/40 → 30/20/36 ms.
- `JIT_POOL_ALLOC`, `JIT_PATCHLOG`, `JIT_FLATMAPS`, `JIT_FLUSH_CTR`, `JIT_REGALLOC_USEMASK` (A): free-list allocators, flat maps, and cache flushes only where `CTR_EL0` requires them. Burst frames −14%.

### Idle and wait-loop skipping
- `IDLE_AGGRESSIVE` (B): loops that poll a register are detected as idle and skipped. Headless +7.5% fps; flat in the app when measured (July build). vs DraStic: its poll-loop detection.
- `JIT_IDLE2` (B): wider idle detection plus a run-time proof for wait loops that store or call. Shrek race emu CPU −5.4% (95 → 96 fps uncapped); The Sims 3 boot −16% (Mac); PW ±0 (its waits already halt the CPU).

## 2. HLE (native versions of hot Nintendo SDK routines)

How it works: the emulator recognises a library function by its **exact code bytes** (signature checked
at compile time; the block is invalidated if any checked byte changes) and runs a native C++ version
that leaves exactly the memory and registers the guest code would. Anything outside the handled case
falls back to the game's own code. Each hook was checked in compare mode with 0 differences over millions
of calls on recorded PW (and MKDS) segments. HLE hooks are **class B**: each call charges a fixed cycle estimate. Games
whose code does not match run unchanged (for example Shrek). Runtime masks: `debug.litev.a9hle` /
`debug.litev.a7hle`.

All HLE together (RG DS, uncapped): emu CPU town 10.74 → 8.65 ms (−19%), gift box −26%, overworld −28%,
title −26%, loading −27%; fps town 78 → 89, gift box 80 → 89, title 74 → 79.

**ARM9 (`LITEV_A9HLE`, all hooks on by default):**

| Mask | Routine | Games | Measured |
|---|---|---|---|
| 1 | `OS_WaitIrq` spurious wake-ups (NitroSDK wakes every waiter on every IRQ; ~220 useless thread switches/frame in PW) | PW, PB, W2 | emu CPU −13.3% (overworld), 86.5 → ~92 fps |
| 2, 4 | `OS_SetIrqFunction` / `OS_GetIrqFunction` | PW, PB, W2 | Mac −1.4% instructions; not measured on device |
| 8 | Whole HBlank IRQ natively when its callback list is empty (~265/frame) | PW, PB, W2 | emu CPU −9.6% (overworld) |
| 16 | `MI_SendGXCommandAsync` display-list chunks through the bulk geometry path | PW, PB, W2 | town −3.6%, gift box −4.8% emu CPU |
| 32 | CARD ROM read, one page natively | PW, PB, W2 | Mac loading −6..−7%; not measured on device |
| 64 | `MIi_UncompressBackward` (LZ decompression) | any NitroSDK game | device loading −4% |
| 128 | NNS G3D material | PW, PB, W2 | with 256/512: town −3.2%, gift box −4.2% emu CPU |
| 256 | `_ll_sdiv` (64-bit divide) | PW, PB, W2, MKDS, The Sims 3 | (in the row above) |
| 512 | `MI_SendGXCommandAsync` synchronous part | PW, PB, W2 | (in the row above) |
| 1024 | Display-list DMA-end IRQ at delivery | PW, PB, W2 | Mac: PW town ARM9 guest work −35%; not measured on device |
| 2048 | NNS G3D shape chain | PW, PB, W2 | (with 1024) |
| 4096 | `VEC_Normalize` | PW, PB, W2 (position-independent) | (with 1024) |
| 8192 | NNS G3D NODEDESC joint matrix step | PW, PB, W2 | Mac: PW title ARM9 guest work −46%; not measured on device |
| 16384 | MKDS stereo sample effect | MKDS | ~26% of MKDS ARM9 guest work per console (Mac) |
| 32768 | NNS G3D material animation (texture SRT/pattern) | PW, PB, W2 | Mac PW town −0.9% instructions |
| 65536 | NNS G3D SBC command loop | PW, PB, W2 | Mac PW overworld −1.0% instructions |

**ARM7 sound driver (`LITEV_A7HLE`):** `SND_ExChannelMain` (16-channel envelope/LFO/sweep/volume/pitch/pan),
`SND_SeqMain` (sequencer), `MI_CpuCopy32`, SND hardware commit. Applies to PW, PB, W2, plus MKDS's
`ExChannelMain` variant. Overworld emu CPU −3.4%. MKDS `SeqMain`/commit variants exist but are OFF by
default (section 13).

**Related GX shortcuts:** `GX_CPUSEND` (B, `MI_CpuSend32` to the geometry FIFO through the bulk path, any
game) and `GX_SEND_DIRECT` (A, display lists read in place). Numbers are in section 4.

## 3. Scheduler, timing, I/O, DMA

- `EVENT_SLICES` (B): each CPU runs to the true next event instead of 64-cycle slices. Headless +13.5% fps; headless A55 +7..16%. Includes the SM64DS fix (section 12). vs DraStic: event slices.
- `INSTANT_DIVSQRT` (B): the divider and square-root results are computed when the register is written, with no completion event. App 24.4 → 26.1 fps (+7%).
- `LAZY_DIV` (A) / `LAZY_SQRT` (A relative to INSTANT_DIVSQRT): results are computed when they are read (PW's HBlank handler rewrites the divider ~264x/frame). Small (−1.3% instructions).
- `SPU_BATCH` (B) + `COARSE_RTC` (B): 8 audio samples per scheduler event; inert 32 kHz RTC ticks skipped. Events 1622 → 596 per frame; 26.1 → 27.0 fps together. vs DraStic: both are its event-flood cuts.
- `TIMER_FAST` (A): the next timer-overflow deadline is cached. +0.7% headless.
- `SCHED_LEAN` (A): timers are caught up only when due or read; the next-event time is cached. ~1%.
- `SCHED_DRAIN` (A): DMA9/GXFIFO-stall steps drain in one iteration while ARM7 is halted. −0.8..−1% cycles.
- `DMA_TIMING_LAZY`, `DMA_ARMED_MASK` (A): DMA timing lookups only where used; only armed channels are checked. −1.0..−1.5% instructions together with `GXFIFO_READ_INLINE`.
- `CART_SYNC` (B): cartridge reads refill the FIFO at once instead of one event per word (PW polls ~6300x/frame while streaming the map). Overworld emu CPU −3.3%.
- `WIFI_BATCH` (B, N=8): up to 8 idle Wi-Fi ticks per event. ~+1 fps in Netplay.
- Unflagged (A): power-of-two FIFO index masks (−0.2% instructions); the GXFIFO IRQ line is touched only when it changes.

## 4. 3D geometry engine (emulator thread)

- `GXFIFO_THREADED` (A): GX commands run in a batched threaded-code loop. Dispatch −6% per command. vs DraStic: same design.
- `DMA_GXFIFO_FAST` + `GXFIFO_DMA_INLINE` (A): geometry DMA writes straight from MainRAM into the FIFO, inlined. 27.0 → 27.8 fps; DMA time −47% headless. vs DraStic: same fast path.
- `GXFIFO_UNIFIED`, `GXFIFO_READ_INLINE` (A): command PIPE and FIFO in one ring; inline FIFO pop. −0.5%/−1.1% cycles.
- `GX_BULK` (B): geometry DMA into an empty FIFO is decoded and run in chunks of 64 words. Gift box +4.9% average fps, +9% worst second.
- `GX_CPUSEND` (B) + `GX_DMA_PREFETCH`, `GX_POLY_LEAN`, `GX_CLIP_REJECT` (A): town emu CPU −7.2% uncapped / −7.7% normal speed; gift box −5.4%, overworld −3.0%.
- `GX_SEND_DIRECT`, `GX_EARLY_REJECT` (one NEON outcode pass rejects ~750 of ~1900 town polygons before culling), `GX_BULK_LEAN` (A), shipped with a retrained PGO profile: town emu CPU −7.5%, gift box −9%, title −8.5%, overworld −5.7%; town fast-forward +10%.
- `GX_CLIP_LEAN` (A): the clipper skips per-vertex copies (shipped with the display-list HLE hook, mask 16).
- `GX_CMD_SLIM` (A): a compact command executor (hot code 22 KB → 6 KB). L1I misses −5.6..−8.6%; town emu CPU −0.9%.
- `NEON_GEOMETRY`, `GEOM_NEON2`, `GEOM_NEON3` (A): integer-NEON vertex transform, lighting, culling, matrix and box tests. Geometry −1%; under 1 fps.
- Unflagged trivial-accept clip (A): polygons fully inside the view skip the clipper. −1.7..−1.9% of the emu thread.
- `GEOM_CLIP_PLANESKIP` (A): clip planes that no vertex crosses are skipped. −0.66% cycles.
- `GEOM_PREFETCH2` (A): staged deep prefetch in BuildFrameGeom. Its stall share 19.3% → 8.9% (~3% of emu cycles); no fps measurement.
- `POLY_RADIX` (A): an allocation-free radix sort for the polygon Y-sort. fps-neutral.

## 5. 3D rendering

### 5a. OpenGL renderer (the default: "OpenGL", 3x)
SereneDS's OpenGL option is a **hybrid**: 3D on the GPU at the internal resolution, 2D on the CPU at native
resolution, merged on the GPU. The full-GPU renderer is still available as "OpenGL (hi-res 2D)". All items
are render-side (R).
- GLES3 port, with a Mali vertex-attribute signedness fix.
- Hybrid pipeline: GL 3D on its own thread, descriptors uploaded straight from RAM, upload/merge/present on a present thread, line polygons drawn as thin quads. Shrek 3x 65.5 → 80 fps across these steps.
- GPU shader cost: mediump merge, no integer divide in the merge, plain clears with `glClearBuffer`, mediump 3D colour maths. Hybrid 4x (GPU-bound) 55.9 → 68.5 fps.
- `HYB_TEXSTAGE`: texture VRAM updates are staged for the GL thread instead of waiting for it. PW overworld 3x +10.6%.
- `GL_SORT_OPAQUE`: opaque polygons are sorted by state and texture so draws merge. PW town intro 3x 22.8 → 56-60 fps.
- `GL_BATCH_NEEDOPAQUE`: alpha-31 alpha-textured polygons go in one blended pass. PW gift box 24 → 60 fps (1040 → 17 draws).
- `GL_WBUF_EARLYZ`, `HYB_MERGE_1X_2D`, `HYB_MERGE_FASTLINES`: W-buffer depth without `gl_FragDepth`; 2D-only screens merged at 1x; plain-3D lines copied. Together with the above: PW town 45 → 60 fps capped.
- `GL_OPAQUE_NODISCARD`: opaque polygons without transparent texels skip the alpha test (Mali early-Z). Town 3x median 67 → 71 fps.
- `GL_ALPHATEST_2PASS`, `GL_TEX_UNORM`, `GL_OPAQUE_FULLMASK`: Mali-friendly alpha test, normalized textures, forward pixel kill. GPU ms/frame town 11.2 → 9.3, gift box 9.9 → 8.4 (800 MHz); at 400 MHz town 45 → 55.5 fps.
- `GL_SPEC_TEXMOD`: a short shader for textured-modulate opaque polygons. Town GPU 17.05 → 15.6 ms at 400 MHz (55 → 60.4 fps).
- `HYB_MERGE_FASTCOPY`: fast merge lines copy the 3D texel (display only, within 2/255 per channel). Not separately measured.
- `GL_NOSNAPCOPY`: the GL job reads polygons in place; at normal speed the per-frame snapshot copies are gone.
- `GL_WARM_VARIANTS`, `GL_TEXARRAY_PREALLOC`, `GL_FOG_SHADER_BLEND`: no first-use stalls (shader variant compiles of 20.8 ms, fog blend shader of 23 ms, texture-array creation of 10-25 ms per new scene).
- `HYB_CAPTURE_ASYNC` (display capture shows one frame late) + `HYB_CAPTURE_OFFTHREAD`: display capture no longer stalls the emulator on the GPU (that stall cost 5-19 ms/frame). PW title 40 → 56-60 fps.
- `HYB_SLOTS4` + `SOFT2D_HYB_BANDS`: 4 framebuffer slots, and hybrid 2D drawn in parallel line bands. Overworld uncapped 91.7 → 108.3 fps; gift box 72 → 83.
- `HYB_COMPOSITE_NEON`: the hybrid 2D composite takes one NEON pass per line. 2D thread CPU −~25%; PW battle 58.1 → 59.5 fps average.
- `GL_EDGE_MARK`: DS edge marking (model outlines) in the GL renderer, which drew none before. Setting "Edge outlines", **default OFF**; costs ~5% in scenes that use it.

### 5b. Software renderer ("Software")
- `SOFT3D_DRASTIC` (R): a tile-based software 3D renderer on worker threads. 44.7 → 49.4 fps. **Approximate:** it skips shadow polygons, line polygons, wireframe and the clear bitmap, and approximates anti-aliasing and translucency. "Accurate software 3D" switches back to melonDS's reference renderer. vs DraStic: modelled on its tile renderer.
- `SOFT3D_ASYNC` (R): the 3D raster may run into the next frame. ~38 → ~44.5 fps.
- `TILE_COORD` (R): a coordinator thread with double-buffered geometry, so the emulator never waits for the raster. 49.3 → 53.1 fps. vs DraStic: its 3D decouple.
- `SNAP_DIRTY` (R): only dirty VRAM chunks are copied into the render snapshots. Snapshot 0.78 → 0.05 ms/frame.

## 6. 2D, compositing, presentation, frame pacing

- `SOFT2D_THREADED` + `SOFT2D_DEPTH2` (R): software 2D is drawn off the emulator thread, one frame behind. +2.8 fps (DEPTH2).
- `SOFT2D_NEON`, `SOFT2D_OBJNEON`, `SOFT2D_BG3DNEON`, `NEON_RENDERER` (A): NEON 2D compositor, sprite scan, 3D-layer compositor and colour conversion. Neutral on the device when measured (the work is off the critical path).
- Present only new frames: unchanged frames are not presented again. Netplay race 45.5 → 51.3 fps.
- `SKIP_REPEAT_FRAMES` (display-only): when a game draws 3D at 30 Hz, the repeated frames skip 2D drawing and present. Netplay +6 fps (to the 60 cap); Shrek LAN 36-37 → 41.6 fps. Setting "Skip repeated frames": Always (default) / Auto / Off.
- `MASTERBRIGHT_LATCH` (display-only): a mid-frame brightness change applies from the next frame, so fades no longer tear.
- `PACE_CATCHUP`: after a slow frame the frame limiter catches up to 3 frames of debt. PW town: 1-second windows below 59 fps 10/8 → 0.
- `AGGRESSIVE_SKIP` + `AUTO_FRAMESKIP`: frameskip up to 9; "Auto frameskip" (default **on**) drops drawn frames rather than run in slow motion. Not a speed gain; turn it off when benchmarking.

## 7. Audio (SPU)

- `SPU_MIX_NEON` (A): the 16-channel pan-accumulate in integer NEON. 2.3x faster per call; under 1 fps in game.
- `SPU_ADPCM_TABLE` (A): table-driven ADPCM decode. ADPCM share 5.3% → 4.1%.
- `SPU_INLINE`, `SPU_FAST_FETCH` (A): inlined decoders; samples read straight from main RAM. Not measured reliably.
- `SPU_CHMAJOR` + `SPU_FAST_ADPCM`: each channel runs over the whole batch with its state in registers. Audio share of the emu thread 10.0% → 7.7%; bit-identical audio on PW.
- `SPU_CAPTURE_CHMAJOR` (A): the channel-major path stays on with sound capture enabled. MKDS −0.35%.
- `SPU_RATE_DIV` (R): setting "Audio quality": Full / Balanced / **Performance (default, quarter mix rate)**. Emu CPU ~−3%, fast-forward ~+2.5%.
- `SPU_FAST_INTERP` (R): cheap linear interpolation when channel interpolation is enabled. Not measured.
- `SPU_SILENT_LAZY` (A): a console nobody hears (a Netplay remote) only steps channel timing. Remote cost ~−3%.

## 8. Threads and cores (RG DS)

- `PIN_RENDER` (R): render threads are pinned off the emulator core. 28 → 40.6 fps (+45%), the largest single lever.
- The emulator thread is pinned to core 3, and the pin is re-asserted every 300 frames. Under background load, dips to 46-58 fps become a flat 60.
- `TOPO_PIN`: threads are placed by role from the CPU topology, so big.LITTLE devices (for example the AYN Thor) keep render threads off the little cores. Falls back when the OS parks a core.
- ADPF performance hint (reported every 10 frames); the hybrid GL thread runs at nice −10.
- `NP_FLOAT`: in Netplay, remote console threads float over all cores. 2P race 55.5 → 56.1 fps.

## 9. Build

- `PGO_USE`: profile-guided optimisation from on-device training runs (PW, Shrek, MKDS 2P/8P). Front-end PGO −8.6% emu CPU; context-sensitive PGO −1.6% more.
- `THINLTO`: ThinLTO across the core and frontend (the core had never been LTO'd). −1.8%.
- `HOT_ORDER`: hot C++ functions are laid out together at link time. −1.6%.
- `MTUNE_A55`: `-mtune=cortex-a55`. −1.0%.
- `SPLIT_FUNCS`: cold blocks of profiled functions are split out. ~Neutral.
- `LINKOPT`: `-fno-plt` + `-Bsymbolic-functions`. PLT relocations −92%; fps flat.
- `-O3` in every variant, native ELF TLS (`-fno-emulated-tls`), arm64-v8a only.
- Together (PW, RG DS): emu CPU ~11.0 → 9.5 ms, uncapped ~78 → ~85 fps.

## 10. Multiplayer

**Netplay (new):** every device runs every player's console and only inputs cross the network
(lockstep). 2 to 16 players.
- `LockstepMP`: a deterministic in-process link that makes the game's own wireless reproducible on every device. Includes a desync check (state hashes every 60 frames, shown in the FPS counter), save exchange, and a ROM check.
- Cross-game: each player boots their own game; a player who lacks the ROM can receive it with consent. `ROM_SHARE` makes consoles with the same ROM share one copy in memory.
- Remote console cost: `NETPLAY_HEADLESS` and `NETPLAY_CAPTURE` (consoles you don't watch skip rendering, render prep, lighting and sound), `REMOTE_GX_SINK` (they skip geometry math), `SPU_SILENT_LAZY`, `MP_FASTPOLL`, `MP_CLOCKWAKE`, `MP_FUTEX`, `MP_POLL_INLINE`, `MP_REPLY_DEFER`, `NP_FLOAT`. `MP_FASTPOLL` + `MP_CLOCKWAKE`: MKDS 8-console Hosted race on the AYN Thor 38.9/33.9 → 59.6/59.6 fps.
- High latency: `NP_WIRE_RUNS` (input packets carry runs of equal inputs; the window no longer caps fps) + `NP_ADAPTIVE_DELAY` (input delay follows the measured round trip). Mac harness at 250/500/1000 ms one way: 46.6/30.6/15.5 → 60 fps. Device at 1000 ms + jitter + 5% loss: 59.1/58.4 fps, no desync.
- `NP_SPEED`: any player can fast-forward or uncap the whole session; the request travels in the input stream, so every device switches on the same frame. Device: 2P uncapped 66-71 fps, 3x 60-79 fps.
- `MP_BEACON_TSF` (B): a delivered beacon's TSF clock advances with the link delay, so the client stops repeating scanlines to catch up. Client frame length = host's.
- Robustness: a peer that leaves is reported after ~10.8 s and the others keep running; no deadlock when speeds change; the MKDS 6+ and 8-console session drops are fixed (section 12).
- Results: Shrek 2P on two RG DS 56.1/55.7 and 53.8/53.2 fps (3-minute races); PW Netplay ~60 fps.

**Hosted Netplay (`HOSTED_NETPLAY`, new):** one device (the server) runs every console; each guest
replays its own console from the server's records. 2 RG DS: 57.7/56.8 fps. 3P with the AYN Thor as server: ~60/59/59.

**LAN (the game's own wireless over Wi-Fi), improved:**
- `LAN_EARLY_REPLY` (a client replies when it picks up the command) and `LAN_RX_SPIN` (a short socket poll before sleeping). LAN works over a Wi-Fi Direct hotspot or a shared Wi-Fi network.
- Shrek 2P LAN ~41 fps: the Wi-Fi round-trip time is the ceiling, because 3 exact sync exchanges per frame need a ≤3 ms round trip.
- Multiplayer menu: Netplay / Hosted / LAN from the pause menu, with player groups and lobby hand-over. "USB" is listed but not available yet.

## 11. App features

- Renamed to **SereneDS** (`com.sereneds.app`; installs next to melonDS). arm64-only, Android 10+. Update checks against upstream melonDS releases are disabled.
- Out-of-box defaults: OpenGL (hybrid) at 3x, JIT, threaded rendering, Auto frameskip on, Audio quality Performance.
- **Record mode** (bug reports): records the start state, save, every input, per-frame timings and light hardware-encoded footage (~1.5 MB/min). Replays exactly (verified on device and Mac). Pause menu: "Share a recording"; L2 marks "investigate here".
- Fast-forward on R2 by default, 2.5x default speed. `FF_SKIP_PRESENT`, `FF_ADAPTIVE_SKIP`, `FF_CAP_PREFETCH`: skipped frames present nothing and skip depth adapts. Real 2.5x FF: town 101 → 125 fps, gift box 112 → 144-146, overworld 137 → 147. `FF_HEADLESS3D`: skipped frames skip render-only geometry (gift box +3%, town +2%).
- New settings: Audio quality, Edge outlines, Skip repeated frames, Auto frameskip, Fast-forward max frameskip (default 4).
- Dual-screen handhelds (Anbernic RG DS, AYN Thor): both physical screens are used, buttons are mapped by their labels, the D-pad works as a hat, and the bottom screen's system bars are hidden.
- FPS counter drawn in the colour opposite to the screen under it; it shows Netplay status.

## 12. Correctness fixes

Upstream melonDS bugs found and fixed:
- **JIT, Thumb `LDR [pc]` with an invalidated literal** (2026-10-11): the fallback path used an unloaded r15 and an unaligned PC, so after an overlay reload a load read garbage. This was the Pokémon White battle freeze (ARM9 data abort). In core `liteDS-main` 10b3f104; the app picks it up with its next core update.
- **Savestates lacked the divider/sqrt operands and results** (state 14.1): games whose IRQ handler saves and restores them (Shrek's menu) ran differently after every load.
- **Wi-Fi MP ACK left 2 bytes uninitialised**: savestates of the same console could differ between devices.
- **GXFIFO stall queue overflow under the JIT** dropped geometry commands (The Sims 3: green bottom screen). The geometry engine now runs ahead instead.

Fixes to our own levers and new code (each inside its flag):
- SM64DS white bottom screen: `EVENT_SLICES` starved the GXFIFO DMA refill. Slices are now capped while the refill is armed.
- Lazy flags: carry/overflow deferral miscompiled Pokémon White ("white professor"). C/V are now eager.
- GL: a line polygon's edges reached into the next polygon (spike in the PW town); the threaded GL job drew half-rewritten polygons (wedges); depth-equal decals z-fought (the DS ±margin is now applied); missing edge outlines; the alpha-test 2-pass half-white sprite; GL 2D direct-colour channel order.
- Adreno (AYN Thor) shader miscompile in the hi-res 2D sprite prerender (The Sims 3 intro garbage).
- Async software 2D read live VRAM instead of its frame's snapshot (The Sims 3 intro tearing); the software 3D async deadlock.
- MKDS multiplayer with 6+ consoles ("Communication error"), the MKDS 8P session fragility (beacons on time for lockstep clients), and Netplay disconnects (host frames delivered before they were sent).
- App: a fast-forward frame-queue crash at skip ≥3; a ROM-list crash when launching by path during a scan.

Known upstream behaviour still present: with the JIT, a savestate save/load round trip can diverge (affects rewind).

## 13. Built but not shipped / tried and dropped (short list)

- USB cable link play: a prototype exists; the app says "coming soon".
- `JIT_BGEMIT` (JIT code emitted on a worker thread) and `JIT_LINK_FLUSHBATCH`: built and exact; waiting for a device A/B.
- MKDS ARM7 `SeqMain`/commit HLE (a7 bit 16): OFF (timing not yet count-equivalent in 8P sessions).
- `MP_HOSTDELAY`: OFF (breaks 2P Netplay setup). `NP_SCHED`, `REMOTE_GX_MODEL`, `MP_FIBERS`: OFF (gate failure, no gain, slower).
- `SPU_ADPCM_MEMO`: compiled in but off by default (slower on the A55: its 8 MB table misses the caches).
- `RELAXED_ARM9_TIMING` (−27%), huge pages (not available), successor prefetch and code-layout changes, JIT tail sharing, card-read loop HLE: measured flat or slower; dropped.
