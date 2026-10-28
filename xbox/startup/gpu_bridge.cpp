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
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <renderdoc_app.h>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <atomic>
#include "guest_preview.h"
#include <rex/ui/vulkan/presenter.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <algorithm>
#include <iterator>
// Custom Vulkan driver (RexGlue vulkan_instance.cpp hook): ME_TURNIP=1 loads a driver through
// libadrenotools (and its hook libraries) from the APK's nativeLibraryDir (ME_NATIVE_LIB_DIR,
// extracted native libraries). The driver is the package the user imported in the launcher
// (ME_GPU_DRIVER_DIR in internal storage, ME_GPU_DRIVER_LIB), or else libvulkan_freedreno.so
// bundled in the diagnostic RunnerRecompiled-turnip.apk. Null = the system Vulkan loader/driver
// (also when loading fails: the game then runs on the device's own driver).
extern "C" void* me_custom_vulkan_loader(){
    static void* const handle=[]()->void*{
        const char* turnip=std::getenv("ME_TURNIP");
        const char* lib_dir=std::getenv("ME_NATIVE_LIB_DIR");
        if(!turnip || std::strcmp(turnip,"1") || !lib_dir)return nullptr;
        const std::string dir(lib_dir);
        const char* driver_dir_env=std::getenv("ME_GPU_DRIVER_DIR");
        const char* driver_lib_env=std::getenv("ME_GPU_DRIVER_LIB");
        const std::string driver_dir=driver_dir_env?std::string(driver_dir_env):dir+"/";
        const std::string driver_lib=driver_lib_env?std::string(driver_lib_env):std::string("libvulkan_freedreno.so");
        void* tools=::dlopen((dir+"/libadrenotools.so").c_str(),RTLD_NOW);
        using OpenFn=void*(*)(int,int,const char*,const char*,const char*,const char*,const char*,void**);
        auto open=tools?reinterpret_cast<OpenFn>(::dlsym(tools,"adrenotools_open_libvulkan")):nullptr;
        void* vulkan=open?open(RTLD_NOW,1/*ADRENOTOOLS_DRIVER_CUSTOM*/,nullptr,(dir+"/").c_str(),driver_dir.c_str(),
                               driver_lib.c_str(),nullptr,nullptr):nullptr;
        const char* tu_debug=std::getenv("TU_DEBUG");
        const char* error=vulkan ? nullptr : ::dlerror();
        std::printf("{\"event\":\"custom_vulkan_driver\",\"driver\":\"%s%s\",\"adrenotools\":%s,\"loaded\":%s,\"tu_debug\":\"%s\",\"error\":\"%s\"}\n",
                    driver_dir.c_str(),driver_lib.c_str(),tools?"true":"false",vulkan?"true":"false",tu_debug?tu_debug:"",error?error:"");
        return vulkan;
    }();
    return handle;
}
// Process-lifetime probe state. Import-stop/_exit terminates all native workers.
// Explicit partial-initialization cleanup/retry is not exposed by this bridge.
// Performance A/B toggles (defaults unchanged). RexGlue defines these at global scope.
REXCVAR_DECLARE(bool, occlusion_query_enable);
REXCVAR_DECLARE(bool, clear_memory_page_state);
REXCVAR_DECLARE(bool, vulkan_async_skip_incomplete_frames);
REXCVAR_DECLARE(int32_t, anisotropic_override);
REXCVAR_DECLARE(bool, depth_float24_round);
REXCVAR_DECLARE(bool, depth_float24_convert_in_pixel_shader);
REXCVAR_DECLARE(bool, vulkan_depth_unorm24_on_qualcomm);
REXCVAR_DECLARE(bool, direct_host_resolve);
REXCVAR_DECLARE(bool, gamma_render_target_as_unorm16);
REXCVAR_DECLARE(bool, vulkan_validation_enabled);
REXCVAR_DECLARE(bool, vulkan_sparse_shared_memory);
static rex::graphics::vulkan::VulkanGraphicsSystem* graphics;
static MeRexGpuMemory* memory;
static std::atomic<uint64_t> generated_swaps{0};
// Longest interval between two guest swaps since the last frame_rates line (freezes are
// invisible in a 2 s average): max_frame_ms in frame_rates; reset by whoever logs it.
static std::atomic<int64_t> last_swap_ns{0}, max_swap_interval_ns{0};
static int64_t take_max_frame_ns() { return max_swap_interval_ns.exchange(0, std::memory_order_relaxed); }
// Performance overlay (GameActivity, polled once a second): total guest swaps and the longest
// swap interval since the previous call (its own maximum, independent of the log's).
static std::atomic<int64_t> overlay_max_swap_ns{0};
extern "C" void me_overlay_stats(uint64_t* swaps, int64_t* max_frame_ns) {
    *swaps = generated_swaps.load(std::memory_order_relaxed);
    *max_frame_ns = overlay_max_swap_ns.exchange(0, std::memory_order_relaxed);
}
// Stutter: guest swap intervals and window present intervals per log line, bucketed
// <=20, <=34, <=50, <=100, >100 ms (a 2 s average hides uneven frames).
static constexpr int kFrameBuckets=5;
static int frame_bucket(int64_t ns){return ns<=20000000?0:ns<=34000000?1:ns<=50000000?2:ns<=100000000?3:4;}
static std::atomic<uint32_t> swap_hist[kFrameBuckets];
static std::string take_swap_hist(){
    std::string out="[";
    for(int i=0;i<kFrameBuckets;++i){out+=std::to_string(swap_hist[i].exchange(0,std::memory_order_relaxed));if(i+1<kFrameBuckets)out+=',';}
    return out+"]";
}
// Session log fields for frame_rates: seconds since the bridge started and the current
// scene (entry_probe.cpp, from the level packages the game opens).
extern "C" void me_current_scene(char* out, size_t capacity);
// Frame the command processor is processing (RexGlue shared_memory.cpp, set per CP frame).
extern "C" std::atomic<uint64_t> me_trace_frame;
// RexGlue deferred_command_buffer.cpp: shrink render areas of render target cache passes.
extern "C" int me_render_area_shrink;
// RexGlue vulkan/render_target_cache.cpp: separate host depth copies on EDRAM ownership changes.
extern "C" int me_host_depth_store;
// RexGlue deferred_command_buffer.cpp: diagnostic GPU cost ablation.
extern "C" int me_gpu_ablate;
// RexGlue vulkan/shared_memory.cpp: shared memory uploads at the submission start.
extern "C" int me_hoist_uploads;
static const auto bridge_start = std::chrono::steady_clock::now();
static double uptime_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - bridge_start).count(); }
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
    auto period=std::chrono::microseconds(1000000/hz);
    // Diagnostics: ME_FRAME_LIMIT_FILE + ".display" holds a new cap (the app also switches the
    // display mode), re-read with the rate log every 2 s.
    const char* limit_file=std::getenv("ME_FRAME_LIMIT_FILE");
    const std::string display_file=limit_file?std::string(limit_file)+".display":std::string();
    int display_hz=hz;
    const bool preview=!(std::getenv("ME_PREVIEW") && !std::strcmp(std::getenv("ME_PREVIEW"),"0"));
    std::printf("{\"event\":\"display_thread\",\"max_hz\":%d,\"poll_ms\":%d,\"new_frames_only\":true}\n",hz,int(poll.count()));
    auto* presenter=static_cast<rex::ui::vulkan::VulkanPresenter*>(graphics->presenter());
    uint64_t presented=0,idle=0,failed=0,prior_swaps=generated_swaps.load(std::memory_order_relaxed);
    uint32_t present_hist[kFrameBuckets]={};
    auto rate_start=std::chrono::steady_clock::now(),next=rate_start;
    std::chrono::steady_clock::time_point last_present{};
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
            if(result>0){
                const auto done=std::chrono::steady_clock::now();
                if(last_present.time_since_epoch().count())
                    ++present_hist[frame_bucket(std::chrono::duration_cast<std::chrono::nanoseconds>(done-last_present).count())];
                last_present=done;++presented;next=done+period;
            }
            else{if(result==0)++idle;else ++failed;next=std::chrono::steady_clock::now()+poll;}
        }
        const auto now=std::chrono::steady_clock::now();
        const double seconds=std::chrono::duration<double>(now-rate_start).count();
        if(seconds>=2){
            // Without the preview thread this is the frame-rate log (same event as there).
            if(!preview){
                const auto swaps=generated_swaps.load(std::memory_order_relaxed);
                char scene[48];me_current_scene(scene,sizeof(scene));
                std::printf("{\"event\":\"frame_rates\",\"t\":%.1f,\"seconds\":%.3f,\"capture_hz\":0,\"generated_swap_hz\":%.2f,\"max_frame_ms\":%.1f,\"swap_ms_hist\":%s,\"scene\":\"%s\"}\n",uptime_s(),seconds,(swaps-prior_swaps)/seconds,take_max_frame_ns()/1e6,take_swap_hist().c_str(),scene);
                prior_swaps=swaps;
            }
            std::printf("{\"event\":\"display_rate\",\"presented_hz\":%.1f,\"idle_polls\":%llu,\"failed\":%llu,\"present_ms_hist\":[%u,%u,%u,%u,%u]}\n",presented/seconds,(unsigned long long)idle,(unsigned long long)failed,
                present_hist[0],present_hist[1],present_hist[2],present_hist[3],present_hist[4]);
            presented=idle=failed=0;std::fill(std::begin(present_hist),std::end(present_hist),0u);rate_start=now;
            // "<ME_FRAME_LIMIT_FILE>.shrink" / ".hds": ME_RENDER_AREA_SHRINK / ME_HOST_DEPTH_STORE on the fly.
            const struct{const char* suffix;const char* event;int* value;} toggles[]={
                {".shrink","render_area_shrink",&me_render_area_shrink},{".hds","host_depth_store",&me_host_depth_store},
                {".ablate","gpu_ablate",&me_gpu_ablate},{".hoist","hoist_uploads",&me_hoist_uploads}};
            if(!display_file.empty()) for(const auto& toggle:toggles) if(FILE* f=std::fopen((std::string(limit_file)+toggle.suffix).c_str(),"r")){
                int value=0;
                if(std::fscanf(f,"%d",&value)==1 && value!=*toggle.value){
                    *toggle.value=value;
                    std::printf("{\"event\":\"%s\",\"enabled\":%d,\"source\":\"file\"}\n",toggle.event,value);
                }
                std::fclose(f);
            }
            if(!display_file.empty()) if(FILE* f=std::fopen(display_file.c_str(),"r")){
                int value=0;
                if(std::fscanf(f,"%d",&value)==1){
                    value=value?std::clamp(value,10,240):60;
                    if(value!=display_hz){
                        display_hz=value;period=std::chrono::microseconds(1000000/value);
                        std::printf("{\"event\":\"display_thread\",\"max_hz\":%d,\"source\":\"file\"}\n",value);
                    }
                }
                std::fclose(f);
            }
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
        // ME_VALIDATION=1: Khronos validation layer packaged in a diagnostic APK (explicit app layer);
        // ME_VALIDATION_SYNC=1 adds synchronization validation (vulkan_instance.cpp).
        if (const char* v = std::getenv("ME_VALIDATION"); v && !std::strcmp(v, "1")) REXCVAR_SET(vulkan_validation_enabled, true);
        // ME_SPARSE_SHARED_MEMORY=0: one fully bound 512 MB shared memory buffer instead of the
        // sparse one (missing Faith legs: the body index buffer read zeros on the GPU).
        if (const char* v = std::getenv("ME_SPARSE_SHARED_MEMORY"); v && !std::strcmp(v, "0")) REXCVAR_SET(vulkan_sparse_shared_memory, false);
        std::printf("{\"event\":\"gpu_sparse_shared_memory\",\"enabled\":%s}\n", REXCVAR_GET(vulkan_sparse_shared_memory) ? "true" : "false");
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
        // OFF by default again (2026-10-25). The black helicopter/doors/runner bag seen with
        // it off were stale textures from memory reused without invalidation; allocation-
        // time invalidation (map_physical alloc_invalidate, on by default) fixed that, and
        // the user confirmed correct colours at the helicopter. With it on, every draw
        // re-uploads its ranges, each copy ends the render pass (~330 passes per frame vs
        // ~140) and level fps drops from ~27.6 to 23-24. ME_GPU_PAGE_STATE=1 turns it on.
        // Shadow flicker A/B: ME_DEPTH_UNORM24=1 keeps guest D24S8 as D24_UNORM_S8 on Qualcomm (as
        // Xenia on PC) instead of D32_SFLOAT_S8 (our 2026-10-22 fix for depth round trips).
        if (const char* v = std::getenv("ME_DEPTH_UNORM24"); v && !std::strcmp(v, "1")) REXCVAR_SET(vulkan_depth_unorm24_on_qualcomm, true);
        // ME_DIRECT_RESOLVE=0: resolve through the EDRAM buffer as Xenia Canary does, instead of
        // RexGlue's direct host-render-target-to-memory resolve (not present in Canary).
        if (const char* v = std::getenv("ME_DIRECT_RESOLVE"); v && !std::strcmp(v, "0")) REXCVAR_SET(direct_host_resolve, false);
        std::printf("{\"event\":\"gpu_direct_resolve\",\"enabled\":%s}\n", REXCVAR_GET(direct_host_resolve) ? "true" : "false");
        // ME_GAMMA_UNORM16=0: store k_8_8_8_8_GAMMA as R8G8B8A8_UNORM with PWL gamma in shaders, as
        // Xenia Canary's Vulkan host render target path always does (RexGlue defaults to linear RGBA16).
        if (const char* v = std::getenv("ME_GAMMA_UNORM16"); v && !std::strcmp(v, "0")) REXCVAR_SET(gamma_render_target_as_unorm16, false);
        std::printf("{\"event\":\"gpu_gamma_unorm16\",\"enabled\":%s}\n", REXCVAR_GET(gamma_render_target_as_unorm16) ? "true" : "false");
        // ME_SHADER_A2C=0 (read in the SPIR-V translator): no gl_SampleMask alpha to coverage, as Canary.
        {
            const char* a2c = std::getenv("ME_SHADER_A2C");
            std::printf("{\"event\":\"gpu_shader_a2c\",\"enabled\":%s}\n", a2c && !std::strcmp(a2c, "0") ? "false" : "true");
        }
        REXCVAR_SET(clear_memory_page_state, page_state && !std::strcmp(page_state, "1"));
        std::printf("{\"event\":\"gpu_perf_toggles\",\"occlusion_queries\":%s,\"page_state_refresh\":%s}\n",
                    REXCVAR_GET(occlusion_query_enable) ? "true" : "false", REXCVAR_GET(clear_memory_page_state) ? "true" : "false");
        // Diagnostic: ME_VULKAN_RENDER_PASSES=1 uses classic render passes, the
        // path Xenia Canary validated the sample-position fix on.
        const char* render_passes = std::getenv("ME_VULKAN_RENDER_PASSES");
        if (render_passes && !std::strcmp(render_passes, "1")) REXCVAR_SET(vulkan_dynamic_rendering, false);
        // Shadow flicker A/B (Xenos 20e4 depth emulated with host 32-bit float depth):
        // ME_DEPTH24_PS=1 converts depth to 20e4 in the guest pixel shaders, ME_DEPTH24_ROUND=1
        // rounds instead of truncating when converting (Xenia depth_float24_* options).
        if (const char* v = std::getenv("ME_DEPTH24_PS"); v && !std::strcmp(v, "1")) REXCVAR_SET(depth_float24_convert_in_pixel_shader, true);
        if (const char* v = std::getenv("ME_DEPTH24_ROUND"); v && !std::strcmp(v, "1")) REXCVAR_SET(depth_float24_round, true);
        std::printf("{\"event\":\"gpu_depth24\",\"convert_in_ps\":%s,\"round\":%s}\n",
                    REXCVAR_GET(depth_float24_convert_in_pixel_shader) ? "true" : "false", REXCVAR_GET(depth_float24_round) ? "true" : "false");
        std::printf("{\"event\":\"xenos_sample_positions\",\"requested\":%s,\"dynamic_rendering\":%s}\n",
                    REXCVAR_GET(xenos_sample_positions) ? "true" : "false",
                    REXCVAR_GET(vulkan_dynamic_rendering) ? "true" : "false");
        graphics = new rex::graphics::vulkan::VulkanGraphicsSystem;
        auto presentation_status=graphics->SetupExternalPresenter();
        std::printf("{\"event\":\"offscreen_presenter_setup\",\"status\":\"%08x\",\"android_surface\":false}\n",unsigned(presentation_status));
        if(presentation_status!=0)return 0;
        auto status = graphics->SetupExternalGpu(*memory, {enter, leave}, irq, 1280, 720, 60);
        std::printf("{\"event\":\"game_gpu_setup_result\",\"status\":\"%08x\",\"same_loader_memory\":true,\"presentation\":false}\n",unsigned(status));
        if(status==0){
            // Persistent shader/pipeline storage (RexGlue): pipelines the game has used are
            // recorded and created again at startup, before the first frame, instead of
            // on first use in game (a synchronous placeholder compile on the command
            // processor plus skipped frames = visible freezes). Also holds the Vulkan driver
            // pipeline cache (vulkan_pipeline_cache.bin). ME_CACHE_ROOT; the APK passes its
            // files directory, the ADB executable uses /data/local/tmp/me_cache.
            const char* cache_env=std::getenv("ME_CACHE_ROOT");
            const std::string cache_root=cache_env?cache_env:"/data/local/tmp/me_cache";
            const auto storage_begin=std::chrono::steady_clock::now();
            graphics->InitializeShaderStorage(cache_root,0x45410850,true);
            std::printf("{\"event\":\"shader_storage_ready\",\"root\":\"%s\",\"ms\":%lld}\n",cache_root.c_str(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-storage_begin).count());
        }
        // ME_GPU_ANISO: anisotropic filtering override (-1 = as the game requests, 0 = off,
        // 1..5 = forced 1x..16x). RexGlue's default forces 4x on eligible textures.
        if(const char* aniso=std::getenv("ME_GPU_ANISO"))REXCVAR_SET(anisotropic_override,std::atoi(aniso));
        std::printf("{\"event\":\"gpu_aniso\",\"override\":%d}\n",int(REXCVAR_GET(anisotropic_override)));
        // ME_ASYNC_SKIP_FRAMES=0: present frames that still use placeholder pipelines (an
        // object missing for a few frames) instead of not presenting them (a freeze).
        if(const char* skip=std::getenv("ME_ASYNC_SKIP_FRAMES"); skip && !std::strcmp(skip,"0"))
            REXCVAR_SET(vulkan_async_skip_incomplete_frames,false);
        if(status==0 && std::getenv("ME_DISPLAY"))new std::thread(display_loop);
        // Diagnostic burst capture (shadow flicker): ME_FRAME_BURST=<dir>. When <dir>/go
        // appears, the next distinct guest frames (up to 90 or 6 s) are written at half
        // resolution as <dir>/fNNN.ppm with the guest swap number in burst.txt.
        if(const char* burst=std::getenv("ME_FRAME_BURST"); status==0 && burst){
            new std::thread([dir=std::string(burst)]{
                for(;;){
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const std::string go=dir+"/go";
                    if(::access(go.c_str(),F_OK))continue;
                    ::unlink(go.c_str());
                    FILE* index=std::fopen((dir+"/burst.txt").c_str(),"w");
                    uint64_t last=UINT64_MAX;unsigned n=0;
                    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(6);
                    while(n<90 && std::chrono::steady_clock::now()<end){
                        const auto swaps=generated_swaps.load(std::memory_order_relaxed);
                        if(swaps==last){std::this_thread::sleep_for(std::chrono::milliseconds(3));continue;}
                        rex::ui::RawImage frame;
                        if(!graphics->presenter()->CaptureGuestOutput(frame) || !frame.width || !frame.height)continue;
                        last=swaps;
                        char name[32];std::snprintf(name,sizeof(name),"/f%03u.ppm",n);
                        FILE* file=std::fopen((dir+name).c_str(),"wb");if(!file)break;
                        const unsigned w=frame.width/2,h=frame.height/2;
                        std::fprintf(file,"P6\n%u %u\n255\n",w,h);
                        std::vector<uint8_t> row(size_t(w)*3);
                        for(unsigned y=0;y<h;++y){
                            const uint8_t* src=frame.data.data()+size_t(y*2)*frame.stride;
                            for(unsigned x=0;x<w;++x)std::memcpy(row.data()+x*3,src+size_t(x*2)*4,3);
                            std::fwrite(row.data(),1,row.size(),file);
                        }
                        std::fclose(file);
                        ::chmod((dir+name).c_str(),0666);  // readable by adb for analysis
                        if(index)std::fprintf(index,"%u %llu\n",n,(unsigned long long)swaps);
                        ++n;
                    }
                    if(index){std::fclose(index);::chmod((dir+"/burst.txt").c_str(),0666);}
                    std::printf("{\"event\":\"frame_burst_done\",\"frames\":%u}\n",n);
                }
            });
        }
        // Diagnostic RenderDoc capture: ME_RENDERDOC=1 (the layer is enabled at instance creation,
        // RunnerRecompiled-renderdoc.apk) and ME_RENDERDOC_DIR=<dir>. When <dir>/go appears, all
        // Vulkan work from one guest swap to the second next one is captured to <dir>/me_*.rdc.
        if(const char* rd_dir=std::getenv("ME_RENDERDOC_DIR"); status==0 && rd_dir && std::getenv("ME_RENDERDOC")){
            new std::thread([dir=std::string(rd_dir)]{
                void* layer=::dlopen("libVkLayer_GLES_RenderDoc.so",RTLD_NOW|RTLD_NOLOAD);
                auto get_api=layer?reinterpret_cast<pRENDERDOC_GetAPI>(::dlsym(layer,"RENDERDOC_GetAPI")):nullptr;
                RENDERDOC_API_1_0_1* api=nullptr;
                if(!get_api || !get_api(eRENDERDOC_API_Version_1_0_1,reinterpret_cast<void**>(&api)) || !api){
                    std::printf("{\"event\":\"renderdoc\",\"loaded\":false}\n");return;
                }
                // API 1.1.0 = 1.0.x layout + TriggerMultiFrameCapture (the vendored header is 1.0.1).
                struct RenderDocApi110 { RENDERDOC_API_1_0_1 base; void (RENDERDOC_CC* TriggerMultiFrameCapture)(uint32_t); };
                RenderDocApi110* api11=nullptr;
                if(!get_api(RENDERDOC_Version(10100),reinterpret_cast<void**>(&api11)))api11=nullptr;
                api->SetLogFilePathTemplate((dir+"/me").c_str());
                std::printf("{\"event\":\"renderdoc\",\"loaded\":true,\"dir\":\"%s\"}\n",dir.c_str());
                auto wait_swap=[](uint64_t after){
                    while(generated_swaps.load(std::memory_order_relaxed)<=after)std::this_thread::sleep_for(std::chrono::microseconds(200));
                    return generated_swaps.load(std::memory_order_relaxed);
                };
                for(;;){
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const std::string go=dir+"/go";
                    if(::access(go.c_str(),F_OK))continue;
                    ::unlink(go.c_str());
                    // ME_RENDERDOC_FRAMES=<n> (1..8): RenderDoc's own multi-frame capture (API 1.1
                    // TriggerMultiFrameCapture), n consecutive frames delimited by vkQueuePresentKHR
                    // (the display thread presents each new guest frame once), one file per frame
                    // (shadow flicker). Without it: one capture over two guest swap intervals.
                    const char* frames_env=std::getenv("ME_RENDERDOC_FRAMES");
                    if(frames_env && api11){
                        const uint32_t frames=uint32_t(std::clamp(std::atoi(frames_env),1,8));
                        const uint32_t before=api->GetNumCaptures();
                        api11->TriggerMultiFrameCapture(frames);
                        for(int t=0;t<200 && api->GetNumCaptures()<before+frames;++t)std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        for(uint32_t i=before;i<api->GetNumCaptures();++i){
                            char path[512]={};uint32_t length=sizeof(path);
                            api->GetCapture(i,path,&length,nullptr);
                            if(path[0])::chmod(path,0666);
                            std::printf("{\"event\":\"renderdoc_capture\",\"ok\":1,\"frame_index\":%u,\"file\":\"%s\"}\n",i-before,path);
                        }
                        continue;
                    }
                    const uint64_t start=wait_swap(generated_swaps.load(std::memory_order_relaxed));
                    api->StartFrameCapture(nullptr,nullptr);
                    const uint64_t end=wait_swap(start+1);
                    const uint32_t ok=api->EndFrameCapture(nullptr,nullptr);
                    char path[512]={};uint32_t length=sizeof(path);
                    const uint32_t count=api->GetNumCaptures();
                    if(count)api->GetCapture(count-1,path,&length,nullptr);
                    if(path[0])::chmod(path,0666);
                    std::printf("{\"event\":\"renderdoc_capture\",\"ok\":%u,\"swaps\":[%llu,%llu],\"file\":\"%s\"}\n",
                                ok,(unsigned long long)start,(unsigned long long)end,path);
                }
            });
        }
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
                        char scene[48];me_current_scene(scene,sizeof(scene));
                        std::printf("{\"event\":\"frame_rates\",\"t\":%.1f,\"seconds\":%.3f,\"capture_hz\":%.2f,\"generated_swap_hz\":%.2f,\"max_frame_ms\":%.1f,\"swap_ms_hist\":%s,\"scene\":\"%s\"}\n",uptime_s(),seconds,captures/seconds,(swaps-prior_swaps)/seconds,take_max_frame_ns()/1e6,take_swap_hist().c_str(),scene);
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
    const uint64_t cpu_swaps=generated_swaps.fetch_add(1,std::memory_order_relaxed)+1;
    // ME_DIAGNOSTIC_CP_LAG=1: how many guest frames the command processor is behind the CPU at
    // each swap the game queues (CPU swap count minus the frame the CP is processing), min/max
    // per second (shadow flicker: constants read from guest memory by a lagging CP).
    if(static const bool cp_lag=std::getenv("ME_DIAGNOSTIC_CP_LAG")!=nullptr;cp_lag){
        static int64_t lag_min=INT64_MAX,lag_max=INT64_MIN,lag_sum=0,lag_n=0,histogram[6]={};
        static auto lag_start=std::chrono::steady_clock::now();
        const int64_t lag=int64_t(cpu_swaps)-int64_t(me_trace_frame.load(std::memory_order_relaxed));
        lag_min=std::min(lag_min,lag);lag_max=std::max(lag_max,lag);lag_sum+=lag;++lag_n;
        ++histogram[std::clamp<int64_t>(lag-lag_min,0,5)];
        if(std::chrono::steady_clock::now()-lag_start>=std::chrono::seconds(1)){
            std::printf("{\"event\":\"cp_lag\",\"cpu_swaps\":%llu,\"cp_frame\":%llu,\"lag_min\":%lld,\"lag_max\":%lld,\"lag_mean\":%.2f,\"swaps\":%lld}\n",
                (unsigned long long)cpu_swaps,(unsigned long long)me_trace_frame.load(std::memory_order_relaxed),
                (long long)lag_min,(long long)lag_max,double(lag_sum)/double(lag_n),(long long)lag_n);
            lag_min=INT64_MAX;lag_max=INT64_MIN;lag_sum=lag_n=0;std::fill(std::begin(histogram),std::end(histogram),0);
            lag_start=std::chrono::steady_clock::now();
        }
    }
    {
        const int64_t now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t last=last_swap_ns.exchange(now,std::memory_order_relaxed);
        if(last){
            int64_t interval=now-last,seen=max_swap_interval_ns.load(std::memory_order_relaxed);
            while(interval>seen && !max_swap_interval_ns.compare_exchange_weak(seen,interval,std::memory_order_relaxed)){}
            seen=overlay_max_swap_ns.load(std::memory_order_relaxed);
            while(interval>seen && !overlay_max_swap_ns.compare_exchange_weak(seen,interval,std::memory_order_relaxed)){}
            swap_hist[frame_bucket(interval)].fetch_add(1,std::memory_order_relaxed);
        }
    }
    return 1;
}
