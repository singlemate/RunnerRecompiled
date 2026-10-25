#pragma once
#include <sys/mman.h>
#include <atomic>
#include <cstdint>
#include <cerrno>

// Page-protection write watch for GPU-cached guest physical memory (Xenia's
// approach). A watched page is read-only in all three guest physical aliases;
// the first CPU write faults, the handler marks the page dirty and makes it
// writable again. The GPU's own raw memfd view stays writable, so GPU-side
// writes (resolve, memexport, read-pointer write-back) are not reported as CPU
// writes. Writes by system calls (pread into a guest buffer) do not fault, so
// callers mark such ranges with host_write() first.
struct MeGpuWriteWatch {
    static constexpr uint32_t page_count = 0x20000000u / 4096u;
    static constexpr uint32_t words = page_count / 64;
    uint8_t* guest = nullptr;
    std::atomic<uint64_t> watched[words]{}, dirty[words]{};
    // Committed physical pages (maintained by the loader's physical mapper). Lets the
    // GPU thread validate ranges without the loader's allocation list, and lets the
    // fault handler recognise any write fault on committed memory as a watch fault.
    std::atomic<uint64_t> committed[words]{};
    // Summary flag: set after any dirty bit; lets synchronization skip the 2048-word
    // scan when nothing was written (it runs per draw on the GPU thread).
    std::atomic<bool> any_dirty{false};
    std::atomic<uint64_t> faults{0};
    mutable std::atomic<uint64_t> protect_failures{0};
    mutable std::atomic<int> last_protect_errno{0};
    mutable std::atomic<uint32_t> last_protect_page{0};

