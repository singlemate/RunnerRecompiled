#pragma once
#include <array>
#include <cstdint>

// Caller holds the guest execution gate. Slot ownership only, no decoder state.
struct MeXmaContextPool {
    static constexpr unsigned context_count=320,context_bytes=64;
    std::array<bool,context_count> allocated{};
    uint32_t acquire(uint32_t base){
        if(!base)return 0;
        for(unsigned i=0;i<context_count;++i)if(!allocated[i]){allocated[i]=true;return base+i*context_bytes;}
        return 0;
    }
    bool release(uint32_t base,uint32_t physical){
        if(!base || physical<base)return false;
        const uint32_t offset=physical-base;
        if(offset>=context_count*context_bytes || offset%context_bytes)return false;
        const auto index=offset/context_bytes;if(!allocated[index])return false;
        allocated[index]=false;return true;
    }
};
