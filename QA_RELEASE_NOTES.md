# SereneDS 1.0.0-qa1 — QA release notes

## What SereneDS is

SereneDS is a Nintendo DS emulator for Android. It is a fork of
[melonDS Android](https://github.com/rafaelvcaetano/melonDS-android) with a heavily optimised
emulator core (a fork of [melonDS](https://melonds.kuribo64.net/)). SereneDS is based on melonDS
and melonDS Android and, like them, is licensed under the GPLv3.

The core work targets in-order ARM cores (Cortex-A55, e.g. the Anbernic RG DS): a reworked A64
JIT (block dispatcher and linking, lazy flags, register pinning, inline caches), scheduler and
timing shortcuts, NEON geometry and audio, a tile-based multi-threaded software 3D renderer, and a
hybrid GPU-3D / CPU-2D OpenGL path. Every optimisation is a compile-time `LITEV_*` option in the
core; the per-option documentation is `docs/LITEV-OPTIMIZATIONS.md` in the core repository.

- Package: `com.sereneds.app` (installs side by side with melonDS; its data is separate).
- Version: 1.0.0-qa1 (versionCode 1), arm64-v8a only, Android 10 or newer (the native code uses ELF TLS).
- In-app update checking is disabled in this build (there is no SereneDS release feed yet).

## Renderers (Settings > Video)

| Renderer | What it does | Resolution scaling |
|---|---|---|
| **Software** (default) | Fast tile-based 3D renderer on worker threads. Turn on **Accurate software 3D** to use melonDS's reference 3D renderer instead (slower, use it when 3D looks wrong). | 1x only |
| **OpenGL** | Hybrid: 3D on the GPU, 2D on the CPU. | Internal resolution 1x-4x applies to 3D |
| **OpenGL (hi-res 2D)** | Full GPU renderer (2D and 3D on the GPU). | Internal resolution 1x-4x |
| Compute | Upstream option, not tuned or tested for SereneDS. | — |

### Expected performance (Anbernic RG DS, uncapped, frameskip off)

| Scene | Renderer | fps |
|---|---|---|
| Shrek Smash n' Crash Racing, 8-kart race | Software | ~67 |
| | OpenGL (hybrid) 3x | ~85 |
| | OpenGL (hybrid) 4x | ~69 |
| | OpenGL (hi-res 2D) 3x | ~45 |
| Pokémon White, overworld | Software | ~64 |
| | OpenGL (hybrid) 3x | ~79 |

Normal play is capped at 60 fps, so any figure above 60 means the scene runs at full speed with
headroom.

## New settings

- **Auto frameskip** (Settings > General, default **off**): skips rendered frames to keep the game
  at full speed when the device cannot hit 60 fps (choppier video instead of slow motion).
- **Fast-forward max frameskip** (Settings > General, 0-9, default **0**): how many frames
  fast-forward may skip rendering.
- `debug.litev.*` system properties exist for developer measurement and diagnostics only. Testers
  should not set them; if a device has any set (`adb shell getprop | grep litev`), mention it in
  the report.

## Known issues

- The fast tile renderer (Software, Accurate software 3D off) skips shadow polygons, line
  polygons, wireframe and the clear bitmap, and approximates anti-aliasing and translucency. Use
  **Accurate software 3D** for games that rely on these.
- OpenGL (hybrid): display capture runs at 1x and is untested (neither test game uses it).
- OpenGL (hi-res 2D) shows 3D one frame late.
- With the JIT on, a savestate save/load round trip can diverge (upstream melonDS behaviour); this
  only affects rewind.
- Multiplayer (local wireless) has not been hard-tested.
- Tested only on the Anbernic RG DS, with Shrek Smash n' Crash Racing and Pokémon White.

## QA test plan

Report the build (1.0.0-qa1) with every result. For each item: pass/fail and notes.

1. **Install / launch.** Install the APK next to any existing melonDS. The launcher shows
   "SereneDS". The app starts without a crash; first-run setup works.
2. **ROM import.** Add a ROM folder; the ROM list populates with titles and icons; a ROM starts.
3. **Renderers × scale.** For each of Software (accurate 3D off, then on), OpenGL, and OpenGL
   (hi-res 2D) at 1x, 2x and 3x (scale applies to the OpenGL ones): start a 3D scene and a 2D menu,
   check both screens for missing/garbled graphics, note the fps counter after ~30 s.
4. **Savestates.** Save to a slot, load it, quit and relaunch, load it again. Check the quick slot
   too.
5. **Fast-forward.** Toggle fast-forward; the game speeds up; it returns to normal speed. Try max
   frameskip 0 and a higher value.
6. **Auto frameskip.** Turn it on in a heavy scene: the game keeps full speed (fewer drawn frames,
   no slow motion). Turn it off again afterwards.
7. **Audio.** Music and effects play without crackle or drift; volume setting works; audio
   resumes after pause.
8. **Controls.** On-screen controls, physical buttons/gamepad and touch on the bottom screen all
   work; remapping works.
9. **Both screens.** Both DS screens show correct content in the chosen layout (on dual-screen
   devices check each physical display).
10. **Suspend / resume.** Press Home, lock the screen, return after a minute; the game resumes
    with picture and sound. Rotate the device if supported.

### How to report

Include: device model and Android version, game (and region), renderer, internal resolution,
Accurate software 3D on/off, fps from the on-screen counter, what you did, what happened, and a
screenshot of the game screen. On the Anbernic RG DS the game is on **Display 1**, not Display 0:

```sh
adb shell "su 0 screencap -p -d 1 /sdcard/d1.png" && adb pull /sdcard/d1.png
```

A capture of about 7 KB is the blank control display (Display 0), not the game.
