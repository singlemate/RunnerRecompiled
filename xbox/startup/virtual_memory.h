#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>

// Initial virtual-memory subset: reserve/commit, RW, 4/64 KiB guest pages.
// Addresses are owned by this manager, separate from image/stack/module memory.
struct MeVirtualMemory {
    struct Region {
        uint32_t base, size, page;
        std::vector<uint8_t> committed;
    };
    std::vector<Region> regions;
    bool (*commit_pages)(uint32_t, uint32_t) = nullptr;
    bool (*discard_pages)(uint32_t, uint32_t) = nullptr;
    static constexpr uint32_t ok = 0, invalid = 0xc000000d, conflict = 0xc0000018, no_memory = 0xc0000017;
    // A 128-byte aligned CPU cache line cannot cross a supported guest page.
    // Consult current ownership/commit state every time; never cache a Region*.
    bool committed_cache_line(uint32_t line) const {
        if (line & 127) return false;
        for (const auto& r : regions) {
            if (line < r.base || uint64_t(line) + 128 > uint64_t(r.base) + r.size) continue;
            if (r.page != 4096 && r.page != 65536) return false;
            const auto index = (line - r.base) / r.page;
            return index < r.committed.size() && r.committed[index] != 0;
        }
        return false;
    }
    uint32_t free(uint32_t& address, uint32_t& size, uint32_t type) {
        if (type != 0x4000 && type != 0x8000) return invalid;
        for (size_t i = 0; i < regions.size(); ++i) {
            auto& r = regions[i];
            if (address < r.base || uint64_t(address) >= uint64_t(r.base) + r.size) continue;
            uint32_t low = address, length = size;
            if (type == 0x8000) {
                if (address != r.base || (size && size != r.size)) return invalid;
                length = r.size;
            } else {
                if (!size) return invalid;
                low = address & ~(r.page - 1);
                uint64_t rounded = (uint64_t(size) + address - low + r.page - 1) & ~uint64_t(r.page - 1);
                if (uint64_t(low) + rounded > uint64_t(r.base) + r.size) return invalid;
                length = uint32_t(rounded);
            }
            if (!discard_pages || !discard_pages(low, length)) return no_memory;
            if (type == 0x8000) regions.erase(regions.begin() + i);
            else for (size_t j = (low - r.base) / r.page; j < (low - r.base + length) / r.page; ++j) r.committed[j] = 0;
            address = low; size = length; return ok;
        }
        return 0xc00000a0; // STATUS_MEMORY_NOT_ALLOCATED
    }
    bool supports(uint32_t type, uint32_t protection, uint32_t debug) const {
        constexpr uint32_t allowed = 0x40000000 | 0x20000000 | 0x00800000 | 0x3000;
        return debug == 0 && protection == 4 && (type & 0x3000) && !(type & ~allowed);
    }
    uint32_t allocate(uint32_t& requested_base, uint32_t& requested_size, uint32_t type) {
        if (!requested_size || requested_size > 0x10000000 || !(type & 0x3000)) return invalid;
        const bool reserve = (type & 0x2000) != 0;
        const bool commit = (type & 0x1000) != 0;
        uint32_t page = (type & 0x20000000) ? 65536 : 4096;
        uint32_t address = requested_base;
        size_t index = regions.size();
        if (address) {
            for (size_t i = 0; i < regions.size(); ++i)
                if (address >= regions[i].base && uint64_t(address) < uint64_t(regions[i].base) + regions[i].size) {
                    index = i; page = regions[i].page; break;
                }
        }
        const uint32_t alignment = address ? page : 65536;
        const uint64_t low = uint64_t(address) & ~(uint64_t(page) - 1);
        const uint64_t size = (uint64_t(requested_size) + (address - low) + alignment - 1) & ~(uint64_t(alignment) - 1);
        if (!size || size > 0x10000000) return invalid;
        address = uint32_t(low);
        bool created = false;
        if (index == regions.size()) {
            if (address && !reserve) return conflict;
            if (!address) {
                address = 0x10000000;
                auto sorted = regions;
                std::sort(sorted.begin(), sorted.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
                for (const auto& r : sorted) {
                    if (uint64_t(address) + size <= r.base) break;
                    if (address < uint64_t(r.base) + r.size)
                        address = uint32_t((uint64_t(r.base) + r.size + 65535) & ~uint64_t(65535));
                }
            }
            if (address < 0x10000000 || uint64_t(address) + size > 0x40000000) return no_memory;
            for (const auto& r : regions)
                if (address < uint64_t(r.base) + r.size && r.base < uint64_t(address) + size) return conflict;
            regions.push_back({address, uint32_t(size), page, std::vector<uint8_t>(size / page)});
            index = regions.size() - 1; created = true;
        } else {
            if (reserve || uint64_t(address) + size > uint64_t(regions[index].base) + regions[index].size) return conflict;
        }
        auto& region = regions[index];
        if (commit) {
            const size_t first = (address - region.base) / page;
            const size_t count = size / page;
            bool needs_commit = false;
            for (size_t i = 0; i < count; ++i) needs_commit |= !region.committed[first + i];
            // Callback changes protection only: anonymous pages start zeroed,
            // while existing committed bytes must survive a repeat commit.
            if (needs_commit && (!commit_pages || !commit_pages(address, uint32_t(size)))) {
                if (created) regions.pop_back();
                return no_memory;
            }
            for (size_t i = 0; i < count; ++i) region.committed[first + i] = 1;
        }
        requested_base = address; requested_size = uint32_t(size);
        return ok;
    }
};
