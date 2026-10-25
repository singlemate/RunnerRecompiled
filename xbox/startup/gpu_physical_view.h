#pragma once
#include "physical_memory.h"
#include "gpu_write_watch.h"
#include <rex/memory/ring_buffer.h>
#include <memory>
#include <cstring>

// Non-owning GPU view of the SAME memfd backing the guest physical aliases.
// Without a committed map, callers hold the loader execution gate.
struct MeGpuPhysicalView {
    uint8_t* raw = nullptr;
    const MePhysicalMemory* memory = nullptr;
    // When set, ranges are validated against its committed-page map, which is safe to
    // read from the GPU thread while guest threads allocate or free.
    const MeGpuWriteWatch* committed = nullptr;
    uint8_t* resolve(uint32_t physical, uint32_t size) const {
        if (!raw || !size || uint64_t(physical) + size > 0x20000000) return nullptr;
        if (committed) return committed->range_committed(physical, size) ? raw + physical : nullptr;
        if (!memory) return nullptr;
        for (const auto& allocation : memory->allocations)
            if (physical >= allocation.physical && uint64_t(physical) + size <=
                uint64_t(allocation.physical) + allocation.size) return raw + physical;
        return nullptr;
    }
    std::unique_ptr<rex::memory::RingBuffer> bind_ring(uint32_t physical, uint32_t size_log2) const {
        // Xbox CP convention: size in bytes = 2^(size_log2 + 3).
        if ((physical & 3) || size_log2 > 21) return {};
        const uint32_t bytes = 1u << (size_log2 + 3);
        auto* address = resolve(physical, bytes);
        if (!address) return {};
        return std::make_unique<rex::memory::RingBuffer>(address, bytes);
    }
    bool write_read_pointer(uint32_t physical, uint32_t index, uint32_t ring_capacity_words) const {
        if ((physical & 3) || index >= ring_capacity_words) return false;
        auto* address = resolve(physical, 4);
        if (!address) return false;
        const uint32_t encoded = __builtin_bswap32(index);
        std::memcpy(address, &encoded, 4);
        return true;
    }
};
