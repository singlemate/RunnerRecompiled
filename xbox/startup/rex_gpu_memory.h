#pragma once
#include "gpu_physical_view.h"
#include "gpu_mmio.h"
#include "execution_gate.h"
#include "gpu_write_watch.h"
#include <rex/memory/gpu_memory.h>
#include <stdexcept>
#include <map>
#include <array>
#include <vector>
#include <algorithm>

struct MeGpuScanStats { std::atomic<uint64_t> calls{0}, scans{0}, pages{0}; };
// Non-owning bridge; no Rex Memory instance, heap, or new backing store.
// Callable from the GPU thread without the loader execution gate (see gpu_write_watch.h).
class MeRexGpuMemory final : public rex::memory::GpuMemory {
 public:
    MeRexGpuMemory(const MeGpuPhysicalView& view, MeGpuMmio& mmio) : view_(view), mmio_(mmio) { instances_.push_back(this); }
    ~MeRexGpuMemory() override {
        if (mmio_.context == this) mmio_ = {};
        std::erase(instances_, this);
    }
    MeRexGpuMemory(const MeRexGpuMemory&) = delete;
    MeRexGpuMemory& operator=(const MeRexGpuMemory&) = delete;
    uint8_t* physical_membase() const override { return view_.raw; }
    bool IsPhysicalRangeValid(uint32_t address, uint32_t length) const override {
        return view_.resolve(address & 0x1fffffff, length) != nullptr;
    }
    bool AddVirtualMappedRange(uint32_t address, uint32_t mask, uint32_t size, void* context,
            rex::runtime::MMIOReadCallback read, rex::runtime::MMIOWriteCallback write) override {
        // Only GraphicsSystem's window is supported. Its thunks ignore PPCContext.
        if (address != MeGpuMmio::begin || mask != 0xffff0000 || size != 0xffff ||
            !read || !write || mmio_.read || mmio_.write) return false;
        callback_context_ = context; read_ = read; write_ = write;
        mmio_ = {this,
            [](void* p, uint32_t a, uint32_t& v) {
                auto& self = *static_cast<MeRexGpuMemory*>(p);
                v = self.read_(nullptr, self.callback_context_, a); return true;
            },
            [](void* p, uint32_t a, uint32_t v) {
                auto& self = *static_cast<MeRexGpuMemory*>(p);
                self.write_(nullptr, self.callback_context_, a, v); return true;
            }};
        return true;
    }
    // Write watches use page protection (gpu_write_watch.h): CPU writes to a watched
    // page fault once and mark it dirty; synchronization reports only dirty pages.
    void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback, void* context) override {
        if (!callback) throw std::runtime_error("Null GPU invalidation callback");
        auto entry = std::make_unique<Callback>();
        entry->callback = callback; entry->context = context;
        auto* handle = entry.get(); callbacks_.push_back(std::move(entry)); return handle;
    }
    void UnregisterPhysicalMemoryInvalidationCallback(void* handle) override {
        for (auto& entry : callbacks_) if (entry.get() == handle && entry->active) {
            entry->active = false; return;
        }
        throw std::runtime_error("Unknown GPU invalidation handle");
    }
    void EnablePhysicalMemoryAccessCallbacks(uint32_t address, uint32_t length, bool invalidation, bool providers) override {
        if (providers) throw std::runtime_error("GPU memory data providers are not implemented");
        if (!length || uint64_t(address) + length > 0x20000000)
            throw std::runtime_error("GPU watch range is invalid");
        if (!invalidation) return;
        const uint32_t first = address & ~4095u;
        const uint64_t end = (uint64_t(address) + length + 4095) & ~uint64_t(4095);
        // Validate all pages before changing watch state.
        for (uint64_t page = first; page < end; page += 4096)
            if (!view_.resolve(uint32_t(page), 4096)) throw std::runtime_error("GPU watch covers unallocated memory");
        // Pages already watched or holding an unsynchronized write keep their state,
        // so re-enabling never hides a CPU write.
        me_gpu_write_watch.watch(first / 4096, uint32_t((end - first) / 4096));
    }
    // Called per draw (SharedMemory::RequestRanges) and per primary buffer. Guest
    // threads run concurrently with the command processor, so the dirty set (a 16 KiB
    // bitmap filled by write faults) is checked on every call.
    void SynchronizePhysicalWrites() override {
        scan_stats.calls.fetch_add(1, std::memory_order_relaxed);
        scan_stats.scans.fetch_add(1, std::memory_order_relaxed);
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        me_gpu_write_watch.take_dirty([&](uint32_t first, uint32_t count) { runs.emplace_back(first, count); });
        for (auto [first, count] : runs) {
            scan_stats.pages.fetch_add(count, std::memory_order_relaxed);
            notify(first * 4096u, count * 4096u);
        }
    }
    // Allocation/free notification regardless of watch state (Xenia Canary fork PR #93:
    // pages the GPU marked valid without a watch keep a stale copy after the guest
    // reallocates and fills them -> black textures). Default on; ME_GPU_ALLOC_INVALIDATE=0 disables.
    static void InvalidateAllocatedRange(uint32_t address, uint32_t length) {
        me_gpu_write_watch.release(address, length);
        for (auto* instance : instances_) instance->notify(address, length);
    }
    static void InvalidateReleasedRange(uint32_t address, uint32_t length) {
        if (!me_gpu_write_watch.release(address, length)) return;
        for (auto* instance : instances_) instance->notify(address, length);
    }
    size_t watched_pages() const { return me_gpu_write_watch.watched_pages(); }

    static inline MeGpuScanStats scan_stats;
 private:
    struct Callback { PhysicalMemoryInvalidationCallback callback; void* context; bool active = true; };
    std::vector<std::unique_ptr<Callback>> callbacks_;
    static inline std::vector<MeRexGpuMemory*> instances_;
    void notify(uint32_t address, uint32_t length) {
        // Snapshot stable handles so reentrant unregister/register cannot invalidate iteration.
        std::vector<Callback*> callbacks;
        for (auto& entry : callbacks_) if (entry->active) callbacks.push_back(entry.get());
        for (auto* entry : callbacks) if (entry->active)
            entry->callback(entry->context, address, length, false);
        // Return ranges may allow extra unwatches; conservatively retain all others.
    }
    const MeGpuPhysicalView& view_;
    MeGpuMmio& mmio_;
    void* callback_context_ = nullptr;
    rex::runtime::MMIOReadCallback read_ = nullptr;
    rex::runtime::MMIOWriteCallback write_ = nullptr;
};
