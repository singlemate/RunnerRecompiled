#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>

struct MePhysicalMemory {
    struct Allocation { uint32_t physical, size, address, protection; };
    std::vector<Allocation> allocations;
    bool (*map)(uint32_t, uint32_t, bool) = nullptr;
    static uint32_t physical(uint32_t address) {
        if (address >= 0xe0000000) return address - 0xe0000000 + 0x1000;
        if (address >= 0xc0000000) return address - 0xc0000000;
        if (address >= 0xa0000000) return address - 0xa0000000;
        return UINT32_MAX;
    }
    const Allocation* find(uint32_t address) const {
        const auto p = physical(address);
        for (const auto& a : allocations)
            if (p >= a.physical && uint64_t(p) < uint64_t(a.physical) + a.size) return &a;
        return nullptr;
    }
    static bool supports(uint32_t flags, uint32_t protection) {
        return !flags && (protection & 0xff) == 4 && !(protection & ~0xa0000604u)
            && (protection & 0xa0000000u) != 0xa0000000u;
    }
    uint32_t allocate(uint32_t size, uint32_t protection, uint32_t minimum, uint32_t maximum, uint32_t alignment) {
        const uint32_t page = protection & 0x20000000 ? 65536 : protection & 0x80000000 ? 0x1000000 : 4096;
        const uint64_t length = (uint64_t(size) + page - 1) & ~uint64_t(page - 1);
        if (!size || length > 0x1ec00000 || minimum > maximum) return 0;
        uint64_t align = std::max(uint64_t(page), (uint64_t(alignment) + page - 1) & ~uint64_t(page - 1));
        if (align & (align - 1)) return 0;
        // Bounded research pool; lower 16 MiB remain reserved for GPU/kernel work.
        const uint64_t low = std::max(minimum, 0x1000000u);
        const uint64_t high = std::min(uint64_t(maximum) + 1, uint64_t(0x1fc00000));
        if (high < low + length) return 0;
        uint64_t candidate = (high - length) & ~(align - 1);
        for (;;) {
            if (candidate < low) return 0;
            const Allocation* overlap = nullptr;
            for (const auto& a : allocations)
                if (candidate < uint64_t(a.physical) + a.size && a.physical < candidate + length) { overlap = &a; break; }
            if (!overlap) break;
            if (overlap->physical < low + length) return 0;
            candidate = (uint64_t(overlap->physical) - length) & ~(align - 1);
        }
        if (!map || !map(uint32_t(candidate), uint32_t(length), true)) return 0;
        const uint32_t alias = page == 4096 ? 0xe0000000 + uint32_t(candidate) - 0x1000 :
            (page == 65536 ? 0xa0000000 : 0xc0000000) + uint32_t(candidate);
        allocations.push_back({uint32_t(candidate), uint32_t(length), alias, protection});
        return alias;
    }
    bool free(uint32_t address) {
        for (size_t i = 0; i < allocations.size(); ++i) {
            auto a = allocations[i];
            if (a.address != address) continue;
            if (!map || !map(a.physical, a.size, false)) return false;
            allocations.erase(allocations.begin() + i); return true;
        }
        return false;
    }
};
