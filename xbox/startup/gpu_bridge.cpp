#include "gpu_bridge.h"
#include "rex_gpu_memory.h"
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/flags.h>
#include <rex/cvar.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <atomic>
#include "guest_preview.h"
#include <rex/ui/vulkan/presenter.h>
#include <condition_variable>
#include <mutex>
#include <string>
// Process-lifetime probe state. Import-stop/_exit terminates all native workers.
// Explicit partial-initialization cleanup/retry is not exposed by this bridge.
// Performance A/B toggles (defaults unchanged). RexGlue defines these at global scope.
REXCVAR_DECLARE(bool, occlusion_query_enable);
REXCVAR_DECLARE(bool, clear_memory_page_state);
static rex::graphics::vulkan::VulkanGraphicsSystem* graphics;
static MeRexGpuMemory* memory;
static std::atomic<uint64_t> generated_swaps{0};
// Direct display (APK): the Java side hands over an ANativeWindow; the display thread
// presents the latest guest output into it (VulkanPresenter::PresentGuestOutputToWindow).
// me_display_set_window(nullptr) blocks until the presenter has released the window,
// so the caller may release it afterwards (SurfaceHolder.Callback.surfaceDestroyed).
static std::mutex display_mutex;
static std::condition_variable display_changed;
static void* display_window=nullptr;      // requested by the UI
static void* display_active=nullptr;      // in use by the presenter
static void display_loop(){
    // Presents only new guest frames: between them the thread polls the mailbox every
    // ME_DISPLAY_POLL_MS (default 3 ms, no GPU work); ME_DISPLAY_HZ caps the present rate.
    const char* hz_env=std::getenv("ME_DISPLAY_HZ");
    const int hz=hz_env?std::clamp(std::atoi(hz_env),10,240):60;
    const char* poll_env=std::getenv("ME_DISPLAY_POLL_MS");
    const auto poll=std::chrono::milliseconds(poll_env?std::clamp(std::atoi(poll_env),1,50):3);
    const auto period=std::chrono::microseconds(1000000/hz);
    const bool preview=!(std::getenv("ME_PREVIEW") && !std::strcmp(std::getenv("ME_PREVIEW"),"0"));
    std::printf("{\"event\":\"display_thread\",\"max_hz\":%d,\"poll_ms\":%d,\"new_frames_only\":true}\n",hz,int(poll.count()));
    auto* presenter=static_cast<rex::ui::vulkan::VulkanPresenter*>(graphics->presenter());
    uint64_t presented=0,idle=0,failed=0,prior_swaps=generated_swaps.load(std::memory_order_relaxed);
    auto rate_start=std::chrono::steady_clock::now(),next=rate_start;
    for(;;){
        void* window;
        {
            std::unique_lock<std::mutex> lock(display_mutex);
            if(display_active && display_active!=display_window){
                presenter->ReleaseExternalWindow();display_active=nullptr;display_changed.notify_all();
            }
            // Bounded wait: without a window (screen off, app in background) the frame-rate
            // log below must keep going.
            display_changed.wait_for(lock,std::chrono::seconds(1),[]{return display_window!=nullptr;});
            window=display_active=display_window;
        }
        if(window){
            std::this_thread::sleep_until(next);
            // FIFO acquire may block until vsync; the UI waits in set_window meanwhile.
            const int result=presenter->PresentGuestOutputToWindow(window);
            if(result>0){++presented;next=std::chrono::steady_clock::now()+period;}
            else{if(result==0)++idle;else ++failed;next=std::chrono::steady_clock::now()+poll;}
        }
        const auto now=std::chrono::steady_clock::now();
        const double seconds=std::chrono::duration<double>(now-rate_start).count();
        if(seconds>=2){
            // Without the preview thread this is the frame-rate log (same event as there).
            if(!preview){
                const auto swaps=generated_swaps.load(std::memory_order_relaxed);
                std::printf("{\"event\":\"frame_rates\",\"seconds\":%.3f,\"capture_hz\":0,\"generated_swap_hz\":%.2f}\n",seconds,(swaps-prior_swaps)/seconds);
                prior_swaps=swaps;
            }
            std::printf("{\"event\":\"display_rate\",\"presented_hz\":%.1f,\"idle_polls\":%llu,\"failed\":%llu}\n",presented/seconds,(unsigned long long)idle,(unsigned long long)failed);
            presented=idle=failed=0;rate_start=now;
        }
    }
}
extern "C" void me_display_set_window(void* window){
    std::unique_lock<std::mutex> lock(display_mutex);
    display_window=window;display_changed.notify_all();
    // Wait until the presenter no longer uses any other window (bounded: the GPU may be gone).
    display_changed.wait_for(lock,std::chrono::seconds(3),[&]{return display_active==nullptr || display_active==window;});
}
// Frame dump file (automated route checks): ME_FRAME_DUMP, default for the ADB executable.
static std::string frame_dump_path(){const char* v=std::getenv("ME_FRAME_DUMP");return v?v:"/data/local/tmp/me_guest_frame.ppm";}
extern "C" int me_gpu_start(void* view, void* mmio, MeGpuGate enter, MeGpuGate leave, MeGpuIrq irq) {
    if (graphics || !view || !mmio || !enter || !leave || !irq) return 0;
    try {
        enter();
        try { memory = new MeRexGpuMemory(*static_cast<MeGpuPhysicalView*>(view), *static_cast<MeGpuMmio*>(mmio)); }
        catch (...) { leave(); throw; }
        leave();
        // Per-title setting, as recommended by the Xenia Canary compatibility
        // report for 45410850: UE3 predicated tiling aligns sample 0 with
        // non-MSAA depth using Xenos MSAA sample positions. ME_XENOS_SAMPLE_POSITIONS=0
        // turns it off for A/B comparison.
        const char* sample_positions = std::getenv("ME_XENOS_SAMPLE_POSITIONS");
        REXCVAR_SET(xenos_sample_positions, !(sample_positions && !std::strcmp(sample_positions, "0")));
        // ME_GPU_OCCLUSION_QUERIES=0: guest occlusion queries get the fake sample count
        // (query_occlusion_fake_sample_count) instead of host queries. Host queries end the
        // render pass for every query, which is expensive on tile-based GPUs (Adreno).
        // (diagnostic, not default).
        // clear_memory_page_state is OFF by default here: it drops the valid bit of every
        // CPU-uploaded page each frame, so all buffers/textures are re-read, re-hashed and
        // re-uploaded every frame (~45% of level fps on the Y700). It exists for hosts that
        // cannot see CPU writes; this loader reports every CPU write precisely through the
        // page-protection write watch (gpu_write_watch.h) and host_write() for syscalls.
        // (Measured: off = ~45% more level fps, but see below.)
        const char* occlusion = std::getenv("ME_GPU_OCCLUSION_QUERIES");
        if (occlusion && !std::strcmp(occlusion, "0")) REXCVAR_SET(occlusion_query_enable, false);
        const char* page_state = std::getenv("ME_GPU_PAGE_STATE");
        // Default back ON: with it off the user saw black helicopter/doors/runner bag
        // (stale textures). ME_GPU_PAGE_STATE=0 turns it off for A/B with
        // ME_GPU_ALLOC_INVALIDATE=1 (allocation-time invalidation, the suspected fix).
        REXCVAR_SET(clear_memory_page_state, !(page_state && !std::strcmp(page_state, "0")));
        std::printf("{\"event\":\"gpu_perf_toggles\",\"occlusion_queries\":%s,\"page_state_refresh\":%s}\n",
                    REXCVAR_GET(occlusion_query_enable) ? "true" : "false", REXCVAR_GET(clear_memory_page_state) ? "true" : "false");
        // Diagnostic: ME_VULKAN_RENDER_PASSES=1 uses classic render passes, the
        // path Xenia Canary validated the sample-position fix on.
        const char* render_passes = std::getenv("ME_VULKAN_RENDER_PASSES");
        if (render_passes && !std::strcmp(render_passes, "1")) REXCVAR_SET(vulkan_dynamic_rendering, false);
        std::printf("{\"event\":\"xenos_sample_positions\",\"requested\":%s,\"dynamic_rendering\":%s}\n",
                    REXCVAR_GET(xenos_sample_positions) ? "true" : "false",
                    REXCVAR_GET(vulkan_dynamic_rendering) ? "true" : "false");
        graphics = new rex::graphics::vulkan::VulkanGraphicsSystem;
        auto presentation_status=graphics->SetupExternalPresenter();
        std::printf("{\"event\":\"offscreen_presenter_setup\",\"status\":\"%08x\",\"android_surface\":false}\n",unsigned(presentation_status));
        if(presentation_status!=0)return 0;
        auto status = graphics->SetupExternalGpu(*memory, {enter, leave}, irq, 1280, 720, 60);
        std::printf("{\"event\":\"game_gpu_setup_result\",\"status\":\"%08x\",\"same_loader_memory\":true,\"presentation\":false}\n",unsigned(status));
        if(status==0 && std::getenv("ME_DISPLAY"))new std::thread(display_loop);
        // ME_PREVIEW=0 (APK): no readback/preview worker (no socket, no frame dumps); the
        // display thread then logs the frame rates.
        const char* preview_env=std::getenv("ME_PREVIEW");
        if(status==0 && !(preview_env && !std::strcmp(preview_env,"0"))){
            // Diagnostic readback worker; never hold the guest gate across a
            // capture fence wait. The probe process owns it until _exit.
            new std::thread([]{
                MeGuestPreview preview;
                const char* period_env=std::getenv("ME_PREVIEW_PERIOD_MS");
                const int period_ms=period_env?std::clamp(std::atoi(period_env),16,500):33;
                auto next_dump=std::chrono::steady_clock::now();
                auto next_capture=next_dump,rate_start=next_dump;
                uint64_t captures=0,prior_swaps=generated_swaps.load(std::memory_order_relaxed);
                std::printf("{\"event\":\"preview_cadence\",\"period_ms\":%d}\n",period_ms);
                for(unsigned attempt=1;;++attempt){
                    const bool live=preview.connected();
                    std::this_thread::sleep_until(next_capture);
                    next_capture=std::chrono::steady_clock::now()+std::chrono::milliseconds(live?period_ms:500);
                    const auto now=std::chrono::steady_clock::now();
                    const bool dump_due=now>=next_dump;
                    if(!live && !dump_due)continue;
                    const auto capture_begin=std::chrono::steady_clock::now();
                    rex::ui::RawImage frame;
                    if(!graphics->presenter()->CaptureGuestOutput(frame))continue;
                    if(!frame.width || !frame.height || frame.width>4096 || frame.height>4096 ||
                       frame.stride<size_t(frame.width)*4 || frame.data.size()<size_t(frame.height-1)*frame.stride+size_t(frame.width)*4)continue;
                    const auto capture_end=std::chrono::steady_clock::now();
                    preview.frame(frame);
                    ++captures;
                    const auto rate_now=std::chrono::steady_clock::now();
                    const double seconds=std::chrono::duration<double>(rate_now-rate_start).count();
                    if(seconds>=2){
                        const auto swaps=generated_swaps.load(std::memory_order_relaxed);
                        std::printf("{\"event\":\"frame_rates\",\"seconds\":%.3f,\"capture_hz\":%.2f,\"generated_swap_hz\":%.2f}\n",seconds,captures/seconds,(swaps-prior_swaps)/seconds);
                        captures=0;prior_swaps=swaps;rate_start=rate_now;
                    }
                    if(live && attempt%10==0){
                        const auto send_end=std::chrono::steady_clock::now();
                        std::printf("{\"event\":\"preview_timing\",\"attempt\":%u,\"capture_us\":%lld,\"send_us\":%lld,\"period_ms\":%d}\n",attempt,
                            (long long)std::chrono::duration_cast<std::chrono::microseconds>(capture_end-capture_begin).count(),
                            (long long)std::chrono::duration_cast<std::chrono::microseconds>(send_end-capture_end).count(),period_ms);
                    }
                    if(!dump_due)continue;
                    next_dump=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                    const std::string dump=frame_dump_path(),dump_tmp=dump+".tmp";
                    FILE* file=std::fopen(dump_tmp.c_str(),"wb");if(!file)continue;
                    bool ok=std::fprintf(file,"P6\n%u %u\n255\n",frame.width,frame.height)>0;
                    std::vector<uint8_t> row(size_t(frame.width)*3);
                    for(unsigned y=0;y<frame.height && ok;++y){
                        for(unsigned x=0;x<frame.width;++x)std::memcpy(row.data()+x*3,frame.data.data()+y*frame.stride+x*4,3);
                        ok=std::fwrite(row.data(),1,row.size(),file)==row.size();
                    }
                    if(std::fclose(file))ok=false;
                    if(ok && !std::rename(dump_tmp.c_str(),dump.c_str()))
                        std::printf("{\"event\":\"guest_frame_captured\",\"attempt\":%u,\"width\":%u,\"height\":%u,\"file\":\"me_guest_frame.ppm\"}\n",attempt,frame.width,frame.height);
                }
            });
        }
        return status == 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU bridge setup failed: %s\n", error.what()); return 0;
    }
}
extern "C" void me_gpu_ring(uint32_t address, uint32_t log2) { graphics->InitializeRingBuffer(address, log2); }
extern "C" void me_gpu_readback(uint32_t address, uint32_t log2) { graphics->EnableReadPointerWriteBack(address, log2); }
extern "C" void me_gpu_irq_callback(uint32_t callback, uint32_t argument) { graphics->SetInterruptCallback(callback, argument); }
extern "C" void me_gpu_vblank() { graphics->PumpExternalVblank(); }
extern "C" int me_gpu_make_swap(uint32_t* words, const uint32_t* fetch, uint32_t virtual_address,
    uint32_t physical_address, uint32_t format, uint32_t color_space, uint32_t width, uint32_t height) {
    namespace x = rex::graphics::xenos;
    x::xe_gpu_texture_fetch_t texture;
    static_assert(sizeof(texture) == 24);
    std::memcpy(&texture, fetch, sizeof(texture));
    if ((texture.base_address << 12) != virtual_address || (physical_address & 4095) ||
        (format != uint32_t(x::TextureFormat::k_8_8_8_8) && format != uint32_t(x::TextureFormat::k_2_10_10_10_AS_16_16_16_16)) ||
        color_space || width != 1 + texture.size_2d.width || height != 1 + texture.size_2d.height) return 0;
    texture.base_address = physical_address >> 12;
    words[0] = x::MakePacketType0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
    std::memcpy(words + 1, &texture, sizeof(texture));
    words[7] = x::MakePacketType3(x::PM4_XE_SWAP, 4);
    words[8] = x::kSwapSignature;
    words[9] = physical_address; words[10] = width; words[11] = height;
    for (unsigned i = 12; i < 64; ++i) words[i] = x::MakePacketType2();
    generated_swaps.fetch_add(1,std::memory_order_relaxed);
    return 1;
}
