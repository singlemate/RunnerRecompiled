#include <rex/graphics/vulkan/graphics_system.h>
#include "../startup/rex_gpu_memory.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <mutex>
#include <cstdlib>
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(45);
    int fd = memfd_create("me-cp-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 0x20000000)) return 2;
    auto* raw = static_cast<uint8_t*>(mmap(nullptr, 0x20000000, PROT_NONE, MAP_SHARED, fd, 0));
    if (raw == MAP_FAILED || mprotect(raw + 0x1000000, 4096, PROT_READ | PROT_WRITE)) return 2;
    MePhysicalMemory allocation;
    allocation.allocations.push_back({0x1000000,4096,0xa1000000,4});
    MeGpuPhysicalView view{raw, &allocation}; MeGpuMmio mmio;
    MeRexGpuMemory memory(view, mmio);
    std::mutex gate;
    rex::graphics::vulkan::VulkanGraphicsSystem graphics;
    std::puts("{\"event\":\"real_vulkan_cp_setup_begin\",\"game_data\":false}");
    auto status = graphics.SetupExternalGpu(memory, {[&] {gate.lock();}, [&] {gate.unlock();}},
        [](uint32_t, uint32_t, uint32_t, uint32_t) { return false; }, 1280,720,60);
    std::printf("{\"event\":\"real_vulkan_cp_setup_result\",\"status\":\"%08x\",\"mmio_bound\":%s,\"presentation\":false}\n",unsigned(status),mmio.read ? "true":"false");
    if (status) std::_Exit(3); // Partial Vulkan setup cleanup has not yet been audited.
    {
        std::lock_guard<std::mutex> lock(gate);
        auto* words = reinterpret_cast<uint32_t*>(raw + 0x1000000);
        words[0] = __builtin_bswap32(0x2080); // Type0: PA_SC_WINDOW_OFFSET, one register.
        words[1] = __builtin_bswap32(0x1234);
        words[2] = __builtin_bswap32(0xc0013f00); // Type3 INDIRECT_BUFFER, two payload dwords.
        words[3] = __builtin_bswap32(0x1000ff8);
        words[4] = __builtin_bswap32(2);
        auto* indirect = reinterpret_cast<uint32_t*>(raw + 0x1000ff8);
        indirect[0] = __builtin_bswap32(0x2080);
        indirect[1] = __builtin_bswap32(0x5678); // Exactly at allocation end; sentinel is PROT_NONE.
        graphics.InitializeRingBuffer(0x1000000, 3);
        graphics.EnableReadPointerWriteBack(0x1000100, 4);
        if (!mmio.store(0x7fc80714, 4, 5)) std::_Exit(4);
    }
    bool completed = false;
    for (unsigned i = 0; i < 2000; ++i) {
        {
            std::lock_guard<std::mutex> lock(gate);
            uint32_t index; std::memcpy(&index, raw + 0x1000100, 4);
            if (__builtin_bswap32(index) == 5) {
                completed = graphics.register_file()->values[0x2080] == 0x5678;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("{\"event\":\"real_pm4_type0_indirect_result\",\"completed\":%s,\"draw\":false}\n",completed ? "true":"false");
    graphics.Shutdown();
    mmio = {}; // All workers joined; detach callbacks before GraphicsSystem destruction.
    if (!completed) return 5;
    std::puts("{\"event\":\"real_vulkan_cp_shutdown_done\"}");
    return 0;
}
