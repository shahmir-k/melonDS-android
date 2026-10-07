# PGO profile for the emulator core (LITEV_PGO_USE)

`core-cs.profdata` is a clang IR-level profile plus a context-sensitive (CSPGO) pass,
recorded on the RG DS by instrumented `liteDS-headless` binaries built from the same core
source, LITEV flag set and codegen (ThinLTO, -mtune=cortex-a55) as this app.

Trained on (2026-10-06): Pokemon White `.ml2` (overworld), `.ml1` (intro), `.ml3` (cutscene),
2000 frames each; Shrek `race-fresh.mln`, 1000 frames.

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
