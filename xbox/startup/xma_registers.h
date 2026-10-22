#pragma once
#include <cstdint>
// Host-order device state; PPC lwbrx sees the little-endian register encoding.
// No decoding, context completion or worker activity is synthesized here.
struct MeXmaRegisters {
    uint32_t context_physical = 0, control = 0;
    static bool overlaps(uint64_t a, unsigned n) { return a < 0x7feb0000ull && a + n > 0x7fea0000ull; }
    static bool valid(uint64_t a, unsigned n) { return n == 4 && !(a & 3) && a >= 0x7fea0000 && a <= 0x7feafffc; }
    bool read(uint64_t a, unsigned n, uint64_t& value) {
        if (!valid(a,n) || !context_physical) return false;
        uint32_t result;
        if (a == 0x7fea1800) result = context_physical;
        else if (a == 0x7fea1804) result = control;
        else return false;
        value = __builtin_bswap32(result); return true;
    }
    bool write(uint64_t a, unsigned n, uint64_t value) {
        if (!valid(a,n) || a != 0x7fea1804) return false;
        const auto decoded = __builtin_bswap32(uint32_t(value));
        if (decoded != 0 && decoded != 2 && decoded != 3) return false;
        control = decoded; return true;
    }
};
