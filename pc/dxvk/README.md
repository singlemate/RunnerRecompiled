# DXVK Native for Android (PC port)

The PC port's Direct3D 9 bridge (`pc/runtime/rt_d3d9*.c`) calls the host's D3D9 objects through their vtables by index. On Android ARM64 the host D3D9 is DXVK Native 3.1.1 with an Android window-system backend added by this project.

## Contents

- `dxvk-3.1.1-android.patch` changes the DXVK 3.1.1 sources (DXVK is under the zlib license; the patch and the new `src/wsi/android/wsi_platform_android.cpp` are offered under the same license):
  - a new WSI backend, `src/wsi/android/wsi_platform_android.cpp`. It has one monitor (the app's view) and creates its surface with `vkCreateAndroidSurfaceKHR` from the `ANativeWindow` handed over with `dxvk_android_set_window()`.
  - `meson.build`: on Android, no SDL or GLFW is needed (`-DDXVK_WSI_ANDROID`). `src/wsi/meson.build` adds the backend and links `libandroid`.
  - `src/d3d9/d3d9.sym`: `dxvk_android_set_window` is exported.
- `android-arm64.cross.txt` is the meson cross file. Replace `@NDK_BIN@` with `<NDK>/toolchains/llvm/prebuilt/<host>/bin`.

## Build (Windows host, NDK r30, meson 1.9.1, ninja 1.12.1, glslang 16.5.0 on PATH)

```
cd dxvk-3.1.1                       # release tarball with submodules
patch -p1 < dxvk-3.1.1-android.patch  # sources with LF line ends, as in the release tarball
meson setup build-android --cross-file android-arm64.cross.txt --buildtype=release \
  -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
  -Dnative_sdl2=disabled -Dnative_sdl3=disabled -Dnative_glfw=disabled
ninja -C build-android
```

The result is `build-android/src/d3d9/libdxvk_d3d9.so` (~11.6 MB). It depends only on libandroid, libm, libdl and libc, plus the system Vulkan loader, which is loaded at run time. The runtime's Android host (`pc/runtime/rt_host_android.c`) loads it with `dlopen`, passes the view to it, and calls `Direct3DCreate9`.
