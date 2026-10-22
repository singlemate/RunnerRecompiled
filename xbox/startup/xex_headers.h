#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Checked view of a single user's original XEX header in guest memory.
struct MeXexHeaders {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    uint32_t guest_base = 0;
    static uint32_t be32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    bool valid() const {
        return bytes && size >= 24 && size <= UINT32_MAX &&
            uint64_t(guest_base) + size <= 0x100000000ull &&
            std::memcmp(bytes, "XEX2", 4) == 0 && be32(bytes + 8) == size &&
            be32(bytes + 20) <= (size - 24) / 8;
    }
    bool import_version(const char* name,uint32_t& target,uint32_t& minimum) const {
        target=minimum=0;uint32_t address=0;
        if(!lookup(0x103ff,address))return false;
        const size_t start=address-guest_base;
        if(start>size || size-start<12)return false;
        const auto* table=bytes+start;const size_t length=be32(table),strings=be32(table+4),count=be32(table+8);
        if(length<12 || length>size-start || strings>length-12 || count>256)return false;
        size_t cursor=12+strings;
        for(size_t i=0;i<count;++i){
            if(cursor>length || length-cursor<40)return false;
            const auto* library=table+cursor;const size_t entry_size=be32(library);
            if(entry_size<40 || entry_size>length-cursor)return false;
            const size_t index=(uint16_t(library[36])<<8)|library[37];size_t offset=0;
            for(size_t n=0;n<=index;++n){
                if(offset>=strings)return false;
                const auto* text=table+12+offset;
                const auto* end=static_cast<const uint8_t*>(std::memchr(text,0,strings-offset));
                if(!end)return false;
                if(n==index){
                    if(size_t(end-text)==std::strlen(name) && !std::memcmp(text,name,end-text)){
                        target=be32(library+28);minimum=be32(library+32);return true;
                    }
                    break;
                }
                offset=(size_t(end-(table+12))+4)&~size_t(3);
            }
            cursor+=entry_size;
        }
        return false;
    }
    bool lookup(uint32_t key, uint32_t& result) const {
        result = 0;
        if (!valid()) return false;
        for (uint32_t i = 0; i < be32(bytes + 20); ++i) {
            const size_t at = 24 + size_t(i) * 8;
            if (be32(bytes + at) != key) continue;
            const uint32_t value = be32(bytes + at + 4);
            const unsigned tag = key & 255;
            if (tag == 0) result = value;
            else if (tag == 1) result = guest_base + uint32_t(at) + 4;
            else {
                if (value > size || size - value < 4) return false;
                const uint32_t length = tag == 255 ? be32(bytes + value) : tag * 4;
                if (length < 4 || length > size - value) return false;
                result = guest_base + value;
            }
            return true;
        }
        return false;
    }
};
