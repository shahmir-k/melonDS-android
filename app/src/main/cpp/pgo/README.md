# PGO profile for the emulator core (LITEV_PGO_USE)

`core-cs.profdata` is a clang IR-level profile plus a context-sensitive (CSPGO) pass,
recorded on the RG DS by instrumented `liteDS-headless` binaries built from the same
core source and LITEV flag set as the app, with ThinLTO. It goes stale as the core
changes: a stale profile is still correct (functions whose control flow changed are
compiled without profile data), it just loses speed. Regenerate it after significant
core changes.

Common to every build below: the lib repo, the Android NDK toolchain, the app's
`-DLITEV_*=ON` flag set from `app/build.gradle.kts`, and `-flto=thin` in the C, C++ and
linker flags. Check the LITEV options in each CMakeCache.txt match the shipping set (zsh
does not word-split a flags variable).

Training scenes (on the device, `/data/local/tmp/liteds`, each run with
`LLVM_PROFILE_FILE=/data/local/tmp/pgo/%p.profraw`): Pokemon White `.ml2` (overworld),
`.ml1` (intro), `.ml3` (cutscene), 2000 frames each; Shrek `race-fresh.mln`, 1000
frames. Common args: `--fixed-rtc 1600000000 --mode jit --fastmem on --frameskip 0`.

1. IR pass: build with `-fprofile-generate` (compile and link), run the scenes, merge:
   `llvm-profdata merge -o ir.profdata *.profraw`.
2. CS pass: build with `-fprofile-use=ir.profdata -fcs-profile-generate` (compile and
   link), run the scenes again, then merge both:
   `llvm-profdata merge -o core-cs.profdata ir.profdata *.profraw`.
   Use the NDK's own `llvm-profdata` (its version must match the compiler).
3. Linking with the profile: the CS part is applied at LTO link time, so the link needs
   `-fprofile-use=...` too, plus `-Wl,--no-lto-pgo-warn-mismatch`. Functions that differ
   from the headless build (e.g. faad_cli's `main`) mismatch, and the NDK links with
   `--fatal-warnings`. `-mllvm -no-pgo-warn-mismatch` does not silence lld's check.
4. Gate before committing: a guest-state trace from a headless built with the new
   profile must be identical to a non-PGO build on the same scenes.
