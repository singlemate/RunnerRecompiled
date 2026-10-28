#pragma once
#include "gpu_mmio.h"
#include "xma_registers.h"
#include <cstdint>
#include <type_traits>

uint64_t me_gpu_mmio_load(uint64_t address, unsigned size);
void me_gpu_mmio_store(uint64_t address, unsigned size, uint64_t value);
void me_guest_poll_window(uint64_t address);
// Indirect calls (bctr/bctrl through the function lookup table): a target with no
// recompiled function stops with its address instead of jumping to address 0.
[[noreturn]] void me_unmapped_indirect_call(uint32_t target, uint32_t lr);
#define PPC_CALL_INDIRECT_FUNC(x) \
    do { \
        auto* const me_indirect_ = PPC_LOOKUP_FUNC(base, x); \
        if (__builtin_expect(!me_indirect_, 0)) me_unmapped_indirect_call(uint32_t(x), uint32_t(ctx.lr)); \
        me_indirect_(ctx, base); \
    } while (0)
bool me_pool_scan_enabled();
uint64_t me_xma_load(uint64_t address, unsigned size);
void me_xma_store(uint64_t address, unsigned size, uint64_t value);

template <class T> inline T me_guest_load(uint8_t* base, uint64_t address) {
    static_assert(std::is_unsigned_v<T>);
    if (MeXmaRegisters::overlaps(address, sizeof(T))) return T(me_xma_load(address, sizeof(T)));
    if (MeGpuMmio::overlaps(address, sizeof(T)) || address > 0x100000000ull - sizeof(T))
        return T(me_gpu_mmio_load(address, sizeof(T)));
    if (address >= 0xa0000000 && address < 0x100000000ull) me_guest_poll_window(address);
    T value = *reinterpret_cast<volatile T*>(base + address);
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
    if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) return __builtin_bswap64(value);
    return value;
}
template <class T> inline void me_guest_store(uint8_t* base, uint64_t address, T value) {
    static_assert(std::is_unsigned_v<T>);
    if (MeXmaRegisters::overlaps(address, sizeof(T))) { me_xma_store(address, sizeof(T), value); return; }
    if (MeGpuMmio::overlaps(address, sizeof(T)) || address > 0x100000000ull - sizeof(T)) {
        me_gpu_mmio_store(address, sizeof(T), value);
        return;
    }
    if constexpr (sizeof(T) == 2) value = __builtin_bswap16(value);
    if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
    *reinterpret_cast<volatile T*>(base + address) = value;
}
#define PPC_LOAD_U8(x) me_guest_load<uint8_t>(base, (x))
#define PPC_LOAD_U16(x) me_guest_load<uint16_t>(base, (x))
#define PPC_LOAD_U32(x) me_guest_load<uint32_t>(base, (x))
#define PPC_LOAD_U64(x) me_guest_load<uint64_t>(base, (x))
#define PPC_STORE_U8(x,y) me_guest_store<uint8_t>(base, (x), uint8_t(y))
#define PPC_STORE_U16(x,y) me_guest_store<uint16_t>(base, (x), uint16_t(y))
#define PPC_STORE_U32(x,y) me_guest_store<uint32_t>(base, (x), uint32_t(y))
#define PPC_STORE_U64(x,y) me_guest_store<uint64_t>(base, (x), uint64_t(y))

// Diagnostic finite-positive reciprocal square-root seed; unsupported modes stop.
double me_frsqrte_diagnostic(double value, uint32_t instruction);

void me_dcbst_diagnostic(uint32_t address);
