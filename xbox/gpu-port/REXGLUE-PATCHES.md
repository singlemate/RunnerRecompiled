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
- Diagnostic tooling used during the depth/lightmap investigations (`ME_DIAGNOSTIC_*`
  switches, all off unless set).

## Older partial patches

`rexglue-android-gpu.patch`, `rexglue-android-runtime.patch`, `rexglue-packet-failure.patch`,
`rexglue-qualcomm-d24-depth.patch` and `rexglue-xenos-sample-positions.patch` are the
individual steps from earlier days. They are kept for history; the cumulative patch
contains all of them.
