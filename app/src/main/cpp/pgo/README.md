# PGO profile for the emulator core (LITEV_PGO_USE)

`core.profdata` is a clang instrumentation profile recorded on the RG DS by an
instrumented `liteDS-headless` built from the same core source and LITEV flag set as
the app. It goes stale as the core changes: a stale profile is still correct (functions
whose control flow changed are compiled without profile data), it just loses speed.
Regenerate it after significant core changes.

1. Build the instrumented headless binary (lib repo, Android NDK toolchain), passing the
   app's `-DLITEV_*=ON` flag set from `app/build.gradle.kts` plus
   `-DCMAKE_C_FLAGS=-fprofile-instr-generate -DCMAKE_CXX_FLAGS=-fprofile-instr-generate
   -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate`. Check the LITEV options in its
   CMakeCache.txt match the shipping set (zsh does not word-split a flags variable).
2. On the device (`/data/local/tmp/liteds`), run each training scene with
   `LLVM_PROFILE_FILE=/data/local/tmp/pgo/%p.profraw`:
   Pokemon White `.ml2` (overworld), `.ml1` (intro), `.ml3` (cutscene), 2000 frames each;
   Shrek `race-fresh.mln`, 1000 frames. Common args:
   `--fixed-rtc 1600000000 --mode jit --fastmem on --frameskip 0`.
3. Pull the `.profraw` files and merge with the NDK's own tool (the version must match the
   compiler): `$NDK/toolchains/llvm/prebuilt/*/bin/llvm-profdata merge -o core.profdata *.profraw`.
4. Gate before committing: a guest-state trace from a headless built with the new profile
   (`-fprofile-instr-use=...`) must be identical to a non-PGO build on the same scenes.
