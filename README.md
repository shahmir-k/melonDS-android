# melonDS Android port
Android port of [melonDS](https://melonds.kuribo64.net/), a DS and DSi emulator.

[<img src="https://play.google.com/intl/en_us/badges/static/images/badges/en_badge_web_generic.png" alt="Get it on Google Play" height="80">](https://play.google.com/store/apps/details?id=me.magnum.melonds&pcampaignid=pcampaignidMKT-Other-global-all-co-prtnr-py-PartBadge-Mar2515-1)[<img src="https://raw.githubusercontent.com/Kunzisoft/Github-badge/main/get-it-on-github.png" alt="Get it on GitHub" height="80">](https://github.com/rafaelvcaetano/melonDS-android/releases/latest)

|Rom List|Dark Theme|Pocket Physics|Layout Editor|
|---|---|---|---|
|![Screenshot 1](./.github/images/screenshot_mobile0.png)|![Screenshot 2](./.github/images/screenshot_mobile1.png)|![Screenshot 3](./.github/images/screenshot_mobile2.png)|![Screenshot 4](./.github/images/screenshot_mobile3.png)|

# What SereneDS changes
SereneDS (`com.sereneds.app`) is a fork of this app and of the melonDS core
([quickmelonDS](https://github.com/shahmir-k/quickmelonDS), based on upstream melonDS of 2026-06-07).
It is tuned for in-order ARM handhelds such as the Anbernic RG DS (Cortex-A55). The full list, with
measurements and the exactness class of every item: **[docs/OPTIMIZATIONS.md](docs/OPTIMIZATIONS.md)**.

*  **JIT (ARM64):** the dispatcher runs in generated code, with block linking, inline caches, a return-address stack, lazy flags, pinned guest registers, wider fastmem (DTCM, block transfers, user-bank STM), code laid out for the in-order A55 (emu CPU −5%), and cheaper compiling (fewer scene-entry hitches).
*  **HLE:** hot Nintendo SDK routines are recognised by their exact code bytes and run natively, with a fallback to the game's code: OS IRQ/wake-ups, HBlank IRQ, GX display lists, NNS G3D models/materials/animation, LZ decompression, card reads, `_ll_sdiv`, and the ARM7 sound driver. Matches Pokémon Black/White/White 2 (most hooks) and Mario Kart DS / The Sims 3 (some). PW emu CPU −19..−28%.
*  **Scheduler/timing:** event-true CPU slices, instant/lazy divider and sqrt, batched SPU/Wi-Fi/cart events, idle-loop skipping (deterministic approximations, see below).
*  **3D:** a default hybrid OpenGL renderer (3D on the GPU at 3x, 2D on the CPU) with many Mali GPU-cost levers. Also a DraStic-style multi-threaded tile software renderer, plus a faster geometry engine (NEON math, bulk GX DMA, early reject, lean clipping).
*  **2D/presentation/audio:** threaded NEON 2D, repeated 30 Hz frames skipped, pacing catch-up, NEON/table/batched audio, and an Audio quality setting.
*  **Threads and build:** render threads kept off the emulator core (largest single lever, +45%), role-based pinning, PGO/CSPGO, ThinLTO, hot-function ordering, `-mtune=cortex-a55` (together −14% emu CPU).
*  **Multiplayer:** new lockstep **Netplay** (2-16 players, inputs only, desync check, cross-game ROM sharing, adaptive delay that holds 60 fps up to ~1 s one-way latency in tests, synchronized fast-forward), new **Hosted Netplay** (one device runs every console), and faster, more robust LAN.
*  **App:** record mode for exact-replay bug reports, a stronger fast-forward (R2, ~2x+), an Edge outlines setting (default off), Auto frameskip (default on), and dual-screen handheld support (RG DS, AYN Thor).
*  **Correctness:** fixes to upstream bugs (a JIT Thumb literal bug that froze Pokémon White battles, a savestate divider/sqrt gap, an uninitialised MP ACK, a GXFIFO stall-queue overflow) and to our own levers (for example SM64DS).

**What we don't change:** emulation stays deterministic and multiplayer-safe. Speed-first approximations
are deterministic and version-locked: every SereneDS build of the same version agrees with every other,
so Netplay peers must run the same version. Pervasive cycle-timing changes are not allowed. The fast tile
software renderer is approximate; "Accurate software 3D" keeps melonDS's reference renderer.

# Missing Features
*  Local Multiplayer
*  DSi SD card support
*  Customizable button skins
*  More display filters

# Performance
Performance is solid on 64 bit devices with thread rendering and JIT enabled, and should run at full speed on flagship devices. Performance on older devices, specially
32 bit devices, is very poor due to the lack of JIT support.

# Integration with third-party frontends
It's possible to launch melonDS from third part frontends. For that, you will need to have the ROMs you want to launch already scanned by melonDS. Then, you can configure your
third-party frontend with the following configuration:
*  Package name: `me.magnum.melonds`
*  Activity name: `me.magnum.melonds.ui.emulator.EmulatorActivity`
*  Parameters (choose one):
    * Intent data (preferred) - a URI of the NDS ROM (ZIP and 7z files are supported). Ensure [read permission is granted](https://developer.android.com/reference/android/content/Intent#FLAG_GRANT_READ_URI_PERMISSION)
    * `uri` (deprecated) - a string with the [SAF](https://developer.android.com/guide/topics/providers/create-document-provider) URI of the NDS ROM (ZIP and 7z files are supported)
    * `PATH` (deprecated) - a string with the absolute path to the NDS ROM (ZIP and 7z files are supported)

### Pegasus metadata files
* [melonds.metadata.txt](./.github/pegasus/melonds.metadata.txt) 
* [melonds-nightly.metadata.txt](./.github/pegasus/melonds-nightly.metadata.txt) 

### Info regarding save files
When launching ROMs from third-party frontends, if melonDS hasn't scanned that particular ROM previously, it won't be able to create the save file next to the ROM file if the
option "Save next to ROM file" is enabled in the settings or the save file directory is not set. Instead, melonDS will create a save file in
`Android/data/me.magnum.melonds/files/saves`

# Nightly Builds

To have access to the latest changes, you can install nightly builds that you can find [here](https://github.com/rafaelvcaetano/melonDS-android/releases/tag/nightly-release).

Be aware that these builds can contain more bugs than usual and you may need to clear your app data to get it to work properly after updates.

# Building
To build the project you will need Android SDK, NDK and CMake.

## Build steps:
1.  Clone the project, including submodules with:
    
    `git clone --recurse-submodules https://github.com/rafaelvcaetano/melonDS-android.git`
2.  Install the Android SDK, NDK and CMake
3.  Build with:
    1.  Unix: `./gradlew :app:assembleGitHubProdDebug`
    2.  Windows: `gradlew.bat :app:assembleGitHubProdDebug`
4.  The generated APK can be found at `app/gitHubProd/debug`

If you want to create a release build, you will need to modify your `local.properties` file to include the following fields:  
*  `MELONDS_KEYSTORE=<path_to_your_keystore>`
*  `MELONDS_KEYSTORE_PASSWORD=<keystore_password>`
*  `MELONDS_KEY_ALIAS=<name_of_your_key_alias>`
*  `MELONDS_KEY_PASSWORD=<key_alias_password>`
