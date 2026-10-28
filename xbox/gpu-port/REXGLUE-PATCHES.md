# RexGlue patches

The runtime uses a modified RexGlue SDK (a Xenia fork). Base: RexGlue **v0.10.0**
(commit `c94f5eb`).

## Current state: `rexglue-v0.10.0-full.patch`

A cumulative patch: everything changed from clean v0.10.0 to the state the runtime is
built from, including the new file `src/ui/vulkan/vulkan_presenter_window.cpp`. It was
checked with `git apply --cached --check` against the v0.10.0 tree.

```bash
git clone https://github.com/rexglue/rexglue-sdk && cd rexglue-sdk
git checkout v0.10.0
git apply path/to/rexglue-v0.10.0-full.patch
```

The main changes, in order of appearance in the project history (details in HANDOFF.md):

- External GPU host build for Android: guest memory and MMIO owned by the loader, the guest
  execution gate around GPU interrupts, offscreen presenter (`SetupExternalPresenter`).
- Adreno fixes: D24S8 depth as D32_SFLOAT_S8, Xenos MSAA sample positions, packet failure
  reporting, batched TYPE0 register writes.
- Direct display into an Android `ANativeWindow` (`VulkanPresenter::PresentGuestOutputToWindow`,
  `vulkan_presenter_window.cpp`): letterboxed blit into a FIFO swapchain. Presentation goes to a
  second queue of the graphics family when the device has one (`ME_DISPLAY_PRESENT_QUEUE=0`
  restores the shared queue).
- Command processor performance on Adreno:
  - per-frame texture descriptor set cache (`ME_GPU_DESCRIPTOR_CACHE`);
  - asynchronous command pool reset (`ME_GPU_ASYNC_POOL_RESET`);
  - persistent Vulkan pipeline cache next to the pipeline storage (`vulkan_pipeline_cache.bin`);
  - diagnostic environment switches read once;
  - counters in the `gpu_descriptor_stats` log event.
  - experimental batched vertex fetch residency (`ME_GPU_BATCH_VFETCH=1`, off by
    default); one A/B had 31.54 vs 30.74 FPS, but the viewpoints and animations
    diverged slightly, so no performance gain has been established.
- Diagnostic tooling used during the depth/lightmap investigations (`ME_DIAGNOSTIC_*`
  switches, all off unless set).
- 2026-10-26/27 (patch regenerated 2026-10-27, previous version kept as
  `rexglue-v0.10.0-full.patch.before-2026-10-27`), on by default:
  - oDepth on the host render target path is scaled from guest [0, 1] to host [0, 0.5] for
    float24 depth (`kSysFlag_DepthFloat24`), as the DXBC translator does
    (`spirv_translator_rb.cpp`; `ME_ODEPTH_FLOAT24_SCALE=0` reverts). Fixes Faith's missing legs
    on the MSAA path.
  - Guest D24S8 stored as D32_SFLOAT_S8 on every Adreno driver (vendor 0x5143), not only the
    proprietary one (Turnip showed the same first-tile depth failure).
  - Texture fetch `exp_adjust` read from fetch constant word 3 (bits 13:18), as in Xenia Canary
    (`ME_FETCH_EXP_WORD4=1` reverts).
  - Front polygon offset scale rounded to the nearest order of magnitude, as in Canary
    (`draw.cpp`; `ME_POLY_OFFSET_ROUND=0` reverts).
  - `kComputeWrite` shared memory access mask includes SHADER_WRITE; physical writes are
    synchronized before a range is marked GPU-written (`ME_GPU_SYNC_BEFORE_GPU_WRITE=0` reverts).
  - Host-provided Vulkan loader hook (`me_custom_vulkan_loader`, Android only) for a custom driver
    loaded through libadrenotools; explicit app layers `VK_LAYER_RENDERDOC_Capture`
    (`ME_RENDERDOC=1`) and Khronos validation (`vulkan_validation_enabled`, sync validation via
    `VK_EXT_layer_settings` with `ME_VALIDATION_SYNC=1`).
  - A/B switches, off by default: `ME_SHADER_A2C=0`, `gamma_render_target_as_unorm16`,
    `ME_DIAGNOSTIC_INDEX_PROBE`, `ME_DIAGNOSTIC_GPU_RANGE`, resolve/tile traces.
- 2026-10-28 (frame pacing / GPU cost work, previous file
  `rexglue-v0.10.0-full.patch.before-2026-10-28`), all off by default unless noted:
  - `ME_GPU_PASS_PROFILE=1`: GPU time per render pass group and per dispatch source
    (`deferred_command_buffer.cpp`, event `gpu_pass_profile`); `CmdVkDispatch` takes a
    `me_source` tag (texture load, resolve copy/clear, host depth store, EDRAM dump, swap,
    readback downscale).
  - `ME_GPU_BARRIER_PROFILE=1`: why passes end (`barrier_kinds_per_s`) and how many uploads
    were hoisted.
  - `ME_GPU_ABLATE=1|2`: skip draws / 1x1 scissors (cost split; image is wrong while on).
  - `ME_RENDER_AREA_SHRINK=1`: render area of render target cache passes shrunk to the
    scissor/clear union at pass end (no measurable gain on Adreno 830).
  - `ME_HOST_DEPTH_STORE=0`: no separate float32 host depth copies on EDRAM ownership changes
    (faster, but decals flicker; not for default).
  - `ME_HOIST_UPLOADS=1`: shared memory uploads of pages untouched in the current submission
    run in an upload prologue at the submission start (`SharedMemory::MeTouch*`,
    `VulkanCommandProcessor::upload_prologue()`): pass breaks 122 → 86 per frame.

## Older partial patches

`rexglue-android-gpu.patch`, `rexglue-android-runtime.patch`, `rexglue-packet-failure.patch`,
`rexglue-qualcomm-d24-depth.patch` and `rexglue-xenos-sample-positions.patch` are the
individual steps from earlier days. They are kept for history; the cumulative patch
contains all of them.
