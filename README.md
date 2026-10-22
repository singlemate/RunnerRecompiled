# RunnerRecompiled

An unofficial, fan-made static recompilation of the **Xbox 360** game *Mirror's Edge*
(2008) to native **Android ARM64**, with its own Xbox kernel/XAM runtime and a Vulkan
GPU backend. No emulator runs at play time: the game's PowerPC code is translated
ahead of time.

> **Status: in progress, not playable.** The game boots to the title screen on a
> real Android device, accepts controller input and signs in a local offline
> profile. Menus and levels are not reached yet. There is no audio decoding and no
> save storage yet.

**No game files are included or linked.** You need your own legally obtained Xbox 360
copy of the game. This repository holds only original code, patches and notes.

## What works today

Tested on a Lenovo Legion Y700 Gen 4 (Snapdragon, Adreno, Android 16):

- All 300 recompiled code modules build for ARM64 and run on the device.
- Kernel services the game reaches at startup: threads/TLS, events, semaphores,
  mutants, multi-object waits, virtual and physical memory, read-only game files,
  time and time zone, notifications, offline network stubs that report "no network".
- GPU: real PM4 ring buffer execution through RexGlue's Vulkan backend, interrupts
  and a virtual 60 Hz vblank; the title screen renders (offscreen, previewed in the app).
- Input: Android gamepad → `XamInputGetState`; START is accepted.
- Profile: a persistent local offline profile, sign-in UI completion, profile
  settings, user context, and the title's real achievement list (none unlocked).
- 718 runtime self-tests pass on the device.

## Known gaps

- Rendering artifacts (dark silhouettes on distant buildings) — root cause not found yet.
- XMA audio decoding, continuous audio output.
- Xbox save storage and the storage device selector (current stop point).
- Direct Android Surface/swapchain and normal app lifecycle; the runtime is started
  over ADB for now.

The full, honest progress log is in [CHECKLIST.md](CHECKLIST.md). Most detailed notes
are in Russian.

## Layout

| Path | Contents |
|---|---|
| `xbox/` | Recompilation passes, XenonRecomp config examples and patch |
| `xbox/startup/` | Native loader and Xbox kernel/XAM runtime (`entry_probe.cpp`), self-tests, build script |
| `xbox/gpu-port/` | RexGlue Android patches, GPU integration and diagnostic notes |
| `android/` | Research Android app: input bridge, live preview, local profile |
| `tools/` | Helpers that read *your own* Xbox 360 copy (GOD/SVOD, XEX resources) |

## Building (outline)

1. From your own copy: extract `default.xex` and the game data (`tools/inspect_god.py`,
   `tools/extract_svod_file.py`).
2. Recompile with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) plus
   `xbox/xenonrecomp-local.patch` and a config based on `xbox/*-pass-config.example.toml`.
3. Build [RexGlue SDK](https://github.com/rexglue/rexglue-sdk) (pinned commit
   `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`) for Android with the patches in `xbox/gpu-port/`.
4. Build the runtime with Android NDK r30: `xbox/startup/build.ps1 -Workspace <root>`.
5. Push the runtime and your extracted data to the device and run it over ADB
   (see `xbox/startup/README.md`).

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
