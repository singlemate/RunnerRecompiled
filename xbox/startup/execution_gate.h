#pragma once
#include <mutex>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
// Counts gate handovers. Guest threads only run while holding the gate, so memory
// written by another guest thread becomes observable only after some unlock; GPU
// write tracking uses this to skip rescans while the command processor keeps the gate.
inline std::atomic<uint64_t> me_gate_epoch{0};
// Diagnostics: per-thread acquisitions, time queued for the gate and time holding it.
struct MeGateThreadStats { std::atomic<uint32_t> label{0}; std::atomic<uint64_t> acquisitions{0}, wait_ns{0}, hold_ns{0}; };
inline MeGateThreadStats me_gate_stats[64];
inline uint32_t (*me_gate_label)() = nullptr; // set by the loader: guest thread address
inline std::atomic<uint32_t> me_gate_slots{0};
inline uint64_t me_gate_now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// FIFO serialization for cooperative guest execution. This does not preempt a
// running guest function. Every unlock hands the gate directly to the oldest
// waiter and wakes only that thread (no herd wake-up). When a thread acquires the
// gate, its successor is woken to spin briefly, so the next handover usually
// completes without a sleep/wake round trip through the scheduler.
class MeExecutionGate {
    enum : uint32_t { kSleep = 0, kSpin = 2, kGranted = 1 };
    struct Waiter { std::atomic<uint32_t> state{kSleep}; Waiter* next = nullptr; };
    std::mutex mutex_;
    Waiter* head_ = nullptr; Waiter* tail_ = nullptr;
    bool held_ = false; uint64_t waiting_ = 0;
    static void futex_wait(std::atomic<uint32_t>& word, uint32_t expected) {
        syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
    }
    static void futex_wake(std::atomic<uint32_t>& word) {
        syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }
    static MeGateThreadStats* stats() {
        static thread_local MeGateThreadStats* slot = nullptr;
        static thread_local bool tried = false;
        if (!tried) { tried = true; const auto i = me_gate_slots.fetch_add(1); if (i < 64) slot = &me_gate_stats[i]; }
        if (slot && me_gate_label) slot->label.store(me_gate_label(), std::memory_order_relaxed);
        return slot;
    }
    static inline thread_local uint64_t acquired_ns_ = 0;
    std::atomic<int> owner_{0}; // host tid of the holder (per gate instance)
    static int self_tid() { static thread_local const int tid = int(syscall(SYS_gettid)); return tid; }
    // Caller holds mutex_: let the next waiter spin while the new owner runs.
    void warm_successor() {
        if (!head_) return;
        uint32_t expected = kSleep;
        if (head_->state.compare_exchange_strong(expected, kSpin, std::memory_order_acq_rel)) futex_wake(head_->state);
    }
    // Barging mode (parallel guest execution, where the gate is a short kernel lock):
    // a plain futex mutex (0 free, 1 held, 2 held with sleepers) with a brief spin.
    // Strict FIFO handoff would force a context switch on every contended import.
    bool barging_ = false;
    std::atomic<uint32_t> word_{0};
    void lock_barging() {
        uint32_t c = 0;
        if (word_.compare_exchange_strong(c, 1, std::memory_order_acquire)) return;
        for (int i = 0; i < 256; ++i) {
            c = 0;
            if (word_.load(std::memory_order_relaxed) == 0 && word_.compare_exchange_weak(c, 1, std::memory_order_acquire)) return;
            asm volatile("yield" ::: "memory");
        }
        c = word_.exchange(2, std::memory_order_acquire);
        while (c != 0) { futex_wait(word_, 2); c = word_.exchange(2, std::memory_order_acquire); }
    }
    void unlock_barging() {
        if (word_.fetch_sub(1, std::memory_order_release) != 1) { word_.store(0, std::memory_order_release); futex_wake(word_); }
    }
public:
    // Switch to barging mode; only before any thread uses this gate.
    void set_barging(bool value) { barging_ = value; }
    // True while the calling thread holds the gate.
    bool owned() const { return owner_.load(std::memory_order_acquire) == self_tid(); }
    uint64_t queued_waiters() {
        if (barging_) return word_.load(std::memory_order_relaxed) == 2 ? 1 : 0;
        std::lock_guard<std::mutex> lock(mutex_); return waiting_;
    }
    void lock() {
        const auto begin = me_gate_now_ns();
        if (barging_) { lock_barging(); goto acquired; }
        static thread_local Waiter self;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!held_) { held_ = true; warm_successor(); goto acquired; }
            self.state.store(kSleep, std::memory_order_relaxed); self.next = nullptr;
            if (tail_) tail_->next = &self; else head_ = &self;
            tail_ = &self; ++waiting_;
        }
        for (;;) {
            auto state = self.state.load(std::memory_order_acquire);
            if (state == kGranted) break;
            if (state == kSpin) {
                // Bounded spin (~100 us), then sleep until granted or warmed again.
                const auto until = me_gate_now_ns() + 100000;
                while (self.state.load(std::memory_order_acquire) == kSpin && me_gate_now_ns() < until)
                    asm volatile("yield" ::: "memory");
                uint32_t expected = kSpin;
                self.state.compare_exchange_strong(expected, kSleep, std::memory_order_acq_rel);
                continue;
            }
            futex_wait(self.state, kSleep);
        }
        { std::lock_guard<std::mutex> lock(mutex_); warm_successor(); }
    acquired:
        owner_.store(self_tid(), std::memory_order_release);
        acquired_ns_ = me_gate_now_ns();
        if (auto* s = stats()) { s->acquisitions.fetch_add(1, std::memory_order_relaxed); s->wait_ns.fetch_add(acquired_ns_ - begin, std::memory_order_relaxed); }
    }
    void unlock() {
        if (acquired_ns_) { if (auto* s = stats()) s->hold_ns.fetch_add(me_gate_now_ns() - acquired_ns_, std::memory_order_relaxed); acquired_ns_ = 0; }
        owner_.store(0, std::memory_order_release);
        me_gate_epoch.fetch_add(1, std::memory_order_acq_rel);
        if (barging_) { unlock_barging(); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!head_) { held_ = false; return; }
        Waiter* next = head_;
        head_ = next->next; if (!head_) tail_ = nullptr;
        --waiting_;
        // Direct handoff: the gate stays held and passes to the oldest waiter.
        const auto previous = next->state.exchange(kGranted, std::memory_order_acq_rel);
        if (previous == kSleep) futex_wake(next->state);
    }
};
