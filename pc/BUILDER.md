# RunnerBuilder

A program that turns **your own GOG copy** of Mirror's Edge into a folder ready to copy to a tablet: your personal APK, a ZIP of your game files and short instructions. You point it at the game folder and press Build.

The program itself holds no game code and no game files. **What it produces does: keep the result to yourself, do not publish or share it.**

## Why it builds instead of shipping an APK

The app around the game (launcher, runtime, DXVK, controls) is open source and comes inside the program ready-made. What it cannot ship is `libmepc.so`: the game's own x86 code translated to ARM, about 230 MB. That is a derived copy of EA's code, so it is made on the PC of someone who owns the game, from their copy, and nowhere else.

## What it does

1. Checks `Binaries\MirrorsEdge.exe` (SHA-256 of the supported GOG build).
2. Downloads what the translation needs, about 100 MB in all: the compiler, linker and Android headers out of Google's Android NDK, and the x86 disassembler (`libLLVM`) out of llvm-mingw. Only those files are taken out of the two archives (`remotezip.py`, about 100 MB of 920): the archive's list of files is checked by SHA-256, every file by its CRC-32.
3. Finds the Visual C++ 2005 and PhysX 2.8.0 DLLs on the PC, or takes them out of Microsoft's and NVIDIA's installers without running them (`redist.py`).
4. Translates and compiles the game's code (`selfbuild.py`, see [SELFBUILD.md](SELFBUILD.md)): 5 to 30 minutes.
5. Puts the result into the ready-made app and signs it (`apk_pack.py`: no Java and no Android SDK).
6. Writes the APK, a ZIP of the game files and the instructions to the chosen folder.

Everything except the window is Python (standard library only) and reads its formats itself: cabinets with LZX, 7-Zip with LZMA and BCJ2, ZIP ranges over HTTP, PKCS#12 keys with AES, APK Signature Scheme v2. So it needs no tools of the system, which is also what makes it work under Wine and Proton.

Tools and temporary files go to `%LOCALAPPDATA%\RunnerBuilder`, or `C:\RunnerBuilder` when that path has non-ASCII letters. The signing key lives there as well (`personal-key.p12` from earlier versions, made by keytool, or `personal-key.json`): with the same key a new APK installs over the old one, keeping the game files and saves on the device.

## Installing on the tablet

After a build the window offers **Install on the tablet…** (`device.py`): it installs the APK over USB or Wi-Fi debugging and copies the game files, so nothing has to be copied by hand. Over Wi-Fi a 20 MB test went at 38 MB/s on a Lenovo Y700, so the 11.5 GB should take about 5 minutes; the full copy and USB have not been tried yet. It only needs developer options on the tablet: USB debugging and a cable, or wireless debugging with a pairing code once.

- adb comes out of Google's platform-tools, only `adb.exe` and its two DLLs (`remotezip.py`).
- The app writes the game files itself. `device.py` starts it with the extra `receiveFromPc` and streams the files through `adb forward` into a local socket of the app (`PcSetup.receive`, protocol described there). The app accepts only adbd (shell uid) as the other end, keeps every path inside `Binaries`, `Engine` and `TdGame`, checks the SHA-256 of `MirrorsEdge.exe` and prepares the libraries as after a ZIP import. Files that adb itself pushes into `Android/data` belong to the shell user, and on Android 16 the app could not be shown to read them.
- Files already on the tablet with the same size are skipped: a second run resumes, and checking all 4237 files takes about 30 seconds.
- A tablet with the app signed by another key is not touched: the window says to uninstall it there first.

## Parts

| Part | What it does |
|---|---|
| `pc/tools/builder.py` | The core. Lines starting with `==` name the current stage. |
| `pc/tools/selfbuild.py` | Checks the inputs, translates, compiles, packs. |
| `pc/tools/device.py` | Installs the APK on the tablet and copies the game files (adb). |
| `pc/tools/remotezip.py` | Downloads chosen files out of a ZIP on a server. |
| `pc/tools/redist.py` | Takes DLLs out of the Visual C++ and PhysX installers. |
| `pc/tools/apk_pack.py` | Adds the personal parts to the shell APK and signs it; `--make-shell` makes the shell from a full build. |
| `pc/tools/builder-gui/RunnerBuilder.cs` | The Windows window: paths, progress, log, check for a new version. |
| `pc/tools/builder-gui/make_exe.py` | Compiles the window with the `csc.exe` of Windows and embeds the scripts, the runtime sources and the shell APK. |

To make a release: build the full APK (`android/build.ps1 ... -PcRuntime ...`), cut the shell, build the exe:

```
python -I pc/tools/apk_pack.py --make-shell RunnerRecompiled-pc.apk --out RunnerRecompiled-shell.apk
python -I pc/tools/builder-gui/make_exe.py --shell RunnerRecompiled-shell.apk --licenses "DXVK=..." --out <folder>
```

At start the program reads `version-builder.json` from the root of the repository and offers to open the project page when `versionCode` is greater than `VersionCode` in `RunnerBuilder.cs`.

## Linux and macOS (planned)

The window runs under Wine and Proton; that is not tested on a real system yet. A native version needs:

1. **Tools.** A `HOSTS` entry in `builder.py` with the Linux or macOS NDK and an LLVM that has `libLLVM.so`/`.dylib`; `remotezip.py` has to keep the executable bit.
2. **Disassembler.** `x86cfg.LLVMDisasm` loads `libLLVM-*.dll`; it has to find `.so` or `.dylib` too.
3. **The game.** Many such machines have no installed Windows game: accept the GOG offline installer and unpack it without running it (innoextract format).
4. **Window.** A small Python and tkinter window over the same core.
