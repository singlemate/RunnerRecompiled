#include "gpu_bridge.h"
#include "rex_gpu_memory.h"
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/registers.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdlib>
#include "guest_preview.h"
// Process-lifetime probe state. Import-stop/_exit terminates all native workers.
// Explicit partial-initialization cleanup/retry is not exposed by this bridge.
static rex::graphics::vulkan::VulkanGraphicsSystem* graphics;
static MeRexGpuMemory* memory;
extern "C" int me_gpu_start(void* view, void* mmio, MeGpuGate enter, MeGpuGate leave, MeGpuIrq irq) {
    if (graphics || !view || !mmio || !enter || !leave || !irq) return 0;
    try {
        enter();
        try { memory = new MeRexGpuMemory(*static_cast<MeGpuPhysicalView*>(view), *static_cast<MeGpuMmio*>(mmio)); }
        catch (...) { leave(); throw; }
        leave();
        graphics = new rex::graphics::vulkan::VulkanGraphicsSystem;
        auto presentation_status=graphics->SetupExternalPresenter();
        std::printf("{\"event\":\"offscreen_presenter_setup\",\"status\":\"%08x\",\"android_surface\":false}\n",unsigned(presentation_status));
        if(presentation_status!=0)return 0;
        auto status = graphics->SetupExternalGpu(*memory, {enter, leave}, irq, 1280, 720, 60);
        std::printf("{\"event\":\"game_gpu_setup_result\",\"status\":\"%08x\",\"same_loader_memory\":true,\"presentation\":false}\n",unsigned(status));
        if(status==0){
            // Diagnostic readback worker; never hold the guest gate across a
            // capture fence wait. The probe process owns it until _exit.
            new std::thread([]{
                MeGuestPreview preview;
                auto next_dump=std::chrono::steady_clock::now();
                for(unsigned attempt=1;;++attempt){
                    const bool live=preview.connected();
                    std::this_thread::sleep_for(std::chrono::milliseconds(live?100:500));
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
                    if(live && attempt%10==0){
                        const auto send_end=std::chrono::steady_clock::now();
                        std::printf("{\"event\":\"preview_timing\",\"attempt\":%u,\"capture_us\":%lld,\"send_us\":%lld,\"minimum_sleep_ms\":100}\n",attempt,
                            (long long)std::chrono::duration_cast<std::chrono::microseconds>(capture_end-capture_begin).count(),
                            (long long)std::chrono::duration_cast<std::chrono::microseconds>(send_end-capture_end).count());
                    }
                    if(!dump_due)continue;
                    next_dump=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                    FILE* file=std::fopen("/data/local/tmp/me_guest_frame.ppm.tmp","wb");if(!file)continue;
                    bool ok=std::fprintf(file,"P6\n%u %u\n255\n",frame.width,frame.height)>0;
                    std::vector<uint8_t> row(size_t(frame.width)*3);
                    for(unsigned y=0;y<frame.height && ok;++y){
                        for(unsigned x=0;x<frame.width;++x)std::memcpy(row.data()+x*3,frame.data.data()+y*frame.stride+x*4,3);
                        ok=std::fwrite(row.data(),1,row.size(),file)==row.size();
                    }
                    if(std::fclose(file))ok=false;
                    if(ok && !std::rename("/data/local/tmp/me_guest_frame.ppm.tmp","/data/local/tmp/me_guest_frame.ppm"))
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
    return 1;
}
