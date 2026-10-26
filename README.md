# RunnerRecompiled

An unofficial, fan-made static recompilation of the **Xbox 360** game *Mirror's Edge*
(2008) to native **Android ARM64**, with its own Xbox kernel/XAM runtime and a Vulkan
GPU backend. No emulator runs at play time: the game's PowerPC code is translated
ahead of time.

> **Status: in progress, early testing.** The game boots, plays the intro videos,
> goes through the menus, continues an Xbox save and renders the prologue level on a
> real Android device; it has been played up to the first chapter. Performance and
> stability are still being worked on: about 23–28 fps on the level, with freezes.

**No game files are included or linked.** You need your own legally obtained Xbox 360
copy of the game. This repository holds only original code, patches and notes.

## What works today

Tested on a Lenovo Legion Y700 Gen 4 (Snapdragon, Adreno 830, Android 16):

- All 300 recompiled code modules build for ARM64 (-O2) and run on the device, with the
  game's threads running in parallel under a short kernel lock.
- Kernel/XAM services the game uses: threads/TLS, events, semaphores, mutants,
  critical sections, multi-object waits, virtual and physical memory, read-only game
  files, Xbox save content (`XamContent*`) with a storage device, local offline profile,
  achievements list, notifications, offline network stubs.
- GPU: PM4 ring buffer execution through a patched RexGlue Vulkan backend
  (Adreno fixes, Xenos MSAA sample positions), CPU write tracking by page protection.
- Audio: XMA decoding (FFmpeg) to AAudio.
- Input: Android gamepads (buttons, both sticks, triggers, d-pad) and basic touch buttons.
- Android app: the runtime runs inside the APK and renders with Vulkan straight to the
  screen; a launcher imports your game data from a folder you pick, backs up the Xbox
  saves before every launch and pauses the game when the app goes to the background.
  The runtime library holds recompiled game code and is never part of the APK.
- 766 runtime self-tests pass on the device.

## Known gaps

- Frame rate below the console's 30 fps on levels and visible freezes (pipeline
  compilation during play, level streaming); fixes are being tested.
- Some rendering issues (a square shadow around the player, a siren visible through walls).
- Launcher features still missing: settings menu, language, profile management, full
  touch controls, controller button-layout warnings.
- One crash later in the chapter "Edge" is not diagnosed yet.

The full, honest progress log is in [CHECKLIST.md](CHECKLIST.md). Most detailed notes
are in Russian.

## Layout

| Path | Contents |
|---|---|
| `xbox/` | Recompilation passes, XenonRecomp config examples and patch |
| `xbox/startup/` | Native loader and Xbox kernel/XAM runtime (`entry_probe.cpp`), self-tests, build script |
| `xbox/gpu-port/` | RexGlue Android patches, GPU integration and diagnostic notes |
| `android/` | Android app: launcher (data import, save backup), in-app runtime with direct Vulkan output, input, research stand |
| `tools/` | Helpers that read *your own* Xbox 360 copy (GOD/SVOD, XEX resources) |

## Building (outline)

1. From your own copy: extract `default.xex` and the game data (`tools/inspect_god.py`,
   `tools/extract_svod_file.py`).
2. Recompile with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) plus
   `xbox/xenonrecomp-local.patch` and a config based on `xbox/*-pass-config.example.toml`.
3. Build [RexGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 (commit
   `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`) for Android with
   `xbox/gpu-port/rexglue-v0.10.0-full.patch` (see `xbox/gpu-port/REXGLUE-PATCHES.md`).
4. Build the runtime with Android NDK r30: `xbox/startup/build.ps1 -Workspace <root>`.
5. Either run the native executable over ADB (self-tests, automated test routes; see
   `xbox/startup/README.md`), or build the APK (`android/build.ps1`), copy
   `libmeruntime.so`, the bundle files and your extracted data into a folder on the
   device and import it from the launcher.

Generated code, extracted game data and build outputs stay in a private `work/`
folder and must never be committed.

## Credits and licenses

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — MIT.
- [RexGlue SDK](https://github.com/rexglue/rexglue-sdk), derived from [Xenia](https://github.com/xenia-project/xenia) — BSD-style license.
- Patches in this repository keep the license of the project they modify.
- Original code here: MIT, see [LICENSE](LICENSE).

*Mirror's Edge* is a trademark of Electronic Arts. RunnerRecompiled is an independent
fan project; it is not affiliated with, endorsed by or sponsored by EA or DICE. The game
title is used only to say which game this project works with.
