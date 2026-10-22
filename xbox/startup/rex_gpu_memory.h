#pragma once
#include "gpu_physical_view.h"
#include "gpu_mmio.h"
#include <rex/memory/gpu_memory.h>
#include <stdexcept>
#include <map>
#include <array>
#include <vector>
#include <algorithm>

// Non-owning bridge; no Rex Memory instance, heap, or new backing store.
// All access must hold the loader execution gate and retain allocations.
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
    // Software watches: compare exact bytes at GPU consumption boundaries.
    // All CPU producers and GPU consumers MUST share execution_gate.
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
        for (uint64_t page = first; page < end; page += 4096) {
            // Do not refresh a pre-existing watch and hide an unobserved CPU write.
            if (pages_.contains(uint32_t(page))) continue;
            auto& snapshot = pages_[uint32_t(page)];
            std::memcpy(snapshot.data(), view_.resolve(uint32_t(page), 4096), 4096);
        }
    }
    void SynchronizePhysicalWrites() override {
        std::vector<uint32_t> changed;
        for (const auto& [page, snapshot] : pages_) {
            auto* bytes = view_.resolve(page, 4096);
            if (!bytes || std::memcmp(bytes, snapshot.data(), 4096)) changed.push_back(page);
        }
        for (auto page : changed) invalidate(page, 4096);
    }
    static void InvalidateReleasedRange(uint32_t address, uint32_t length) {
        for (auto* instance : instances_) instance->invalidate(address, length);
    }
    size_t watched_pages() const { return pages_.size(); }
 private:
    struct Callback { PhysicalMemoryInvalidationCallback callback; void* context; bool active = true; };
    std::vector<std::unique_ptr<Callback>> callbacks_;
    std::map<uint32_t, std::array<uint8_t, 4096>> pages_;
    static inline std::vector<MeRexGpuMemory*> instances_;
    void invalidate(uint32_t address, uint32_t length) {
        bool watched = false;
        for (auto it = pages_.begin(); it != pages_.end();) {
            if (uint64_t(it->first) < uint64_t(address) + length && uint64_t(it->first) + 4096 > address) {
                it = pages_.erase(it); watched = true;
            } else ++it;
        }
        if (!watched) return;
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