    static bool bit(const std::atomic<uint64_t>* set, uint32_t page) {
        return set[page / 64].load(std::memory_order_acquire) >> (page % 64) & 1;
    }
    static uintptr_t alias(unsigned which, uint32_t physical) {
        return which == 0 ? 0xa0000000ull + physical : which == 1 ? 0xc0000000ull + physical
                                                                : 0xe0000000ull + physical - 0x1000;
    }
    // Protects `count` pages from `first` in every guest alias.
    bool protect(uint32_t first, uint32_t count, int access) const {
        if (!guest || !count) return true;
        bool ok = true;
        for (unsigned which = 0; which < 3; ++which) {
            const uintptr_t at = alias(which, first * 4096u);
            if (at + uint64_t(count) * 4096u > 0x100000000ull) { ok = false; continue; }
            if (mprotect(guest + at, size_t(count) * 4096u, access)) {
                last_protect_errno.store(errno,std::memory_order_relaxed);
                last_protect_page.store(first,std::memory_order_relaxed);
                protect_failures.fetch_add(1,std::memory_order_relaxed);ok=false;
            }
        }
        return ok;
    }
    // Starts watching pages [first, first+count) that are neither watched nor
    // carrying an unsynchronized write. Returns false if protection failed; such
    // pages are reported dirty so they are never silently unwatched.
    bool watch(uint32_t first, uint32_t count) {
        uint32_t run = 0, run_first = 0; bool ok = true;
        auto flush = [&] {
            if (!run) return;
            if (!protect(run_first, run, PROT_READ)) {
                ok = false;
                for (uint32_t p = run_first; p < run_first + run; ++p) {
                    watched[p / 64].fetch_and(~(1ull << (p % 64)), std::memory_order_acq_rel);
                    dirty[p / 64].fetch_or(1ull << (p % 64), std::memory_order_acq_rel); any_dirty.store(true, std::memory_order_release);
                }
            }
            run = 0;
        };
        for (uint32_t p = first; p < first + count; ++p) {
            if (bit(watched, p) || bit(dirty, p)) { flush(); continue; }
            watched[p / 64].fetch_or(1ull << (p % 64), std::memory_order_acq_rel);
            if (!run) run_first = p;
            ++run;
        }
        flush();
        return ok;
    }
    // Marks one watched page written: dirty, unwatched, writable. Async-signal-safe.
    bool written(uint32_t page) {
        const uint64_t mask = 1ull << (page % 64);
        if (!(watched[page / 64].fetch_and(~mask, std::memory_order_acq_rel) & mask)) return false;
        dirty[page / 64].fetch_or(mask, std::memory_order_acq_rel); any_dirty.store(true, std::memory_order_release);
        return protect(page, 1, PROT_READ | PROT_WRITE);
    }
    void set_committed(uint32_t physical, uint32_t length, bool value) {
        const uint64_t end = (uint64_t(physical) + length + 4095) / 4096;
        for (uint64_t p = physical / 4096; p < end && p < page_count; ++p) {
            const uint64_t mask = 1ull << (p % 64);
            if (value) committed[p / 64].fetch_or(mask, std::memory_order_acq_rel);
            else committed[p / 64].fetch_and(~mask, std::memory_order_acq_rel);
        }
    }
    bool range_committed(uint32_t physical, uint32_t size) const {
        if (!size || uint64_t(physical) + size > 0x20000000u) return false;
        const uint64_t end = (uint64_t(physical) + size + 4095) / 4096;
        for (uint64_t p = physical / 4096; p < end; ++p) if (!bit(committed, uint32_t(p))) return false;
        return true;
    }
    // Guest address (any alias) -> physical page candidates.
    template <class F> static void pages_of(uint64_t address, F&& f) {
        if (address >= 0xa0000000ull && address < 0xc0000000ull) f(uint32_t((address - 0xa0000000ull) / 4096));
        if (address >= 0xc0000000ull && address < 0xe0000000ull) f(uint32_t((address - 0xc0000000ull) / 4096));
        if (address >= 0xdffff000ull && address < 0xfffff000ull) f(uint32_t((address - 0xdffff000ull) / 4096));
    }
    // SIGSEGV path: true when the fault was a write to a watched page (now writable).
    bool fault(uintptr_t host) {
        if (!guest || host < uintptr_t(guest)) return false;
        const uint64_t address = host - uintptr_t(guest);
        bool handled = false;
        pages_of(address, [&](uint32_t page) {
            if (page >= page_count || handled) return;
            if (written(page)) { handled = true; return; }
            // Committed guest physical memory is always RW except for our watch, so a
            // fault here is a watch that raced with an unwatch: report it and reopen.
            if (bit(committed, page)) {
                dirty[page / 64].fetch_or(1ull << (page % 64), std::memory_order_acq_rel); any_dirty.store(true, std::memory_order_release);
                handled = protect(page, 1, PROT_READ | PROT_WRITE);
            }
        });
        if (handled) faults.fetch_add(1, std::memory_order_relaxed);
        return handled;
    }
    // A host system call is about to write [address, address+length) of guest memory.
    void host_write(uint32_t address, uint32_t length) {
        if (!length) return;
        const uint64_t end = uint64_t(address) + length;
        for (uint64_t at = address & ~4095ull; at < end; at += 4096)
            pages_of(at, [&](uint32_t page) { if (page < page_count) written(page); });
    }
    // Physical range released: forget watch and dirty state. Returns whether any page had state.
    bool release(uint32_t physical, uint32_t length) {
        bool any = false;
        const uint64_t end = (uint64_t(physical) + length + 4095) / 4096;
        for (uint64_t p = physical / 4096; p < end && p < page_count; ++p) {
            const uint64_t mask = 1ull << (p % 64);
            if (watched[p / 64].fetch_and(~mask, std::memory_order_acq_rel) & mask) any = true;
            if (dirty[p / 64].fetch_and(~mask, std::memory_order_acq_rel) & mask) any = true;
        }
        return any;
    }
    // Takes the dirty set, calling f(first_page, count) for each contiguous run.
    template <class F> void take_dirty(F&& f) {
        if (!any_dirty.exchange(false, std::memory_order_acq_rel)) return;
        uint32_t run = 0, run_first = 0;
        for (uint32_t w = 0; w < words; ++w) {
            uint64_t bits = dirty[w].load(std::memory_order_acquire);
            if (bits) bits = dirty[w].exchange(0, std::memory_order_acq_rel);
            if (!bits) { if (run) { f(run_first, run); run = 0; } continue; }
            for (uint32_t b = 0; b < 64; ++b) {
                const uint32_t p = w * 64 + b;
                if (bits >> b & 1) { if (!run) run_first = p; ++run; }
                else if (run) { f(run_first, run); run = 0; }
            }
        }
        if (run) f(run_first, run);
    }
    size_t watched_pages() const {
        size_t n = 0;
        for (uint32_t w = 0; w < words; ++w) n += __builtin_popcountll(watched[w].load(std::memory_order_relaxed));
        return n;
    }
};
inline MeGpuWriteWatch me_gpu_write_watch;
