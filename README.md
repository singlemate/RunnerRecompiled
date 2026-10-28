# RunnerRecompiled

An unofficial, fan-made static recompilation of the **Xbox 360** game *Mirror's Edge*
(2008) to native **Android ARM64**, with its own Xbox kernel/XAM runtime and a Vulkan
GPU backend. No emulator runs at play time: the game's PowerPC code is translated
ahead of time.

> **Status: in progress, testing.** The game boots, goes through the menus, continues
> an Xbox save and has been played through the prologue and the first chapters into the
> Stormdrain level on a real Android device. With the default 30 fps cap it holds a
> steady 30 fps on most of the levels tested so far, with occasional freezes. There is
> still an intermittent crash during level loads (often after dying), and some rendering bugs.

**No game files are included or linked.** You need your own legally obtained Xbox 360
copy of the game. This repository holds only original code, patches and notes.

## What works today

Mainly tested on a Lenovo Legion Y700 Gen 4 (Snapdragon 8 Elite, Adreno 830, Android 16).
Early tester reports come from a Galaxy Z Fold6 (Snapdragon 8 Gen 3) and a Snapdragon 8
Elite Gen 5 tablet.

- All 300 recompiled code modules build for ARM64 (-O2) and run on the device, with the
  game's threads running in parallel under a short kernel lock.
- Kernel/XAM services the game uses: threads/TLS, events, semaphores, mutants,
  critical sections, multi-object waits, virtual and physical memory, read-only game
  files, Xbox save content (`XamContent*`) with a storage device, local offline profile,
  achievements, notifications, offline network and Marketplace stubs.
- GPU: PM4 ring buffer execution through a patched RexGlue Vulkan backend:
  - Adreno fixes, including D24 depth stored as D32 on all Adreno drivers and an
    oDepth scaling fix;
  - Xenos MSAA sample positions;
  - CPU write tracking by page protection;
  - shader/pipeline storage, so pipelines are rebuilt at start instead of during play;
  - real occlusion queries, split into segments inside render passes because Adreno
    does not count outside a pass (fixes lens flares and sirens visible through walls).
- Optional Vulkan driver choice: the system driver (default) or your own Mesa Turnip
  package in AdrenoTools format.
- Audio: XMA decoding (FFmpeg) to AAudio, with an output buffer that grows by itself on
  underruns (no crackle on slower devices).
- Input: Android gamepads (Xbox, PlayStation and Switch Pro layouts are detected, with
  an optional Nintendo A/B–X/Y swap) and floating on-screen touch controls.
- Android app: the runtime runs inside the APK and renders with Vulkan straight to the
  screen. Launcher features:
  - first-run setup;
  - install from your own ISO on the device;
  - game language;
  - skip intro video;
  - frame cap (30 by default, 60 or none);
  - optional on-screen performance monitor (fps, longest frame, GPU load where the
    system exposes it, battery temperature, thermal status, memory);
  - achievements list;
  - Xbox save backup before every launch;
  - pause menu with "send logs".
- The runtime library holds recompiled game code and is never part of the public APK:
  you build it yourself from your own copy.
- 770 runtime self-tests pass on the device.

## Known gaps

- An intermittent crash during level loads (often after dying and reloading a
  checkpoint): a freed object stays linked in a skeletal-mesh attachment list. It is
  being investigated.
- Occasional freezes on heavy scenes. The GPU is the limit; render-pass breaks and
  uploads are being reduced.
- A missing valve wheel in Stormdrain: a skeletal mesh whose bones place it behind the
  pipe.
- Shadows flicker when the camera turns. Xenia does the same with the same patch.
- Third-party Turnip builds can lose the GPU device on some phones; start with the
  system driver.
- To do: field-of-view setting, more launcher languages.

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
