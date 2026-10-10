# PGO profile for the emulator core (LITEV_PGO_USE)

`core-cs.profdata` is a clang IR-level profile plus a context-sensitive (CSPGO) pass,
recorded on the RG DS by instrumented `liteDS-headless` binaries built from the same core
source, LITEV flag set and codegen (ThinLTO, -mtune=cortex-a55) as this app.

Trained on (2026-10-10, lib litev-town2 incl. A7HLE/A9HLE/GX bulk levers): Pokemon White `.ml2`
(overworld, 1500 f), `.ml1` (intro, 1000 f), `.ml3` (cutscene, 1000 f) and four record-mode segments
of recording 20261009-144242 with their inputs (town f6500 600 f, gift box f10100 600 f, title
f1300 700 f, overworld f17000 900 f); Shrek slot-2 race `.ml2`, 1000 f; the two-console Netplay
race, 4200 f. The 2026-10-07 profile predated the HLE and GX levers: A9HLE::Run, A7HLE, BulkWords,
the bulk GX executor and RunADPCMFast had no profile data, and the hot order missed every template
(weak symbol) such as the GX executor and ARMv5::Execute.

The app CMake hashes the profile into a define and relinks when hot-symbols.order changes; before
that, replacing either file left ninja's objects up to date and the APK unchanged.

Code those scenes never ran is compiled as cold, so games outside the training set can be
slightly slower on paths only they use; a stale profile (core changed since) is still correct
but less effective. To add games or regenerate, use the profiler repo's tooling:

    melonDS-profiler/tools/profiler/pgo-train.sh   (scenes: tools/profiler/pgo-scenes.txt)

which builds, trains (IR + CS passes), merges and gates the profile (guest-state traces of a
profile build must equal a plain build). Method, adding a game, A/B rules and the build traps
(`-Wl,--no-lto-pgo-warn-mismatch`, NDK llvm-profdata, flag verification) are in
`melonDS-profiler/docs/PROFILE-GUIDED-BUILD.md`.

`../hot-symbols.order` (LITEV_HOT_ORDER) is the companion trained output; regenerate it with
`tools/profiler/hot-order.py` from capture.sh runs of the same games.
