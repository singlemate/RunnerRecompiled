#pragma once
#include <cstdint>

// Access is serialized by the loader execution gate. The backend owns context.
// Values at this boundary are host-order PPC integer values, not byte-swapped.
struct MeGpuMmio {
    static constexpr uint32_t begin = 0x7fc80000, end = 0x7fc90000;
    using Read = bool (*)(void*, uint32_t, uint32_t&);
    using Write = bool (*)(void*, uint32_t, uint32_t);
    void* context = nullptr;
    Read read = nullptr;
    Write write = nullptr;
    static bool overlaps(uint64_t address, unsigned size) {
        return address < end && address + size > begin;
    }
    static bool valid(uint64_t address, unsigned size) {
        return size == 4 && !(address & 3) && address >= begin && address <= end - 4;
    }
    bool load(uint64_t address, unsigned size, uint64_t& value) const {
        uint32_t result = 0;
        if (!valid(address, size) || !read || !read(context, uint32_t(address), result)) return false;
        value = result;
        return true;
    }
    bool store(uint64_t address, unsigned size, uint64_t value) const {
        return valid(address, size) && write && write(context, uint32_t(address), uint32_t(value));
    }
};
