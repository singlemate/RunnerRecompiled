# Building the PC version for Android from your own copy

This repository holds no game code and no game files. `pc/tools/selfbuild.py` turns **your own GOG copy** of Mirror's Edge into a personal APK on your own PC.

**The APK and the build folder contain code derived from your copy of the game. Keep them to yourself: do not publish or share them.**

The program described in [BUILDER.md](BUILDER.md) does all of this for you. This page is the manual way.

## What you need

- A Windows 10/11 x64 PC with about 2 GB free for the build. It took about 10 minutes on a 20-thread CPU with 32 GB of RAM; expect longer on fewer cores.
- **Mirror's Edge from GOG**, installed. Other releases will not work: the build checks the SHA-256 of `MirrorsEdge.exe`, and Steam, EA and retail executables differ or are protected.
- **Microsoft Visual C++ 2005 (x86)** `msvcr80.dll` and `msvcp80.dll`, builds 8.0.50727.9680 or 8.0.50727.6195, in `WinSxS` or in a folder given with `--crt`.
- **PhysX 2.8.0** `PhysXCore.dll` and `PhysXCooking.dll` as NVIDIA PhysX System Software 9.23.1019 installs them, or in a folder given with `--physx`.
- Python 3.10 or newer.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (ucrt, x86_64): its `bin` folder with `libLLVM-*.dll`.
- Android NDK r30.
- `RunnerRecompiled-shell.apk`: the app without game code, from the RunnerBuilder release or made with `apk_pack.py --make-shell`.

## Build

From the root of this repository:

```
python -I pc/tools/selfbuild.py --game "C:/GOG Games/Mirror's Edge" --shell RunnerRecompiled-shell.apk ^
    --toolchain path/to/llvm-mingw --ndk path/to/android-ndk --workspace selfbuild --keystore my-key.json
```

Every input is checked first, and the script stops with a message naming what is missing or different. The result is `selfbuild/RunnerRecompiled-pc-personal.apk`, signed with the key given (made on first use). A later run reuses what is already built.

## Install

1. Install the APK on an Android 10+ device with an ARMv8.4 CPU and Vulkan. The first version was tested on a Snapdragon 8 Elite (Adreno 830).
2. Zip your game folder (`Binaries`, `Engine` and `TdGame` inside; about 12 GB free is needed on the device) and copy the ZIP to the device, or copy the unpacked folder.
3. Open the app and choose the ZIP or folder in the setup screen.

Reporting a problem: **Settings → Support → Report a bug**, or **Send logs** right after it happens and before starting the game again.

## Credits

The high-resolution font page fix and the removal of black bars follow the analysis in [Mirror's Edge Tweaks](https://github.com/softsoundd/MirrorsEdgeTweaks) by softsoundd; they are reimplemented in `pc/runtime/rt_ue3.c`.
