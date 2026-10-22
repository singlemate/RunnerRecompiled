#pragma once
#include <cstdint>
extern "C" {
using MeGpuGate = void (*)();
using MeGpuIrq = bool (*)(uint32_t, uint32_t, uint32_t, uint32_t);
// Start/shutdown are called without holding the guest gate. Other calls hold it.
int me_gpu_start(void* physical_view, void* mmio, MeGpuGate enter, MeGpuGate leave, MeGpuIrq irq);
void me_gpu_ring(uint32_t physical, uint32_t size_log2);
void me_gpu_readback(uint32_t physical, uint32_t block_log2);
void me_gpu_irq_callback(uint32_t callback, uint32_t argument);
void me_gpu_vblank();
int me_gpu_make_swap(uint32_t* words, const uint32_t* fetch, uint32_t virtual_address,
    uint32_t physical_address, uint32_t format, uint32_t color_space, uint32_t width, uint32_t height);
}
