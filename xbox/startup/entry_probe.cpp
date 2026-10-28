#include "../audio/context_merge.h"
// Incremental ARM64 startup runtime; unsupported Xbox services stop with diagnostics.
#include "guest_memory_access.h"
#include "ppc_recomp_shared.h"
#include "xex_headers.h"
#include "virtual_memory.h"
#include "events.h"
#include "execution_gate.h"
#include "mutants.h"
#include "notifications.h"
#include "offline_network.h"
#include "time_fields.h"
#include "xma_contexts.h"
#include "physical_memory.h"
#include "gpu_physical_view.h"
#include "rex_gpu_memory.h"
#include "gpu_interrupt_worker.h"
#include "gpu_vblank_worker.h"
#include "gpu_bridge.h"
#include "audio_output.h"
#include "guest_input.h"
#include "achievements.h"
#include "readonly_files.h"
#include "pcm_frame.h"
#include <limits>
#include <source_location>
#include <cmath>
#include <rex/graphics/host_worker.h>
#include <rex/graphics/guest_execution_guard.h>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ucontext.h>
#include <dlfcn.h>
#include <atomic>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <thread>
#include <mutex>
#include <cctype>
#include <ctime>
#include <fstream>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>

struct Guard { uint32_t iat, address, ordinal; char name[96]; };
static Guard guards[128];
static unsigned guard_count;
static uint8_t* guest;
static thread_local PPCContext* active_context;
static std::atomic<int> diagnostic_tids[64];
static_assert(std::atomic<int>::is_always_lock_free);
static void register_diagnostic_thread() {
    struct Registration {
        int slot=-1;
        Registration(){const int tid=int(syscall(SYS_gettid));for(int i=0;i<64;++i){int empty=0;if(diagnostic_tids[i].compare_exchange_strong(empty,tid)){slot=i;break;}}}
        ~Registration(){if(slot>=0)diagnostic_tids[slot].store(0);}
    };
    static thread_local Registration registration;
}
static size_t page_size;
static MeXexHeaders module_headers;
static MeVirtualMemory virtual_memory;
struct ImageRange { uint32_t address, size; };
static ImageRange image_ranges[32];
static unsigned image_range_count;
static thread_local uint32_t thread_address = 0x61001000, pcr_address = 0x61000000;
static thread_local uint32_t tls_dynamic;
static uint32_t tls_slots;
static bool tls_used[256];
static MeEvents events;
static MeMutants mutants;
static MeNotifications notifications;
static MeOfflineNetwork offline_network;
static MePhysicalMemory physical_memory;
// Per-call events (waits, virtual alloc/free, file reads, swaps...) are ~3000 lines/s
// in a level. stdout is unbuffered, so each is a write syscall - on /sdcard (FUSE) in the
// APK, often made while holding the kernel lock. Printed only with ME_LOG_VERBOSE=1.
static bool me_log_verbose() {
    static const bool verbose = [] { const char* v = std::getenv("ME_LOG_VERBOSE"); return v && !std::strcmp(v, "1"); }();
    return verbose;
}
static MeReadOnlyFiles readonly_files("/data/local/tmp/me_game_readonly");
static int physical_fd = -1;
static MeGpuPhysicalView gpu_physical;
static MeGpuMmio gpu_mmio;
static bool gpu_explore_mode = false;
static uint32_t gpu_identifier_address = 0;
static uint32_t timestamp_bundle_address=0;
static uint64_t timestamp_origin_ns=0;
static MeExecutionGate execution_gate;
// Parallel guest execution: guest code runs on its own host thread without the gate,
// as on the console's six hardware threads. The gate is a big kernel lock: every
// kernel/XAM import, MMIO handler and loader hook that touches loader state holds it
// (MeKernelLock; nested calls reuse the caller's hold), and blocking waits release it.
// ME_GUEST_SERIAL=1 keeps the former fully serialized mode for A/B comparison.
static bool guest_parallel = true;
struct MeKernelLock {
    bool took = false;
    MeKernelLock() { if (!execution_gate.owned()) { execution_gate.lock(); took = true; } }
    ~MeKernelLock() { if (took && execution_gate.owned()) execution_gate.unlock(); }
    MeKernelLock(const MeKernelLock&) = delete;
    MeKernelLock& operator=(const MeKernelLock&) = delete;
};
// Per-import call counters (diagnostics: which imports load the kernel lock).
struct MeImportCounter {
    const char* name; std::atomic<uint64_t> calls{0}; MeImportCounter* next;
    static inline std::atomic<MeImportCounter*> head{nullptr};
    explicit MeImportCounter(const char* n) : name(n), next(head.load()) { while (!head.compare_exchange_weak(next, this)) {} }
};
#define ME_IMPORT(name) \
    static void name##_locked(PPCContext& __restrict ctx, uint8_t* base); \
    static MeImportCounter name##_counter(#name); \
    PPC_FUNC(name) { name##_counter.calls.fetch_add(1, std::memory_order_relaxed); MeKernelLock kernel_lock; name##_locked(ctx, base); } \
    static void name##_locked(PPCContext& __restrict ctx, uint8_t* base)
// Sleeps without holding the gate (releases and reacquires it if held).
template <class D> static void me_diagnostic_sleep(D duration) {
    const bool held = execution_gate.owned();
    if (held) execution_gate.unlock();
    std::this_thread::sleep_for(duration);
    if (held) execution_gate.lock();
}
static unsigned gpu_mmio_writes = 0;
static uint32_t gpu_last_write_index = 0, gpu_read_pointer_physical = 0;
static void gpu_publication_window();
uint64_t me_gpu_mmio_load(uint64_t address, unsigned size) {
    static MeImportCounter counter("hook_gpu_mmio_load"); counter.calls.fetch_add(1, std::memory_order_relaxed);
    MeKernelLock kernel_lock;
    uint64_t value = 0;
    if (gpu_mmio.load(address, size, value)) return value;
    std::fprintf(stderr, "{\"event\":\"gpu_mmio_unavailable\",\"write\":false,\"address\":\"%llx\",\"size\":%u}\n", (unsigned long long)address, size);
    std::_Exit(95);
}
void me_gpu_mmio_store(uint64_t address, unsigned size, uint64_t value) {
    static MeImportCounter counter("hook_gpu_mmio_store"); counter.calls.fetch_add(1, std::memory_order_relaxed);
    MeKernelLock kernel_lock;
    if (gpu_mmio.store(address, size, value)) {
        if (address == 0x7fc80714 && size == 4) gpu_last_write_index = uint32_t(value);
        if (gpu_explore_mode && (++gpu_mmio_writes <= 24 || !(gpu_mmio_writes & (gpu_mmio_writes - 1))))
            std::printf("{\"event\":\"game_mmio_write\",\"address\":\"%08llx\",\"value\":\"%08llx\"}\n",(unsigned long long)address,(unsigned long long)value);
        if (address == 0x7fc80714 && size == 4) gpu_publication_window();
        return;
    }
    std::fprintf(stderr, "{\"event\":\"gpu_mmio_unavailable\",\"write\":true,\"address\":\"%llx\",\"size\":%u}\n", (unsigned long long)address, size);
    std::_Exit(95);
}
// Guest code runs under one gate and yields it at blocking kernel calls.
// This provides kernel-boundary scheduling, not preemption of guest spin loops.
static bool polling_yield_experiment=false;
static bool pool_scan_experiment=false;
bool me_pool_scan_enabled(){
    static unsigned uses=0;
    if(pool_scan_experiment && gpu_explore_mode && ++uses<=8 && active_context)
        std::printf("{\"event\":\"pool_scan_compatibility_step\",\"thread\":\"%08x\",\"from_index\":%u,\"next_index\":%d}\n",thread_address,active_context->r30.u32,active_context->r11.s32);
    return pool_scan_experiment;
}
static uint32_t pool_test_selected_worker=0,pool_test_selected_job=0;
static void pool_test_assign(PPCContext& ctx,uint8_t*){pool_test_selected_worker=ctx.r3.u32;pool_test_selected_job=ctx.r4.u32;}
static sig_atomic_t diagnostic_timeout_rounds=4;
static decltype(&sub_8238E850) original_stream_poll=nullptr;
static void diagnostic_stream_poll(PPCContext& ctx,uint8_t* base) {
    // Experiment: preserve the original call and result; allow queued workers
    // to run when the observed nonblocking poll returns not-ready.
    original_stream_poll(ctx,base);
    static thread_local uint32_t polls=0;
    if(!guest_parallel && ctx.r3.u32==0 && ++polls%256==0){
        if(polls==256)std::puts("{\"event\":\"stream_poll_scheduling_window\"}");
        std::unique_lock<MeExecutionGate> gate(execution_gate,std::adopt_lock);
        gate.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(1));gate.lock();gate.release();
    }
}
static void trace_worker_poll(uint32_t object,uint32_t state,uint32_t index,uint32_t cs);
static decltype(&sub_826CCC08) original_worker_poll=nullptr;
static void diagnostic_worker_poll(PPCContext& ctx,uint8_t* base) {
    const auto object=ctx.r3.u32;
    original_worker_poll(ctx,base);
    trace_worker_poll(object,ctx.r3.u32,ctx.r30.u32,ctx.r27.u32);
    static thread_local uint32_t polls=0;
    if(!guest_parallel && ctx.r3.u32!=0 && ++polls%16==0){
        if(polls==16)std::puts("{\"event\":\"worker_poll_scheduling_window\"}");
        std::unique_lock<MeExecutionGate> gate(execution_gate,std::adopt_lock);
        gate.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(1));gate.lock();gate.release();
    }
}
double me_frsqrte_diagnostic(double value,uint32_t instruction) {
    if(!polling_yield_experiment || !std::isfinite(value) || value<=0){
        std::fprintf(stderr,"{\"event\":\"frsqrte_mode_or_input_unsupported\",\"instruction\":\"%08x\"}\n",instruction);std::_Exit(97);
    }
    // Same seed formula as pinned RexGlue builder, not Xbox bit-exact estimate.
    const double result=double(float(1.0/std::sqrt(value)));
    if(!std::isfinite(result) || result<=0)std::_Exit(97);
    static thread_local unsigned calls[3]={};
    const unsigned site=instruction==0x82384344?2:instruction==0x82ebc040?1:0;
    if(++calls[site]<=2)std::printf("{\"event\":\"frsqrte_diagnostic_seed\",\"instruction\":\"%08x\",\"input\":%.17g,\"seed\":%.17g,\"xbox_bit_exact\":false}\n",instruction,value,result);
    return result;
}
static bool game_gpu_ready = false;
// Adaptive backoff for guest spin-waits (heat/battery). On the console a thread
// spinning on Sleep(0) or on a memory flag costs nothing; here it holds a big core at
// 100 %. ME_SPIN_BACKOFF_US (default 100, 0 = off) is slept instead of a yield once a
// thread has made 32 consecutive zero-length delays (no gap > 2 ms between them), and
// after 256 consecutive reads of the same physical address returning the same value.
static int64_t me_spin_backoff_us() {
    static const int64_t us = [] { const char* v = std::getenv("ME_SPIN_BACKOFF_US"); return v ? std::max<int64_t>(0, std::min<int64_t>(2000, std::atoll(v))) : 100; }();
    return us;
}
static void me_zero_delay_yield() {
    static thread_local uint32_t streak = 0; static thread_local int64_t last_ns = 0;
    const int64_t now = int64_t(me_gate_now_ns());
    if (now - last_ns > 2000000) streak = 0;
    last_ns = now;
    const int64_t us = me_spin_backoff_us();
    if (us && ++streak > 32) std::this_thread::sleep_for(std::chrono::microseconds(us));
    else std::this_thread::yield();
}
void me_guest_poll_window(uint64_t address) {
    {   // spin detection on the same physical word (see me_spin_backoff_us)
        static thread_local uint64_t last_address = 0; static thread_local uint32_t last_value = 0, same = 0;
        uint32_t value; std::memcpy(&value, guest + (address & ~3ull), 4);
        if (address == last_address && value == last_value) {
            if (++same >= 256) { same = 0; if (const int64_t us = me_spin_backoff_us()) me_diagnostic_sleep(std::chrono::microseconds(us)); }
        } else { last_address = address; last_value = value; same = 0; }
    }
    if(timestamp_bundle_address && address>=timestamp_bundle_address && address<uint64_t(timestamp_bundle_address)+24){
        timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
        const uint64_t ns=uint64_t(now.tv_sec)*1000000000ull+uint64_t(now.tv_nsec);
        const uint32_t value=__builtin_bswap32(uint32_t((ns-timestamp_origin_ns)/1000000));
        std::memcpy(guest+timestamp_bundle_address+16,&value,4);
        return;
    }
    if (!gpu_explore_mode || !game_gpu_ready || !gpu_identifier_address) return;
    const auto physical = MePhysicalMemory::physical(uint32_t(address));
    const auto identifier = MePhysicalMemory::physical(gpu_identifier_address);
    if ((physical & ~31u) != (identifier & ~31u)) return;
    static thread_local uint32_t reads = 0;
    if (++reads % 1024) return;
    const auto sample=reads/1024;
    if(sample<=4 || !(sample&(sample-1))){
        // Observe both the CPU writeback line and its preceding allocation:
        // current PM4 WAIT_REG_MEM packets poll that neighbouring physical page.
        const uint32_t line=identifier&~31u;
        for(unsigned previous=0;previous<2;++previous){
            if(previous && line<4096)continue;
            const uint32_t base=line-previous*4096;auto* p=gpu_physical.resolve(base,32);if(!p)continue;
            std::printf("{\"event\":\"gpu_cpu_poll_memory\",\"sample\":%u,\"physical\":\"%08x\",\"words\":[\"%08x\",\"%08x\",\"%08x\",\"%08x\",\"%08x\",\"%08x\"],\"ring_wptr\":%u,\"guest_lr\":\"%08llx\"}\n",sample,base,MeXexHeaders::be32(p),MeXexHeaders::be32(p+4),MeXexHeaders::be32(p+8),MeXexHeaders::be32(p+12),MeXexHeaders::be32(p+16),MeXexHeaders::be32(p+20),gpu_last_write_index,(unsigned long long)(active_context?active_context->lr:0));
        }
    }
    // Diagnostic toggle (bisecting a stall): ME_POLL_SLEEP=1 restores the former
    // 1 ms window, releasing the gate only if this thread holds it.
    static const bool poll_sleep = std::getenv("ME_POLL_SLEEP") != nullptr;
    if (poll_sleep) me_diagnostic_sleep(std::chrono::milliseconds(1));
}
static void gpu_publication_window() {
    if (!gpu_explore_mode || !game_gpu_ready) return;
    // Former 10 ms sleep per ring write-pointer update removed (it ran on the
    // submitting guest thread for every kick). Snapshot logging is throttled.
    static const bool publication_sleep = std::getenv("ME_PUBLICATION_SLEEP") != nullptr;
    if (publication_sleep) me_diagnostic_sleep(std::chrono::milliseconds(10));
    static unsigned publications = 0;
    if (++publications > 16 && (publications & (publications - 1))) return;
    if (gpu_read_pointer_physical) {
        auto* pointer = gpu_physical.resolve(gpu_read_pointer_physical, 4);
        if (pointer) {
            uint32_t value; std::memcpy(&value, pointer, 4);
            std::printf("{\"event\":\"gpu_publication_snapshot\",\"write_index\":%u,\"read_index\":%u}\n", gpu_last_write_index, __builtin_bswap32(value));
        }
    }
}
static MeGpuInterruptWorker* game_gpu_irq_worker;
static void start_game_gpu(PPCContext&);
static std::condition_variable_any state_changed;
struct ThreadRecord { uint32_t id, handle, pcr, kthread, tls, low, high; bool done = false, closed = false; int32_t priority = 0; uint32_t exit_code = 0, references = 0, affinity = 1; uint8_t suspend_count = 0; };
static constexpr unsigned max_guest_threads=64;
static_assert(0x61000000ull+(max_guest_threads-1)*0x10000+0x2000<=0x62000000ull);
static_assert(0x62000000ull+(max_guest_threads-1)*0x200000+0x104000<=0x70000000ull);
static_assert(0x71001000ull+(max_guest_threads-1)*0x200000+0x100000<=0x7fc80000ull);
static std::vector<std::shared_ptr<ThreadRecord>> threads;
struct ThreadExit { uint32_t code; };
static void initialize_thread(PPCContext&, unsigned, uint32_t);
static void create_guest_thread(PPCContext&);
static void register_audio_client(PPCContext&);
static bool audio_output_is_ready();
ME_IMPORT(__imp__XAudioRegisterRenderDriverClient) { register_audio_client(ctx); }
static std::shared_ptr<ThreadRecord> find_thread(uint32_t handle) {
    if (handle == 0xfffffffe) for (auto& t : threads) if (t->kthread == thread_address) return t;
    for (auto& t : threads) if (t->handle == handle && !t->closed) return t;
    return {};
}
static void wait_for_change(const std::function<bool()>& ready) {
    std::unique_lock<MeExecutionGate> lock(execution_gate, std::adopt_lock);
    state_changed.wait(lock, ready);
    lock.release();
}
[[noreturn]] void me_stop_import(const char* name, PPCContext& ctx);

// This loader runs one user-title process; no system/DPC execution is supported.
static constexpr uint32_t process_type = 1; // X_PROCTYPE_USER, kernel_state.h
ME_IMPORT(__imp__KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }
ME_IMPORT(__imp__FscSetCacheElementCount) {
    // No file-cache implementation is installed yet. Exercise the caller's
    // real NTSTATUS failure path rather than claiming the requested cache exists.
    std::printf("{\"event\":\"file_cache_unavailable\",\"selector\":%u,\"requested_elements\":%u,\"status\":\"c0000002\"}\n", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0xc0000002; // STATUS_NOT_IMPLEMENTED
}
static bool word_writable(uint32_t);
static void put_word(uint32_t, uint32_t);
static bool image_span_mapped(uint32_t, uint32_t);
// XConfig setting 9 (console language), Xbox codes: 1 English, 3 German, 4 French,
// 5 Spanish, 6 Italian, 12 Russian (the languages this game ships). ME_LANGUAGE picks
// one; anything else keeps English. Read once: the game queries it from several threads.
static uint32_t guest_console_language() {
    static const uint32_t language = [] {
        const char* v = std::getenv("ME_LANGUAGE");
        const unsigned code = v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 1u;
        return (code == 1 || code == 3 || code == 4 || code == 5 || code == 6 || code == 12) ? code : 1u;
    }();
    return language;
}
ME_IMPORT(__imp__XexGetModuleHandle) {
    if (ctx.r3.u32) me_stop_import("named_module_lookup_not_implemented", ctx);
    if (!word_writable(ctx.r4.u32)) me_stop_import("module_handle_output_invalid", ctx);
    put_word(ctx.r4.u32, 0x60001000); ctx.r3.u64 = 0;
}

ME_IMPORT(__imp__XamGetSystemVersion) {
    if(!gpu_explore_mode)me_stop_import("xam_version_requires_legacy_compat_profile",ctx);
    std::puts("{\"event\":\"xam_version_compatibility\",\"value\":0,\"actual_console_version\":false}");
    ctx.r3.u64=0;
}

ME_IMPORT(__imp__KeEnableFpuExceptions) {
    if (ctx.r3.u32) me_stop_import("guest_fpu_trap_delivery_not_implemented", ctx);
    // ARM FPCR IOE/DZE/OFE/UFE/IXE/IDE: mask traps, preserve rounding/FZ.
    ctx.fpscr.csr = ctx.fpscr.getcsr() & ~uint32_t(0x9f00);
    ctx.fpscr.setcsr(ctx.fpscr.csr);
}
ME_IMPORT(__imp__RtlNtStatusToDosError) {
    const auto status = ctx.r3.u32;
    if (!status || (status & 0x20000000)) return;
    if (status == 0xc000000e || status == 0xc000000f) { ctx.r3.u64 = 2; return; } // ERROR_FILE_NOT_FOUND, pinned SDK error table.
    if (status == 0xc0000008) { ctx.r3.u64 = 6; return; } // SDK error table: ERROR_INVALID_HANDLE.
    if (status == 0xc0000035) { ctx.r3.u64 = 183; return; } // SDK error table: ERROR_ALREADY_EXISTS.
    if (status == 0xc0000002) { ctx.r3.u64 = 1; return; } // Xbox ERROR_INVALID_FUNCTION
    me_stop_import("RtlNtStatusToDosError_unmapped_status", ctx);
}
ME_IMPORT(__imp__ExCreateThread) { create_guest_thread(ctx); }
ME_IMPORT(__imp__ExTerminateThread) {
    if (thread_address == 0x61001000) me_stop_import("main_thread_exit_requires_process_lifetime_model", ctx);
    throw ThreadExit{ctx.r3.u32};
}
ME_IMPORT(__imp__KeSetBasePriorityThread) {
    std::shared_ptr<ThreadRecord> target;
    for (const auto& record : threads) if (record->kthread == ctx.r3.u32) target = record;
    if (!target) me_stop_import("set_priority_unknown_thread", ctx);
    const int32_t previous = target->priority, requested = ctx.r4.s32;
    target->priority = requested;
    guest[target->kthread + 0x70] = uint8_t(std::clamp(requested, 0, 31));
    ctx.r3.u64 = uint32_t(previous);
    std::printf("{\"event\":\"guest_priority_hint\",\"thread\":\"%08x\",\"requested\":%d,\"previous\":%d,\"host_priority_changed\":false}\n",target->kthread,requested,previous);
}
ME_IMPORT(__imp__KeGetCurrentProcessType) {
    if (ctx.r13.u32 != pcr_address || guest[pcr_address + 0x150])
        me_stop_import("KeGetCurrentProcessType_unsupported_thread_or_dpc", ctx);
    ctx.r3.u64 = guest[thread_address + 0x73];
    static thread_local uint64_t calls=0;
    if(++calls<=8 || !(calls&(calls-1)))std::printf("{\"event\":\"process_type\",\"value\":%u,\"call\":%llu}\n",process_type,(unsigned long long)calls);
}

ME_IMPORT(__imp__RtlImageXexHeaderField) {
    if (ctx.r3.u32 != module_headers.guest_base || !module_headers.valid())
        me_stop_import("RtlImageXexHeaderField_invalid_module_header", ctx);
    const uint32_t key = ctx.r4.u32;
    uint32_t value = 0;
    const bool found = module_headers.lookup(key, value);
    std::printf("{\"event\":\"xex_header_query\",\"key\":\"%08x\",\"found\":%s,\"result\":\"%08x\"}\n",
        key, found ? "true" : "false", value);
    ctx.r3.u64 = value;
}
ME_IMPORT(__imp__XexCheckExecutablePrivilege) {
    const uint32_t bit = ctx.r3.u32;
    if (bit >= 32) me_stop_import("XexCheckExecutablePrivilege_invalid_bit", ctx);
    uint32_t flags = 0;
    module_headers.lookup(0x30000, flags);
    ctx.r3.u64 = (flags >> bit) & 1;
    std::printf("{\"event\":\"executable_privilege\",\"bit\":%u,\"enabled\":%u}\n", bit, ctx.r3.u32);
}

// End (exclusive) of the contiguous interval of words accepted by word_writable that
// contains this aligned address, or 0 if the word is not writable.
static uint64_t writable_extent(uint32_t address) {
    if (address & 3) return 0;
    if (address == 0x60003000 || address == 0x60003020) return uint64_t(address) + 4;
    if (address >= 0x60003080 && address <= 0x60003080 + 24) return 0x60003080 + 28;
    if (address >= 0x70000000 && address <= 0x70100000 - 4) return 0x70100000;
    for (unsigned i = 0; i < image_range_count; ++i)
        if (address >= image_ranges[i].address && uint64_t(address) + 4 <= uint64_t(image_ranges[i].address) + image_ranges[i].size)
            return (uint64_t(image_ranges[i].address) + image_ranges[i].size) & ~uint64_t(3);
    if (const auto* a = physical_memory.find(address))
        return uint64_t(address) + (uint64_t(a->physical) + a->size - MePhysicalMemory::physical(address));
    for (const auto& t : threads) {
        if (address >= t->low && uint64_t(address) + 4 <= t->high) return t->high;
        if (address >= t->pcr && uint64_t(address) + 4 <= uint64_t(t->pcr) + 0x2000) return uint64_t(t->pcr) + 0x2000;
        if (address >= t->tls && uint64_t(address) + 4 <= uint64_t(t->tls) + tls_slots * 4) return uint64_t(t->tls) + tls_slots * 4;
    }
    for (const auto& r : virtual_memory.regions)
        if (address >= r.base && uint64_t(address) + 4 <= uint64_t(r.base) + r.size) {
            const uint64_t page = (address - r.base) / r.page;
            return r.committed[page] ? std::min(uint64_t(r.base) + (page + 1) * r.page, uint64_t(r.base) + r.size) : 0;
        }
    return 0;
}
static bool word_writable(uint32_t address) {
    if (address & 3) return false;
    if (address == 0x60003000 || address == 0x60003020) return true;
    if (address >= 0x60003080 && address <= 0x60003080 + 24) return true;
    if (address >= 0x70000000 && address <= 0x70100000 - 4) return true;
    for (unsigned i = 0; i < image_range_count; ++i)
        if (address >= image_ranges[i].address && uint64_t(address) + 4 <=
            uint64_t(image_ranges[i].address) + image_ranges[i].size) return true;
    if (physical_memory.find(address)) return true;
    for (const auto& t : threads) {
        if (address >= t->low && uint64_t(address) + 4 <= t->high) return true;
        if (address >= t->pcr && uint64_t(address) + 4 <= uint64_t(t->pcr) + 0x2000) return true;
        if (address >= t->tls && uint64_t(address) + 4 <= uint64_t(t->tls) + tls_slots * 4) return true;
    }
    for (const auto& r : virtual_memory.regions)
        if (address >= r.base && uint64_t(address) + 4 <= uint64_t(r.base) + r.size)
            return r.committed[(address - r.base) / r.page] != 0;
    return false;
}
static uint32_t load_word(uint32_t address) { return MeXexHeaders::be32(guest + address); }
static void put_word(uint32_t address, uint32_t value) {
    value = __builtin_bswap32(value); std::memcpy(guest + address, &value, 4);
}
static std::shared_ptr<ThreadRecord> thread_by_pointer(uint32_t pointer) {
    for (auto& t : threads) if (t->kthread == pointer) return t;
    return {};
}
ME_IMPORT(__imp__ObReferenceObjectByHandle) {
    auto t = find_thread(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = 0xc0000008; return; }
    const auto type = ctx.r4.u32, output = ctx.r5.u32;
    if (type) {
        bool valid_type = false;
        for (unsigned i = 0; i < guard_count; ++i)
            if (!std::strcmp(guards[i].name, "ExThreadObjectType") && type == guards[i].address) valid_type = true;
        if (!valid_type) { ctx.r3.u64 = 0xc0000024; return; }
    }
    if (output && !word_writable(output)) me_stop_import("ObReferenceObjectByHandle_invalid_output", ctx);
    ++t->references; if (output) put_word(output, t->kthread);
    ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__ObDereferenceObject) {
    auto t = thread_by_pointer(ctx.r3.u32);
    if (!t || !t->references) me_stop_import("ObDereferenceObject_invalid_reference", ctx);
    --t->references; ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__KeSetAffinityThread) {
    auto t = thread_by_pointer(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = 0xc0000008; return; }
    const auto mask = ctx.r4.u32, output = ctx.r5.u32;
    if (!mask || (mask & ~63u)) { ctx.r3.u64 = 0xc000000d; return; }
    if (output && !word_writable(output)) me_stop_import("KeSetAffinityThread_invalid_output", ctx);
    if (output) put_word(output, t->affinity);
    t->affinity = mask;
    const auto cpu = __builtin_ctz(mask);
    guest[t->pcr + 0x10c] = guest[t->kthread + 0xbf] = cpu;
    put_word(t->pcr + 0x110, 1u << cpu);
    ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__XamLoaderGetLaunchDataSize) {
    // Standalone cold launch: no title-to-title launch payload was supplied.
    if (!ctx.r3.u32) { ctx.r3.u64 = 87; return; }
    if (!word_writable(ctx.r3.u32)) me_stop_import("launch_data_size_invalid_pointer", ctx);
    put_word(ctx.r3.u32, 0); ctx.r3.u64 = 1168; // ERROR_NOT_FOUND
}
static bool checked_span(uint32_t address, uint32_t size, bool reading = false) {
    if (size > 0x100000 || uint64_t(address) + size > 0x100000000ull) return false;
    if (reading && address >= 0x60002000 && uint64_t(address) + size <= 0x60002400) return true;
    if (reading && address >= 0x60003100 && uint64_t(address) + size <= 0x60003108) return true;
    if (reading && address >= 0x60003120 && uint64_t(address) + size <= 0x60003124) return true;
    // Walk contiguous writable intervals instead of single words (same result as
    // word_writable on every word; a 128 KiB read buffer is a few steps, not 32768).
    const uint64_t end = uint64_t(address) + size;
    for (uint64_t at = address & ~3u; at < end;) {
        const uint64_t next = writable_extent(uint32_t(at));
        if (next <= at) return false;
        at = next;
    }
    return true;
}
// Initial guest-console configuration: English, 720p, fixed UTC timezone.
// These are runtime defaults, not settings extracted from the user's console.
ME_IMPORT(__imp__ExGetXConfigSetting) {
    const uint16_t category = ctx.r3.u16, setting = ctx.r4.u16, capacity = ctx.r6.u16;
    const auto output = ctx.r5.u32, required = ctx.r7.u32;
    if (category != 3 || !((setting>=1 && setting<=7) || setting==9 || setting==10 || setting==12)) me_stop_import("XConfig_setting_not_implemented", ctx);
    // Names are four ASCII bytes, transition dates four packed bytes. A zero
    // transition month disables DST; both biases remain zero for fixed UTC.
    const uint32_t profile_value = setting==9?guest_console_language():setting==10?0x00040000u:setting==12?0x40u:
        (setting==2 || setting==3)?0x55544300u:0u;
    if (required && ((required & 1) || !checked_span(required, 2))) me_stop_import("XConfig_required_size_invalid", ctx);
    uint32_t status = 0;
    uint16_t size = 0;
    if (output) {
        if (capacity < 4) status = 0xc0000023; // BUFFER_TOO_SMALL
        else {
            if (!checked_span(output, 4)) me_stop_import("XConfig_output_invalid", ctx);
            const uint32_t value = __builtin_bswap32(profile_value);
            std::memcpy(guest + output, &value, 4);
            size = 4;
        }
    } else if (capacity) status = 0xc00000f1; // INVALID_PARAMETER_3
    else size = 4;
    // Match the SDK entry wrapper: size is zero on a failed query.
    if (required) { const uint16_t encoded = __builtin_bswap16(size); std::memcpy(guest + required, &encoded, 2); }
    ctx.r3.u64 = status;
    if (me_log_verbose()) std::printf("{\"event\":\"xconfig_query\",\"setting\":%u,\"value\":\"%08x\",\"capacity\":%u,\"required\":%u,\"status\":\"%08x\"}\n",setting,profile_value,capacity,size,status);
}
static void query_video_mode(PPCContext& ctx) {
    const auto output = ctx.r3.u32;
    if (!checked_span(output, 48)) me_stop_import("video_mode_invalid_output", ctx);
    // Guest display profile. This is independent of the tablet's physical
    // panel and does not imply that a GPU or presentation surface is ready.
    const uint32_t words[12] = {1280, 720, 0, 1, 1, 0x42700000, 1, 0x4a, 1, 0, 0, 0};
    for (unsigned i = 0; i < 12; ++i) put_word(output + i * 4, words[i]);
    std::printf("{\"event\":\"video_mode_query\",\"width\":1280,\"height\":720,\"refresh_hz\":60,\"gpu_initialized\":%s}\n",game_gpu_ready ? "true" : "false");
}
struct TitleTerminateNotification { uint32_t routine, priority; };
static std::vector<TitleTerminateNotification> title_terminate_notifications;
ME_IMPORT(__imp__ExRegisterTitleTerminateNotification) {
    const auto registration = ctx.r3.u32;
    if (!checked_span(registration, 16, true)) me_stop_import("title_termination_registration_invalid", ctx);
    const auto routine = load_word(registration), priority = load_word(registration + 4);
    if (ctx.r4.u32) {
        if ((routine & 3) || routine < PPC_CODE_BASE || routine >= PPC_CODE_BASE + PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(guest, routine))
            me_stop_import("title_termination_callback_unmapped", ctx);
        if (title_terminate_notifications.size() >= 64) me_stop_import("title_termination_registration_capacity", ctx);
        title_terminate_notifications.push_back({routine, priority});
    } else {
        auto it = std::find_if(title_terminate_notifications.begin(), title_terminate_notifications.end(),
            [&](const auto& item) { return item.routine == routine; });
        if (it != title_terminate_notifications.end()) title_terminate_notifications.erase(it);
    }
    std::printf("{\"event\":\"title_termination_registration\",\"create\":%s,\"routine\":\"%08x\",\"priority\":%u,\"count\":%zu}\n",ctx.r4.u32 ? "true":"false",routine,priority,title_terminate_notifications.size());
}
ME_IMPORT(__imp__VdCallGraphicsNotificationRoutines) {
    // RegisterGraphicsNotification remains a stop import: no subscribers can exist.
    if (ctx.r3.u32 != 1 || !checked_span(ctx.r4.u32, 8, true))
        me_stop_import("graphics_notification_request_invalid", ctx);
    std::printf("{\"event\":\"graphics_notifications\",\"subscribers\":0,\"scaling0\":\"%08x\",\"scaling1\":\"%08x\"}\n",load_word(ctx.r4.u32),load_word(ctx.r4.u32+4));
    ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__VdRetrainEDRAMWorker) {
    if (!gpu_explore_mode || !game_gpu_ready || ctx.r3.u32)
        me_stop_import("EDRAM_worker_requires_known_explore_case", ctx);
    // Compatibility probe only: pinned SDK also returns zero here. No hardware training.
    std::puts("{\"event\":\"edram_worker_compat\",\"hardware_training\":false,\"result\":0}");
    ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__VdRetrainEDRAM) {
    if (!gpu_explore_mode || !game_gpu_ready || ctx.r5.u32 != 4096 || ctx.r8.u32 != 2048 ||
        !checked_span(ctx.r4.u32, 4) || !checked_span(ctx.r6.u32, 4) || !checked_span(ctx.r7.u32, 8))
        me_stop_import("EDRAM_retrain_requires_known_explore_case", ctx);
    // At this callsite zero selects the no-command/no-retry path. Leave outputs unchanged.
    if (me_log_verbose()) std::puts("{\"event\":\"edram_retrain_compat\",\"hardware_training\":false,\"commands_generated\":0,\"result\":0}");
    ctx.r3.u64 = 0;
}
static MeXmaRegisters xma_registers;
static MeXmaContextPool xma_context_pool;
// XMA decoder contexts (xbox/audio/xma_bridge.cpp: RexGlue XmaContext + FFmpeg).
extern "C" int me_xma_bridge_setup(uint8_t* guest_base,uint32_t context_physical);
extern "C" void me_xma_bridge_allocate(unsigned id);
extern "C" void me_xma_bridge_release(unsigned id);
extern "C" int me_xma_bridge_kick(unsigned id);
extern "C" void me_xma_bridge_lock(unsigned id);
extern "C" void me_xma_bridge_clear(unsigned id);
extern "C" int me_xma_bridge_enabled(unsigned id);
extern "C" bool me_xma_trace_release(uint32_t physical,uint32_t size);
static bool ensure_xma_context_memory(){
    if(xma_registers.context_physical)return true;
    const auto alias=physical_memory.allocate(MeXmaContextPool::context_count*MeXmaContextPool::context_bytes,0x204,0,UINT32_MAX,256);
    if(!alias)return false;
    const auto physical=MePhysicalMemory::physical(alias);
    if(!me_xma_bridge_setup(guest,physical))return false;
    xma_registers.context_physical=physical;
    std::printf("{\"event\":\"xma_context_memory\",\"physical\":\"%08x\",\"contexts\":320,\"bytes_per_context\":64,\"decoder_ready\":true}\n",xma_registers.context_physical);
    return true;
}
ME_IMPORT(__imp__XMACreateContext) {
    if(!gpu_explore_mode || !word_writable(ctx.r3.u32))me_stop_import("xma_create_context_output_invalid",ctx);
    uint32_t physical=0;
    if(ensure_xma_context_memory())physical=xma_context_pool.acquire(xma_registers.context_physical);
    const uint32_t address=physical?0xa0000000u+physical:0;
    if(address){std::memset(guest+address,0,MeXmaContextPool::context_bytes);me_xma_bridge_allocate((physical-xma_registers.context_physical)/MeXmaContextPool::context_bytes);}
    put_word(ctx.r3.u32,address);ctx.r3.u64=address?0:0xc0000017;
    static unsigned calls=0;if(++calls<=8 || !address)std::printf("{\"event\":\"xma_context_created\",\"address\":\"%08x\",\"status\":\"%08x\",\"decoder_ready\":true}\n",address,ctx.r3.u32);
}
ME_IMPORT(__imp__XMAReleaseContext) {
    if(!gpu_explore_mode)me_stop_import("xma_release_context_requires_explore",ctx);
    const auto physical=MePhysicalMemory::physical(ctx.r3.u32);
    if(!xma_context_pool.release(xma_registers.context_physical,physical))me_stop_import("xma_release_context_invalid_or_not_allocated",ctx);
    // XmaContext::Release resets the decoder stream and zeroes the 64-byte context.
    me_xma_bridge_release((physical-xma_registers.context_physical)/MeXmaContextPool::context_bytes);ctx.r3.u64=0;
}
uint64_t me_xma_load(uint64_t address, unsigned size) {
    static MeImportCounter counter("hook_xma_load"); counter.calls.fetch_add(1, std::memory_order_relaxed);
    MeKernelLock kernel_lock;
    if (!gpu_explore_mode || !MeXmaRegisters::valid(address, size))
        me_stop_import("XMA_requires_known_explore_access", *active_context);
    if (!xma_registers.context_physical && address == 0x7fea1800) {
        if(!ensure_xma_context_memory())me_stop_import("XMA_context_memory_unavailable", *active_context);
    }
    uint64_t value;
    if (!xma_registers.read(address, size, value)) {
        std::printf("{\"event\":\"xma_unsupported_read\",\"address\":\"%08x\",\"size\":%u}\n", uint32_t(address), size);
        me_stop_import("XMA_register_read_not_implemented", *active_context);
    }
    static unsigned reads=0;if(++reads>16 && (reads&(reads-1)))return value;
    std::printf("{\"event\":\"xma_register_read\",\"address\":\"%08x\",\"value\":\"%08x\"}\n", uint32_t(address), uint32_t(value));
    return value;
}
void me_xma_store(uint64_t address, unsigned size, uint64_t value) {
    static MeImportCounter counter("hook_xma_store"); counter.calls.fetch_add(1, std::memory_order_relaxed);
    MeKernelLock kernel_lock;
    if(gpu_explore_mode && MeXmaRegisters::valid(address,size) && address>=0x7fea1a80 && address<=0x7fea1aa4){
        const uint32_t mask=__builtin_bswap32(uint32_t(value));
        const unsigned first=unsigned(address-0x7fea1a80)/4*32;
        for(unsigned i=0;i<32;++i)if((mask&(1u<<i)) && !xma_context_pool.allocated[first+i])
            me_stop_import("XMA_clear_requires_owned_idle_context",*active_context);
        // XmaContext::Clear: input/output valid flags and offsets reset, decoder stream reset.
        for(unsigned i=0;i<32;++i)if(mask&(1u<<i))me_xma_bridge_clear(first+i);
        static unsigned calls=0;if(++calls<=8)std::printf("{\"event\":\"xma_context_clear\",\"first_index\":%u,\"mask\":\"%08x\",\"decoder_ready\":true}\n",first,mask);
        return;
    }
    // Kick (0x1940..0x1964): enable and decode inline; Lock (0x1A40..0x1A64): disable and
    // wait for an active decode. Same semantics as RexGlue XmaDecoder::WriteRegister.
    const bool kick=address>=0x7fea1940 && address<=0x7fea1964, lock=address>=0x7fea1a40 && address<=0x7fea1a64;
    if(gpu_explore_mode && MeXmaRegisters::valid(address,size) && (kick || lock)){
        const uint32_t mask=__builtin_bswap32(uint32_t(value));
        const unsigned first=unsigned(address-(kick?0x7fea1940:0x7fea1a40))/4*32;
        for(unsigned i=0;i<32;++i)if((mask&(1u<<i)) && !xma_context_pool.allocated[first+i])
            me_stop_import(kick?"XMA_kick_requires_owned_context":"XMA_lock_requires_owned_context",*active_context);
        unsigned decoded=0;
        for(unsigned i=0;i<32;++i)if(mask&(1u<<i)){
            if(kick)decoded+=me_xma_bridge_kick(first+i);else me_xma_bridge_lock(first+i);
        }
        static unsigned kicks=0,locks=0;
        unsigned& calls=kick?kicks:locks;++calls;
        if(calls<=8 || !(calls&(calls-1)))std::printf("{\"event\":\"xma_context_%s\",\"first_index\":%u,\"mask\":\"%08x\",\"worked\":%u,\"calls\":%u}\n",kick?"kick":"lock",first,mask,decoded,calls);
        return;
    }
    if (!gpu_explore_mode || !xma_registers.write(address, size, value)) {
        std::printf("{\"event\":\"xma_unsupported_write\",\"address\":\"%08x\",\"value\":\"%08x\",\"size\":%u}\n", uint32_t(address), uint32_t(value), size);
        me_stop_import("XMA_register_write_not_implemented", *active_context);
    }
}
// Spinlocks and IRQL without the kernel lock. IRQL is a byte in the calling thread's
// own PCR; a spinlock is one guest word taken by CAS 0 -> PCR address, as the Xbox
// kernel does. Spinners yield; a spinner that holds the execution gate (serialized
// mode) releases it while waiting so the owner can run.
static uint32_t* sl_word(uint32_t address) { return reinterpret_cast<uint32_t*>(guest + address); }
static bool sl_try(uint32_t address, uint32_t owner) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(sl_word(address), &expected, __builtin_bswap32(owner), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static uint32_t sl_owner(uint32_t address) { return __builtin_bswap32(__atomic_load_n(sl_word(address), __ATOMIC_ACQUIRE)); }
static void sl_acquire(uint32_t address, uint32_t owner) {
    for (unsigned spin = 0; !sl_try(address, owner); ++spin) {
        if (spin < 64) { asm volatile("yield" ::: "memory"); continue; }
        if (execution_gate.owned()) { execution_gate.unlock(); std::this_thread::yield(); execution_gate.lock(); }
        else std::this_thread::yield();
    }
}
static bool sl_log(uint64_t& calls) { return ++calls <= 8 || !(calls & (calls - 1)); }
PPC_FUNC(__imp__KfAcquireSpinLock) {
    const auto address = ctx.r3.u32;
    if ((address & 3) || address > 0xfffffffcu || ctx.r13.u32 != pcr_address || guest[pcr_address + 0x18] > 2)
        me_stop_import("spinlock_invalid_context_or_address", ctx);
    if (sl_owner(address) == pcr_address) me_stop_import("spinlock_recursive_acquire", ctx);
    const auto old_irql = guest[pcr_address + 0x18];
    guest[pcr_address + 0x18] = 2;
    sl_acquire(address, pcr_address);
    ctx.r3.u64 = old_irql;
    static thread_local uint64_t calls = 0;
    if (sl_log(calls)) std::printf("{\"event\":\"spinlock_acquired\",\"address\":\"%08x\",\"owner\":\"%08x\",\"old_irql\":%u,\"call\":%llu}\n", address, pcr_address, old_irql, (unsigned long long)calls);
}
PPC_FUNC(__imp__KfReleaseSpinLock) {
    const auto address = ctx.r3.u32, old_irql = ctx.r4.u32;
    if ((address & 3) || address > 0xfffffffcu || ctx.r13.u32 != pcr_address ||
        sl_owner(address) != pcr_address || old_irql > 2 || guest[pcr_address + 0x18] != 2)
        me_stop_import("spinlock_release_invalid_owner_or_irql", ctx);
    __atomic_store_n(sl_word(address), 0u, __ATOMIC_RELEASE); guest[pcr_address + 0x18] = uint8_t(old_irql);
    static thread_local uint64_t calls = 0;
    if (sl_log(calls)) std::printf("{\"event\":\"spinlock_released\",\"address\":\"%08x\",\"restored_irql\":%u,\"call\":%llu}\n", address, old_irql, (unsigned long long)calls);
}
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) {
    const auto address = ctx.r3.u32;
    if ((address & 3) || address > 0xfffffffcu || ctx.r13.u32 != pcr_address ||
        guest[pcr_address + 0x18] < 2 || sl_owner(address) == pcr_address)
        me_stop_import("raised_spinlock_invalid_context_or_recursive", ctx);
    sl_acquire(address, pcr_address);
    static thread_local uint64_t calls = 0;
    if (sl_log(calls)) std::printf("{\"event\":\"raised_spinlock_acquired\",\"address\":\"%08x\",\"owner\":\"%08x\",\"irql\":%u,\"call\":%llu}\n", address, pcr_address, guest[pcr_address + 0x18], (unsigned long long)calls);
}
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) {
    const auto address = ctx.r3.u32;
    if ((address & 3) || address > 0xfffffffcu || ctx.r13.u32 != pcr_address ||
        sl_owner(address) != pcr_address || guest[pcr_address + 0x18] < 2)
        me_stop_import("raised_spinlock_invalid_release", ctx);
    __atomic_store_n(sl_word(address), 0u, __ATOMIC_RELEASE);
    static thread_local uint64_t calls = 0;
    if (sl_log(calls)) std::printf("{\"event\":\"raised_spinlock_released\",\"address\":\"%08x\",\"irql\":%u,\"call\":%llu}\n", address, guest[pcr_address + 0x18], (unsigned long long)calls);
}
ME_IMPORT(__imp__VdGetSystemCommandBuffer) {
    const auto buffer = ctx.r3.u32, identifier = ctx.r4.u32;
    if (!gpu_explore_mode || !game_gpu_ready || ctx.lr != 0x82815008 ||
        !checked_span(buffer, 0x94) || !checked_span(identifier, 4) ||
        (uint64_t(buffer) < uint64_t(identifier) + 4 && uint64_t(identifier) < uint64_t(buffer) + 0x94))
        me_stop_import("system_command_buffer_requires_known_explore_call", ctx);
    // Pinned RexGlue compatibility metadata, not GPU pointers or completions.
    // Its VdSwap ignores both markers and creates its own PM4 swap packet.
    std::memset(guest + buffer, 0, 0x94);
    put_word(buffer, 0xbeef0000);
    put_word(identifier, 0xbeef0001);
    if (me_log_verbose()) std::printf("{\"event\":\"system_command_buffer_metadata\",\"buffer\":\"%08x\",\"identifier_output\":\"%08x\",\"compatibility_markers\":true,\"gpu_buffer_created\":false}\n", buffer, identifier);
}
ME_IMPORT(__imp__VdPersistDisplay) {
    const auto request=ctx.r3.u32,output=ctx.r4.u32;
    if(!gpu_explore_mode || !game_gpu_ready || ctx.lr!=0x82815458 ||
       !checked_span(request,12,true) || !word_writable(output))me_stop_import("persist_display_unknown_request",ctx);
    if(load_word(request) || load_word(request+4) || load_word(request+8))me_stop_import("persist_display_nonzero_descriptor_unsupported",ctx);
    // Standalone headless probe has no display to retain. The observed caller
    // branches around freeing the returned allocation when this BOOL is false.
    // Leave the output untouched; no dummy physical object or retained frame.
    std::puts("{\"event\":\"display_persistence_unavailable\",\"presenter\":false,\"result\":0,\"allocation_created\":false}");
    ctx.r3.u64=0;
}
ME_IMPORT(__imp__VdInitializeScalerCommandBuffer) {
    if(!gpu_explore_mode || !game_gpu_ready || !checked_span(ctx.r1.u32+100,12,true))me_stop_import("scaler_requires_gpu_and_stack",ctx);
    const auto destination=load_word(ctx.r1.u32+100),count=load_word(ctx.r1.u32+108);
    std::printf("{\"event\":\"scaler_request\",\"source_xy\":\"%08x\",\"source_wh\":\"%08x\",\"output_xy\":\"%08x\",\"output_wh\":\"%08x\",\"front_wh\":\"%08x\",\"destination\":\"%08x\",\"count\":%u}\n",ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,destination,count);
    // Native swap uses the texture dimensions. Only a 720p identity scaler is
    // accepted: emit valid PM4 NOPs, no Xbox scaler/filter programming.
    if(ctx.r3.u32 || ctx.r5.u32 || ctx.r4.u32!=0x050002d0 || ctx.r6.u32!=ctx.r4.u32 || ctx.r7.u32!=ctx.r4.u32 ||
       count!=200 || (destination&3) || !checked_span(destination,count*4))me_stop_import("scaler_nonidentity_or_buffer_unsupported",ctx);
    for(uint32_t i=0;i<count;++i)put_word(destination+i*4,0x80000000);
    ctx.r3.u64=count;
}
ME_IMPORT(__imp__VdSetDisplayMode) {
    // Only the existing 720p RGB guest profile; Android display is unchanged.
    if (!gpu_explore_mode || !game_gpu_ready || ctx.r3.u32 != 0x40000000)
        me_stop_import("VdSetDisplayMode_unsupported_profile", ctx);
    std::puts("{\"event\":\"guest_display_mode\",\"width\":1280,\"height\":720,\"color_space\":\"rgb\",\"host_display_changed\":false}");
    ctx.r3.u64 = 0;
}
static void trace_gpu_submission(const char* stage,uint32_t device,uint64_t call) {
    constexpr uint32_t offsets[]={48,56,10896,10908,10940,13232,13516,13536,13540,14928,21532};
    for(auto offset:offsets)if(uint64_t(device)+offset+4>0x100000000ull || !checked_span(device+offset,4,true)){
        std::printf("{\"event\":\"gpu_submission_snapshot_unmapped\",\"stage\":\"%s\",\"device\":\"%08x\"}\n",stage,device);return;
    }
    uint32_t read=0xffffffff;
    if(auto* p=gpu_physical.resolve(gpu_read_pointer_physical,4))read=MeXexHeaders::be32(p);
    std::printf("{\"event\":\"gpu_submission_snapshot\",\"stage\":\"%s\",\"call\":%llu,\"device\":\"%08x\",\"cursor\":\"%08x\",\"limit\":\"%08x\",\"writeback\":\"%08x\",\"serial\":%u,\"flags\":\"%08x\",\"field13232\":\"%08x\",\"field13516\":\"%08x\",\"secondary_cursor\":\"%08x\",\"secondary_limit\":\"%08x\",\"submitted_cursor\":\"%08x\",\"field21532\":\"%08x\",\"ring_wptr\":%u,\"ring_rptr\":%u}\n",
        stage,(unsigned long long)call,device,load_word(device+48),load_word(device+56),load_word(device+10896),load_word(device+10908),load_word(device+10940),load_word(device+13232),load_word(device+13516),load_word(device+13536),load_word(device+13540),load_word(device+14928),load_word(device+21532),gpu_last_write_index,read);
}
// Crash lr 8249b888 (Stormdrain / Edge_Pt2, guest 0x5d..0x405c): sub_8249B6E8 loops over a
// byte index table {u32 table, s32 count} at r5 (bone indices, 4x4 matrices follow) and reads
// it through a null table pointer. Logged with the caller and the object (r3) when the table
// is null and the count positive. ME_GUARD_NULL_BONE_TABLE=1 (test only, off by default)
// skips such a call instead of faulting - the mesh is not drawn/updated for that call.
extern "C" void me_current_scene(char* out, size_t capacity);
extern "C" void __imp__sub_8249B6E8(PPCContext&,uint8_t*);
PPC_FUNC(sub_8249B6E8) {
    static std::atomic<uint64_t> bone_walks{0}, bone_walk_nodes{0};  // attachment-list walks below
    const uint32_t table_desc = ctx.r5.u32;
    if (table_desc && checked_span(table_desc, 8, true)) {
        const uint32_t table = load_word(table_desc), count = load_word(table_desc + 4);
        if (!table && int32_t(count) > 0) {
            static uint64_t hits = 0;
            const auto hit = ++hits;
            if (hit <= 32 || !(hit & (hit - 1))) {
                const uint32_t object = ctx.r3.u32;
                auto word = [&](uint32_t address) { return checked_span(address, 4, true) ? load_word(address) : 0xffffffffu; };
                char scene[48]; me_current_scene(scene, sizeof(scene));
                std::printf("{\"event\":\"null_bone_table\",\"hit\":%llu,\"scene\":\"%s\",\"caller_lr\":\"%08x\",\"r3\":\"%08x\",\"r4\":\"%08x\",\"r5\":\"%08x\",\"count\":%d,"
                            "\"desc_words\":[\"%08x\",\"%08x\",\"%08x\",\"%08x\"],\"obj464\":\"%08x\",\"obj552\":\"%08x\",\"obj592\":\"%08x\",\"r4_0\":\"%08x\"}\n",
                            (unsigned long long)hit, scene, uint32_t(ctx.lr), object, ctx.r4.u32, table_desc, int32_t(count),
                            word(table_desc - 8), word(table_desc - 4), word(table_desc + 8), word(table_desc + 12),
                            word(object + 464), word(object + 552), word(object + 592), word(ctx.r4.u32));
                std::fflush(stdout);
            }
            static const bool guard = [] { const char* v = std::getenv("ME_GUARD_NULL_BONE_TABLE"); return v && !std::strcmp(v, "1"); }();
            if (guard) return;
        }
    }
    // ME_DIAGNOSTIC_BONES_AT=<x>,<y>: for a component (r3) whose first 0x800 bytes hold the float
    // pair (x, y) - its LocalToWorld translation, e.g. the Stormdrain valve wheel 1928.01,-7872.01 -
    // log the bone atoms it gets (r4: +0 array of 32-byte atoms) for the required bones (r5:
    // {u8 table, count}) and the bone matrices it produces (*(r3+592), 64 bytes per bone).
    static const float* bones_at = [] {
        static float xy[2];
        const char* v = std::getenv("ME_DIAGNOSTIC_BONES_AT");
        return v && std::sscanf(v, "%f,%f", &xy[0], &xy[1]) == 2 ? xy : nullptr;
    }();
    int bones_offset = -1;
    uint32_t bones_object = ctx.r3.u32, bones_atoms_desc = ctx.r4.u32, bones_required = ctx.r5.u32;
    static uint64_t bones_logged = 0;
    static int bones_learned_offset = -1;  // offset of (x, y) in r3 found by the memory scan below
    if (bones_at && bones_logged < 12 && checked_span(bones_object, 0x800, true)) {
        auto f32 = [&](uint32_t address) { uint32_t w = load_word(address); float f; std::memcpy(&f, &w, 4); return f; };
        auto near_xy = [&](uint32_t a) { return std::fabs(f32(a) - bones_at[0]) < 0.05f && std::fabs(f32(a + 4) - bones_at[1]) < 0.05f; };
        if (bones_learned_offset >= 0 && checked_span(bones_object + bones_learned_offset, 8, true) && near_xy(bones_object + bones_learned_offset))
            bones_offset = bones_learned_offset;
        for (int off = 0; bones_offset < 0 && off + 8 <= 0x800; off += 4)
            if (near_xy(bones_object + off)) bones_offset = off;
    }
    // With ME_DIAGNOSTIC_BONES_AT: count calls and distinct components (r3), and every 20 s
    // outside menus (up to 60 times) scan all readable guest memory for the float pair (x, y). Each hit is logged
    // with the nearest component at or below it (offset) and the 64 bytes before it (a matrix
    // ending in the translation), so a hit inside a component teaches the probe its offset.
    if (bones_at) {
        static std::mutex scan_mutex;
        static std::vector<uint32_t> objects;  // sorted distinct r3
        static uint64_t calls = 0, scans = 0;
        static auto last_scan = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(scan_mutex);
        ++calls;
        auto at = std::lower_bound(objects.begin(), objects.end(), bones_object);
        if ((at == objects.end() || *at != bones_object) && objects.size() < 16384) objects.insert(at, bones_object);
        const auto now = std::chrono::steady_clock::now();
        char scene[48] = "";
        if (scans < 60 && now - last_scan > std::chrono::seconds(20)) me_current_scene(scene, sizeof(scene));
        if (scene[0] && !std::strstr(scene, "Menu")) {
            last_scan = now;
            ++scans;
            std::printf("{\"event\":\"bones_scan\",\"scan\":%llu,\"scene\":\"%s\",\"calls\":%llu,\"objects\":%zu,\"walks\":%llu,\"walk_nodes\":%llu,\"hits\":[",
                        (unsigned long long)scans, scene, (unsigned long long)calls, objects.size(),
                        (unsigned long long)bone_walks.load(), (unsigned long long)bone_walk_nodes.load());
            uint32_t want_x, want_y; std::memcpy(&want_x, &bones_at[0], 4); std::memcpy(&want_y, &bones_at[1], 4);
            int hits = 0;
            // From 0x10000 up: the game's own heap (UObjects) is at 0x1xxxxxxx, GPU/physical copies at 0xa0000000+.
            for (uint64_t a = 0x10000; a < 0x100000000ull && hits < 24;) {
                const uint64_t end = writable_extent(uint32_t(a));
                if (end <= a) { a = (a | 0xffff) + 1; continue; }
                for (; a + 8 <= end && hits < 24; a += 4) {
                    const uint32_t w = load_word(uint32_t(a));
                    float f; std::memcpy(&f, &w, 4);
                    if (!(std::fabs(f - bones_at[0]) < 0.05f)) continue;  // (NaN never matches)
                    const uint32_t w2 = load_word(uint32_t(a + 4)); float g; std::memcpy(&g, &w2, 4);
                    if (!(std::fabs(g - bones_at[1]) < 0.05f)) continue;
                    auto owner = std::upper_bound(objects.begin(), objects.end(), uint32_t(a));
                    const uint32_t object = owner == objects.begin() ? 0 : *(owner - 1);
                    const uint32_t offset = object ? uint32_t(a) - object : 0;
                    if (object && offset < 0x4000 && bones_learned_offset < 0) bones_learned_offset = int(offset);
                    std::printf("%s{\"address\":\"%08x\",\"object\":\"%08x\",\"offset\":%u,\"prev64\":[", hits ? "," : "", uint32_t(a), object, offset);
                    for (int k = -12; k < 4; ++k) {
                        const uint32_t v = checked_span(uint32_t(a) + k * 4, 4, true) ? load_word(uint32_t(a) + k * 4) : 0;
                        float h; std::memcpy(&h, &v, 4); std::printf("%s%g", k > -12 ? "," : "", h);
                    }
                    std::printf("]}");
                    ++hits;
                }
                if (a + 8 > end) a = end;
            }
            std::printf("],\"learned_offset\":%d,\"scan_ms\":%.1f}\n", bones_learned_offset,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count());
            std::fflush(stdout);
        }
    }
    if (bones_offset >= 0) {
        auto f32 = [&](uint32_t address) { uint32_t w = checked_span(address, 4, true) ? load_word(address) : 0; float f; std::memcpy(&f, &w, 4); return f; };
        const uint32_t table = checked_span(bones_required, 8, true) ? load_word(bones_required) : 0;
        const int32_t count = checked_span(bones_required, 8, true) ? int32_t(load_word(bones_required + 4)) : -1;
        const uint32_t atoms = checked_span(bones_atoms_desc, 4, true) ? load_word(bones_atoms_desc) : 0;
        std::printf("{\"event\":\"bones_probe\",\"stage\":\"before\",\"n\":%llu,\"object\":\"%08x\",\"match_offset\":%d,\"caller_lr\":\"%08x\",\"atoms\":\"%08x\",\"table\":\"%08x\",\"count\":%d,\"bones\":[",
                    (unsigned long long)bones_logged, bones_object, bones_offset, uint32_t(ctx.lr), atoms, table, count);
        for (int32_t i = 0; i < count && i < 24; ++i) {
            const uint32_t bone = table && checked_span(table + i, 1, true) ? base[table + i] : 0xffu;
            const uint32_t atom = atoms + bone * 32;
            std::printf("%s{\"bone\":%u,\"atom\":[%g,%g,%g,%g,%g,%g,%g,%g]}", i ? "," : "", bone,
                        f32(atom), f32(atom + 4), f32(atom + 8), f32(atom + 12), f32(atom + 16), f32(atom + 20), f32(atom + 24), f32(atom + 28));
        }
        std::printf("]}\n");
    }
    // Walk the per-bone attachment lists the function will follow (read only, guest code at
    // loc_8249B9xx): idx = u8 (*(r3+704))[bone] when *(r3+708) > 0 and idx != 255; node =
    // *(*(o+252) + idx*16 + 8) with o = *(r3+552) (the code first checks its class); next =
    // *(node+132). A node or next pointer outside readable guest memory is the corruption
    // that crashes at lr 8249b888 - logged with the chain before the guest reads it.
    {
        static uint64_t corrupt_logged = 0;
        const uint32_t object = ctx.r3.u32, required = ctx.r5.u32;
        auto word = [&](uint32_t a) { return checked_span(a, 4, true) ? load_word(a) : 0xffffffffu; };
        if (corrupt_logged < 16 && checked_span(object + 552, 4, true) && checked_span(object + 704, 8, true) && checked_span(required, 8, true)) {
            const uint32_t owner = load_word(object + 552), map = load_word(object + 704);
            const int32_t map_count = int32_t(load_word(object + 708));
            const uint32_t table = load_word(required);
            const int32_t count = int32_t(load_word(required + 4));
            const uint32_t lists = owner && checked_span(owner + 252, 4, true) ? load_word(owner + 252) : 0;
            for (int32_t i = 0; lists && map_count > 0 && count > 0 && i < count && i < 512; ++i) {
                if (!checked_span(table + i, 1, true)) break;
                const uint32_t bone = base[table + i];
                if (int32_t(bone) >= map_count || !checked_span(map + bone, 1, true)) continue;
                const uint32_t index = base[map + bone];
                if (index == 255 || !checked_span(lists + index * 16 + 8, 4, true)) continue;
                uint32_t node = load_word(lists + index * 16 + 8), chain[8] = {}, depth = 0;
                ++bone_walks;
                bool bad = false;
                for (uint32_t steps = 0; node && steps < 256; ++steps) {
                    if (depth < 8) chain[depth] = node;
                    ++depth;
                    if (!checked_span(node, 136, true)) { bad = true; break; }
                    ++bone_walk_nodes;
                    node = load_word(node + 132);
                }
                // Last healthy look of each linked node (first word = vtable for UObjects): a node
                // that is still linked but changed its first word was freed and reused while linked.
                struct Seen { uint32_t w0, w4, f72, f92, next; double t; };
                static std::unordered_map<uint64_t, Seen> seen;
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                for (uint32_t k = 0; k < depth && k < 8; ++k) {
                    const uint32_t n = chain[k];
                    if (!checked_span(n, 136, true)) break;
                    const uint64_t key = uint64_t(n) << 32 | (uint64_t(object) ^ (uint64_t(bone) << 24));
                    const Seen cur{word(n), word(n + 4), word(n + 72), word(n + 92), word(n + 132), now};
                    auto it = seen.find(key);
                    if (it != seen.end() && it->second.w0 != cur.w0 && corrupt_logged < 16) {
                        ++corrupt_logged;
                        std::printf("{\"event\":\"bone_attach_node_changed\",\"object\":\"%08x\",\"bone\":%u,\"position\":%u,\"node\":\"%08x\",\"before\":{\"w0\":\"%08x\",\"w4\":\"%08x\",\"f72\":\"%08x\",\"f92\":\"%08x\",\"next\":\"%08x\",\"age_s\":%.3f},\"after\":{\"w0\":\"%08x\",\"w4\":\"%08x\",\"f72\":\"%08x\",\"f92\":\"%08x\",\"next\":\"%08x\"},\"caller_lr\":\"%08x\"}\n",
                                    object, bone, k, n, it->second.w0, it->second.w4, it->second.f72, it->second.f92, it->second.next, now - it->second.t,
                                    cur.w0, cur.w4, cur.f72, cur.f92, cur.next, uint32_t(ctx.lr));
                        std::fflush(stdout);
                    }
                    if (seen.size() < 65536 || it != seen.end()) seen[key] = cur;
                }
                if (!bad) continue;
                ++corrupt_logged;
                char scene[48]; me_current_scene(scene, sizeof(scene));
                std::printf("{\"event\":\"bone_attach_corrupt\",\"n\":%llu,\"scene\":\"%s\",\"caller_lr\":\"%08x\",\"object\":\"%08x\",\"owner\":\"%08x\",\"lists\":\"%08x\",\"bone\":%u,\"index\":%u,\"entry\":[\"%08x\",\"%08x\",\"%08x\",\"%08x\"],\"depth\":%u,\"bad_node\":\"%08x\",\"chain\":[",
                            (unsigned long long)corrupt_logged, scene, uint32_t(ctx.lr), object, owner, lists, bone, index,
                            word(lists + index * 16), word(lists + index * 16 + 4), word(lists + index * 16 + 8), word(lists + index * 16 + 12), depth, node);
                for (uint32_t k = 0; k < depth && k < 8; ++k) {
                    const uint32_t n = chain[k];
                    std::printf("%s{\"node\":\"%08x\",\"w0\":\"%08x\",\"w4\":\"%08x\",\"f72\":\"%08x\",\"f92\":\"%08x\",\"f128\":\"%08x\",\"next\":\"%08x\"}", k ? "," : "",
                                n, word(n), word(n + 4), word(n + 72), word(n + 92), word(n + 128), word(n + 132));
                }
                std::printf("]}\n");
                std::fflush(stdout);
                break;
            }
        }
    }
    __imp__sub_8249B6E8(ctx, base);
    if (bones_offset >= 0) {
        auto f32 = [&](uint32_t address) { uint32_t w = checked_span(address, 4, true) ? load_word(address) : 0; float f; std::memcpy(&f, &w, 4); return f; };
        const uint32_t matrices = checked_span(bones_object + 592, 4, true) ? load_word(bones_object + 592) : 0;
        const uint32_t table = checked_span(bones_required, 8, true) ? load_word(bones_required) : 0;
        const int32_t count = checked_span(bones_required, 8, true) ? int32_t(load_word(bones_required + 4)) : -1;
        std::printf("{\"event\":\"bones_probe\",\"stage\":\"after\",\"n\":%llu,\"matrices\":\"%08x\",\"bones\":[", (unsigned long long)bones_logged, matrices);
        for (int32_t i = 0; i < count && i < 24; ++i) {
            const uint32_t bone = table && checked_span(table + i, 1, true) ? base[table + i] : 0xffu;
            const uint32_t m = matrices + bone * 64;
            std::printf("%s{\"bone\":%u,\"m\":[", i ? "," : "", bone);
            for (int k = 0; k < 16; ++k) std::printf("%s%g", k ? "," : "", f32(m + k * 4));
            std::printf("]}");
        }
        std::printf("]}\n");
        std::fflush(stdout);
        ++bones_logged;
    }
}
extern "C" void __imp__sub_828138E0(PPCContext&,uint8_t*);
PPC_FUNC(sub_828138E0) {
    static uint64_t calls=0;const auto call=++calls;const auto device=ctx.r3.u32;
    const bool trace=gpu_explore_mode && (call<=8 || !(call&(call-1)));
    if(trace)trace_gpu_submission("flush_begin",device,call);
    __imp__sub_828138E0(ctx,base);
    if(trace)trace_gpu_submission("flush_end",device,call);
}
ME_IMPORT(__imp__VdSwap) {
    if (!gpu_explore_mode || !game_gpu_ready || ctx.lr != 0x8281506c ||
        !checked_span(ctx.r3.u32, 256) || !checked_span(ctx.r4.u32, 24, true) ||
        !checked_span(ctx.r6.u32, 0x94, true) || ctx.r7.u32 != 0xbeef0001 ||
        !checked_span(ctx.r8.u32, 4, true) || !checked_span(ctx.r9.u32, 4, true) ||
        !checked_span(ctx.r10.u32, 4, true) || !checked_span(ctx.r1.u32 + 84, 12, true))
        me_stop_import("VdSwap_invalid_or_unknown_call", ctx);
    const auto width_ptr = load_word(ctx.r1.u32 + 84), height_ptr = load_word(ctx.r1.u32 + 92);
    if (!checked_span(width_ptr, 4, true) || !checked_span(height_ptr, 4, true))
        me_stop_import("VdSwap_invalid_dimensions", ctx);
    const auto front = load_word(ctx.r8.u32), width = load_word(width_ptr), height = load_word(height_ptr);
    const auto* allocation = physical_memory.find(front);
    if (!allocation || !physical_memory.find(ctx.r3.u32) ||
        uint64_t(MePhysicalMemory::physical(ctx.r3.u32)) + 256 >
            uint64_t(physical_memory.find(ctx.r3.u32)->physical) + physical_memory.find(ctx.r3.u32)->size ||
        ctx.r5.u32 != gpu_identifier_address || load_word(ctx.r6.u32) != 0xbeef0000)
        me_stop_import("VdSwap_invalid_physical_buffers", ctx);
    uint32_t fetch[6], commands[64];
    for (unsigned i = 0; i < 6; ++i) fetch[i] = load_word(ctx.r4.u32 + i * 4);
    const auto physical = MePhysicalMemory::physical(front);
    if (!me_gpu_make_swap(commands, fetch, front, physical, load_word(ctx.r9.u32), load_word(ctx.r10.u32), width, height))
        me_stop_import("VdSwap_invalid_texture_fetch", ctx);
    for (unsigned i = 0; i < 64; ++i) put_word(ctx.r3.u32 + i * 4, commands[i]);
    static uint64_t calls=0;const auto call=++calls;
    if(call<=8 || !(call&(call-1)))trace_gpu_submission("swap_generated",ctx.r31.u32,call);
    if (me_log_verbose()) std::printf("{\"event\":\"swap_commands_generated\",\"buffer\":\"%08x\",\"frontbuffer_physical\":\"%08x\",\"width\":%u,\"height\":%u,\"dwords\":64,\"identifier_completion_emulated\":false}\n", ctx.r3.u32, physical, width, height);
    // Frame limiter (heat/battery, launcher setting): ME_FRAME_LIMIT=30|60 (10..240 Hz,
    // unset/0 = off) paces the guest's swaps on a fixed cadence; the gate is released
    // while waiting so other guest threads keep running. Falling behind restarts the
    // cadence instead of catching up with a burst of frames.
    // ME_FRAME_LIMIT_FILE=<path> (diagnostics): the limit is re-read from that file every 60
    // swaps, so A/B of pacing modes needs no restart at the same spot of a level.
    static auto limit_for = [](int hz) { return hz >= 10 && hz <= 240 ? int64_t(1000000000) / hz : int64_t(0); };
    // Default 30 when unset: above 30 FPS (Unlock FPS patch) the game crashed in Stormdrain
    // (lr 8249b888, bone attachment list); ME_FRAME_LIMIT=0 runs without a limit.
    static int64_t limit_ns = [] {
        const char* v = std::getenv("ME_FRAME_LIMIT"); const int hz = v ? std::atoi(v) : 30;
        if (hz) std::printf("{\"event\":\"frame_limit\",\"hz\":%d}\n", hz);
        return limit_for(hz);
    }();
    static const char* limit_file = std::getenv("ME_FRAME_LIMIT_FILE");
    if (limit_file && call % 60 == 0) {
        if (FILE* f = std::fopen(limit_file, "r")) {
            int hz = 0;
            if (std::fscanf(f, "%d", &hz) == 1 && limit_for(hz) != limit_ns) {
                limit_ns = limit_for(hz);
                std::printf("{\"event\":\"frame_limit\",\"hz\":%d,\"source\":\"file\"}\n", hz);
            }
            std::fclose(f);
        }
    }
    if (limit_ns) {
        static int64_t next_swap_ns = 0;
        const int64_t now = int64_t(me_gate_now_ns());
        if (next_swap_ns && now < next_swap_ns) {
            me_diagnostic_sleep(std::chrono::nanoseconds(next_swap_ns - now));
            next_swap_ns += limit_ns;
        } else {
            next_swap_ns = now + limit_ns;
        }
    }
}
ME_IMPORT(__imp__MmSetAddressProtect) {
    const auto address = ctx.r3.u32, size = ctx.r4.u32, protection = ctx.r5.u32;
    const auto* allocation = physical_memory.find(address);
    const uint64_t start = uint64_t(address) & ~uint64_t(page_size - 1);
    const uint64_t end = (uint64_t(address) + size + page_size - 1) & ~uint64_t(page_size - 1);
    const auto physical = MePhysicalMemory::physical(address);
    // First observed call changes RW to RW|WRITECOMBINE. Shared memfd views
    // remain coherent normal host memory; Xbox cache policy is not emulated.
    // Other access modes require permission tracking in all checked accesses.
    if (!allocation || !size || (protection != 4 && protection != 0x204 && protection != 0x404) ||
        uint64_t(physical) + size > uint64_t(allocation->physical) + allocation->size ||
        end > 0x100000000ull || start < address - (physical - allocation->physical) ||
        (me_gpu_write_watch.host_write(uint32_t(start), uint32_t(end - start)), mprotect(guest + start, end - start, PROT_READ | PROT_WRITE)))
        me_stop_import("MmSetAddressProtect_unsupported_or_invalid_range", ctx);
    std::printf("{\"event\":\"physical_address_protect\",\"address\":\"%08x\",\"size\":%u,\"protection\":\"%08x\",\"host_access\":\"rw\",\"xbox_cache_policy_emulated\":false}\n", address, size, protection);
    // Void kernel export: preserve registers rather than inventing a status.
}
ME_IMPORT(__imp__VdIsHSIOTrainingSucceeded) {
    if (!gpu_explore_mode || !game_gpu_ready)
        me_stop_import("HSIO_query_requires_gpu_explore", ctx);
    // Virtual GPU compatibility: SetupExternalGpu must already have succeeded.
    // False makes this title disable hardware submissions (global 830f5d18).
    // This answers guest GPU readiness, not physical Xbox link calibration.
    std::puts("{\"event\":\"hsio_training_query\",\"hardware_training\":false,\"backend_ready\":true,\"compatibility_result\":1}");
    ctx.r3.u64 = 1;
}
ME_IMPORT(__imp__VdQueryVideoFlags) {
    // Pinned SDK derives these bits from the guest display profile.
    ctx.r3.u64 = 1 | 2; // widescreen and width >= 1024, width < 1920.
}
ME_IMPORT(__imp__VdGetCurrentDisplayGamma) {
    const auto type = ctx.r3.u32, power = ctx.r4.u32;
    if (!checked_span(type, 4) || !checked_span(power, 4)) me_stop_import("display_gamma_output_invalid", ctx);
    const float gamma = 2.22222233f;
    uint32_t bits; std::memcpy(&bits, &gamma, 4);
    put_word(type, 2); put_word(power, bits); // SDK guest BT.709 profile, not tablet calibration.
}
ME_IMPORT(__imp__VdGetCurrentDisplayInformation) {
    const auto output = ctx.r3.u32;
    if (!checked_span(output, 0x58)) me_stop_import("display_information_output_invalid", ctx);
    std::memset(guest + output, 0, 0x58);
    auto half = [&](uint32_t offset, uint16_t value) {
        value = __builtin_bswap16(value); std::memcpy(guest + output + offset, &value, 2);
    };
    half(0, 1280); half(2, 720);
    put_word(output + 0x10, 1280); put_word(output + 0x14, 720); // scaler source rect x2/y2
    put_word(output + 0x18, 1280); put_word(output + 0x1c, 720); // scaled width/height
    put_word(output + 0x20, 1); put_word(output + 0x30, 1); // vertical/horizontal filter type
    half(0x40, 320); half(0x42, 180); half(0x44, 320); half(0x46, 180);
    half(0x48, 1280); half(0x4a, 720); put_word(output + 0x4c, 0x42700000);
    half(0x56, 1280);
}
ME_IMPORT(__imp__XGetVideoMode) { query_video_mode(ctx); }
ME_IMPORT(__imp__VdQueryVideoMode) { query_video_mode(ctx); }
ME_IMPORT(__imp__VdInitializeEngines) {
    std::printf("{\"event\":\"gpu_engine_request\",\"parameter\":\"%08x\",\"callback\":\"%08x\",\"argument\":\"%08x\",\"pfp\":\"%08x\",\"me\":\"%08x\",\"gpu_initialized\":false}\n",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    start_game_gpu(ctx);
}
ME_IMPORT(__imp__VdSetGraphicsInterruptCallback) {
    if (!game_gpu_ready) me_stop_import("GPU_interrupt_before_initialization", ctx);
    const auto callback = ctx.r3.u32;
    if (callback && ((callback & 3) || callback < PPC_CODE_BASE || callback >= PPC_CODE_BASE + PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(guest, callback)))
        me_stop_import("GPU_interrupt_callback_unmapped", ctx);
    me_gpu_irq_callback(callback, ctx.r4.u32);
    std::printf("{\"event\":\"game_gpu_interrupt_callback\",\"callback\":\"%08x\"}\n",callback);
}
ME_IMPORT(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    const auto address = ctx.r3.u32;
    if (address) {
        if (!gpu_explore_mode) me_stop_import("GPU_system_buffer_identifier_nonnull_unsupported", ctx);
        if ((address & 3) || !physical_memory.find(address) ||
            !gpu_physical.resolve(MePhysicalMemory::physical(address), 4))
            me_stop_import("GPU_identifier_registration_invalid", ctx);
    }
    gpu_identifier_address = address;
    std::printf("{\"event\":\"gpu_identifier_registration\",\"address\":\"%08x\",\"registration_only\":true,\"completion_emulated\":false}\n",address);
}
ME_IMPORT(__imp__KiApcNormalRoutineNop) {
    // This exported routine itself does nothing; APC queue/dispatch remains unsupported.
    ctx.r3.u64 = 0;
}

ME_IMPORT(__imp__VdInitializeRingBuffer) {
    if (!game_gpu_ready || !gpu_physical.bind_ring(ctx.r3.u32, ctx.r4.u32))
        me_stop_import("GPU_ring_invalid_or_uninitialized", ctx);
    me_gpu_ring(ctx.r3.u32, ctx.r4.u32);
    std::printf("{\"event\":\"game_gpu_ring\",\"physical\":\"%08x\",\"size_log2\":%u}\n",ctx.r3.u32,ctx.r4.u32);
}
ME_IMPORT(__imp__VdEnableRingBufferRPtrWriteBack) {
    if (!game_gpu_ready || ctx.r4.u32 > 19 || (ctx.r3.u32 & 3) || !gpu_physical.resolve(ctx.r3.u32, 4))
        me_stop_import("GPU_read_pointer_invalid_or_uninitialized", ctx);
    me_gpu_readback(ctx.r3.u32, ctx.r4.u32);
    gpu_read_pointer_physical = ctx.r3.u32;
    std::printf("{\"event\":\"game_gpu_read_pointer\",\"physical\":\"%08x\"}\n",ctx.r3.u32);
}
ME_IMPORT(__imp__MmAllocatePhysicalMemoryEx) {
    if (!MePhysicalMemory::supports(ctx.r3.u32, ctx.r5.u32)) me_stop_import("physical_memory_unsupported_flags", ctx);
    const auto size = ctx.r4.u32, protection = ctx.r5.u32;
    ctx.r3.u64 = physical_memory.allocate(size, protection, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    std::printf("{\"event\":\"physical_allocate\",\"requested_size\":%u,\"protection\":\"%08x\",\"address\":\"%08x\"}\n", size, protection, ctx.r3.u32);
}
ME_IMPORT(__imp__MmGetPhysicalAddress) {
    if (!physical_memory.find(ctx.r3.u32)) me_stop_import("MmGetPhysicalAddress_unknown_allocation", ctx);
    ctx.r3.u64 = MePhysicalMemory::physical(ctx.r3.u32);
}
ME_IMPORT(__imp__MmQueryAllocationSize) {
    auto* allocation = physical_memory.find(ctx.r3.u32);
    ctx.r3.u64 = allocation ? allocation->size : 0;
}
ME_IMPORT(__imp__MmFreePhysicalMemory) {
    if(const auto* a=physical_memory.find(ctx.r4.u32)){
        if(me_xma_trace_release(a->physical,a->size)){
            uint32_t sp=ctx.r1.u32;
            for(unsigned depth=0;depth<12 && checked_span(sp,4,true);++depth){
                const uint32_t caller=load_word(sp);
                if(caller<=sp || !checked_span(caller-8,8,true))break;
                std::printf("{\"event\":\"xma_free_stack\",\"depth\":%u,\"lr\":\"%08x\"}\n",depth,load_word(caller-8));
                sp=caller;
            }
        }
        std::printf("{\"event\":\"physical_free\",\"address\":\"%08x\",\"physical\":\"%08x\",\"size\":%u,\"lr\":\"%08llx\"}\n",ctx.r4.u32,a->physical,a->size,(unsigned long long)ctx.lr);
    }
    if (ctx.r3.u32 || !physical_memory.free(ctx.r4.u32)) me_stop_import("MmFreePhysicalMemory_unknown_type_or_allocation", ctx);
}
// Zero/short delays are frequent polling yields: in parallel mode they run without
// the kernel lock (interval read directly from guest memory).
static PPC_FUNC(me_delay_execution_locked);
PPC_FUNC(__imp__KeDelayExecutionThread) {
    static MeImportCounter counter("__imp__KeDelayExecutionThread"); counter.calls.fetch_add(1, std::memory_order_relaxed);
    const auto pointer = ctx.r5.u32;
    if (!guest_parallel || execution_gate.owned() || ctx.r3.u32 > 1 || ctx.r4.u32 || (pointer & 3) || pointer > 0xfffffff8u) {
        me_delay_execution_locked(ctx, base); return;
    }
    uint64_t raw; std::memcpy(&raw, guest + pointer, 8); raw = __builtin_bswap64(raw);
    if (int64_t(raw) > 0 || (0 - raw) > 600000000ull) { me_delay_execution_locked(ctx, base); return; }
    static thread_local uint32_t fast_calls = 0;
    if (++fast_calls <= 8 || !(fast_calls & (fast_calls - 1)))
        std::printf("{\"event\":\"guest_delay\",\"thread\":\"%08x\",\"call\":%u,\"relative_100ns\":%llu,\"lr\":\"%08llx\",\"kernel_lock\":false}\n",thread_address,fast_calls,(unsigned long long)(0-raw),(unsigned long long)ctx.lr);
    if (raw) std::this_thread::sleep_for(std::chrono::nanoseconds((0 - raw) * 100));
    else me_zero_delay_yield();
    ctx.r3.u64 = 0;
}
ME_IMPORT(me_delay_execution_locked) {
    if (ctx.r3.u32 > 1 || ctx.r4.u32) me_stop_import("KeDelayExecutionThread_unsupported_mode_or_apc", ctx);
    const auto pointer = ctx.r5.u32;
    if (!checked_span(pointer, 8, true)) me_stop_import("KeDelayExecutionThread_invalid_interval", ctx);
    const auto raw = (uint64_t(load_word(pointer)) << 32) | load_word(pointer + 4);
    if (int64_t(raw) > 0 || (0 - raw) > 600000000ull) me_stop_import("KeDelayExecutionThread_interval_not_supported", ctx);
    static thread_local uint32_t delay_calls=0;
    ++delay_calls;
    if(gpu_explore_mode && (delay_calls<=8 || (delay_calls&(delay_calls-1))==0))
        std::printf("{\"event\":\"guest_delay\",\"thread\":\"%08x\",\"call\":%u,\"relative_100ns\":%llu,\"lr\":\"%08llx\",\"caller\":\"%08x\"}\n",thread_address,delay_calls,(unsigned long long)(0-raw),(unsigned long long)ctx.lr,checked_span(ctx.r1.u32+104,4,true)?load_word(ctx.r1.u32+104):0);
    if(polling_yield_experiment && (delay_calls<=8 || (delay_calls&(delay_calls-1))==0) &&
       checked_span(ctx.r1.u32+96,12,true) && load_word(ctx.r1.u32+104)==0x82372140){
        const auto queue=load_word(ctx.r1.u32+100);
        if(checked_span(queue,28,true))std::printf("{\"event\":\"job_queue_poll\",\"queue\":\"%08x\",\"base\":\"%08x\",\"producer\":\"%08x\",\"limit\":\"%08x\",\"consumer\":\"%08x\",\"task\":\"%08x\"}\n",queue,load_word(queue),load_word(queue+8),load_word(queue+12),load_word(queue+20),ctx.r29.u32);
    }
    std::unique_lock<MeExecutionGate> lock(execution_gate, std::adopt_lock);
    lock.unlock();
    if (raw) std::this_thread::sleep_for(std::chrono::nanoseconds((0 - raw) * 100));
    else me_zero_delay_yield();
    lock.lock(); lock.release(); ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__NtCreateEvent) {
    const auto output = ctx.r3.u32, attributes = ctx.r4.u32, type = ctx.r5.u32, state = ctx.r6.u32;

    std::string name;bool insensitive=false;
    if(attributes){
        if(!checked_span(attributes,12,true))me_stop_import("event_object_attributes_invalid",ctx);
        const auto root=load_word(attributes),descriptor=load_word(attributes+4),flags=load_word(attributes+8);
        if((root && root!=0xfffffffd && root!=0xfffffffc) || (flags & ~0xc2u))me_stop_import("event_object_namespace_or_flags_unsupported",ctx);
        insensitive=(flags&0x40)!=0;
        if(descriptor){
            if(!checked_span(descriptor,8,true))me_stop_import("event_name_descriptor_invalid",ctx);
            const auto length=load_word(descriptor)>>16,source=load_word(descriptor+4);
            if(length>1024 || !checked_span(source,length,true))me_stop_import("event_name_range_invalid",ctx);
            if(length){
                name=root==0xfffffffd?"dos:":root==0xfffffffc?"win32:":"object:";
                for(unsigned i=0;i<length;++i){const auto c=guest[source+i];if(c<32 || c>126)me_stop_import("event_name_requires_ascii",ctx);name.push_back(char(c));}
            }
        }
    }

    if (!word_writable(output)) me_stop_import("NtCreateEvent_invalid_output_pointer", ctx);
    uint32_t handle = 0;
    const auto status = events.create(type, state != 0, handle, name, insensitive);
    if (int32_t(status)>=0) put_word(output, handle);
    std::printf("{\"event\":\"event_create\",\"handle\":\"%08x\",\"type\":%u,\"state\":%u,\"status\":\"%08x\"}\n", handle, type, state, status);
    ctx.r3.u64 = status;
}
ME_IMPORT(__imp__NtSetEvent) {
    const auto output = ctx.r4.u32;
    if (output && !word_writable(output)) me_stop_import("NtSetEvent_invalid_output_pointer", ctx);
    uint32_t previous = 0; const auto status = events.set(ctx.r3.u32, previous);
    if (!status && output) put_word(output, previous);
    state_changed.notify_all();
    ctx.r3.u64 = status;
}
ME_IMPORT(__imp__NtClearEvent) { ctx.r3.u64 = events.clear(ctx.r3.u32); }
extern "C" void __imp__sub_82255468(PPCContext&,uint8_t*);
PPC_FUNC(sub_82255468) {
    const auto caller=ctx.lr;const auto before=load_word(0x830dd060);
    if(gpu_explore_mode)std::printf("{\"event\":\"global_object_create_begin\",\"caller\":\"%08llx\",\"pointer\":\"%08x\"}\n",(unsigned long long)caller,before);
    __imp__sub_82255468(ctx,base);
    if(gpu_explore_mode)std::printf("{\"event\":\"global_object_create_end\",\"result\":\"%08x\",\"pointer\":\"%08x\"}\n",ctx.r3.u32,load_word(0x830dd060));
}
extern "C" void __imp__sub_822555F0(PPCContext&,uint8_t*);
PPC_FUNC(sub_822555F0) {
    const auto caller=ctx.lr;const auto before=load_word(0x830dd060);
    if(gpu_explore_mode)std::printf("{\"event\":\"global_object_cleanup_begin\",\"caller\":\"%08llx\",\"pointer\":\"%08x\"}\n",(unsigned long long)caller,before);
    __imp__sub_822555F0(ctx,base);
    if(gpu_explore_mode)std::printf("{\"event\":\"global_object_cleanup_end\",\"result\":\"%08x\",\"pointer\":\"%08x\"}\n",ctx.r3.u32,load_word(0x830dd060));
}
extern "C" void __imp__sub_822565B8(PPCContext&,uint8_t*);
PPC_FUNC(sub_822565B8) {
    const auto caller=ctx.lr;const auto before=load_word(0x830dd060);
    if(gpu_explore_mode)std::printf("{\"event\":\"global_object_initialize_begin\",\"caller\":\"%08llx\",\"pointer\":\"%08x\"}\n",(unsigned long long)caller,before);
    __imp__sub_822565B8(ctx,base);
    if(gpu_explore_mode)std::printf("{\"event\":\"global_object_initialize_end\",\"result\":\"%08x\",\"pointer\":\"%08x\"}\n",ctx.r3.u32,load_word(0x830dd060));
}
extern "C" void __imp__sub_8223CF38(PPCContext&,uint8_t*);
PPC_FUNC(sub_8223CF38) {
    const auto caller=ctx.lr;__imp__sub_8223CF38(ctx,base);
    static unsigned failures=0;
    if(gpu_explore_mode && !ctx.r3.u32 && ++failures<=4)std::printf("{\"event\":\"global_object_get_null\",\"caller\":\"%08llx\"}\n",(unsigned long long)caller);
}
static void trace_worker_poll(uint32_t object,uint32_t state,uint32_t index,uint32_t cs) {
    static thread_local uint32_t last_object=0,last_state=0,calls=0;
    ++calls;if(calls>4 && (calls&(calls-1)))return; // count-throttled (state changes alternate constantly on the level)
    last_object=object;last_state=state;MeKernelLock kernel_lock; // checked_span reads loader maps
    if(!checked_span(object,40,true))return;
    std::printf("{\"event\":\"worker_state_poll\",\"object\":\"%08x\",\"state\":%u,\"vtable\":\"%08x\",\"field4\":\"%08x\",\"field8\":\"%08x\",\"field12\":\"%08x\",\"field16\":\"%08x\",\"field20\":\"%08x\",\"field24\":\"%08x\",\"field28\":\"%08x\",\"field36\":\"%08x\"}\n",object,state,load_word(object),load_word(object+4),load_word(object+8),load_word(object+12),load_word(object+16),load_word(object+20),load_word(object+24),load_word(object+28),load_word(object+36));
    if(checked_span(cs,28,true))std::printf("{\"event\":\"worker_selection_lock\",\"index\":%u,\"critical_section\":\"%08x\",\"owner\":\"%08x\",\"thread\":\"%08x\",\"recursion\":%u}\n",index,cs,load_word(cs+24),thread_address,load_word(cs+20));
    if(active_context)std::printf("{\"event\":\"worker_selection_registers\",\"r10_offset\":\"%08x\",\"r11_array\":\"%08x\",\"r30_full\":\"%016llx\",\"r31_pool\":\"%08x\"}\n",active_context->r10.u32,active_context->r11.u32,(unsigned long long)active_context->r30.u64,active_context->r31.u32);
    const auto pool=load_word(object+28);
    if(!checked_span(pool,24,true))return;
    const auto count=load_word(pool+20),array=load_word(pool+16);
    if(count>64 || !checked_span(array,count*4,true))return;
    std::printf("{\"event\":\"worker_pool_snapshot\",\"pool\":\"%08x\",\"count\":%u,\"entries\":[",pool,count);
    for(unsigned i=0;i<count && i<8;++i){const auto entry=load_word(array+i*4);
        if(i)std::printf(",");
        if(!checked_span(entry,40,true)){std::printf("null");continue;}
        std::printf("{\"object\":\"%08x\",\"thread_handle\":\"%08x\",\"field20\":\"%08x\",\"field32\":%u}",entry,load_word(entry+8),load_word(entry+20),load_word(entry+32));
    }
    std::puts("]}");
}
static void trace_pool_transition(const char* phase,uint32_t pool,uint32_t item,uint32_t flag) {
    if(!gpu_explore_mode || !checked_span(pool,32,true))return;
    std::printf("{\"event\":\"pool_transition\",\"phase\":\"%s\",\"thread\":\"%08x\",\"pool\":\"%08x\",\"item\":\"%08x\",\"flag\":%u,\"pending\":%u,\"free\":[",phase,thread_address,pool,item,flag,load_word(pool+8));
    const auto count=load_word(pool+20),array=load_word(pool+16);
    if(count<=64 && checked_span(array,count*4,true))for(uint32_t i=0;i<count;++i){if(i)std::printf(",");std::printf("\"%08x\"",load_word(array+i*4));}
    std::puts("]}");
}
extern "C" void __imp__sub_8266A990(PPCContext&,uint8_t*);
PPC_FUNC(sub_8266A990) {
    const auto pool=ctx.r3.u32,worker=ctx.r4.u32;
    static unsigned calls=0;const bool trace=++calls<=128;
    if(trace)trace_pool_transition("return_begin",pool,worker,0);
    __imp__sub_8266A990(ctx,base);
    if(trace)trace_pool_transition("return_end",pool,worker,0);
}
extern "C" void __imp__sub_8266A880(PPCContext&,uint8_t*);
PPC_FUNC(sub_8266A880) {
    const auto pool=ctx.r3.u32,job=ctx.r4.u32,flag=ctx.r5.u32;
    static unsigned calls=0;const bool trace=++calls<=128;
    if(trace)trace_pool_transition("enqueue_begin",pool,job,flag);
    __imp__sub_8266A880(ctx,base);
    if(trace)trace_pool_transition("enqueue_end",pool,job,flag);
}
static bool local_player_signed_in=false;
static MeLocalPlayer selected_local_player{};
static bool local_player_setting(uint32_t id,uint32_t& value,bool& unset){
 unset=false;value=0;
 switch(id){
 case 0x10040002:case 0x10040003:case 0x10040004:case 0x10040005:
 case 0x1004000c:case 0x1004000d:case 0x10040015:case 0x10040018:
 case 0x10040023:case 0x10040024:case 0x10040026:case 0x10040027:
 case 0x10040028:case 0x10040029:case 0x10040038:case 0x10040039:return true;
 case 0x1004000e:value=100;return true;
 case 0x10040022:value=1;return true;
 case 0x63e83fff:case 0x63e83ffe:case 0x63e83ffd:unset=true;return true;
 default:return false;
 }
}
ME_IMPORT(__imp__XamShowSigninUI) {
 if(!gpu_explore_mode)me_stop_import("signin_ui_outside_game_mode",ctx);
 if(ctx.r3.u32!=1){ctx.r3.u64=87;return;}
 const auto profile=me_guest_input().player();
 if(!profile.xuid){ctx.r3.u64=21;std::puts("{\"event\":\"local_signin_waiting_for_app_profile\"}");return;}
 if(selected_local_player.xuid && selected_local_player.xuid!=profile.xuid)me_stop_import("local_profile_changed_mid_session",ctx);
 selected_local_player=profile;local_player_signed_in=true;
 const int shown=notifications.broadcast(9,1),changed=notifications.broadcast(10,1),hidden=notifications.broadcast(9,0);
 if(shown<0 || changed<0 || hidden<0)me_stop_import("signin_notification_capacity",ctx);
 state_changed.notify_all();ctx.r3.u64=0;
 std::printf("{\"event\":\"local_signin_completed\",\"user\":0,\"name\":\"%s\",\"online\":false,\"notifications\":%d}\n",selected_local_player.name,shown+changed+hidden);
}
// Marketplace (menu "Downloadable content"): the profile is offline, so on a console the Guide
// only says Xbox Live is required and closes. Same here: no UI, the system-UI notification goes
// shown -> hidden and the call succeeds (CHECKLIST: Marketplace offline assumption).
ME_IMPORT(__imp__XamShowMarketplaceUI) {
 const auto user=ctx.r3.u32;
 if(user>=4){ctx.r3.u64=87;return;}
 if(user || !local_player_signed_in){ctx.r3.u64=1317;return;}
 const int shown=notifications.broadcast(9,1),hidden=notifications.broadcast(9,0);
 if(shown<0 || hidden<0)me_stop_import("marketplace_notification_capacity",ctx);
 state_changed.notify_all();ctx.r3.u64=0;
 std::printf("{\"event\":\"marketplace_ui_offline\",\"user\":%u,\"entry_point\":%u,\"notifications\":%d}\n",user,ctx.r4.u32,shown+hidden);
}
ME_IMPORT(__imp__XamUserGetSigninInfo) {
 const auto user=ctx.r3.u32,out=ctx.r5.u32;
 if(!out || !checked_span(out,40,true)){ctx.r3.u64=0x80070057;return;}
 if(!local_player_signed_in || user){ctx.r3.u64=user>=4?0x80070057:0x80070525;return;}
 std::memset(guest+out,0,40);put_word(out,uint32_t(selected_local_player.xuid>>32));put_word(out+4,uint32_t(selected_local_player.xuid));put_word(out+12,1);std::memcpy(guest+out+24,selected_local_player.name,16);ctx.r3.u64=0;
}
ME_IMPORT(__imp__XamUserReadProfileSettings) {
    if(!gpu_explore_mode)me_stop_import("profile_read_service_unavailable",ctx);
    const auto title=ctx.r3.u32,user=ctx.r4.u32,xuid_count=ctx.r5.u32,xuids=ctx.r6.u32;
    const auto count=ctx.r7.u32,ids=ctx.r8.u32,size_ptr=ctx.r9.u32,output=ctx.r10.u32;
    if(!checked_span(ctx.r1.u32+84,4,true))me_stop_import("profile_read_stack_invalid",ctx);
    const auto overlapped=load_word(ctx.r1.u32+84);
    if(overlapped || xuid_count || xuids)me_stop_import("profile_read_xuid_or_overlapped_unsupported",ctx);
    if(user>=4 || !count || count>32 || !checked_span(ids,count*4,true) || !size_ptr || !checked_span(size_ptr,4)) {ctx.r3.u64=87;return;}
    const auto capacity=load_word(size_ptr);
    if(capacity && (!output || !checked_span(output,capacity))){ctx.r3.u64=87;return;}
    if(local_player_signed_in && user==0){
        uint32_t needed=8+count*40;
        for(unsigned i=0;i<count;++i){const auto id=load_word(ids+i*4);uint32_t value;bool unset;
            if(!local_player_setting(id,value,unset)){std::printf("{\"event\":\"local_profile_setting_unsupported\",\"id\":\"%08x\"}\n",id);ctx.r3.u64=87;return;}
            if(unset)needed+=(id>>16)&0xfff;
        }
        if(!output || capacity<needed){if(!capacity)put_word(size_ptr,needed);ctx.r3.u64=122;return;}
        std::memset(guest+output,0,needed);put_word(output,count);put_word(output+4,output+8);
        for(unsigned i=0;i<count;++i){const auto id=load_word(ids+i*4),record=output+8+i*40;uint32_t value;bool unset;local_player_setting(id,value,unset);
            put_word(record+8,user);put_word(record+16,id);
            if(!unset){put_word(record,1);guest[record+24]=1;put_word(record+32,value);}
        }
        ctx.r3.u64=0;return;
    }
    // No local profile exists yet: fail without fabricating settings, modifying
    // size/data, or completing a nonexistent asynchronous request.
    std::printf("{\"event\":\"profile_settings_absent_user\",\"title\":\"%08x\",\"user\":%u,\"setting_count\":%u,\"capacity\":%u,\"result\":1317}\n",title,user,count,capacity);
    ctx.r3.u64=1317;
}
ME_IMPORT(__imp__XamInputSetState) {
    if(!gpu_explore_mode)me_stop_import("input_vibration_bridge_not_connected",ctx);
    if(!ctx.r5.u32){ctx.r3.u64=160;return;}
    if(!checked_span(ctx.r5.u32,4,true))me_stop_import("input_vibration_buffer_invalid",ctx);
    const auto user=ctx.r3.u32;ctx.r3.u64=1167;
    static unsigned calls=0;if(++calls<=8)std::printf("{\"event\":\"input_vibration_provider_absent\",\"user\":%u,\"result\":1167,\"vibration_sent\":false}\n",user);
}
ME_IMPORT(__imp__XamUserGetName) {
    if(!gpu_explore_mode)me_stop_import("user_name_profile_service_unavailable",ctx);
    const auto user=ctx.r3.u32,output=ctx.r4.u32,capacity=ctx.r5.u32;
    if(user>=4 || !output || !capacity || !checked_span(output,std::min(capacity,16u))){ctx.r3.u64=0x80070057;return;}
    if(local_player_signed_in && user==0){const auto n=std::min(capacity,16u);std::memset(guest+output,0,n);std::memcpy(guest+output,selected_local_player.name,std::min(n-1,uint32_t(std::strlen(selected_local_player.name))));ctx.r3.u64=0;return;}
    std::printf("{\"event\":\"user_name_absent_profile\",\"user\":%u,\"capacity\":%u,\"result\":\"80070525\"}\n",user,capacity);
    ctx.r3.u64=0x80070525;
}
ME_IMPORT(__imp__XamUserGetXUID) {
    if(!gpu_explore_mode)me_stop_import("xuid_profile_not_configured",ctx);
    const auto user=ctx.r3.u32,mask=ctx.r4.u32,output=ctx.r5.u32;
    if(mask!=1 && mask!=2 && mask!=3 && mask!=4 && mask!=7)me_stop_import("xuid_type_mask_unsupported",ctx);
    if(!output || !checked_span(output,8)){ctx.r3.u64=0x80070057;return;}
    if(local_player_signed_in && user==0){put_word(output,uint32_t(selected_local_player.xuid>>32));put_word(output+4,uint32_t(selected_local_player.xuid));ctx.r3.u64=0;return;}
    put_word(output,0);put_word(output+4,0);ctx.r3.u64=user<4?0x80070525:0x80070057;
}
ME_IMPORT(__imp__XamUserGetSigninState) {
    if(!gpu_explore_mode)me_stop_import("signin_profile_not_configured",ctx);
    static unsigned calls=0;if(++calls<=4)std::printf("{\"event\":\"signin_profile_absent\",\"user\":%u,\"state\":0}\n",ctx.r3.u32);
    ctx.r3.u64=local_player_signed_in && ctx.r3.u32==0 ? 1 : 0; // Local only, never Xbox Live.
}
static MeAchievementEnumerators achievement_enumerators;
static std::vector<MeAchievement> title_achievements;
static uint32_t title_achievement_language=0;
static uint32_t module_title_id(){uint32_t info=0;return module_headers.lookup(0x40006,info)?load_word(info+12):0;}
// Reads the title XDBF resource (named by the title id) from the mapped image once.
static bool load_title_achievements(){
    if(!title_achievements.empty())return true;
    const auto title=module_title_id();uint32_t table=0;if(!title || !module_headers.lookup(0x2ff,table))return false;
    char name[9];std::snprintf(name,sizeof(name),"%08X",title);
    const auto length=load_word(table);if(length<4 || (length-4)%16)return false;
    for(uint32_t at=4;at<length;at+=16){const auto entry=table+at;
        if(std::memcmp(guest+entry,name,8))continue;
        const auto address=load_word(entry+8),size=load_word(entry+12);MeXdbf db;
        if(!image_span_mapped(address,size) || !db.open(guest+address,size))return false;
        std::vector<MeAchievement> items;uint32_t language=0;
        if(!db.achievements(guest_console_language(),items,language) || items.empty())return false;
        title_achievements=std::move(items);title_achievement_language=language;
        std::printf("{\"event\":\"title_achievements_loaded\",\"title\":\"%s\",\"count\":%zu,\"language\":%u,\"unlocked\":0}\n",name,title_achievements.size(),language);
        return true;
    }
    return false;
}
// XUserSetContextEx values of the signed-in local user: session presence state,
// kept in memory only and never sent anywhere.
static std::vector<std::pair<uint32_t,uint32_t>> local_player_contexts;
static void xgi_write_achievements(PPCContext& ctx,uint32_t overlapped,uint32_t buffer,uint32_t length);
static void xgi_unsupported_message(PPCContext& ctx,uint32_t message,uint32_t overlapped,uint32_t length);
ME_IMPORT(__imp__XMsgStartIORequest) {
    const auto app=ctx.r3.u32,message=ctx.r4.u32,overlapped=ctx.r5.u32,buffer=ctx.r6.u32,length=ctx.r7.u32;
    if(!gpu_explore_mode || app!=0xfb)me_stop_import("xmsg_io_app_or_message_unsupported",ctx);
    if(message==0xb0008){xgi_write_achievements(ctx,overlapped,buffer,length);return;}
    if(message!=0xb0006){xgi_unsupported_message(ctx,message,overlapped,length);return;}
    if(overlapped)me_stop_import("xgi_set_context_overlapped_not_implemented",ctx);
    if(length!=24 || !checked_span(buffer,24,true))me_stop_import("xgi_set_context_buffer_invalid",ctx);
    // Caller sub_829F9A78 layout: user, unwritten word, zero qword, context id, value.
    const auto user=load_word(buffer),id=load_word(buffer+16),value=load_word(buffer+20);
    auto fail=[&](uint32_t error){put_word(thread_address+0x160,error);ctx.r3.u64=0x80070000u|error;};
    if(user>=4){fail(87);return;}
    if(!local_player_signed_in || user){
        static unsigned absent=0;if(++absent<=8)std::printf("{\"event\":\"user_context_absent_user\",\"user\":%u,\"context\":\"%08x\",\"result\":\"80070525\"}\n",user,id);
        fail(0x525);return;
    }
    auto found=std::find_if(local_player_contexts.begin(),local_player_contexts.end(),[&](const auto& c){return c.first==id;});
    if(found==local_player_contexts.end()){
        if(local_player_contexts.size()>=64)me_stop_import("user_context_capacity_exceeded",ctx);
        local_player_contexts.push_back({id,value});
    }else found->second=value;
    put_word(thread_address+0x160,0);ctx.r3.u64=0;
    static unsigned calls=0;if(++calls<=16)std::printf("{\"event\":\"user_context_set\",\"user\":0,\"context\":\"%08x\",\"value\":%u}\n",id,value);
}
static void load_local_unlocks();
static uint64_t local_unlock_filetime(uint32_t id);
ME_IMPORT(__imp__XamUserCreateAchievementEnumerator) {
    if(!gpu_explore_mode)me_stop_import("achievement_profile_service_unavailable",ctx);
    const auto title=ctx.r3.u32,user=ctx.r4.u32,flags=ctx.r6.u32,offset=ctx.r7.u32,count=ctx.r8.u32,size_out=ctx.r9.u32,handle_out=ctx.r10.u32;
    const uint64_t xuid=ctx.r5.u64;
    if(user>=4 || !count || count>1024 || !word_writable(size_out) || !word_writable(handle_out)) {ctx.r3.u64=87;return;}
    if(!local_player_signed_in || user){
        // No profile for this user: preserve output arguments and never allocate a handle.
        std::printf("{\"event\":\"achievement_profile_absent\",\"user\":%u,\"count\":%u,\"result\":1317}\n",user,count);
        ctx.r3.u64=1317;return;
    }
    if(title && title!=module_title_id())me_stop_import("achievement_other_title_unsupported",ctx);
    if(xuid && xuid!=selected_local_player.xuid)me_stop_import("achievement_other_player_unsupported",ctx);
    if(!load_title_achievements())me_stop_import("achievement_title_metadata_invalid",ctx);
    load_local_unlocks();
    if(offset>title_achievements.size()){ctx.r3.u64=87;return;}
    const auto handle=achievement_enumerators.create(user,flags,count,offset);
    if(!handle)me_stop_import("achievement_enumerator_capacity_exceeded",ctx);
    put_word(size_out,count*MeAchievementEnumerators::item_size(flags));put_word(handle_out,handle);ctx.r3.u64=0;
    std::printf("{\"event\":\"achievement_enumerator_created\",\"handle\":\"%08x\",\"flags\":%u,\"offset\":%u,\"count\":%u,\"available\":%zu}\n",handle,flags,offset,count,title_achievements.size());
}
#include "save_content.h"
// Unlocked achievements of the signed-in local profile, persisted next to its saves:
// <ME_SAVE_ROOT>/<XUID>/<title id>/achievements.txt, one "<id> <unix seconds>" line each
// (read by the launcher too). Loaded once per session; rewritten through a temporary
// file on every new unlock. Self-tests switch the store off so they never touch saves.
static std::vector<uint32_t> local_player_unlocked;
static std::vector<uint64_t> local_player_unlock_times;  // unix seconds, parallel to the ids
static bool achievement_store_disabled=false,achievement_store_loaded=false;
static std::string achievement_store_path(){
    if(achievement_store_disabled || !selected_local_player.xuid || !ensure_save_storage())return {};
    char name[64];std::snprintf(name,sizeof(name),"/%016llX/%08X/achievements.txt",(unsigned long long)selected_local_player.xuid,module_title_id());
    return save_storage.root+name;
}
static void load_local_unlocks(){
    if(achievement_store_loaded)return;
    const auto path=achievement_store_path();if(path.empty())return;
    achievement_store_loaded=true;
    std::ifstream in(path);unsigned id=0;unsigned long long when=0;
    while(in>>id>>when){
        if(std::find(local_player_unlocked.begin(),local_player_unlocked.end(),id)!=local_player_unlocked.end() || local_player_unlocked.size()>=1024)continue;
        local_player_unlocked.push_back(id);local_player_unlock_times.push_back(when);
    }
    std::printf("{\"event\":\"achievements_restored\",\"count\":%zu}\n",local_player_unlocked.size());
}
static bool save_local_unlocks(){
    const auto path=achievement_store_path();if(path.empty())return false;
    const auto slash=path.find_last_of('/');::mkdir(path.substr(0,slash).c_str(),0770);
    const auto temp=path+".tmp";
    {std::ofstream out(temp,std::ios::trunc);if(!out)return false;
     for(size_t i=0;i<local_player_unlocked.size();++i)out<<local_player_unlocked[i]<<' '<<local_player_unlock_times[i]<<'\n';
     if(!out.flush())return false;}
    return std::rename(temp.c_str(),path.c_str())==0;
}
// FILETIME (100 ns since 1601) of an unlock, 0 when the id is not unlocked.
static uint64_t local_unlock_filetime(uint32_t id){
    for(size_t i=0;i<local_player_unlocked.size();++i)if(local_player_unlocked[i]==id)
        return i<local_player_unlock_times.size() && local_player_unlock_times[i] ? local_player_unlock_times[i]*10000000ull+116444736000000000ull : 116444736000000000ull;
    return 0;
}
// XGI 0xB0008 = XUserWriteAchievements (Xenia xgi_app.cc): buffer {count, XUSER_ACHIEVEMENT*},
// each achievement {user index, achievement id}. New unlocks are stored in the profile's
// achievements.txt and logged; the request completes immediately, through the XOVERLAPPED
// when the caller passed one.
static void xgi_write_achievements(PPCContext& ctx,uint32_t overlapped,uint32_t buffer,uint32_t length){
    if(length!=8 || !checked_span(buffer,8))me_stop_import("xgi_write_achievements_buffer_invalid",ctx);
    const auto count=load_word(buffer),items=load_word(buffer+4);
    if(count>256 || (count && !checked_span(items,count*8)))me_stop_import("xgi_write_achievements_list_invalid",ctx);
    save_validate_overlapped(ctx,overlapped);
    load_local_unlocks();
    for(uint32_t i=0;i<count;++i){
        const auto user=load_word(items+i*8),id=load_word(items+i*8+4);
        const bool fresh=std::find(local_player_unlocked.begin(),local_player_unlocked.end(),id)==local_player_unlocked.end();
        bool persisted=false;
        if(fresh && local_player_unlocked.size()<1024){
            local_player_unlocked.push_back(id);local_player_unlock_times.push_back(uint64_t(std::time(nullptr)));
            persisted=save_local_unlocks();
        }
        std::printf("{\"event\":\"achievement_unlocked\",\"user\":%u,\"id\":%u,\"new\":%s,\"persisted\":%s}\n",user,id,fresh?"true":"false",persisted?"true":"false");
    }
    put_word(thread_address+0x160,0);
    ctx.r3.u64=save_complete(ctx,overlapped,0);
}
// Other XGI messages: reported as not supported (ERROR_NOT_SUPPORTED, completed through the
// XOVERLAPPED if given) instead of stopping the game; nothing is pretended to succeed.
static void xgi_unsupported_message(PPCContext& ctx,uint32_t message,uint32_t overlapped,uint32_t length){
    save_validate_overlapped(ctx,overlapped);
    static unsigned logged=0;
    if(++logged<=32)std::printf("{\"event\":\"xgi_message_unsupported\",\"message\":\"%08x\",\"length\":%u,\"overlapped\":%s,\"result\":50}\n",message,length,overlapped?"true":"false");
    put_word(thread_address+0x160,50);
    ctx.r3.u64=overlapped?save_complete(ctx,overlapped,50):(0x80070000u|50);
}
ME_IMPORT(__imp__XamEnumerate) {
    const auto handle=ctx.r3.u32,flags=ctx.r4.u32,buffer=ctx.r5.u32,capacity=ctx.r6.u32,returned=ctx.r7.u32,overlapped=ctx.r8.u32;
    if(auto* content=save_enumerator(handle)){enumerate_save_content(ctx,*content);return;}
    if(!gpu_explore_mode)me_stop_import("enumerate_service_unavailable",ctx);
    auto* e=achievement_enumerators.find(handle);
    if(!e){ctx.r3.u64=6;return;} // No other enumerator kinds are created by this runtime.
    if(flags)me_stop_import("enumerate_flags_unsupported",ctx);
    if(overlapped)me_stop_import("enumerate_overlapped_not_implemented",ctx);
    if(!returned){ctx.r3.u64=87;return;}
    if(!word_writable(returned))me_stop_import("enumerate_count_output_invalid",ctx);
    put_word(returned,0);
    if(!buffer){ctx.r3.u64=87;return;}
    const uint64_t needed=uint64_t(e->per_call)*MeAchievementEnumerators::item_size(e->flags);
    if(capacity>=needed && !checked_span(buffer,uint32_t(needed)))me_stop_import("enumerate_buffer_invalid",ctx);
    uint32_t written=0;
    const auto result=MeAchievementEnumerators::write(*e,title_achievements,guest+buffer,buffer,capacity,written,local_unlock_filetime);
    if(!result)put_word(returned,written);
    ctx.r3.u64=result;
    static unsigned calls=0;if(++calls<=16)std::printf("{\"event\":\"achievement_enumerate\",\"handle\":\"%08x\",\"written\":%u,\"result\":%u}\n",handle,written,result);
}
static bool me_input_user(uint32_t user,uint32_t flags){return ((user&255)==255 || user==0 || (flags&0x40000000u)) && (!(flags&255) || (flags&1));}
ME_IMPORT(__imp__XamInputGetCapabilities) {
 if(!gpu_explore_mode)me_stop_import("input_capabilities_provider_not_connected",ctx);
 const auto out=ctx.r5.u32;if(!out){ctx.r3.u64=160;return;}
 auto state=me_guest_input().poll();
 if(!state.connected || !me_input_user(ctx.r3.u32,ctx.r4.u32)){ctx.r3.u64=1167;return;}
 if(!checked_span(out,20,true)){ctx.r3.u64=160;return;}
 std::memset(guest+out,0,20);guest[out]=1;guest[out+1]=1; // gamepad, no vibration advertised
 guest[out+4]=0xf3;guest[out+5]=0xff;guest[out+6]=guest[out+7]=255;
 for(unsigned i=8;i<16;i+=2){guest[out+i]=0x7f;guest[out+i+1]=0xff;}
 ctx.r3.u64=0;
}
ME_IMPORT(__imp__XamInputGetState) {
 if(!gpu_explore_mode)me_stop_import("xam_input_provider_not_connected",ctx);
 if(!me_input_user(ctx.r3.u32,ctx.r4.u32)){ctx.r3.u64=1167;return;}
 auto state=me_guest_input().poll(true);
 if(!state.connected || !me_input_user(ctx.r3.u32,ctx.r4.u32)){ctx.r3.u64=1167;return;}
 const auto out=ctx.r5.u32;
 if(out){if(!checked_span(out,16,true)){ctx.r3.u64=160;return;}
 put_word(out,state.sequence);guest[out+4]=uint8_t(state.buttons>>8);guest[out+5]=uint8_t(state.buttons);guest[out+6]=state.lt;guest[out+7]=state.rt;
 for(unsigned i=0;i<4;++i){const auto value=uint16_t(state.axes[i]);guest[out+8+i*2]=uint8_t(value>>8);guest[out+9+i*2]=uint8_t(value);}}
 static uint16_t last=0;static unsigned logs=0;
 if(state.buttons!=last && logs++<32){std::printf("{\"event\":\"guest_input_state\",\"buttons\":%u,\"sequence\":%u}\n",state.buttons,state.sequence);last=state.buttons;}
 ctx.r3.u64=0;
}
void me_dcbst_diagnostic(uint32_t address) {
    static MeImportCounter counter("hook_dcbst"); counter.calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t line=address&~127u;
    // Mapping check without the kernel lock where possible: physical aliases use the
    // atomic committed-page map; virtual lines are checked under the lock only when
    // serialized (parallel mode: unchecked, the fence is all dcbst needs here).
    bool mapped=polling_yield_experiment;
    if(mapped && line>=0xa0000000u)mapped=me_gpu_write_watch.range_committed(MePhysicalMemory::physical(line),128);
    else if(mapped && (!guest_parallel || execution_gate.owned())){MeKernelLock kernel_lock;mapped=virtual_memory.committed_cache_line(line);}
    if(!mapped){
        std::fprintf(stderr,"{\"event\":\"dcbst_mapping_unsupported\",\"address\":\"%08x\"}\n",address);std::_Exit(98);
    }
    // Only coherent ordinary CPU VM is accepted. No Vulkan/device cache,
    // instruction cache, or GPU completion semantics are claimed here.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    static std::atomic<unsigned> lines{0};if(++lines<=4)std::printf("{\"event\":\"dcbst_coherent_vm\",\"line\":\"%08x\",\"bytes\":128}\n",line);
}
static uint32_t xmp_playback_client=1; // SDK default: title.
ME_IMPORT(__imp__XMsgStartIORequestEx) {
    if(!gpu_explore_mode || ctx.r3.u32!=0xfa || ctx.r4.u32!=0x7001a || ctx.r5.u32 || ctx.r7.u32!=12)
        me_stop_import("xmp_set_message_or_overlapped_unsupported",ctx);
    const auto args=ctx.r6.u32,extra=ctx.r8.u32;
    if(!checked_span(args,12,true) || (extra && !checked_span(extra,8,true)))me_stop_import("xmp_set_argument_range_invalid",ctx);
    const auto client=load_word(args),controller=load_word(args+4),playback=load_word(args+8);
    if(!((client==2 && controller==0)||(client==0 && controller==1)) || playback>1)
        me_stop_import("xmp_set_controller_values_unsupported",ctx);
    const int recipients=notifications.broadcast(0x0a000003,!playback);
    if(recipients<0)me_stop_import("notification_queue_capacity_exceeded",ctx);
    const auto previous=xmp_playback_client;xmp_playback_client=playback;
    put_word(thread_address+0x160,0);ctx.r3.u64=0;state_changed.notify_all();
    std::printf("{\"event\":\"xmp_playback_changed\",\"previous\":%u,\"client\":%u,\"notification\":\"0a000003\",\"data\":%u,\"listeners\":%d,\"overlapped\":false}\n",previous,playback,!playback,recipients);
}
ME_IMPORT(__imp__XMsgInProcessCall) {
    if(!gpu_explore_mode || ctx.r3.u32!=0xfa || ctx.r4.u32!=0x7001b ||
       (ctx.r6.u32 && ctx.r6.u32!=12))me_stop_import("xmsg_app_or_message_unsupported",ctx);
    const auto args=ctx.r5.u32;
    if(!checked_span(args,12,true) || load_word(args)!=2)me_stop_import("xmp_get_controller_arguments_unsupported",ctx);
    const auto controller=load_word(args+4),locked=load_word(args+8);
    if(!word_writable(controller) || !word_writable(locked))me_stop_import("xmp_get_controller_output_invalid",ctx);
    // No external/system music client is registered in this diagnostic profile.
    put_word(controller,0);put_word(locked,0);ctx.r3.u64=0;
    std::puts("{\"event\":\"xmp_playback_controller\",\"controller\":0,\"locked\":false}");
    if(thread_address!=0x61001000){
        std::unique_lock<MeExecutionGate> gate(execution_gate,std::adopt_lock);
        gate.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(10));gate.lock();gate.release();
    }
}
ME_IMPORT(__imp__XamNotifyCreateListener) {
    if(ctx.r4.u32>10)me_stop_import("notification_version_unsupported",ctx);
    const auto mask=ctx.r3.u64;ctx.r3.u64=notifications.create(mask,ctx.r4.u32);
    std::printf("{\"event\":\"notification_listener_created\",\"mask\":\"%016llx\",\"handle\":\"%08x\"}\n",(unsigned long long)mask,ctx.r3.u32);
}
ME_IMPORT(__imp__XNotifyGetNext) {
    const auto handle=ctx.r3.u32,match=ctx.r4.u32,id_out=ctx.r5.u32,data_out=ctx.r6.u32;
    if((id_out && !word_writable(id_out)) || (data_out && !word_writable(data_out)))me_stop_import("notification_outputs_invalid",ctx);
    if(data_out)put_word(data_out,0);
    if(!id_out){ctx.r3.u64=0;return;}put_word(id_out,0);
    uint32_t id=0,data=0;const bool found=notifications.pop(handle,match,id,data);
    if(found){put_word(id_out,id);if(data_out)put_word(data_out,data);}ctx.r3.u64=found;
}
ME_IMPORT(__imp__NtCreateMutant) {
    if(ctx.r4.u32)me_stop_import("named_mutant_attributes_not_implemented",ctx);
    if(!word_writable(ctx.r3.u32) || ctx.r5.u32>1)me_stop_import("mutant_create_invalid_parameters",ctx);
    const auto output=ctx.r3.u32;uint32_t handle=0;
    const auto status=mutants.create(ctx.r5.u32?thread_address:0,handle);
    if(!status)put_word(output,handle);ctx.r3.u64=status;
    std::printf("{\"event\":\"mutant_create\",\"handle\":\"%08x\",\"status\":\"%08x\"}\n",handle,status);
}
ME_IMPORT(__imp__NtReleaseMutant) {
    if(ctx.r4.u32)me_stop_import("mutant_release_flags_unsupported",ctx);
    ctx.r3.u64=mutants.release(ctx.r3.u32,thread_address);state_changed.notify_all();
}
ME_IMPORT(__imp__NtClose) {
    if(save_storage.find(ctx.r3.u32)){ctx.r3.u64=save_storage.close(ctx.r3.u32);return;}
    if(auto* content=save_enumerator(ctx.r3.u32)){content->live=false;ctx.r3.u64=0;return;}
    if(notifications.find(ctx.r3.u32)){ctx.r3.u64=notifications.close(ctx.r3.u32)?0:0xc0000008;return;}
    if(achievement_enumerators.find(ctx.r3.u32)){achievement_enumerators.close(ctx.r3.u32);ctx.r3.u64=0;return;}
    if(auto* m=mutants.find(ctx.r3.u32)){if(m->owner)me_stop_import("mutant_close_while_owned_not_implemented",ctx);ctx.r3.u64=mutants.close(ctx.r3.u32);return;}
    if (readonly_files.find(ctx.r3.u32)) { ctx.r3.u64 = readonly_files.close(ctx.r3.u32) ? 0 : 0xc0000001; return; }
    if (auto t = find_thread(ctx.r3.u32)) { t->closed = true; ctx.r3.u64 = 0; }
    else ctx.r3.u64 = events.close(ctx.r3.u32);
}
// Embedded dispatcher events use guest memory as the authoritative state.
// Types 0/1 are notification/manual and synchronization/auto-reset events.
static uint32_t checked_kernel_event(uint32_t address, PPCContext& ctx) {
    if ((address & 3) || !checked_span(address, 16) || guest[address] > 1)
        me_stop_import("kernel_event_invalid_address_or_type", ctx);
    if (load_word(address + 4) > 1) me_stop_import("kernel_event_invalid_signal_state", ctx);
    return address;
}
ME_IMPORT(__imp__KeEnterCriticalRegion) {
    if (ctx.r13.u32 != pcr_address) me_stop_import("critical_region_context_invalid", ctx);
    const int32_t count = int32_t(load_word(thread_address + 0xb0));
    if (count == INT32_MIN) me_stop_import("critical_region_nesting_overflow", ctx);
    put_word(thread_address + 0xb0, uint32_t(count - 1));
}
ME_IMPORT(__imp__KeLeaveCriticalRegion) {
    if (ctx.r13.u32 != pcr_address) me_stop_import("critical_region_context_invalid", ctx);
    const int32_t count = int32_t(load_word(thread_address + 0xb0));
    if (count >= 0) me_stop_import("critical_region_unbalanced_leave", ctx);
    put_word(thread_address + 0xb0, uint32_t(count + 1));
    // APC insertion/delivery imports remain unsupported; no APC is manufactured.
}
static uint32_t checked_kernel_semaphore(uint32_t address, PPCContext& ctx) {
    if ((address & 3) || !checked_span(address, 20) || guest[address] != 5 ||
        !load_word(address + 16) || load_word(address + 16) > INT32_MAX ||
        load_word(address + 4) > load_word(address + 16))
        me_stop_import("kernel_semaphore_invalid", ctx);
    return address;
}
ME_IMPORT(__imp__KeInitializeSemaphore) {
    const auto address = ctx.r3.u32, count = ctx.r4.u32, limit = ctx.r5.u32;
    if ((address & 3) || !checked_span(address, 20) || !limit || limit > INT32_MAX || count > limit)
        me_stop_import("kernel_semaphore_initialize_invalid", ctx);
    std::memset(guest + address, 0, 20);
    guest[address] = 5; put_word(address + 4, count); put_word(address + 16, limit);
    std::printf("{\"event\":\"kernel_semaphore_initialized\",\"address\":\"%08x\",\"count\":%u,\"limit\":%u}\n", address, count, limit);
}
ME_IMPORT(__imp__KeReleaseSemaphore) {
    const auto address = checked_kernel_semaphore(ctx.r3.u32, ctx);
    const auto previous = load_word(address + 4), adjustment = ctx.r5.u32;
    if (ctx.r6.u32 || !adjustment || adjustment > INT32_MAX ||
        uint64_t(previous) + adjustment > load_word(address + 16))
        me_stop_import("kernel_semaphore_release_limit_or_handoff_unsupported", ctx);
    // Priority boost is a guest hint; no host scheduling priority is changed.
    put_word(address + 4, previous + adjustment);
    state_changed.notify_all(); ctx.r3.u64 = previous;
}
ME_IMPORT(__imp__KeInitializeEvent) {
    const auto address = ctx.r3.u32;
    if ((address & 3) || !checked_span(address, 16) || ctx.r4.u32 > 1)
        me_stop_import("kernel_event_initialize_invalid", ctx);
    std::memset(guest + address, 0, 16);
    guest[address] = uint8_t(ctx.r4.u32); put_word(address + 4, ctx.r5.u32 ? 1 : 0);
}
ME_IMPORT(__imp__KeSetEvent) {
    if (ctx.r5.u32) me_stop_import("kernel_event_set_wait_handoff_unsupported", ctx);
    const auto address = checked_kernel_event(ctx.r3.u32, ctx);
    const auto previous = load_word(address + 4);
    put_word(address + 4, 1); state_changed.notify_all(); ctx.r3.u64 = previous;
}
ME_IMPORT(__imp__KeResetEvent) {
    const auto address = checked_kernel_event(ctx.r3.u32, ctx);
    const auto previous = load_word(address + 4); put_word(address + 4, 0); ctx.r3.u64 = previous;
}
ME_IMPORT(__imp__KeClearEvent) {
    const auto address = checked_kernel_event(ctx.r3.u32, ctx); put_word(address + 4, 0);
}
ME_IMPORT(__imp__KeWaitForSingleObject) {
    const auto address = ctx.r3.u32, timeout = ctx.r7.u32;
    if (ctx.r5.u32 > 1 || ctx.r6.u32) me_stop_import("kernel_wait_mode_or_apc_unsupported", ctx);
    if (timeout && !checked_span(timeout, 8, true)) me_stop_import("kernel_wait_timeout_invalid", ctx);
    const uint64_t raw = timeout ? (uint64_t(load_word(timeout)) << 32) | load_word(timeout + 4) : 0;
    if (timeout && (int64_t(raw) > 0 || (0 - raw) > 600000000ull))
        me_stop_import("kernel_wait_absolute_or_long_timeout_unsupported", ctx);
    std::shared_ptr<ThreadRecord> thread;
    for (const auto& item : threads) if (item->kthread == address) thread = item;
    const bool semaphore = !thread && checked_span(address, 20) && guest[address] == 5;
    if (semaphore) checked_kernel_semaphore(address, ctx);
    else if (!thread) checked_kernel_event(address, ctx);
    auto ready = [&] {
        if (thread) return thread->done;
        if (semaphore) {
            checked_kernel_semaphore(address, ctx);
            const auto count = load_word(address + 4);
            if (!count) return false;
            put_word(address + 4, count - 1); return true;
        }
        checked_kernel_event(address, ctx);
        if (!load_word(address + 4)) return false;
        if (guest[address] == 1) put_word(address + 4, 0);
        return true;
    };
    uint32_t status = 0;
    if (!ready()) {
        if (me_log_verbose()) std::printf("{\"event\":\"kernel_object_wait_begin\",\"object\":\"%08x\",\"thread\":\"%08x\"}\n",address,thread_address);
        if (timeout && !raw) status = 0x102;
        else if (!timeout) wait_for_change(ready);
        else {
            std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock);
            if (!state_changed.wait_for(gate, std::chrono::nanoseconds((0 - raw) * 100), ready)) status = 0x102;
            gate.release();
        }
    }
    ctx.r3.u64 = status;
}
ME_IMPORT(__imp__KeWaitForMultipleObjects) {
    const auto count = ctx.r3.u32, array = ctx.r4.u32, type = ctx.r5.u32, timeout = ctx.r9.u32;
    if (!count || count > 64 || type > 1 || ctx.r7.u32 > 1 || ctx.r8.u32 ||
        !checked_span(array, count * 4, true) || (timeout && !checked_span(timeout, 8, true)))
        me_stop_import("multiple_wait_invalid_or_alertable_unsupported", ctx);
    const uint64_t raw = timeout ? (uint64_t(load_word(timeout)) << 32) | load_word(timeout + 4) : 0;
    if (timeout && (int64_t(raw) > 0 || (0 - raw) > 600000000ull))
        me_stop_import("multiple_wait_absolute_or_long_timeout_unsupported", ctx);
    struct Object { uint32_t address; std::shared_ptr<ThreadRecord> thread; };
    std::vector<Object> objects;
    for (unsigned i = 0; i < count; ++i) {
        const auto address = load_word(array + i * 4);
        for (const auto& object : objects) if (object.address == address)
            me_stop_import("multiple_wait_duplicate_object_unsupported", ctx);
        auto thread = thread_by_pointer(address);
        if (!thread) {
            if (checked_span(address, 20) && guest[address] == 5) checked_kernel_semaphore(address, ctx);
            else checked_kernel_event(address, ctx);
        }
        objects.push_back({address, thread});
    }
    uint32_t status = 0;
    auto available = [&](const Object& object) {
        if (object.thread) return object.thread->done;
        if (guest[object.address] == 5) checked_kernel_semaphore(object.address, ctx);
        else checked_kernel_event(object.address, ctx);
        return load_word(object.address + 4) != 0;
    };
    auto consume = [&](const Object& object) {
        if (object.thread) return;
        const auto address = object.address;
        if (guest[address] == 5) put_word(address + 4, load_word(address + 4) - 1);
        else if (guest[address] == 1) put_word(address + 4, 0);
    };
    auto ready = [&] {
        if (type == 0) { // WaitAll: check every object before consuming anything.
            for (const auto& object : objects) if (!available(object)) return false;
            for (const auto& object : objects) consume(object);
            status = 0; return true;
        }
        for (unsigned i = 0; i < count; ++i) if (available(objects[i])) {
            consume(objects[i]); status = i; return true;
        }
        return false;
    };
    if (!ready()) {
        if (me_log_verbose()) std::printf("{\"event\":\"kernel_multiple_wait_begin\",\"count\":%u,\"wait_all\":%s,\"thread\":\"%08x\"}\n", count, type == 0 ? "true":"false", thread_address);
        if (timeout && !raw) status = 0x102;
        else if (!timeout) wait_for_change(ready);
        else {
            std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock);
            if (!state_changed.wait_for(gate, std::chrono::nanoseconds((0 - raw) * 100), ready)) status = 0x102;
            gate.release();
        }
    }
    ctx.r3.u64 = status;
}


ME_IMPORT(__imp__NetDll_WSAStartup) {
    if(!gpu_explore_mode || ctx.r3.u32!=1)me_stop_import("wsa_startup_requires_offline_explore",ctx);
    // WSASYSNOTREADY: no sockets backend was initialized. Failed startup
    // leaves WSADATA and thread last-error unchanged.
    ctx.r3.u64=checked_span(ctx.r5.u32,0x194)?10091:10014;
    std::puts("{\"event\":\"wsa_unavailable\",\"host_sockets_created\":false}");
}

ME_IMPORT(__imp__InterlockedPopEntrySList) {
    const auto header=ctx.r3.u32;
    if((header&7) || !checked_span(header,8))me_stop_import("slist_header_invalid",ctx);
    auto* cell=reinterpret_cast<uint64_t*>(guest+header);
    auto raw=__atomic_load_n(cell,__ATOMIC_SEQ_CST);
    for(;;){
        const auto value=__builtin_bswap64(raw);const uint32_t head=uint32_t(value>>32);
        if(!head){ctx.r3.u64=0;return;}
        if((head&3) || !checked_span(head,4,true))me_stop_import("slist_entry_invalid",ctx);
        const uint32_t next=load_word(head);const uint16_t depth=uint16_t(value>>16),sequence=uint16_t(value);
        const uint64_t desired=(uint64_t(next)<<32)|(uint64_t(uint16_t(depth-1))<<16)|sequence;
        if(__atomic_compare_exchange_n(cell,&raw,__builtin_bswap64(desired),false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){
            ctx.r3.u64=head;return;
        }
    }
}
ME_IMPORT(__imp__InterlockedFlushSList) {
    const auto header=ctx.r3.u32;
    if((header&7) || !checked_span(header,8))me_stop_import("slist_flush_header_invalid",ctx);
    // The pinned Xbox SDK implementation resets next/depth/sequence together.
    const auto raw=__atomic_exchange_n(reinterpret_cast<uint64_t*>(guest+header),uint64_t(0),__ATOMIC_SEQ_CST);
    ctx.r3.u64=uint32_t(__builtin_bswap64(raw)>>32);
}
ME_IMPORT(__imp__NetDll_XNetGetTitleXnAddr) {
    if(!gpu_explore_mode || ctx.r3.u32!=1)me_stop_import("title_address_caller_unsupported",ctx);
    const auto output=ctx.r4.u32;
    if(!output || !checked_span(output,36))me_stop_import("title_address_output_invalid",ctx);
    // No guest title-address acquisition subsystem exists in this offline
    // backend. NONE is final, unlike PENDING; do not synthesize MAC/online IDs.
    std::memset(guest+output,0,36);ctx.r3.u64=1; // XNET_GET_XNADDR_NONE
    static unsigned calls=0;if(++calls<=8)std::puts("{\"event\":\"guest_title_address_unavailable\",\"status\":1,\"pending\":false,\"online\":false}");
}
ME_IMPORT(__imp__NetDll_XNetGetEthernetLinkStatus) {
    if(!gpu_explore_mode || ctx.r3.u32!=1)me_stop_import("ethernet_status_caller_unsupported",ctx);
    // The guest network backend only owns loopback. No external guest link,
    // speed or duplex flags exist, independently of Android Wi-Fi/Bluetooth.
    ctx.r3.u64=0;
    static unsigned calls=0;if(++calls<=8)std::puts("{\"event\":\"guest_ethernet_status\",\"flags\":0,\"backend\":\"offline_loopback\",\"xbox_live\":false}");
}
ME_IMPORT(__imp__NetDll_XNetStartup) {
    if(!gpu_explore_mode || ctx.r3.u32!=1)me_stop_import("xnet_startup_requires_offline_explore",ctx);
    const auto params=ctx.r4.u32;
    if(params && (!checked_span(params,13,true) || guest[params]!=13)){ctx.r3.u64=10022;return;}
    const bool ready=offline_network.start(params?guest+params:nullptr);
    ctx.r3.u64=ready?0:10091;
    std::printf("{\"event\":\"xnet_local_service\",\"ready\":%s,\"loopback_port\":%u,\"external_connection\":false,\"xbox_live\":false}\n",ready?"true":"false",offline_network.port);
}

// Current scene for the session log: the game streams level packages named
// <Chapter>_<Area>[_Part].xxx (Escape_R1, Escape_R1_Art...). The scene is the chapter
// plus area (Escape_R1), or the bare package name for menus (TdMainMenu); shared
// packages (Textures, CharTextures, startup and engine packages, *_LOC_*) are ignored.
static std::mutex scene_mutex;
static char current_scene[48] = "startup";
extern "C" void me_current_scene(char* out, size_t capacity) {
    std::lock_guard<std::mutex> lock(scene_mutex);
    std::snprintf(out, capacity, "%s", current_scene);
}
static void note_scene_from_path(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".xxx")) return;
    name.resize(name.size() - 4);
    static const char* shared[] = {"Textures", "CharTextures", "GuidCache", "Startup", "TdGame", "GameFramework",
                                   "IpDrv", "Engine", "Core", "Ts", "Tp", "Tk", "TdEditor", "TdSharedContent"};
    for (const char* s : shared) if (!name.compare(0, std::strlen(s), s) && (name.size() == std::strlen(s) || name[std::strlen(s)] == '_')) return;
    if (name.find("_LOC_") != std::string::npos) return;
    std::string scene = name.substr(0, name.find('_'));
    const auto first = name.find('_');
    if (first != std::string::npos) {
        const auto second = name.find('_', first + 1);
        const std::string area = name.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
        if (area.size() >= 2 && (area[0] == 'R' || area[0] == 'r') && std::isdigit(static_cast<unsigned char>(area[1]))) scene += "_" + area;
    }
    std::lock_guard<std::mutex> lock(scene_mutex);
    if (scene != current_scene) {
        std::snprintf(current_scene, sizeof(current_scene), "%s", scene.c_str());
        std::printf("{\"event\":\"scene\",\"name\":\"%s\",\"package\":\"%s\"}\n", current_scene, name.c_str());
    }
}
// Xenia "Dynamic Lighting & Shadows Shading Fix" (apply_xenia_patches.py) is on by
// default; ME_SHADING_FIX=0 runs the game's original store instead (A/B).
// Xenia "Unlock FPS" patch (apply_xenia_patches.py), on unless ME_UNLOCK_FPS=0: off runs the
// game's original frame pacing code (valve wheel / bone attachment A/B, crash lr 8249b888).
extern "C" bool me_unlock_fps() {
    static const bool on = [] {
        const char* v = std::getenv("ME_UNLOCK_FPS"); const bool enabled = !(v && !std::strcmp(v, "0"));
        std::printf("{\"event\":\"xenia_patch\",\"name\":\"unlock_fps\",\"enabled\":%s}\n", enabled ? "true" : "false");
        return enabled;
    }();
    return on;
}
extern "C" bool me_shading_fix() {
    static const bool on=[]{const char* v=std::getenv("ME_SHADING_FIX");return !(v && !std::strcmp(v,"0"));}();
    return on;
}
// Xenia's Coalesced hash-check patch is enabled only for the explicit private
// shadow A/B input. Never alter the game's normal verification path by default.
extern "C" bool me_diagnostic_coalesced_enabled() {
    static const bool on=[]{const char* v=std::getenv("ME_DIAGNOSTIC_COALESCED");return v && *v;}();
    return on;
}
// One-frame copy of a Bink 1 movie (skip-intro setting): same header and audio track
// descriptors, frame count 1, a two-entry frame index and the first frame's data, so the
// player opens a valid movie that ends at once. Written once to <ME_CACHE_ROOT>; empty
// string if the source is not a Bink file this understands.
static std::string me_one_frame_movie(const std::string& source){
    const char* cache=std::getenv("ME_CACHE_ROOT");
    const std::string target=std::string(cache?cache:"/data/local/tmp/me_cache")+"/startupmovie-1frame.bik";
    struct stat existing{};
    if(::stat(target.c_str(),&existing)==0 && existing.st_size>64)return target;
    std::ifstream in(source,std::ios::binary);
    std::vector<uint8_t> head(44);
    if(!in.read(reinterpret_cast<char*>(head.data()),44) || std::memcmp(head.data(),"BIK",3))return {};
    auto le=[](const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};
    auto put=[](uint8_t* p,uint32_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);p[2]=uint8_t(v>>16);p[3]=uint8_t(v>>24);};
    const uint32_t frames=le(&head[8]),tracks=le(&head[40]);
    if(!frames || tracks>8)return {};
    const uint32_t index_at=44+tracks*12;  // per track: max decoded size, rate+flags, id
    head.resize(index_at+8);
    if(!in.read(reinterpret_cast<char*>(head.data()+44),index_at+8-44))return {};
    const uint32_t first=le(&head[index_at]),next=le(&head[index_at+4]);
    const uint32_t start=first&~1u,end=next&~1u;
    if(end<=start || end-start>(16u<<20))return {};
    std::vector<uint8_t> frame(end-start);
    in.seekg(start);
    if(!in.read(reinterpret_cast<char*>(frame.data()),frame.size()))return {};
    const uint32_t frame_at=index_at+8,total=frame_at+uint32_t(frame.size());
    put(&head[4],total-8);put(&head[8],1);put(&head[12],uint32_t(frame.size()));put(&head[16],1);
    put(&head[index_at],frame_at|(first&1u));put(&head[index_at+4],total);
    ::mkdir(target.substr(0,target.find_last_of('/')).c_str(),0770);
    const std::string temp=target+".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);
     out.write(reinterpret_cast<const char*>(head.data()),head.size());out.write(reinterpret_cast<const char*>(frame.data()),frame.size());
     if(!out.flush())return {};}
    if(std::rename(temp.c_str(),target.c_str()))return {};
    std::printf("{\"event\":\"one_frame_movie_written\",\"source_frames\":%u,\"bytes\":%u}\n",frames,total);
    return target;
}
#include "save_file_io.h"
ME_IMPORT(__imp__NtCreateFile) {
    if(!checked_span(ctx.r1.u32+84,4,true))me_stop_import("file_create_stack_invalid",ctx);
    if(save_open_guest(ctx,ctx.r10.u32,load_word(ctx.r1.u32+84)))return;
    const auto attributes = ctx.r5.u32;
    if (!checked_span(attributes,12,true) || !checked_span(ctx.r1.u32+84,4,true)) me_stop_import("create_file_attributes_invalid",ctx);
    const auto name = load_word(attributes+4);
    if (!checked_span(name,8,true)) me_stop_import("create_file_name_invalid",ctx);
    const auto length = load_word(name)>>16, pointer = load_word(name+4);
    if (length>1024 || !checked_span(pointer,length,true)) me_stop_import("create_file_path_invalid",ctx);
    std::string escaped;
    for(unsigned i=0;i<length;++i){auto c=guest[pointer+i];if(c<32||c>126)me_stop_import("create_file_path_not_ascii",ctx);if(c==92||c==34)escaped.push_back(char(92));escaped.push_back(char(c));}
    note_scene_from_path(std::string(reinterpret_cast<const char*>(guest + pointer), length));
    std::printf("{\"event\":\"file_open_probe\",\"root\":\"%08x\",\"path\":\"%s\",\"access\":\"%08x\",\"disposition\":%u,\"options\":\"%08x\"}\n",load_word(attributes),escaped.c_str(),ctx.r4.u32,ctx.r10.u32,load_word(ctx.r1.u32+84));

    const auto output = ctx.r3.u32, iosb = ctx.r6.u32;
    if (!word_writable(output) || !checked_span(iosb,8) || !checked_span(ctx.r1.u32+84,4,true))
        me_stop_import("file_open_output_or_stack_invalid",ctx);

    // NO_INTERMEDIATE_BUFFERING is accepted for the read-only staged backend;
    // no host O_DIRECT/DMA or Xbox cache coherency is claimed.
    const auto options=load_word(ctx.r1.u32+84);
    std::string path(reinterpret_cast<char*>(guest+pointer),length);
    if(path=="\\Device\\Harddisk0\\partition0" && load_word(attributes)==0) {
        put_word(output,UINT32_MAX);put_word(iosb,0xc000000e);put_word(iosb+4,0);ctx.r3.u64=0xc000000e;
        std::puts("{\"event\":\"unmounted_xbox_raw_partition\",\"status\":\"c000000e\"}");return;
    }

    if ((load_word(attributes)!=0 && load_word(attributes)!=0xfffffffd) || ctx.r10.u32!=1 ||
        ctx.r7.u32 || (ctx.r4.u32 & ~0x80100080u) || !(ctx.r4.u32 & 0x80000000u) || (options & ~0x6cu))
        me_stop_import("file_open_only_read_existing_supported",ctx);
    std::string relative;
    if(!MeReadOnlyFiles::relative(path,relative))me_stop_import("file_open_volume_or_path_unsupported",ctx);
    // Launcher setting "skip intro" (ME_SKIP_INTRO=1): the startup movie (the engine's
    // [FullScreenMovie] StartupMovies=StartupMovie, also preloaded via [LoadMovies]) is
    // served as a one-frame copy made from the user's own file. Reporting it absent is not
    // an option: the game then raises the dirty-disc error (XamShowDirtyDiscErrorUI).
    static const bool skip_intro=[]{const char* v=std::getenv("ME_SKIP_INTRO");return v && !std::strcmp(v,"1");}();
    uint32_t handle=0;
    if(skip_intro && relative=="tdgame/movies/startupmovie.bik"){
        const auto movie=me_one_frame_movie(readonly_files.root+"/"+relative);
        if(!movie.empty())handle=readonly_files.open_host(movie);
        std::printf("{\"event\":\"startup_movie_skipped\",\"one_frame_copy\":%s}\n",handle?"true":"false");
    }
    // Diagnostic A/B only: load a separately generated Coalesced.ini copy.
    // The original user file remains untouched. The matching static-recomp patch
    // bypasses the game's Coalesced hash check only in this explicit mode.
    if(!handle && relative=="tdgame/config/xenon/cooked/coalesced.ini"){
        if(const char* probe=std::getenv("ME_DIAGNOSTIC_COALESCED"); probe && *probe){
            handle=readonly_files.open_host(probe);
            if(!handle)me_stop_import("diagnostic_coalesced_open_failed",ctx);
            std::puts("{\"event\":\"diagnostic_coalesced_loaded\"}");
        }
    }
    if(!handle)handle=readonly_files.open(path);

    if(!handle){
        if(errno==ENOENT){put_word(output,UINT32_MAX);put_word(iosb,0xc000000f);put_word(iosb+4,0);ctx.r3.u64=0xc000000f;return;}
        me_stop_import("file_open_not_staged_or_host_error",ctx);
    }

    put_word(output,handle);put_word(iosb,0);put_word(iosb+4,1);ctx.r3.u64=0;
    std::printf("{\"event\":\"file_opened_readonly\",\"handle\":\"%08x\",\"bytes\":%llu}\n",handle,(unsigned long long)readonly_files.find(handle)->size);

}


ME_IMPORT(__imp__NtSetInformationFile) {
    if(save_set_guest(ctx))return;
    auto* file=readonly_files.find(ctx.r3.u32);
    const auto iosb=ctx.r4.u32, input=ctx.r5.u32, length=ctx.r6.u32, kind=ctx.r7.u32;
    if(!file || !checked_span(iosb,8))me_stop_import("file_set_invalid_handle_or_iosb",ctx);
    if(kind!=14)me_stop_import("file_set_information_class_unsupported",ctx);
    uint32_t status=0;
    if(length!=8 || !checked_span(input,8,true))status=0xc000000d;
    else {
        const uint64_t position=(uint64_t(load_word(input))<<32)|load_word(input+4);
        if(position>INT64_MAX)status=0xc000000d;
        else file->position=position;
    }
    put_word(iosb,status);put_word(iosb+4,status?0:8);ctx.r3.u64=status;
    if (me_log_verbose()) std::printf("{\"event\":\"file_position_set\",\"position\":%llu,\"status\":\"%08x\"}\n",(unsigned long long)file->position,status);
}

ME_IMPORT(__imp__NtQueryInformationFile) {
    if(save_query_guest(ctx))return;
    const auto file=readonly_files.find(ctx.r3.u32);
    const auto iosb=ctx.r4.u32,output=ctx.r5.u32,length=ctx.r6.u32,kind=ctx.r7.u32;
    if(!file || !checked_span(iosb,8))me_stop_import("file_query_invalid_handle_or_iosb",ctx);
    if(kind!=34)me_stop_import("file_query_information_class_unsupported",ctx);
    if(length<56){put_word(iosb,0xc0000004);put_word(iosb+4,0);ctx.r3.u64=0xc0000004;return;}
    if(!checked_span(output,56))me_stop_import("file_query_information_output_invalid",ctx);
    std::memset(guest+output,0,56); // Original Xbox timestamps unavailable in staged metadata.
    struct stat info{};if(fstat(file->fd,&info))me_stop_import("file_query_fstat_failed",ctx);
    auto store64=[&](uint32_t a,uint64_t v){put_word(a,uint32_t(v>>32));put_word(a+4,uint32_t(v));};
    store64(output+32,uint64_t(info.st_blocks)*512);store64(output+40,file->size);
    put_word(output+48,1); // Read-only staged file.
    put_word(iosb,0);put_word(iosb+4,56);ctx.r3.u64=0;
}


static bool checked_file_buffer(uint32_t address,uint32_t size) {
    if(size<=0x100000)return checked_span(address,size);
    if(size>0x4000000 || uint64_t(address)+size>0x100000000ull)return false;
    for(const auto& region:virtual_memory.regions)if(address>=region.base && uint64_t(address)+size<=uint64_t(region.base)+region.size){
        const uint64_t first=(address-region.base)/region.page,last=(uint64_t(address)+size-1-region.base)/region.page;
        for(auto page=uint64_t(first);page<=last;++page)if(!region.committed[page])return false;
        return true;
    }
    const auto* physical=physical_memory.find(address);
    return physical && uint64_t(MePhysicalMemory::physical(address))+size<=uint64_t(physical->physical)+physical->size;
}


ME_IMPORT(__imp__MmQueryAddressProtect) {
    const auto address=ctx.r3.u32;
    for(const auto& region:virtual_memory.regions)if(address>=region.base && uint64_t(address)<uint64_t(region.base)+region.size){
        ctx.r3.u64=region.committed[(address-region.base)/region.page]?4:1;return;
    }
    me_stop_import("query_address_protect_outside_virtual_allocations",ctx);
}

ME_IMPORT(__imp__NtReadFile) {
    if(save_read_guest(ctx))return;
    const auto handle=ctx.r3.u32,iosb=ctx.r7.u32,buffer=ctx.r8.u32,length=ctx.r9.u32,offset_ptr=ctx.r10.u32;
    auto* file=readonly_files.find(handle);
    if(!file || ctx.r4.u32 || ctx.r5.u32 || ctx.r6.u32 || !checked_span(iosb,8) || !checked_file_buffer(buffer,length) ||
        (offset_ptr && !checked_span(offset_ptr,8,true)))me_stop_import("file_read_mode_or_range_unsupported",ctx);
    uint64_t offset=offset_ptr?(uint64_t(load_word(offset_ptr))<<32)|load_word(offset_ptr+4):UINT64_MAX;
    if(offset==UINT64_MAX || offset==UINT64_MAX-1)offset=file->position;
    if(offset>INT64_MAX)me_stop_import("file_read_offset_unsupported",ctx);
    // The storage read runs without the execution gate, as a console disc/HDD read
    // does not stop other threads. If the GPU re-protects part of the buffer while the
    // gate is released (EFAULT), the remainder is read under the gate.
    me_gpu_write_watch.host_write(buffer,length);
    int64_t count=-1;
    if(const int fd=readonly_files.duplicate(handle);fd>=0){
        std::unique_lock<MeExecutionGate> gate(execution_gate,std::adopt_lock);
        gate.unlock();
        uint64_t total=0;bool locked=false;
        for(;;){
            if(total==length){count=int64_t(total);break;}
            const ssize_t n=pread(fd,guest+buffer+total,length-total,off_t(offset+total));
            if(n<0 && errno==EINTR)continue;
            if(n<0 && errno==EFAULT && !locked){gate.lock();locked=true;me_gpu_write_watch.host_write(uint32_t(buffer+total),uint32_t(length-total));continue;}
            if(n<0){count=total?int64_t(total):-1;break;}
            if(n==0){count=int64_t(total);break;}
            total+=uint64_t(n);
        }
        ::close(fd);
        if(!locked)gate.lock();
        gate.release();
        if(count>=0)readonly_files.set_position(handle,offset+uint64_t(count));
    }
    const uint32_t status=count<0?0xc0000001:(length && !count?0xc0000011:0);
    put_word(iosb,status);put_word(iosb+4,count<0?0:uint32_t(count));ctx.r3.u64=status;
    if (me_log_verbose()) std::printf("{\"event\":\"file_read\",\"handle\":\"%08x\",\"offset\":%llu,\"requested\":%u,\"read\":%lld,\"status\":\"%08x\"}\n",handle,(unsigned long long)offset,length,(long long)count,status);
}

ME_IMPORT(__imp__NtQueryFullAttributesFile) {
    if(save_full_attributes(ctx))return;
    const auto attributes = ctx.r3.u32, output = ctx.r4.u32;
    if (!checked_span(attributes, 12, true) || !checked_span(output, 56))
        me_stop_import("file_attributes_invalid_outputs", ctx);
    const auto name = load_word(attributes + 4);
    if (!checked_span(name, 8, true)) me_stop_import("file_attributes_invalid_name", ctx);
    const auto length = uint32_t(MeXexHeaders::be32(guest + name) >> 16), source = load_word(name + 4);
    if (!length || length > 1024 || !checked_span(source, length, true))
        me_stop_import("file_attributes_invalid_path", ctx);
    std::string path(reinterpret_cast<char*>(guest + source), length);
    std::string escaped;
    for (unsigned char c : path) {
        if (c < 32 || c > 126) { ctx.r3.u64 = 0xc0000033; return; }
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(char(c));
    }
    std::printf("{\"event\":\"file_query_probe\",\"root\":\"%08x\",\"path\":\"%s\"}\n",load_word(attributes),escaped.c_str());
    std::string lower = path;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    // This diagnostic environment has no ShaderDumpxe developer volume.
    // Do not generalize this response to game/content/save devices.
    if ((load_word(attributes) == 0 || load_word(attributes) == 0xfffffffd) &&
        lower.rfind("shaderdumpxe:\\", 0) == 0) {
        ctx.r3.u64 = 0xc000000f;
        std::puts("{\"event\":\"file_query_unmounted_debug_volume\",\"status\":\"c000000f\"}");
        return;
    }
    me_stop_import("file_query_mapping_not_implemented", ctx);
}

ME_IMPORT(__imp__MmQueryStatistics) {
    const auto output=ctx.r3.u32;
    if(!output){ctx.r3.u64=0xc000000d;return;}
    if(!word_writable(output))me_stop_import("memory_statistics_pointer_invalid",ctx);
    if(load_word(output)!=104){ctx.r3.u64=0xc0000023;return;}
    if(!gpu_explore_mode || !checked_span(output,104))me_stop_import("memory_statistics_requires_explore",ctx);
    uint64_t physical=0,reserved=0,committed=0,stacks=0;
    for(const auto& a:physical_memory.allocations)physical+=a.size/4096;
    for(const auto& r:virtual_memory.regions){reserved+=r.size;for(auto page:r.committed)if(page)committed+=r.page/4096;}
    for(const auto& t:threads)stacks+=(t->high-t->low+4095)/4096;
    std::vector<uint8_t> image_pages(0x1170);
    for(unsigned i=0;i<image_range_count;++i){
        auto start=(image_ranges[i].address-0x82000000)/4096;
        auto end=(uint64_t(image_ranges[i].address)+image_ranges[i].size-0x82000000+4095)/4096;
        if(end>image_pages.size())me_stop_import("memory_statistics_image_outside_profile",ctx);
        for(auto at=start;at<end;++at)image_pages[at]=1;
    }
    const uint32_t images=uint32_t(std::count(image_pages.begin(),image_pages.end(),uint8_t(1)));
    // Bounded 512MiB guest research profile, not Android free-memory statistics.
    // Pool excludes lower16MiB and upper4MiB. Other tracked backing reduces
    // the reported budget; the allocators are not yet one unified page manager.
    const uint64_t used=physical+committed+stacks+images;
    const uint32_t available=used<0x1ec00?uint32_t(0x1ec00-used):0;
    std::memset(guest+output,0,104);
    put_word(output,104);put_word(output+4,0x20000);put_word(output+8,0x1400);
    put_word(output+12,available);put_word(output+16,0x30000000);put_word(output+20,uint32_t(reserved));
    put_word(output+24,uint32_t(physical));put_word(output+32,uint32_t(stacks));put_word(output+36,images);
    put_word(output+44,uint32_t(committed));put_word(output+100,0x1ffff);
    ctx.r3.u64=0;
    std::printf("{\"event\":\"guest_memory_statistics\",\"physical_pages\":%llu,\"virtual_pages\":%llu,\"image_pages\":%u,\"available_budget_pages\":%u,\"unified_allocator\":false}\n",(unsigned long long)physical,(unsigned long long)committed,images,available);
}

static bool image_span_mapped(uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t(address) + size;
    uint64_t cursor = address;
    while (cursor < end) {
        uint64_t next = cursor;
        for (unsigned i = 0; i < image_range_count; ++i)
            if (cursor >= image_ranges[i].address && cursor < uint64_t(image_ranges[i].address) + image_ranges[i].size)
                next = std::max(next, uint64_t(image_ranges[i].address) + image_ranges[i].size);
        if (next == cursor) break;
        cursor = next;
    }
    return size && end <= 0x100000000ull && cursor >= end;
}
ME_IMPORT(__imp__XexGetModuleSection) {
    if (ctx.r3.u32 != 0x60001000) { ctx.r3.u64 = 0xc0000008; return; }
    if (!word_writable(ctx.r5.u32) || !word_writable(ctx.r6.u32)) me_stop_import("module_section_outputs_invalid", ctx);
    std::string name;
    for (unsigned i = 0; i < 9; ++i) {
        if (uint64_t(ctx.r4.u32) + i >= 0x100000000ull || !checked_span(ctx.r4.u32+i, 1, true)) me_stop_import("module_section_name_invalid", ctx);
        const auto c = guest[ctx.r4.u32+i]; if (!c) break;
        name.push_back(char(c));
    }
    uint32_t table = 0;
    if (name.size() <= 8 && module_headers.lookup(0x2ff, table)) {
        const auto length = load_word(table);
        if (length < 4 || (length - 4) % 16) me_stop_import("module_resource_table_invalid", ctx);
        for (unsigned at = 4; at < length; at += 16) {
            const auto entry = table + at;
            size_t count = 0; while (count < 8 && guest[entry+count]) ++count;
            if (name != std::string(reinterpret_cast<char*>(guest+entry),count)) continue;
            const auto address = load_word(entry+8), size = load_word(entry+12);
            if (!image_span_mapped(address, size)) me_stop_import("module_resource_not_mapped", ctx);
            put_word(ctx.r5.u32,address); put_word(ctx.r6.u32,size); ctx.r3.u64=0;
            std::printf("{\"event\":\"module_resource_found\",\"address\":\"%08x\",\"size\":%u}\n",address,size); return;
        }
    }
    ctx.r3.u64=0xc0000225; // STATUS_NOT_FOUND; leave outputs untouched.
}
ME_IMPORT(__imp__RtlInitAnsiString) {
    const auto destination = ctx.r3.u32, source = ctx.r4.u32;
    if (!checked_span(destination, 8)) me_stop_import("ansi_string_destination_invalid", ctx);
    uint32_t length = 0;
    if (source) for (;;) {
        if (length > 65534 || uint64_t(source) + length >= 0x100000000ull || !checked_span(source + length, 1, true))
            me_stop_import("ansi_string_source_invalid_or_too_long", ctx);
        if (!guest[source + length]) break;
        ++length;
    }
    const uint16_t encoded_length = __builtin_bswap16(uint16_t(length));
    const uint16_t encoded_max = __builtin_bswap16(uint16_t(source ? length + 1 : 0));
    std::memcpy(guest + destination, &encoded_length, 2);
    std::memcpy(guest + destination + 2, &encoded_max, 2); put_word(destination + 4, source);
}
ME_IMPORT(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
    // No category-volume setter is implemented, so no change notifications exist.
    if (!audio_output_is_ready() || ctx.r3.u32 != 0x41550000 || !word_writable(ctx.r4.u32))
        me_stop_import("audio_volume_mask_invalid_client_or_output", ctx);
    put_word(ctx.r4.u32, 0); ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__NtWaitForSingleObjectEx) {
    if (ctx.r4.u32 > 1 || ctx.r5.u32) me_stop_import("NtWaitForSingleObjectEx_unsupported_mode_or_apc", ctx);
    const auto timeout = ctx.r6.u32;
    if (timeout && !checked_span(timeout, 8, true)) me_stop_import("NtWaitForSingleObjectEx_invalid_timeout", ctx);
    const uint64_t raw_timeout = timeout ? (uint64_t(load_word(timeout)) << 32) | load_word(timeout + 4) : 0;
    if (timeout && int64_t(raw_timeout) > 0) me_stop_import("NtWaitForSingleObjectEx_absolute_timeout_not_supported", ctx);
    const auto handle = ctx.r3.u32;
    uint32_t status = 0;
    auto ready = [&] {
        if (auto t = find_thread(handle)) { if (!t->done) return false; status = 0; return true; }
        if(auto* listener=notifications.find(handle)){if(listener->queue.empty())return false;status=0;return true;}
        if(mutants.find(handle))return mutants.wait(handle,thread_address,status);
        return events.wait(handle, false, status);
    };
    if (!ready()) {
        if (me_log_verbose()) std::printf("{\"event\":\"object_wait_begin\",\"handle\":\"%08x\",\"thread\":\"%08x\"}\n", handle, thread_address);
        if (timeout && !raw_timeout) status = 0x102;
        else if (!timeout) wait_for_change(ready);
        else {
            const uint64_t ticks = 0 - raw_timeout;
            if (ticks > 600000000ull) me_stop_import("NtWaitForSingleObjectEx_timeout_exceeds_probe_limit", ctx);
            std::unique_lock<MeExecutionGate> lock(execution_gate, std::adopt_lock);
            if (!state_changed.wait_for(lock, std::chrono::nanoseconds(ticks * 100), ready)) status = 0x102;
            lock.release();
        }
    }
    if (me_log_verbose()) std::printf("{\"event\":\"object_wait\",\"handle\":\"%08x\",\"status\":\"%08x\"}\n", handle, status);
    ctx.r3.u64 = status;
}
ME_IMPORT(__imp__NtWaitForMultipleObjectsEx) {
    const auto count=ctx.r3.u32,array=ctx.r4.u32,type=ctx.r5.u32;
    const auto mode=ctx.r6.u32,alertable=ctx.r7.u32,timeout=ctx.r8.u32;
    if(!count || count>64 || type>1 || mode>1 || alertable)me_stop_import("handle_multiwait_mode_unsupported",ctx);
    if(!checked_span(array,count*4,true))me_stop_import("handle_multiwait_array_invalid",ctx);
    if(count==1){
        ctx.r3.u64=load_word(array);ctx.r4.u64=mode;ctx.r5.u64=alertable;ctx.r6.u64=timeout;
        __imp__NtWaitForSingleObjectEx(ctx,base);return;
    }
    if(timeout && !checked_span(timeout,8,true))me_stop_import("handle_multiwait_timeout_invalid",ctx);
    const uint64_t raw=timeout?(uint64_t(load_word(timeout))<<32)|load_word(timeout+4):0;
    if(timeout && int64_t(raw)>0)me_stop_import("handle_multiwait_absolute_timeout_unsupported",ctx);
    std::vector<uint32_t> handles;
    for(unsigned i=0;i<count;++i){const auto handle=load_word(array+i*4);
        if(std::find(handles.begin(),handles.end(),handle)!=handles.end())me_stop_import("handle_multiwait_duplicate_unsupported",ctx);
        if(readonly_files.find(handle))me_stop_import("handle_multiwait_file_objects_unsupported",ctx);
        handles.push_back(handle);
    }
    uint32_t status=0;
    auto valid=[&](uint32_t h){return bool(find_thread(h)) || events.find(h) || notifications.find(h) || mutants.find(h);};
    auto available=[&](uint32_t h){
        if(auto t=find_thread(h))return t->done;
        if(auto* e=events.find(h))return e->signaled;
        if(auto* n=notifications.find(h))return !n->queue.empty();
        if(auto* m=mutants.find(h))return !m->owner || m->owner==thread_address;
        return false;
    };
    auto consume=[&](uint32_t h){
        if(mutants.find(h)){uint32_t acquired=0;mutants.wait(h,thread_address,acquired);return acquired;}
        if(auto* e=events.find(h);e && !e->manual)e->signaled=false;
        return 0u;
    };
    auto ready=[&]{
        for(auto h:handles)if(!valid(h)){status=0xc0000008;return true;}
        if(type==0){
            for(auto h:handles)if(!available(h))return false;
            for(auto h:handles)if(auto* m=mutants.find(h);m && m->depth==0x7fffffff){status=0xc0000047;return true;}
            bool abandoned=false;for(auto h:handles)abandoned|=consume(h)==0x80;
            status=abandoned?0x80:0;return true;
        }
        for(unsigned i=0;i<count;++i)if(available(handles[i])){
            if(auto* m=mutants.find(handles[i]);m && m->depth==0x7fffffff){status=0xc0000047;return true;}
            status=consume(handles[i])+i;return true;
        }
        return false;
    };
    static thread_local uint64_t calls=0;
    ++calls;const bool trace=calls<=8 || !(calls&(calls-1));
    if(trace)std::printf("{\"event\":\"handle_multiwait\",\"count\":%u,\"wait_all\":%s,\"first\":\"%08x\",\"second\":\"%08x\",\"call\":%llu}\n",count,type==0?"true":"false",handles[0],handles[1],(unsigned long long)calls);
    if(!ready()){
        if(timeout && !raw)status=0x102;
        else if(!timeout)wait_for_change(ready);
        else{
            const auto ticks=0-raw;if(ticks>600000000ull)me_stop_import("handle_multiwait_timeout_exceeds_probe_limit",ctx);
            std::unique_lock<MeExecutionGate> gate(execution_gate,std::adopt_lock);
            if(!state_changed.wait_for(gate,std::chrono::nanoseconds(ticks*100),ready))status=0x102;
            gate.release();
        }
    }
    if(trace)std::printf("{\"event\":\"handle_multiwait_done\",\"count\":%u,\"status\":\"%08x\"}\n",count,status);
    ctx.r3.u64=status;
}
ME_IMPORT(__imp__NetDll_WSAWaitForMultipleEvents) {
    const auto count=ctx.r3.u32,array=ctx.r4.u32,all=ctx.r5.u32,ms=ctx.r6.u32,alertable=ctx.r7.u32;
    auto fail=[&](uint32_t error){put_word(thread_address+0x160,error);ctx.r3.u64=0xffffffff;};
    if(!count || count>64 || all>1 || !checked_span(array,count*4,true)){fail(87);return;}
    if(alertable)me_stop_import("wsa_wait_alertable_unsupported",ctx);
    std::vector<uint32_t> handles;
    for(uint32_t i=0;i<count;++i){
        const auto handle=load_word(array+4*i);
        if(!events.find(handle)){fail(6);return;}
        if(std::find(handles.begin(),handles.end(),handle)!=handles.end()){fail(87);return;}
        handles.push_back(handle);
    }
    uint32_t status=0;
    auto ready=[&]{
        for(auto handle:handles)if(!events.find(handle)){status=0xffffffff;return true;}
        if(all){
            for(auto handle:handles)if(!events.find(handle)->signaled)return false;
            for(auto handle:handles){uint32_t acquired;events.wait(handle,false,acquired);}
            status=0;return true;
        }
        for(uint32_t i=0;i<count;++i)if(events.find(handles[i])->signaled){uint32_t acquired;events.wait(handles[i],false,acquired);status=i;return true;}
        return false;
    };
    std::printf("{\"event\":\"wsa_event_wait\",\"count\":%u,\"wait_all\":%u,\"timeout_ms\":%u,\"first_handle\":\"%08x\"}\n",count,all,ms,handles[0]);
    if(!ready()){
        if(!ms)status=0x102;
        else if(ms==0xffffffff)wait_for_change(ready);
        else{
            std::unique_lock<MeExecutionGate> gate(execution_gate,std::adopt_lock);
            if(!state_changed.wait_for(gate,std::chrono::milliseconds(ms),ready))status=0x102;
            gate.release();
        }
    }
    if(status==0xffffffff)fail(6);else ctx.r3.u64=status;
}
ME_IMPORT(__imp__RtlTimeToTimeFields) {
    const auto input=ctx.r3.u32,output=ctx.r4.u32;
    if(!checked_span(input,8,true) || !checked_span(output,16))me_stop_import("time_fields_invalid_buffer",ctx);
    const uint64_t ticks=(uint64_t(load_word(input))<<32)|load_word(input+4);
    MeTimeFields::Fields fields{};
    if(!MeTimeFields::decode(ticks,fields))me_stop_import("time_fields_signed_range_unsupported",ctx);
    for(unsigned i=0;i<fields.size();++i){uint16_t value=__builtin_bswap16(fields[i]);std::memcpy(guest+output+2*i,&value,2);}
    static unsigned calls=0;if(gpu_explore_mode && ++calls<=4)std::printf("{\"event\":\"time_fields_decoded\",\"year\":%u,\"month\":%u,\"day\":%u,\"weekday\":%u}\n",fields[0],fields[1],fields[2],fields[7]);
}
ME_IMPORT(__imp__RtlTimeFieldsToTime) {
    const auto input=ctx.r3.u32,output=ctx.r4.u32;
    if(!checked_span(input,16,true) || !checked_span(output,8))me_stop_import("time_fields_encode_invalid_buffer",ctx);
    MeTimeFields::Fields fields{};
    for(unsigned i=0;i<fields.size();++i){uint16_t value;std::memcpy(&value,guest+input+2*i,2);fields[i]=__builtin_bswap16(value);}
    uint64_t ticks=0;
    if(!MeTimeFields::encode(fields,ticks)){ctx.r3.u64=0;return;}
    put_word(output,uint32_t(ticks>>32));put_word(output+4,uint32_t(ticks));ctx.r3.u64=1;
}
ME_IMPORT(__imp__KeQuerySystemTime) {
    const auto output=ctx.r3.u32;if(!output)return;
    if(!checked_span(output,8))me_stop_import("system_time_output_invalid",ctx);
    timespec now{};
    if(clock_gettime(CLOCK_REALTIME,&now) || now.tv_sec < -11644473600ll)me_stop_import("system_time_host_clock_failed",ctx);
    const uint64_t ticks=(uint64_t(now.tv_sec+11644473600ll)*10000000ull)+uint64_t(now.tv_nsec)/100;
    put_word(output,uint32_t(ticks>>32));put_word(output+4,uint32_t(ticks));
    static unsigned calls=0;if(gpu_explore_mode && ++calls<=4)std::printf("{\"event\":\"system_time_utc\",\"filetime_100ns\":%llu}\n",(unsigned long long)ticks);
}
ME_IMPORT(__imp__RtlFillMemoryUlong) {
    const auto destination=ctx.r3.u32,length=ctx.r4.u32,pattern=ctx.r5.u32;
    if(!length)return;
    if((destination&3) || (length&3) || !checked_span(destination,length))me_stop_import("fill_ulong_invalid_or_unsupported_span",ctx);
    for(uint32_t offset=0;offset<length;offset+=4)put_word(destination+offset,pattern);
    // Void export; preserve guest argument registers.
}
ME_IMPORT(__imp__RtlMultiByteToUnicodeN) {
    const auto dst = ctx.r3.u32, out = ctx.r5.u32, src = ctx.r6.u32;
    const auto count = std::min(ctx.r4.u32 / 2, ctx.r7.u32);
    if (!checked_span(dst, count * 2) || !checked_span(src, count, true) || (out && !word_writable(out)))
        me_stop_import("RtlMultiByteToUnicodeN_invalid_buffers", ctx);
    // Verified ASCII subset. Codepage-dependent input needs an explicit encoding model.
    for (uint32_t i = 0; i < count; ++i)
        if (base[src + i] > 127) me_stop_import("RtlMultiByteToUnicodeN_non_ascii_requires_codepage", ctx);
    for (uint32_t i = 0; i < count; ++i) { base[dst + i * 2] = 0; base[dst + i * 2 + 1] = base[src + i]; }
    if (out) put_word(out, count * 2);
    ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__RtlUnicodeToMultiByteN) {
    const auto destination = ctx.r3.u32, written = ctx.r5.u32, source = ctx.r6.u32;
    const auto count = std::min(ctx.r4.u32, ctx.r7.u32 / 2);
    if ((ctx.r7.u32 & 1) || !checked_span(destination,count) || !checked_span(source,count*2,true) || (written && !word_writable(written)))
        me_stop_import("unicode_to_multibyte_invalid_range", ctx);
    std::vector<uint8_t> converted(count);
    for (unsigned i=0;i<count;++i) {
        const auto value = uint16_t(guest[source+i*2])<<8 | guest[source+i*2+1];
        if (value > 127) me_stop_import("unicode_to_multibyte_requires_codepage", ctx);
        converted[i] = uint8_t(value);
    }
    if (count) std::memcpy(guest+destination,converted.data(),count);
    if (written) put_word(written,count); ctx.r3.u64=0;
}
ME_IMPORT(__imp__KeTlsAlloc) {
    ctx.r3.u64 = UINT32_MAX;
    for (unsigned i = 0; i < 256 && i < tls_slots; ++i) {
        if (tls_used[i]) continue;
        tls_used[i] = true; put_word(tls_dynamic + i * 4, 0); ctx.r3.u64 = i;
        std::printf("{\"event\":\"tls_alloc\",\"slot\":%u}\n", i);
        break;
    }
}
ME_IMPORT(__imp__KeTlsGetValue) {
    if (ctx.r3.u32 >= tls_slots) me_stop_import("tls_get_index_out_of_range", ctx);
    ctx.r3.u64 = load_word(tls_dynamic + ctx.r3.u32 * 4);
}
ME_IMPORT(__imp__KeTlsSetValue) {
    if (ctx.r3.u32 >= tls_slots) me_stop_import("tls_set_index_out_of_range", ctx);
    put_word(tls_dynamic + ctx.r3.u32 * 4, ctx.r4.u32); ctx.r3.u64 = 1;
}
ME_IMPORT(__imp__KeTlsFree) {
    const auto slot = ctx.r3.u32;
    if (slot >= 256 || slot >= tls_slots) { ctx.r3.u64 = 0; return; }
    tls_used[slot] = false;
    for (const auto& t : threads) put_word(t->tls + slot * 4, 0);
    ctx.r3.u64 = 1;
}
static uint32_t checked_cs(PPCContext& ctx) {
    const auto a = ctx.r3.u32;
    if (a > UINT32_MAX - 28) me_stop_import("critical_section_invalid_address", ctx);
    for (unsigned i = 0; i < 28; i += 4)
        if (!word_writable(a + i)) me_stop_import("critical_section_unwritable", ctx);
    return a;
}
// Critical sections follow the Xbox kernel's lock-free design so uncontended use
// never touches the kernel lock: LockCount (+16, -1 = free) counts the owner's
// recursion plus queued waiters and changes only by atomic CAS on guest memory;
// RecursionCount (+20) and OwningThread (+24) are written by the owner. Only a
// contended Enter takes the kernel lock to wait, and a Leave that sees waiters takes
// it to wake them (so a waiter cannot miss the wake-up between check and sleep).
static uint32_t* cs_word(uint32_t address) { return reinterpret_cast<uint32_t*>(guest + address); }
static uint32_t cs_load(uint32_t address) { return __builtin_bswap32(__atomic_load_n(cs_word(address), __ATOMIC_ACQUIRE)); }
static void cs_store(uint32_t address, uint32_t value) { __atomic_store_n(cs_word(address), __builtin_bswap32(value), __ATOMIC_RELEASE); }
static bool cs_cas(uint32_t address, uint32_t expected, uint32_t desired) {
    uint32_t raw = __builtin_bswap32(expected);
    return __atomic_compare_exchange_n(cs_word(address), &raw, __builtin_bswap32(desired), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static uint32_t cs_add(uint32_t address, int32_t delta) { // returns the new value
    uint32_t raw = __atomic_load_n(cs_word(address), __ATOMIC_ACQUIRE);
    for (;;) {
        const uint32_t value = __builtin_bswap32(raw) + uint32_t(delta);
        if (__atomic_compare_exchange_n(cs_word(address), &raw, __builtin_bswap32(value), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return value;
    }
}
static std::unordered_map<uint32_t, uint32_t> cs_handoffs; // CS address -> pending handoffs (kernel lock)
static uint32_t checked_cs_fast(PPCContext& ctx) {
    const auto a = ctx.r3.u32;
    if ((a & 3) || a > UINT32_MAX - 28) me_stop_import("critical_section_invalid_address", ctx);
    return a;
}
static void cs_enter(PPCContext& ctx, uint32_t a) {
    const uint32_t self = thread_address;
    if (cs_add(a + 16, 1) == 0) { cs_store(a + 24, self); cs_store(a + 20, 1); return; }
    if (cs_load(a + 24) == self) {
        const auto count = cs_load(a + 20);
        if (count == UINT32_MAX) me_stop_import("critical_section_recursion_overflow", ctx);
        cs_store(a + 20, count + 1); return;
    }
    // Contended: queued (counted in LockCount, so no fast-path acquire can overtake).
    // Ownership passes by a handoff token that a Leave issues only when it sees
    // queued waiters (the kernel's auto-reset event); exactly one waiter takes it.
    {
        MeKernelLock kernel_lock;
        wait_for_change([&] { auto it = cs_handoffs.find(a); return it != cs_handoffs.end() && it->second; });
        if (!--cs_handoffs[a]) cs_handoffs.erase(a);
    }
    cs_store(a + 24, self); cs_store(a + 20, 1);
}
PPC_FUNC(__imp__RtlEnterCriticalSection) { cs_enter(ctx, checked_cs_fast(ctx)); }
// Time slice for the cooperative gate. On the console the title's threads run in
// parallel, so a thread looping through uncontended critical sections (the UE3 tick
// loop while a Bink movie plays) never blocks the audio callback or workers there.
// After fully releasing a critical section, a thread that has run for 1 ms hands the
// gate to queued guest threads (FIFO). Guest-visible results are unchanged.
static std::atomic<uint64_t> time_slice_yields{0};
static void me_time_slice_yield() {
    if (!gpu_explore_mode || guest_parallel) return;
    static thread_local uint32_t calls = 0;
    static thread_local auto last = std::chrono::steady_clock::now();
    if (++calls & 31) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(1)) return;
    if (!execution_gate.queued_waiters()) { last = now; return; }
    { std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock); gate.unlock(); gate.lock(); gate.release(); }
    last = std::chrono::steady_clock::now();
    const auto n = time_slice_yields.fetch_add(1) + 1;
    if (n <= 4 || !(n & (n - 1)))
        std::printf("{\"event\":\"time_slice_yield\",\"thread\":\"%08x\",\"count\":%llu}\n", thread_address, (unsigned long long)n);
}
PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    const auto a = checked_cs_fast(ctx);
    const auto count = cs_load(a + 20);
    if (cs_load(a + 24) != thread_address || !count) me_stop_import("critical_section_invalid_owner", ctx);
    if (count > 1) { cs_store(a + 20, count - 1); cs_add(a + 16, -1); return; }
    cs_store(a + 20, 0);
    cs_store(a + 24, 0); // release ownership before LockCount, so no new owner is overwritten
    if (cs_add(a + 16, -1) != UINT32_MAX) { MeKernelLock kernel_lock; ++cs_handoffs[a]; state_changed.notify_all(); }
    if (!guest_parallel && execution_gate.owned()) me_time_slice_yield();
}
PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    const auto a = checked_cs_fast(ctx);
    if (cs_cas(a + 16, UINT32_MAX, 0)) { cs_store(a + 24, thread_address); cs_store(a + 20, 1); ctx.r3.u64 = 1; return; }
    if (cs_load(a + 24) == thread_address) { cs_enter(ctx, a); ctx.r3.u64 = 1; return; }
    ctx.r3.u64 = 0;
}
ME_IMPORT(__imp__RtlInitializeCriticalSection) {
    const uint32_t address = ctx.r3.u32;
    if (address > UINT32_MAX - 28) me_stop_import("critical_section_invalid_address", ctx);
    for (unsigned i = 0; i < 28; i += 4)
        if (!word_writable(address + i)) me_stop_import("critical_section_unwritable", ctx);
    // Only fields initialized by the Xbox API; preserve dispatcher list bytes.
    base[address] = 1;
    base[address + 1] = 0;
    uint32_t zero = 0, unlocked = UINT32_MAX;
    std::memcpy(base + address + 4, &zero, 4);
    std::memcpy(base + address + 16, &unlocked, 4);
    std::memcpy(base + address + 20, &zero, 4);
    std::memcpy(base + address + 24, &zero, 4);
    std::printf("{\"event\":\"critical_section_initialized\",\"address\":\"%08x\"}\n", address);
}
ME_IMPORT(__imp__NtFreeVirtualMemory) {
    auto ap = ctx.r3.u32, sp = ctx.r4.u32, type = ctx.r5.u32;
    if (ctx.r6.u32 || (type != 0x4000 && type != 0x8000)) me_stop_import("NtFreeVirtualMemory_unsupported_flags", ctx);
    if (!word_writable(ap) || !word_writable(sp) || ap == sp) me_stop_import("NtFreeVirtualMemory_invalid_pointers", ctx);
    auto address = load_word(ap), size = load_word(sp);
    const auto status = virtual_memory.free(address, size, type);
    if (!status) { put_word(ap, address); put_word(sp, size); }
    if (me_log_verbose()) std::printf("{\"event\":\"virtual_free\",\"base\":\"%08x\",\"size\":%u,\"type\":\"%08x\",\"status\":\"%08x\"}\n", address, size, type, status);
    ctx.r3.u64 = status;
}
ME_IMPORT(__imp__NtAllocateVirtualMemory) {
    const uint32_t base_pointer = ctx.r3.u32, size_pointer = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32, protect = ctx.r6.u32, debug = ctx.r7.u32;
    if (!word_writable(base_pointer) || !word_writable(size_pointer) || base_pointer == size_pointer)
        me_stop_import("NtAllocateVirtualMemory_unsupported_output_pointers", ctx);
    if (!virtual_memory.supports(type, protect, debug))
        me_stop_import("NtAllocateVirtualMemory_unsupported_flags_or_protection", ctx);
    uint32_t address = MeXexHeaders::be32(base + base_pointer);
    uint32_t size = MeXexHeaders::be32(base + size_pointer);
    const uint32_t old_address = address, old_size = size;
    const uint32_t status = virtual_memory.allocate(address, size, type);
    if (!status) {
        uint32_t encoded = __builtin_bswap32(address); std::memcpy(base + base_pointer, &encoded, 4);
        encoded = __builtin_bswap32(size); std::memcpy(base + size_pointer, &encoded, 4);
    }
    if (me_log_verbose()) std::printf("{\"event\":\"virtual_allocate\",\"requested_base\":\"%08x\",\"requested_size\":%u,\"type\":\"%08x\",\"base\":\"%08x\",\"size\":%u,\"status\":\"%08x\"}\n",
        old_address, old_size, type, address, size, status);
    ctx.r3.u64 = status;
}

[[noreturn]] void me_stop_import(const char* name, PPCContext& ctx) {
    if (gpu_explore_mode && game_gpu_ready && gpu_read_pointer_physical) {
        // Diagnostic drain opportunity only: no completion value is synthesized.
        // Parent keeps its guest context but releases the gate so native CP can run.
        std::printf("{\"event\":\"probe_stop_pending\",\"import\":\"%s\"}\n",name);
        std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock);
        gate.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gate.lock(); gate.release();
        auto* read_pointer = gpu_physical.resolve(gpu_read_pointer_physical, 4);
        if (read_pointer) std::printf("{\"event\":\"gpu_stop_snapshot\",\"write_index\":%u,\"read_index\":%u,\"wait_ms\":50}\n",gpu_last_write_index,MeXexHeaders::be32(read_pointer));
    }

    std::printf("{\"event\":\"unimplemented_function_import\",\"name\":\"%s\",\"lr\":\"%08llx\",\"r3\":\"%08x\",\"thread\":\"%08x\"}\n",
                name, static_cast<unsigned long long>(ctx.lr), ctx.r3.u32, thread_address);
    std::fflush(stdout);
    _exit(42);
}

static void fault(int signal, siginfo_t* info, void* raw_context) {
    if (signal == SIGSEGV && info && me_gpu_write_watch.fault(uintptr_t(info->si_addr))) return;
    // The alarm is a bounded diagnostic stop, not a guest memory fault.
    // Capture host PC plus guest registers; LR alone is only a saved return site.
    auto* machine = static_cast<ucontext_t*>(raw_context);
    const uintptr_t pc = machine ? machine->uc_mcontext.pc : 0;
    Dl_info module{}; dladdr(reinterpret_cast<void*>(pc), &module);
    char snapshot[1024];
    const int count=std::snprintf(snapshot,sizeof(snapshot),
        "{\"event\":\"stop_snapshot\",\"timeout\":%s,\"thread\":\"%08x\",\"host_pc\":\"%llx\",\"module_offset\":\"%llx\",\"module\":\"%s\",\"lr\":\"%08llx\",\"ctr\":\"%08llx\",\"r3\":\"%08x\",\"r4\":\"%08x\",\"r5\":\"%08x\",\"r31\":\"%08x\",\"sp\":\"%08x\"}\n",
        signal==SIGALRM?"true":"false",thread_address,(unsigned long long)pc,
        (unsigned long long)(pc-reinterpret_cast<uintptr_t>(module.dli_fbase)),module.dli_fname?module.dli_fname:"unknown",
        (unsigned long long)(active_context?active_context->lr:0),(unsigned long long)(active_context?active_context->ctr.u64:0),
        active_context?active_context->r3.u32:0,active_context?active_context->r4.u32:0,
        active_context?active_context->r5.u32:0,active_context?active_context->r31.u32:0,active_context?active_context->r1.u32:0);
    if(count>0)write(STDOUT_FILENO,snapshot,size_t(count)<sizeof(snapshot)?size_t(count):sizeof(snapshot)-1);
    if(signal==SIGUSR1)return;
    if(signal==SIGALRM){
        const int self=int(syscall(SYS_gettid)),process=int(getpid());
        for(auto& slot:diagnostic_tids){const int tid=slot.load();if(tid>0 && tid!=self)syscall(SYS_tgkill,process,tid,SIGUSR1);}
        static volatile sig_atomic_t samples=0;samples=samples+1;if(samples<diagnostic_timeout_rounds){alarm(5);return;}_exit(44);} // Bounded snapshots, no guest mutation.
    const auto host = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto origin = reinterpret_cast<uintptr_t>(guest);
    const uint64_t address = host >= origin ? host - origin : UINT64_MAX;
    char line[512];
    if(signal==SIGSEGV){
        unsigned committed=0,watched=0;
        MeGpuWriteWatch::pages_of(address,[&](uint32_t p){committed|=MeGpuWriteWatch::bit(me_gpu_write_watch.committed,p);watched|=MeGpuWriteWatch::bit(me_gpu_write_watch.watched,p);});
        int n=std::snprintf(line,sizeof(line),"{\"event\":\"write_watch_fault_detail\",\"si_code\":%d,\"host_address\":\"%llx\",\"committed\":%u,\"watched\":%u,\"protect_failures\":%llu,\"protect_errno\":%d,\"protect_page\":%u}\n",info->si_code,(unsigned long long)host,committed,watched,(unsigned long long)me_gpu_write_watch.protect_failures.load(),me_gpu_write_watch.last_protect_errno.load(),me_gpu_write_watch.last_protect_page.load());
        if(n>0)write(STDOUT_FILENO,line,std::min(size_t(n),sizeof(line)-1));
    }
    for (unsigned i = 0; i < guard_count; ++i) {
        if (address >= guards[i].address && address < guards[i].address + 0x1000) {
            int n = std::snprintf(line, sizeof(line),
                "{\"event\":\"unimplemented_variable_import\",\"name\":\"%s\",\"iat\":\"%08x\",\"ordinal\":%u,\"lr\":\"%08llx\",\"sp\":\"%08x\"}\n",
                guards[i].name, guards[i].iat, guards[i].ordinal,
                static_cast<unsigned long long>(active_context->lr), active_context->r1.u32);
            write(STDOUT_FILENO, line, size_t(n));
            _exit(42);
        }
    }
    int n = std::snprintf(line, sizeof(line),
        "{\"event\":\"execution_fault\",\"signal\":%d,\"guest_address\":\"%llx\",\"lr\":\"%llx\"}\n",
        signal, static_cast<unsigned long long>(address),
        static_cast<unsigned long long>(active_context ? active_context->lr : 0));
    write(STDOUT_FILENO, line, size_t(n));
    _exit(43);
}

static void read_exact(FILE* file, void* to, size_t bytes) {
    if (std::fread(to, 1, bytes, file) != bytes) { std::fprintf(stderr, "Short bundle read\n"); std::exit(2); }
}
static uint32_t read32(FILE* file) { uint32_t result; read_exact(file, &result, 4); return result; }
static void writable(uint32_t address, size_t size) {
    const uint64_t start = uint64_t(address) & ~(uint64_t(page_size) - 1);
    const uint64_t end = (uint64_t(address) + size + page_size - 1) & ~(uint64_t(page_size) - 1);
    if (end > 0x100000000ull || mprotect(guest + start, end - start, PROT_READ | PROT_WRITE)) {
        std::perror("mprotect"); std::exit(2);
    }
}
static bool commit_pages(uint32_t address, uint32_t size) {
    if ((address % page_size) || (size % page_size)) return false;
    return mprotect(guest + address, size, PROT_READ | PROT_WRITE) == 0;
}

static uint32_t gate_label() { return active_context ? thread_address : 0xf0000000u | (uint32_t(syscall(SYS_gettid)) & 0xffff); }
static void write_watch_fault(int signal, siginfo_t* info, void*) {
    if (info && me_gpu_write_watch.fault(uintptr_t(info->si_addr))) return;
    ::signal(signal, SIG_DFL); // re-executes and dies with the original fault
}
// Release semantics, matching the console and Xenia: MmFreePhysicalMemory is
// pure bookkeeping and the physical pages stay mapped and readable in every
// guest alias window until the next allocation re-commits and zeroes them.
// Xenia's kernel never unmaps freed physical allocations, and the console
// kernel behaves the same way, so titles legally race a deferred kick against a
// just-freed buffer. Mirror's Edge's audio teardown does exactly that: the UE3
// wave destructor frees the 2.5 MB XMA input buffer at 0x11c40000 while the
// submission loop in sub_8282C598 still kicks context 0, the XMA bridge then
// reads a packet header inside the now-freed buffer, and the fault is only
// reachable here because a previous build detached freed aliases to an
// anonymous PROT_NONE mapping (work/startup/xma-order-game.log, exit 43 with
// committed=0, watched=0). Retaining the mapping is therefore not a workaround
// that hides the fault: it is the platform contract, and the commit bitmap is
// still cleared so MeGpuPhysicalView::resolve keeps rejecting the range for
// every GPU consume. Set ME_STRICT_PHYSICAL_FREE=1 to restore the detaching
// behavior when auditing who touches a freed allocation.
static bool strict_physical_free() {
    static const bool strict = [] {
        const char* flag = std::getenv("ME_STRICT_PHYSICAL_FREE");
        return flag && *flag && std::strcmp(flag, "0") != 0;
    }();
    return strict;
}
static bool map_physical(uint32_t physical, uint32_t length, bool commit) {
    if (page_size != 4096) return false;
    if (physical_fd < 0) {
        physical_fd = memfd_create("me-guest-physical", MFD_CLOEXEC);
        if (physical_fd < 0) return false;
        if (ftruncate(physical_fd, 0x20000000)) {
            close(physical_fd); physical_fd = -1; return false;
        }
        auto* raw = static_cast<uint8_t*>(mmap(nullptr, 0x20000000, PROT_NONE, MAP_SHARED, physical_fd, 0));
        if (raw == MAP_FAILED) { close(physical_fd); physical_fd = -1; return false; }
        gpu_physical = {raw, &physical_memory, &me_gpu_write_watch};
        me_gpu_write_watch.guest = guest;
        // Write-watch faults must be handled before the game's own handler is installed
        // (self-tests); fault() also forwards them once it replaces this one.
        struct sigaction action{};
        action.sa_sigaction = write_watch_fault; action.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, nullptr);
    }
    static const bool alloc_invalidate = [] { const char* v = std::getenv("ME_GPU_ALLOC_INVALIDATE"); return !(v && !std::strcmp(v, "0")); }(); // default on (route A/B 2026-10-25)
    if (!commit) {
        me_gpu_write_watch.set_committed(physical, length, false);
        if (alloc_invalidate) MeRexGpuMemory::InvalidateAllocatedRange(physical, length);
        else MeRexGpuMemory::InvalidateReleasedRange(physical, length);
    }
    const uint32_t aliases[] = {0xa0000000 + physical, 0xc0000000 + physical, 0xe0000000 + physical - 0x1000};
    // A release keeps the memfd attachment and only the strict audit mode
    // detaches; the next commit overwrites all three windows in either case.
    const bool detach = !commit && strict_physical_free();
    if (commit) {
        for (unsigned i = 0; i < 3; ++i) {
            auto* target = guest + aliases[i];
            void* result = mmap(target, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, physical_fd, physical);
            if (result != target) {
                // Roll back every view inside our own reserved guest mapping.
                for (unsigned j = 0; j < i; ++j)
                    if (mmap(guest + aliases[j], length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED) std::exit(2);
                return false;
            }
        }
    } else if (detach) {
        for (unsigned i = 0; i < 3; ++i) {
            if (mmap(guest + aliases[i], length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED) std::exit(2);
        }
    } else {
        for (unsigned i = 0; i < 3; ++i)
            if (mprotect(guest + aliases[i], length, PROT_READ | PROT_WRITE)) std::exit(2);
    }
    // The raw view stays readable with the mapping retained; the GPU is kept
    // out by the cleared commit bitmap, not these permissions.
    if (mprotect(gpu_physical.raw + physical, length, detach ? PROT_NONE : PROT_READ | PROT_WRITE)) {
        std::perror("physical raw view protection"); std::exit(2);
    }
    if (commit) {
        std::memset(guest + aliases[0], 0, length); me_gpu_write_watch.set_committed(physical, length, true);
        if (alloc_invalidate) MeRexGpuMemory::InvalidateAllocatedRange(physical, length); // drop stale GPU copies
    }
    return true;
}

static void store_be32(uint32_t address, uint32_t value) {
    const uint32_t encoded = __builtin_bswap32(value);
    std::memcpy(guest + address, &encoded, 4);
}
static void install_module(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) { std::perror("XEX headers"); std::exit(2); }
    if (std::fseek(file, 0, SEEK_END)) std::exit(2);
    const long length = std::ftell(file);
    if (length < 24 || length > 1024 * 1024 || std::fseek(file, 0, SEEK_SET)) std::exit(2);
    constexpr uint32_t header_base = 0x60010000, variable = 0x60000000, module = 0x60001000;
    writable(variable, 0x4000);
    store_be32(0x60003000, 0); // VdGlobalDevice: game-owned D3D object pointer.
    store_be32(0x60003020, 0); // VdGlobalXamDevice: no XAM D3D device.
    store_be32(0x60003040, 0); // No Xbox kernel debug monitor attached.
    store_be32(0x60003060, 0); // No Xbox certification monitor attached.
    // VdHSIOCalibrationLock, real shared guest RTL critical-section state.
    std::memset(guest + 0x60003080, 0, 28);
    guest[0x60003080] = 1; guest[0x60003081] = 40; // ceil(10000 / 256)
    store_be32(0x60003090, UINT32_MAX);
    constexpr uint32_t command_line = 0x60002000;
    const char launch_command[] = "\"default.xex\"";
    std::memcpy(guest + command_line, launch_command, sizeof(launch_command));
    writable(header_base, size_t(length));
    read_exact(file, guest + header_base, size_t(length));
    std::fclose(file);
    module_headers = {guest + header_base, size_t(length), header_base};
    if (!module_headers.valid()) { std::fprintf(stderr, "Invalid XEX header\n"); std::exit(2); }
    uint32_t kernel_target=0,kernel_minimum=0;
    if(!module_headers.import_version("xboxkrnl.exe",kernel_target,kernel_minimum) || kernel_target<kernel_minimum || (kernel_target>>24)!=0x20){
        std::fprintf(stderr,"Unsupported or invalid XEX kernel version profile\n");std::exit(2);
    }
    const uint16_t kernel_fields[]={uint16_t(kernel_target>>28),uint16_t((kernel_target>>24)&15),uint16_t((kernel_target>>8)&65535),uint16_t(kernel_target&255)};
    for(unsigned i=0;i<4;++i){const uint16_t encoded=__builtin_bswap16(kernel_fields[i]);std::memcpy(guest+0x60003100+i*2,&encoded,2);}
    std::printf("{\"event\":\"guest_kernel_version_profile\",\"source\":\"xex_import_target\",\"major\":%u,\"minor\":%u,\"build\":%u,\"qfe\":%u,\"actual_console_version\":false}\n",kernel_fields[0],kernel_fields[1],kernel_fields[2],kernel_fields[3]);
    uint32_t entry = 0, image_cell = 0;
    if (!module_headers.lookup(0x10100, entry) || !module_headers.lookup(0x10201, image_cell) ||
        MeXexHeaders::be32(guest + image_cell) != PPC_IMAGE_BASE) std::exit(2);
    // Minimal LDR_DATA_TABLE_ENTRY fields, all derived from the loaded module.
    // Lists, kernel object handle and name lookup are deliberately not implemented.
    store_be32(variable, module);
    store_be32(module + 0x1c, uint32_t(PPC_IMAGE_BASE));
    store_be32(module + 0x20, uint32_t(PPC_IMAGE_SIZE));
    store_be32(module + 0x38, uint32_t(PPC_IMAGE_SIZE));
    store_be32(module + 0x3c, entry);
    store_be32(module + 0x58, header_base);
    bool bound = false;
    for (unsigned i = 0; i < guard_count; ++i) {
        if (std::strcmp(guards[i].name, "XexExecutableModuleHandle") == 0) {
            store_be32(guards[i].iat, variable);
            bound = true;
        }
        if (std::strcmp(guards[i].name, "KeDebugMonitorData") == 0) store_be32(guards[i].iat, 0x60003040);
        if (std::strcmp(guards[i].name, "KeCertMonitorData") == 0) store_be32(guards[i].iat, 0x60003060);
        if (std::strcmp(guards[i].name, "VdHSIOCalibrationLock") == 0) store_be32(guards[i].iat, 0x60003080);
        if (std::strcmp(guards[i].name, "VdGlobalDevice") == 0) store_be32(guards[i].iat, 0x60003000);
        if (std::strcmp(guards[i].name, "VdGlobalXamDevice") == 0) store_be32(guards[i].iat, 0x60003020);
        if (std::strcmp(guards[i].name, "XboxKrnlVersion") == 0) store_be32(guards[i].iat, 0x60003100);
        if (std::strcmp(guards[i].name, "VdGpuClockInMHz") == 0) {
            // Fixed guest Xenos clock profile, matching pinned SDK, not Adreno telemetry.
            store_be32(0x60003120,500);store_be32(guards[i].iat,0x60003120);
        }
        if (std::strcmp(guards[i].name, "ExLoadedCommandLine") == 0)
            store_be32(guards[i].iat, command_line);
    }
    if (!bound) std::exit(2);
    // Read-only header pages after installation.
    const size_t mapped = (size_t(length) + page_size - 1) & ~(page_size - 1);
    if (mprotect(guest + header_base, mapped, PROT_READ)) std::exit(2);
    std::printf("{\"event\":\"module_installed\",\"module\":\"%08x\",\"header\":\"%08x\",\"header_bytes\":%ld,\"entry\":\"%08x\"}\n",
        module, header_base, length, entry);
}

static void initialize_thread(PPCContext& context, unsigned index, uint32_t stack_size) {
    if(index>=max_guest_threads || stack_size>0x100000){std::fprintf(stderr,"Thread address-space capacity exceeded\n");std::exit(2);}
    pcr_address = 0x61000000 + index * 0x10000;
    thread_address = pcr_address + 0x1000;
    const uint32_t low = index ? 0x71001000 + index * 0x200000 : 0x70000000;
    const uint32_t high = low + stack_size;
    writable(low, stack_size);

    writable(pcr_address, 0x2000);
    put_word(pcr_address + 0x30 + 4, pcr_address); // 64-bit BE self pointer
    put_word(pcr_address + 0x70, high);
    put_word(pcr_address + 0x74, low);
    put_word(pcr_address + 0x100, thread_address);
    put_word(pcr_address + 0x2a8, pcr_address + 0x100);
    put_word(thread_address + 0x5c, high);
    put_word(thread_address + 0x60, low);
    guest[thread_address + 0x72] = guest[thread_address + 0x73] = process_type;
    put_word(thread_address + 0x14c, index + 1);
    uint32_t tls_header = 0, raw = 0, extended = 0, raw_size = 0;
    tls_slots = 1024;
    if (module_headers.lookup(0x20104, tls_header)) {
        tls_slots = load_word(tls_header);
        raw = load_word(tls_header + 4);
        extended = load_word(tls_header + 8);
        raw_size = load_word(tls_header + 12);
    }
    if (!tls_slots || tls_slots > 4096 || extended > 0x100000 || raw_size > extended) std::exit(2);
    if (raw_size) {
        bool valid_raw = false;
        for (unsigned i = 0; i < image_range_count; ++i)
            if (raw >= image_ranges[i].address && uint64_t(raw) + raw_size <=
                uint64_t(image_ranges[i].address) + image_ranges[i].size) valid_raw = true;
        if (!valid_raw) std::exit(2);
    }
    const uint32_t tls_static = 0x62000000 + index * 0x200000;
    writable(tls_static, extended + tls_slots * 4);
    if (raw_size) std::memcpy(guest + tls_static, guest + raw, raw_size);
    tls_dynamic = tls_static + extended;
    put_word(pcr_address, tls_static); put_word(thread_address + 0x68, tls_dynamic);
    std::printf("{\"event\":\"thread_initialized\",\"pcr\":\"%08x\",\"thread\":\"%08x\",\"tls_slots\":%u,\"tls_static_bytes\":%u}\n", pcr_address, thread_address, tls_slots, extended);
    context.r13.u64 = pcr_address;
    put_word(thread_address + 0x64, high - 240);
    context.r1.u64 = high - 0x100;
    context.lr = 0;
    context.fpscr.csr = context.fpscr.getcsr();
    auto record = std::make_shared<ThreadRecord>();
    record->id = index + 1; record->handle = 0x20000 + index * 4;
    record->pcr = pcr_address; record->kthread = thread_address; record->tls = tls_dynamic;
    record->low = low; record->high = high;
    threads.push_back(record);
}
// Caller holds execution_gate. Only this loader's XenonRecomp context is used.
static uint64_t dispatch_gpu_interrupt_locked(PPCContext& context, const ThreadRecord& record,
        const MeGpuInterruptWorker::Request& request) {
    static uint64_t dispatches=0;const auto dispatch=++dispatches;
    const bool trace=gpu_explore_mode && (dispatch<=16 || !(dispatch&(dispatch-1)));
    auto snapshot=[&](const char* stage){
        uint32_t mailbox=0;
        if(uint64_t(request.argument)+10904<=0x100000000ull && checked_span(request.argument+10900,4,true))mailbox=load_word(request.argument+10900);
        const bool valid=checked_span(mailbox,24,true);
        std::printf("{\"event\":\"gpu_irq_dispatch\",\"stage\":\"%s\",\"call\":%llu,\"source\":%u,\"cpu\":%u,\"mailbox\":\"%08x\",\"valid\":%s,\"cpu_mask\":\"%08x\",\"semaphore\":%u,\"callback\":\"%08x\",\"callback_argument\":\"%08x\"}\n",stage,(unsigned long long)dispatch,request.source,request.cpu,mailbox,valid?"true":"false",valid?load_word(mailbox):0,valid?load_word(mailbox+4):0,valid?load_word(mailbox+16):0,valid?load_word(mailbox+20):0);
    };
    if(trace)snapshot("begin");
    const uint32_t cpu = request.cpu == UINT32_MAX ? 2 : request.cpu;
    if (cpu >= 6 || (request.callback & 3) || request.callback < PPC_CODE_BASE ||
        request.callback >= PPC_CODE_BASE + PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(guest, request.callback))
        throw std::runtime_error("GPU interrupt callback is not mapped");
    if (context.r13.u32 != record.pcr || context.r1.u64 < record.low + 176 || context.r1.u64 > record.high)
        throw std::runtime_error("GPU interrupt context/stack is invalid");
    const auto previous_context = active_context;
    const auto previous_pcr = pcr_address, previous_thread = thread_address, previous_tls = tls_dynamic;
    const auto saved_stack = context.r1.u64, saved_lr = context.lr;
    const auto saved_tls = load_word(record.pcr);
    const auto saved_irql = guest[record.pcr + 0x18];
    const auto saved_cpu = guest[record.pcr + 0x10c], saved_thread_cpu = guest[record.kthread + 0xbf];
    const auto saved_fpscr = context.fpscr.getcsr();
    struct Restore {
        std::function<void()> action;
        ~Restore() { action(); }
    } restore{[&] {
        put_word(record.pcr, saved_tls);
        guest[record.pcr + 0x18] = saved_irql;
        guest[record.pcr + 0x10c] = saved_cpu; guest[record.kthread + 0xbf] = saved_thread_cpu;
        context.r1.u64 = saved_stack; context.lr = saved_lr;
        context.fpscr.setcsr(saved_fpscr);
        active_context = previous_context;
        pcr_address = previous_pcr; thread_address = previous_thread; tls_dynamic = previous_tls;
    }};
    active_context = &context;
    pcr_address = record.pcr; thread_address = record.kthread; tls_dynamic = record.tls;
    guest[record.pcr + 0x10c] = guest[record.kthread + 0xbf] = uint8_t(cpu);
    put_word(record.pcr, 0); // SDK interrupt convention: PCR TLS pointer is zero.
    // The external GPU callback runs as deferred guest interrupt work at DISPATCH.
    // This is guest bookkeeping; it does not mask Android/host interrupts.
    guest[record.pcr + 0x18] = 2;
    context.r1.u64 -= 176; context.lr = 0xbcbcbcbc;
    context.r3.u64 = request.source; context.r4.u64 = request.argument;
    context.fpscr.setcsr(context.fpscr.csr);
    (PPC_LOOKUP_FUNC(guest, request.callback))(context, guest);
    if(trace)snapshot("end");
    return context.r3.u64;
}
// The GPU command processor runs beside guest threads, as on the console: after
// setup it does not take the execution gate. CPU writes reach it through the page
// write watch, interrupts through MeGpuInterruptWorker (which takes the gate), and
// SharedMemory/texture state is guarded by RexGlue's own global critical region.
// ME_GPU_GATE=1 restores the serialized behaviour for A/B comparison.
static std::atomic<bool> gpu_gate_always{false}, gpu_cp_released{false};
static thread_local std::vector<bool> gpu_gate_held;
static void gpu_gate_enter() {
    const bool take = gpu_gate_always.load() || !gpu_cp_released.load();
    if (take) execution_gate.lock();
    gpu_gate_held.push_back(take);
}
static void gpu_gate_leave() {
    if (gpu_gate_held.empty()) return;
    const bool held = gpu_gate_held.back(); gpu_gate_held.pop_back();
    if (held) execution_gate.unlock();
}
static void start_game_gpu(PPCContext& ctx) {
    if (game_gpu_ready) { ctx.r3.u64 = 1; return; }
    if (!gpu_physical.raw || threads.size() >= max_guest_threads) me_stop_import("GPU_memory_or_thread_capacity_unavailable", ctx);
    const auto old_pcr = pcr_address, old_thread = thread_address, old_tls = tls_dynamic;
    auto irq_context = std::make_shared<PPCContext>();
    initialize_thread(*irq_context, unsigned(threads.size()), 0x8000);
    auto record = threads.back();
    pcr_address = old_pcr; thread_address = old_thread; tls_dynamic = old_tls;
    game_gpu_irq_worker = new MeGpuInterruptWorker;
    if (!game_gpu_irq_worker->start([irq_context, record](const MeGpuInterruptWorker::Request& request) {
        std::lock_guard<MeExecutionGate> lock(execution_gate);
        dispatch_gpu_interrupt_locked(*irq_context, *record, request);
    })) me_stop_import("GPU_interrupt_worker_start_failed", ctx);
    int ready = 0;
    {
        std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock);
        gate.unlock(); // SDK SetupContext acquires the same gate on its own worker.
        ready = me_gpu_start(&gpu_physical, &gpu_mmio,
            +[] { gpu_gate_enter(); }, +[] { gpu_gate_leave(); },
            +[](uint32_t callback, uint32_t argument, uint32_t source, uint32_t cpu) {
                const bool accepted=game_gpu_irq_worker->submit({callback,argument,source,cpu});
                static std::atomic<uint64_t> calls{0};const auto call=++calls;
                if(call<=16 || !(call&(call-1)))std::printf("{\"event\":\"gpu_irq_submitted\",\"call\":%llu,\"source\":%u,\"cpu\":%u,\"accepted\":%s}\n",(unsigned long long)call,source,cpu,accepted?"true":"false");
                return accepted;
            });
        gate.lock(); gate.release();
    }
    if (!ready) me_stop_import("GPU_real_context_setup_failed", ctx);
    if (const char* v = std::getenv("ME_GPU_GATE"); v && !std::strcmp(v, "1")) gpu_gate_always = true;
    game_gpu_ready = true; gpu_cp_released = true;
    std::printf("{\"event\":\"gpu_command_processor_gate\",\"serialized\":%s}\n", gpu_gate_always ? "true" : "false");
    if(gpu_explore_mode){
        auto* vblank=new MeGpuVblankWorker;
        if(!vblank->start([]{
            // Diagnostics: delivered vblanks and the wait for the shared gate, every 2 s.
            static auto window=std::chrono::steady_clock::now();
            static uint64_t delivered=0,wait_total_us=0,wait_max_us=0,epoch_start=me_gate_epoch.load();
            const auto requested=std::chrono::steady_clock::now();
            std::lock_guard<MeExecutionGate> lock(execution_gate);
            const auto waited=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-requested).count());
            me_gpu_vblank();
            ++delivered;wait_total_us+=waited;wait_max_us=std::max(wait_max_us,waited);
            const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-window).count();
            if(seconds>=2){
                const auto epoch=me_gate_epoch.load();
                auto& stats=MeRexGpuMemory::scan_stats;
                const auto calls=stats.calls.exchange(0),scans=stats.scans.exchange(0),pages=stats.pages.exchange(0);
                std::printf("{\"event\":\"vblank_rate\",\"hz\":%.2f,\"gate_wait_avg_us\":%llu,\"gate_wait_max_us\":%llu,\"gate_handovers_per_s\":%.0f,\"gpu_sync_calls_per_s\":%.0f,\"gpu_scans_per_s\":%.0f,\"gpu_pages_per_scan\":%llu}\n",
                    delivered/seconds,(unsigned long long)(wait_total_us/delivered),(unsigned long long)wait_max_us,(epoch-epoch_start)/seconds,
                    calls/seconds,scans/seconds,(unsigned long long)(scans?pages/scans:0));
                // Per-thread gate use over the window (host threads: 0xf000xxxx = host tid).
                char line[2048];int n=std::snprintf(line,sizeof(line),"{\"event\":\"gate_threads\",\"seconds\":%.2f,\"threads\":[",seconds);
                bool first=true;
                for(uint32_t i=0;i<std::min<uint32_t>(me_gate_slots.load(),64);++i){
                    auto& t=me_gate_stats[i];
                    const auto acq=t.acquisitions.exchange(0),wait=t.wait_ns.exchange(0),hold=t.hold_ns.exchange(0);
                    if(!acq || n>=int(sizeof(line))-128)continue;
                    n+=std::snprintf(line+n,sizeof(line)-n,"%s{\"t\":\"%08x\",\"acq_s\":%.0f,\"wait_ms_s\":%.1f,\"hold_ms_s\":%.1f}",first?"":",",t.label.load(),acq/seconds,wait/1e6/seconds,hold/1e6/seconds);
                    first=false;
                }
                std::printf("%s]}\n",line);
                {   // Top kernel-lock imports over the window.
                    MeImportCounter* top[6]{}; uint64_t counts[6]{};
                    for (auto* c = MeImportCounter::head.load(); c; c = c->next) {
                        const auto n = c->calls.exchange(0);
                        for (int k = 0; k < 6; ++k) if (n > counts[k]) { for (int m = 5; m > k; --m) { counts[m] = counts[m-1]; top[m] = top[m-1]; } counts[k] = n; top[k] = c; break; }
                    }
                    std::printf("{\"event\":\"hot_imports\",\"per_s\":[");
                    for (int k = 0; k < 6 && top[k]; ++k) std::printf("%s{\"%s\":%.0f}", k ? "," : "", top[k]->name, counts[k] / seconds);
                    std::printf("]}\n");
                }
                window=std::chrono::steady_clock::now();delivered=wait_total_us=wait_max_us=0;epoch_start=epoch;
            }
        }))me_stop_import("GPU_virtual_vblank_start_failed",ctx);
        // Probe process owns this worker until _exit; normal lifecycle is pending.
        std::puts("{\"event\":\"virtual_vblank_started\",\"hz\":60,\"clock\":\"monotonic\",\"physical_display_vsync\":false}");
    }
    ctx.r3.u64 = 1;
    std::printf("{\"event\":\"game_gpu_ready\",\"same_loader_memory\":true,\"presentation\":false,\"automatic_vblank\":%s}\n",gpu_explore_mode?"true":"false");
}
static void create_guest_thread(PPCContext& ctx) {
    const auto out = ctx.r3.u32, id_out = ctx.r5.u32, startup = ctx.r6.u32;
    const auto entry = ctx.r7.u32, argument = ctx.r8.u32, flags = ctx.r9.u32;
    uint32_t stack = ctx.r4.u32;
    const auto affinity = flags >> 24;
    if ((flags & ~0x3f000001u) || affinity > 0x3f)
        me_stop_import("ExCreateThread_flags_not_supported", ctx);
    if (!stack) { if (!module_headers.lookup(0x20200, stack)) stack = 0x100000; }
    if (stack > 0x100000 || threads.size() >= max_guest_threads) me_stop_import("ExCreateThread_probe_capacity_exceeded", ctx);
    stack = std::max(0x4000u, (stack + 0xfff) & ~0xfffu);
    if ((out && !word_writable(out)) || (id_out && !word_writable(id_out))) me_stop_import("ExCreateThread_invalid_outputs", ctx);
    const uint32_t address = startup ? startup : entry;
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(guest, address))
        me_stop_import("ExCreateThread_unknown_entry", ctx);
    const unsigned index = threads.size();
    auto child = std::make_shared<PPCContext>();
    // Initialize memory while owning the execution gate; only the child host
    // thread's thread_local bindings are changed when it starts running.
    const auto old_pcr = pcr_address, old_thread = thread_address, old_tls = tls_dynamic;
    initialize_thread(*child, index, stack);
    auto record = threads.back();
    pcr_address = old_pcr; thread_address = old_thread; tls_dynamic = old_tls;
    record->suspend_count = uint8_t(flags & 1);
    record->affinity = affinity ? affinity : 1;
    unsigned cpu = 0; while (!(record->affinity & (1u << cpu))) ++cpu;
    guest[record->kthread + 0xbc] = record->suspend_count;
    guest[record->pcr + 0x10c] = guest[record->kthread + 0xbf] = uint8_t(cpu);
    std::printf("{\"event\":\"thread_start_policy\",\"id\":%u,\"flags\":\"%08x\",\"suspended\":%s,\"guest_cpu\":%u,\"host_affinity_changed\":false}\n",record->id,flags,record->suspend_count ? "true":"false",cpu);
    child->r3.u64 = startup ? entry : argument; child->r4.u64 = startup ? argument : 0;
    try {
        std::thread([child, record, address] {
            active_context = child.get(); pcr_address = record->pcr; thread_address = record->kthread; tls_dynamic = record->tls;
            register_diagnostic_thread();
            std::unique_lock<MeExecutionGate> run(execution_gate);
            state_changed.wait(run, [&] { return !record->suspend_count; });
            child->fpscr.setcsr(child->fpscr.csr);
            std::printf("{\"event\":\"thread_begin\",\"id\":%u,\"entry\":\"%08x\"}\n", record->id, address);
            if (guest_parallel) run.unlock(); // guest code runs beside other threads
            try { (PPC_LOOKUP_FUNC(guest, address))(*child, guest); record->exit_code = child->r3.u32; }
            catch (const ThreadExit& e) { record->exit_code = e.code; }
            if (!execution_gate.owned()) { run.release(); execution_gate.lock(); run = std::unique_lock<MeExecutionGate>(execution_gate, std::adopt_lock); }
            mutants.abandon(record->kthread);
            record->done = true;
            std::printf("{\"event\":\"thread_exit\",\"id\":%u,\"code\":%u}\n", record->id, record->exit_code);
            state_changed.notify_all();
        }).detach();
    } catch (const std::system_error&) { threads.pop_back(); ctx.r3.u64 = 0xc000009a; return; }
    if (out) put_word(out, record->handle);
    if (id_out) put_word(id_out, record->id);
    ctx.r3.u64 = 0;
    std::printf("{\"event\":\"thread_created\",\"id\":%u,\"handle\":\"%08x\",\"stack_bytes\":%u,\"pcr\":\"%08x\",\"tls\":\"%08x\"}\n", record->id, record->handle, stack, record->pcr, record->tls);
}

PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) { // thread-local PCR byte: no kernel lock
    if (ctx.r13.u32 != pcr_address || guest[pcr_address + 0x18] > 2)
        me_stop_import("raise_irql_to_dispatch_invalid", ctx);
    ctx.r3.u64 = guest[pcr_address + 0x18]; guest[pcr_address + 0x18] = 2;
}
PPC_FUNC(__imp__KfLowerIrql) { // thread-local PCR byte: no kernel lock
    if (ctx.r13.u32 != pcr_address || ctx.r3.u32 > guest[pcr_address + 0x18])
        me_stop_import("lower_irql_invalid", ctx);
    guest[pcr_address + 0x18] = uint8_t(ctx.r3.u32);
}
ME_IMPORT(__imp__NtResumeThread) {
    const auto output=ctx.r4.u32;
    if(output && !word_writable(output))me_stop_import("resume_thread_invalid_count_pointer",ctx);
    auto record=find_thread(ctx.r3.u32);uint32_t previous=0,status=0xc0000008;
    if(record && !record->done){previous=record->suspend_count;if(previous)--record->suspend_count;
        guest[record->kthread+0xbc]=record->suspend_count;status=0;state_changed.notify_all();}
    if(output)put_word(output,previous);ctx.r3.u64=status;
}
ME_IMPORT(__imp__KeResumeThread) {
    for (const auto& record : threads) if (record->kthread == ctx.r3.u32 && !record->done) {
        if (record->suspend_count) --record->suspend_count;
        guest[record->kthread + 0xbc] = record->suspend_count;
        state_changed.notify_all(); ctx.r3.u64 = 0; return;
    }
    ctx.r3.u64 = 0xc0000008;
}
static PPC_FUNC(suspended_thread_test_entry) {
    put_word(0x70000c00, guest[ctx.r13.u32 + 0x10c] + 1);
    ctx.r3.u64 = 0;
}
static MeAudioOutput* audio_output;
static bool audio_output_is_ready() { return audio_output && audio_output->stream; }
static std::mutex audio_write_mutex;
static std::atomic<uint64_t> audio_frames_submitted{0};
ME_IMPORT(__imp__XAudioSubmitRenderDriverFrame) {
    if (!gpu_explore_mode || ctx.r3.u32 != 0x41550000 || !audio_output_is_ready() ||
        !checked_span(ctx.r4.u32, 6 * 256 * 4, true))
        me_stop_import("audio_submit_invalid_client_or_frame", ctx);
    float stereo[512]; me_pcm_stereo(guest + ctx.r4.u32, stereo);
    MePcmWriteResult result;
    { std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock);
      gate.unlock();
      { std::lock_guard<std::mutex> writer(audio_write_mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        result = me_pcm_write_all(stereo, [&](const float* data, int frames) {
            if (std::chrono::steady_clock::now() >= deadline) return int(AAUDIO_ERROR_TIMEOUT);
            return int(AAudioStream_write(audio_output->stream, data, frames, 20000000));
        });
        if (const int32_t grown = audio_output->adapt())
          std::printf("{\"event\":\"audio_xrun\",\"underruns\":%d,\"buffer_frames\":%d}\n", audio_output->xruns, grown); }
      gate.lock(); gate.release(); }
    const bool accepted = result.frames == 256 && !result.error;
    const auto submitted = accepted ? audio_frames_submitted.fetch_add(1) + 1 : audio_frames_submitted.load();
    if (!accepted || submitted <= 8 || !(submitted & (submitted - 1)))
        std::printf("{\"event\":\"audio_pcm_submit\",\"accepted_frames\":%d,\"requested_frames\":256,\"error\":%d,\"submitted\":%llu,\"playback_completion_claimed\":false}\n", result.frames, result.error, (unsigned long long)submitted);
    ctx.r3.u64 = accepted ? 0 : 0x80004005;
}
static void register_audio_client(PPCContext& ctx) {
    if (!gpu_explore_mode) me_stop_import("audio_client_requires_explore", ctx);
    const auto input = ctx.r3.u32, output = ctx.r4.u32;
    if (!input || !checked_span(input, 8, true) || !checked_span(output, 4)) { ctx.r3.u64 = 0x80070057; return; }
    const auto callback = load_word(input), argument = load_word(input + 4);
    if ((callback & 3) || callback < PPC_CODE_BASE || callback >= PPC_CODE_BASE + PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(guest, callback)) {
        ctx.r3.u64 = 0x80070057; return;
    }
    if (audio_output || threads.size() >= max_guest_threads) me_stop_import("audio_client_capacity_exceeded", ctx);
    auto* endpoint = new MeAudioOutput;
    { std::unique_lock<MeExecutionGate> gate(execution_gate, std::adopt_lock);
      gate.unlock(); const bool opened = endpoint->open(); gate.lock(); gate.release();
      if (!opened) { delete endpoint; ctx.r3.u64 = 0x80004005; return; } }
    const auto old_pcr = pcr_address, old_thread = thread_address, old_tls = tls_dynamic;
    auto context = std::make_shared<PPCContext>();
    initialize_thread(*context, unsigned(threads.size()), 0x10000);
    auto record = threads.back();
    pcr_address = old_pcr; thread_address = old_thread; tls_dynamic = old_tls;
    // SDK passes a pointer to a BE32 wrapped argument, not the value itself.
    const auto wrapped = record->pcr + 0x1800; // Reserved loader-owned metadata, outside KTHREAD.
    if (!word_writable(wrapped)) { delete endpoint; me_stop_import("audio_wrapped_arg_unmapped", ctx); }
    put_word(wrapped, argument);
    audio_output = endpoint;
    try {
        std::thread([context, record, callback, wrapped] {
            active_context = context.get(); pcr_address = record->pcr;
            thread_address = record->kthread; tls_dynamic = record->tls;
            register_diagnostic_thread();
            std::unique_lock<MeExecutionGate> gate(execution_gate);
            if (guest_parallel) gate.unlock(); // the callback's imports take the kernel lock
            // The render driver asks for one 256-sample frame per callback. Each
            // submitted frame is a blocking AAudio write, so the callback rate
            // follows real device consumption; no consumed-frame notification is
            // invented. A callback that submits nothing waits one device period
            // (256 / 48000 s) instead of spinning.
            constexpr auto period = std::chrono::nanoseconds(5333333);
            auto next = std::chrono::steady_clock::now();
            for (uint64_t call = 0;; ++call) {
                const auto before = audio_frames_submitted.load();
                const auto stack = context->r1.u64;
                context->r1.u64 -= 176; context->lr = 0xbcbcbcbc;
                context->r3.u64 = wrapped; context->fpscr.setcsr(context->fpscr.csr);
                const bool log = call < 4 || !(call & (call - 1));
                if (log) std::printf("{\"event\":\"audio_callback_begin\",\"callback\":\"%08x\",\"wrapped_argument\":\"%08x\",\"call\":%llu}\n", callback, wrapped, (unsigned long long)call);
                try { (PPC_LOOKUP_FUNC(guest, callback))(*context, guest); }
                catch (...) { std::fprintf(stderr,"audio callback threw; stopping probe\n"); std::_Exit(96); }
                context->r1.u64 = stack; context->lr = 0;
                const bool submitted = audio_frames_submitted.load() != before;
                if (log) std::printf("{\"event\":\"audio_callback_returned\",\"call\":%llu,\"submitted\":%s}\n", (unsigned long long)call, submitted ? "true" : "false");
                next += period;
                const auto now = std::chrono::steady_clock::now();
                if (submitted) {
                    // The blocking write already waited for space; resynchronize the idle clock.
                    if (next < now) next = now;
                } else {
                    if (next < now) next = now + period;
                    if (gate.owns_lock()) gate.unlock();
                    std::this_thread::sleep_until(next);
                    if (!guest_parallel) gate.lock();
                }
            }
        }).detach();
    } catch (const std::system_error&) { audio_output = nullptr; delete endpoint; threads.pop_back(); ctx.r3.u64 = 0x80004005; return; }
    put_word(output, 0x41550000); ctx.r3.u64 = 0;
    std::printf("{\"event\":\"audio_client_registered\",\"callback\":\"%08x\",\"argument\":\"%08x\",\"endpoint\":\"AAudio\",\"rate\":48000,\"channels\":2,\"pcm_submission_implemented\":true}\n", callback, argument);
}
// App pause (GameActivity.onStop/onStart): a host thread takes the big kernel lock and
// keeps it until resume, so every guest thread stops at its next kernel import (waits,
// critical sections, audio submit, GPU interrupts all take it); the AAudio stream is
// paused so the device plays nothing. Guest time is host time: after a resume the game
// sees the paused interval as one long frame.
static std::mutex pause_mutex;
static std::condition_variable pause_changed;
static bool pause_requested = false, pause_holding = false;
extern "C" void me_runtime_set_paused(bool paused) {
    std::unique_lock<std::mutex> lock(pause_mutex);
    if (paused == pause_requested) return;
    pause_requested = paused;
    if (paused) {
        std::thread([] {
            execution_gate.lock();   // may wait for the current owner's import to finish
            if (audio_output_is_ready()) AAudioStream_requestPause(audio_output->stream);
            std::unique_lock<std::mutex> lock(pause_mutex);
            pause_holding = true; pause_changed.notify_all();
            std::puts("{\"event\":\"runtime_paused\"}");
            pause_changed.wait(lock, [] { return !pause_requested; });
            if (audio_output_is_ready()) { AAudioStream_requestFlush(audio_output->stream); AAudioStream_requestStart(audio_output->stream); }
            pause_holding = false;
            execution_gate.unlock();
            pause_changed.notify_all();
            std::puts("{\"event\":\"runtime_resumed\"}");
        }).detach();
        // Bounded: the UI thread must not hang if a guest thread holds the lock for long.
        pause_changed.wait_for(lock, std::chrono::seconds(2), [] { return pause_holding; });
    } else {
        pause_changed.notify_all();
    }
}
// Jump-table cases XenonRecomp missed. sub_82D37AD8 switches on r11 (0..6) through a
// table at 0x82D37B80 that the analysis took for code: case 0 (0x82D37B9C) ended up
// inside "sub_82D37B80" and case 6 (0x82D37BD0) inside sub_82D37BC8, so neither had a
// lookup entry and the indirect call jumped to address 0 (crash in chapter "Edge",
// lr 82d3cd78). Translated by hand from the guest instructions:
//   82D37B9C: andi. r11,r3,40 ; b 0x82D37BC8
//   82D37BD0: li r10,1 ; mr r3,r10 ; blr
static PPC_FUNC(me_case_82D37B9C) {
    ctx.r11.u64 = ctx.r3.u64 & 40;
    ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
    (PPC_LOOKUP_FUNC(base, 0x82D37BC8))(ctx, base);
}
static PPC_FUNC(me_case_82D37BD0) {
    ctx.r10.s64 = 1;
    ctx.r3.u64 = ctx.r10.u64;
}
static void install_missing_jump_cases() {
    const struct { uint32_t guest; PPCFunc* host; } cases[] = {{0x82D37B9C, me_case_82D37B9C}, {0x82D37BD0, me_case_82D37BD0}};
    for (const auto& c : cases)
        if (!PPC_LOOKUP_FUNC(guest, c.guest)) PPC_LOOKUP_FUNC(guest, c.guest) = c.host;
}
void me_unmapped_indirect_call(uint32_t target, uint32_t lr) {
    std::printf("{\"event\":\"indirect_call_unmapped\",\"target\":\"%08x\",\"lr\":\"%08x\",\"thread\":\"%08x\"}\n", target, lr, thread_address);
    std::fflush(stdout);
    _exit(97);
}
static PPC_FUNC(thread_test_entry) {
    const auto event = ctx.r3.u32;
    ctx.r3.u64 = 17; __imp__KeTlsGetValue(ctx, base);
    if (ctx.r3.u32 != 0 || ctx.r13.u32 == 0x61000000 || ctx.r1.u32 < 0x71000000) _exit(70);
    ctx.r3.u64 = 17; ctx.r4.u64 = 0x22222222; __imp__KeTlsSetValue(ctx, base);
    ctx.r3.u64 = event; ctx.r4.u64 = 0; __imp__NtSetEvent(ctx, base);
    ctx.r3.u64 = 0x70000300; __imp__RtlEnterCriticalSection(ctx, base);
    put_word(0x70000200, load_word(0x70000200) + 1);
    __imp__RtlLeaveCriticalSection(ctx, base);
    ctx.r3.u64 = 7; __imp__ExTerminateThread(ctx, base);
}
PPC_FUNC(me_test_gpu_interrupt) {
    if (ctx.r4.u32 == UINT32_MAX) throw std::runtime_error("test GPU interrupt failure");
    put_word(0x70000520, ctx.r3.u32);
    put_word(0x70000524, ctx.r4.u32);
    put_word(0x70000528, load_word(ctx.r13.u32));
    put_word(0x7000052c, guest[ctx.r13.u32 + 0x10c]);
    put_word(0x70000530, ctx.r13.u32);
    put_word(0x70000534, thread_address);
    put_word(0x70000538, guest[ctx.r13.u32 + 0x18]);
    ctx.r3.u64 = 0x12345678;
}
static int self_test(PPCContext& ctx) {
    unsigned checks = 0;
    static const bool trace_checks = std::getenv("ME_TEST_TRACE") != nullptr; // prints every check line (hang hunting)
    auto expect = [&](bool value, std::source_location where = std::source_location::current()) { ++checks; if (trace_checks) std::fprintf(stderr, "check %u line %u owned %d\n", checks, unsigned(where.line()), int(execution_gate.owned())); if (!value) { std::fprintf(stderr, "self-test check %u failed (line %u)\n", checks, unsigned(where.line())); std::exit(1); } };
    {
        char path[]="/data/local/tmp/me-position-test-XXXXXX";
        int fd=mkstemp(path); expect(fd>=0); unlink(path);
        const uint8_t bytes[]={10,20,30,40}; expect(write(fd,bytes,4)==4);
        const uint32_t handle=0x30000+uint32_t(readonly_files.files.size())*4;
        readonly_files.files.push_back({fd,4,0});
        put_word(0x70001400,0);put_word(0x70001404,2);
        ctx.r3.u64=handle;ctx.r4.u64=0x70001410;ctx.r5.u64=0x70001400;ctx.r6.u64=8;ctx.r7.u64=14;
        __imp__NtSetInformationFile(ctx,guest);
        expect(ctx.r3.u32==0 && readonly_files.find(handle)->position==2 && load_word(0x70001414)==8);
        ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;ctx.r7.u64=0x70001410;
        ctx.r8.u64=0x70001420;ctx.r9.u64=2;ctx.r10.u64=0;__imp__NtReadFile(ctx,guest);
        expect(ctx.r3.u32==0 && guest[0x70001420]==30 && guest[0x70001421]==40 && readonly_files.find(handle)->position==4);
        put_word(0x70001400,0xffffffff);put_word(0x70001404,0xffffffff);
        ctx.r3.u64=handle;ctx.r4.u64=0x70001410;ctx.r5.u64=0x70001400;ctx.r6.u64=8;ctx.r7.u64=14;
        __imp__NtSetInformationFile(ctx,guest);
        expect(ctx.r3.u32==0xc000000d && readonly_files.find(handle)->position==4 && load_word(0x70001414)==0);
        expect(readonly_files.close(handle));
    }
    {
        ctx.r3.u64=0x70001400;ctx.r4.u64=0;ctx.r5.u64=0;__imp__NtCreateMutant(ctx,guest);
        expect(ctx.r3.u32==0);const auto handle=load_word(0x70001400);
        ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;__imp__NtWaitForSingleObjectEx(ctx,guest);
        expect(ctx.r3.u32==0 && mutants.find(handle)->owner==thread_address);
        ctx.r3.u64=handle;ctx.r4.u64=0;__imp__NtReleaseMutant(ctx,guest);expect(ctx.r3.u32==0);
        ctx.r3.u64=handle;__imp__NtClose(ctx,guest);expect(ctx.r3.u32==0 && !mutants.find(handle));
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        put_word(0x70001400,2);put_word(0x70001404,0x70001410);put_word(0x70001408,0x70001414);
        put_word(0x70001410,0xffffffff);put_word(0x70001414,0xffffffff);put_word(0x70001418,0xfeedbeef);
        ctx.r3.u64=0xfa;ctx.r4.u64=0x7001b;ctx.r5.u64=0x70001400;ctx.r6.u64=0;
        __imp__XMsgInProcessCall(ctx,guest);
        expect(ctx.r3.u32==0 && load_word(0x70001410)==0 && load_word(0x70001414)==0);
        expect(load_word(0x70001418)==0xfeedbeef && load_word(0x70001400)==2);
        ctx.r3.u64=0xfa;ctx.r4.u64=0x7001b;ctx.r5.u64=0x70001400;ctx.r6.u64=12;
        __imp__XMsgInProcessCall(ctx,guest);expect(ctx.r3.u32==0);
        gpu_explore_mode=saved;
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;const auto old=xmp_playback_client;
        put_word(0x70001400,2);put_word(0x70001404,0);put_word(0x70001408,0);put_word(thread_address+0x160,123);
        ctx.r3.u64=0xfa;ctx.r4.u64=0x7001a;ctx.r5.u64=0;ctx.r6.u64=0x70001400;ctx.r7.u64=12;ctx.r8.u64=0;
        __imp__XMsgStartIORequestEx(ctx,guest);expect(ctx.r3.u32==0 && xmp_playback_client==0);
        expect(load_word(thread_address+0x160)==0 && load_word(0x70001400)==2);
        put_word(0x70001408,1);ctx.r3.u64=0xfa;
        __imp__XMsgStartIORequestEx(ctx,guest);expect(ctx.r3.u32==0 && xmp_playback_client==1);
        xmp_playback_client=old;gpu_explore_mode=saved;
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        std::memset(guest+0x70001440,0x5a,32);
        ctx.r3.u64=0;ctx.r4.u64=1;ctx.r5.u64=0x70001440;__imp__XamInputGetCapabilities(ctx,guest);expect(ctx.r3.u32==1167);
        bool caps_unchanged=true;for(int i=0;i<32;++i)caps_unchanged&=guest[0x70001440+i]==0x5a;expect(caps_unchanged);
        ctx.r3.u64=0;ctx.r5.u64=0;__imp__XamInputGetCapabilities(ctx,guest);expect(ctx.r3.u32==160);
        std::memset(guest+0x70001400,0x5a,16);
        ctx.r3.u64=0;ctx.r4.u64=1;ctx.r5.u64=0x70001400;__imp__XamInputGetState(ctx,guest);
        expect(ctx.r3.u32==1167);bool unchanged=true;for(int i=0;i<16;++i)unchanged&=guest[0x70001400+i]==0x5a;expect(unchanged);
        ctx.r3.u64=0xff;ctx.r4.u64=0;ctx.r5.u64=0;__imp__XamInputGetState(ctx,guest);expect(ctx.r3.u32==1167);
        gpu_explore_mode=saved;
    }
    {
        uint8_t packet[32]={'M','E','I','1'};packet[11]=1;packet[15]=0x10;packet[16]=0x80;packet[24]=255;
        MeInputSnapshot value;expect(me_decode_input(packet,32,value));expect(value.connected && value.buttons==0x10 && value.axes[0]==-32768 && value.lt==255);
        expect(!me_decode_input(packet,31,value));packet[31]=1;expect(!me_decode_input(packet,32,value));packet[31]=0;packet[0]='X';expect(!me_decode_input(packet,32,value));
    }
    {
        uint32_t address=0,size=4096;expect(virtual_memory.allocate(address,size,0x3000)==0);
        std::memset(guest+address,0x5a,128);const bool saved=polling_yield_experiment;polling_yield_experiment=true;
        me_dcbst_diagnostic(address+17);bool unchanged=true;for(int i=0;i<128;++i)unchanged&=guest[address+i]==0x5a;expect(unchanged);
        polling_yield_experiment=saved;size=0;expect(virtual_memory.free(address,size,0x8000)==0);
    }
    {
        MeVirtualMemory vm;
        for(unsigned i=0;i<128;++i)vm.regions.push_back({0x10000000+i*0x20000,0x10000,4096,std::vector<uint8_t>(16,1)});
        vm.regions.back().committed[7]=0;
        vm.regions.push_back({0x20000000,0x20000,65536,{1,0}});
        auto reference=[&](uint32_t line){
            bool contained=false;for(const auto& r:vm.regions)if(line>=r.base && uint64_t(line)+128<=uint64_t(r.base)+r.size){contained=true;break;}
            if(!contained)return false;
            for(uint64_t at=line;at<uint64_t(line)+128;at+=4){
                bool committed=false;for(const auto& r:vm.regions)if(at>=r.base && at+4<=uint64_t(r.base)+r.size){committed=r.committed[(at-r.base)/r.page]!=0;break;}
                if(!committed)return false;
            }return true;
        };
        bool equivalent=true;for(const auto& r:vm.regions)for(uint32_t offset=0;offset<=r.size;offset+=128)
            equivalent &= vm.committed_cache_line(r.base+offset)==reference(r.base+offset);
        expect(equivalent);expect(!vm.committed_cache_line(0xffffffff));expect(!vm.committed_cache_line(0xffffff80));
        expect(!vm.committed_cache_line(0x20000001));expect(vm.committed_cache_line(0x2000ff80));expect(!vm.committed_cache_line(0x20010000));
        const auto last=vm.regions[127].base;
        expect(!vm.committed_cache_line(last+7*4096));vm.regions[127].committed[7]=1;expect(vm.committed_cache_line(last+7*4096));
        vm.regions[127].committed[7]=0;expect(!vm.committed_cache_line(last+7*4096));
        // Measure both validators on identical late-region lines. No speed threshold.
        auto measure=[&](bool fast){uint32_t accepted=0;const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<8192;++i){const uint32_t line=last+(i%16)*4096;std::atomic_signal_fence(std::memory_order_seq_cst);accepted+=fast?vm.committed_cache_line(line):reference(line);}
            return std::pair{std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count(),accepted};};
        const auto slow=measure(false),fast=measure(true);expect(slow.second==fast.second && fast.second==7680);
        std::printf("{\"event\":\"cache_line_validation_benchmark\",\"iterations\":8192,\"regions\":129,\"reference_ns\":%lld,\"page_check_ns\":%lld}\n",(long long)slow.first,(long long)fast.first);
        vm.regions.erase(vm.regions.begin()+127);expect(!vm.committed_cache_line(last));
    }
    {
        const bool saved_mode=gpu_explore_mode,saved_ready=game_gpu_ready;
        gpu_explore_mode=true;game_gpu_ready=true;ctx.r3.u64=0;ctx.r4.u64=0x12345678;
        __imp__VdIsHSIOTrainingSucceeded(ctx,guest);expect(ctx.r3.u64==1);expect(ctx.r4.u64==0x12345678);
        gpu_explore_mode=saved_mode;game_gpu_ready=saved_ready;
    }
    {
        std::memset(guest+0x70001400,0x5a,24);ctx.r3.u64=0x70001404;ctx.r4.u64=16;ctx.r5.u64=0x1234abcd;
        __imp__RtlFillMemoryUlong(ctx,guest);
        expect(load_word(0x70001400)==0x5a5a5a5a && load_word(0x70001414)==0x5a5a5a5a);
        bool filled=true;for(unsigned i=0;i<4;++i)filled&=load_word(0x70001404+i*4)==0x1234abcd;expect(filled);
        expect(guest[0x70001404]==0x12 && guest[0x70001405]==0x34 && guest[0x70001406]==0xab && guest[0x70001407]==0xcd);
        expect(ctx.r3.u64==0x70001404 && ctx.r4.u64==16 && ctx.r5.u64==0x1234abcd);
        ctx.r3.u64=0xffffffff;ctx.r4.u64=0;__imp__RtlFillMemoryUlong(ctx,guest);expect(ctx.r3.u64==0xffffffff);
    }
    {
        const bool mode=gpu_explore_mode,ready=game_gpu_ready;gpu_explore_mode=true;game_gpu_ready=true;
        put_word(0x70001400,0x12345678);put_word(0x70001724,0x87654321);
        put_word(ctx.r1.u32+100,0x70001404);put_word(ctx.r1.u32+108,200);
        ctx.r3.u64=0;ctx.r4.u64=0x050002d0;ctx.r5.u64=0;ctx.r6.u64=ctx.r4.u64;ctx.r7.u64=ctx.r4.u64;
        __imp__VdInitializeScalerCommandBuffer(ctx,guest);expect(ctx.r3.u64==200);
        bool nops=true;for(unsigned i=0;i<200;++i)nops&=load_word(0x70001404+i*4)==0x80000000;expect(nops);
        expect(load_word(0x70001400)==0x12345678 && load_word(0x70001724)==0x87654321);
        gpu_explore_mode=mode;game_gpu_ready=ready;
    }
    {
        const bool mode=gpu_explore_mode,ready=game_gpu_ready;const auto lr=ctx.lr;
        gpu_explore_mode=true;game_gpu_ready=true;ctx.lr=0x82815458;
        const auto allocations=physical_memory.allocations.size();
        std::memset(guest+0x70001400,0,12);put_word(0x70001410,0x12345678);
        ctx.r3.u64=0x70001400;ctx.r4.u64=0x70001410;__imp__VdPersistDisplay(ctx,guest);
        expect(ctx.r3.u64==0 && ctx.r4.u64==0x70001410);expect(load_word(0x70001410)==0x12345678);
        expect(physical_memory.allocations.size()==allocations);
        expect(!load_word(0x70001400) && !load_word(0x70001404) && !load_word(0x70001408));
        gpu_explore_mode=mode;game_gpu_ready=ready;ctx.lr=lr;
    }
    {
        uint32_t handle=0;expect(events.create(1,true,handle)==0);
        put_word(0x70001400,handle);put_word(0x70001410,0);put_word(0x70001414,0);
        auto poll=[&](uint32_t type){ctx.r3.u64=1;ctx.r4.u64=0x70001400;ctx.r5.u64=type;ctx.r6.u64=0;ctx.r7.u64=0;ctx.r8.u64=0x70001410;__imp__NtWaitForMultipleObjectsEx(ctx,guest);return ctx.r3.u32;};
        expect(poll(1)==0);expect(poll(0)==0x102);expect(events.close(handle)==0);
    }
    {
        uint32_t a=0,b=0;expect(events.create(1,false,a)==0);expect(events.create(0,true,b)==0);
        put_word(0x70001400,a);put_word(0x70001404,b);put_word(thread_address+0x160,123);
        auto wait=[&](uint32_t count,uint32_t all,uint32_t ms){ctx.r3.u64=count;ctx.r4.u64=0x70001400;ctx.r5.u64=all;ctx.r6.u64=ms;ctx.r7.u64=0;__imp__NetDll_WSAWaitForMultipleEvents(ctx,guest);return ctx.r3.u32;};
        expect(wait(2,0,0)==1);expect(wait(2,0,0)==1);expect(load_word(thread_address+0x160)==123);
        expect(wait(2,1,0)==0x102 && events.find(b)->signaled);
        uint32_t previous=0;events.set(a,previous);expect(wait(2,1,0)==0 && !events.find(a)->signaled && events.find(b)->signaled);
        events.clear(b);auto begin=std::chrono::steady_clock::now();expect(wait(2,0,5)==0x102);
        expect(std::chrono::steady_clock::now()-begin>=std::chrono::milliseconds(5));
        std::thread signal_wsa([&]{std::lock_guard<MeExecutionGate> gate(execution_gate);uint32_t old=0;events.set(a,old);state_changed.notify_all();});
        expect(wait(1,0,0xffffffff)==0 && !events.find(a)->signaled);signal_wsa.join();
        expect(wait(0,0,0)==0xffffffff && load_word(thread_address+0x160)==87);
        put_word(0x70001404,a);expect(wait(2,0,0)==0xffffffff && load_word(thread_address+0x160)==87);
        events.close(a);expect(wait(1,0,0)==0xffffffff && load_word(thread_address+0x160)==6);expect(events.close(b)==0);
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        put_word(0x70001450,0x12345678);put_word(0x70001454,0x87654321);
        ctx.r3.u64=0;ctx.r4.u64=0;ctx.r8.u64=1;ctx.r9.u64=0x70001450;ctx.r10.u64=0x70001454;
        __imp__XamUserCreateAchievementEnumerator(ctx,guest);expect(ctx.r3.u32==1317);
        expect(load_word(0x70001450)==0x12345678 && load_word(0x70001454)==0x87654321);
        ctx.r4.u64=4;__imp__XamUserCreateAchievementEnumerator(ctx,guest);expect(ctx.r3.u32==87);
        ctx.r4.u64=0;ctx.r8.u64=0;__imp__XamUserCreateAchievementEnumerator(ctx,guest);expect(ctx.r3.u32==87);
        put_word(0x70001440,0xffffffff);put_word(0x70001444,0xffffffff);
        ctx.r3.u64=0;ctx.r4.u64=7;ctx.r5.u64=0x70001440;__imp__XamUserGetXUID(ctx,guest);
        expect(ctx.r3.u32==0x80070525 && load_word(0x70001440)==0 && load_word(0x70001444)==0);
        ctx.r3.u64=9;__imp__XamUserGetXUID(ctx,guest);expect(ctx.r3.u32==0x80070057);
        ctx.r3.u64=0;ctx.r5.u64=0;__imp__XamUserGetXUID(ctx,guest);expect(ctx.r3.u32==0x80070057);
        for(uint32_t user:{0u,1u,3u,255u}){ctx.r3.u64=user;__imp__XamUserGetSigninState(ctx,guest);expect(ctx.r3.u32==0);}
        ctx.r3.u64=1ull<<5;ctx.r4.u64=0;__imp__XamNotifyCreateListener(ctx,guest);const auto handle=ctx.r3.u32;
        expect(notifications.find(handle)!=nullptr);
        const auto old=xmp_playback_client;put_word(0x70001400,2);put_word(0x70001404,0);put_word(0x70001408,1);
        ctx.r3.u64=0xfa;ctx.r4.u64=0x7001a;ctx.r5.u64=0;ctx.r6.u64=0x70001400;ctx.r7.u64=12;ctx.r8.u64=0;
        __imp__XMsgStartIORequestEx(ctx,guest);expect(ctx.r3.u32==0 && notifications.find(handle)->queue.size()==1);
        put_word(0x70001420,0);put_word(0x70001424,0);
        auto poll=[&]{ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0x70001420;__imp__NtWaitForSingleObjectEx(ctx,guest);return ctx.r3.u32;};
        expect(poll()==0);expect(poll()==0);
        ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=0x70001430;ctx.r6.u64=0x70001434;__imp__XNotifyGetNext(ctx,guest);
        expect(ctx.r3.u32==1 && load_word(0x70001430)==0x0a000003 && load_word(0x70001434)==0);
        expect(poll()==0x102);ctx.r3.u64=handle;__imp__NtClose(ctx,guest);expect(ctx.r3.u32==0);
        xmp_playback_client=old;gpu_explore_mode=saved;
    }
    {
        const bool saved=gpu_explore_mode,saved_signed=local_player_signed_in;const auto saved_player=selected_local_player;
        const auto saved_contexts=local_player_contexts;gpu_explore_mode=true;local_player_signed_in=false;local_player_contexts.clear();
        auto set_context=[&](uint32_t user,uint32_t id,uint32_t value){
            put_word(0x70008000,user);put_word(0x70008004,0);put_word(0x70008008,0);put_word(0x7000800c,0);put_word(0x70008010,id);put_word(0x70008014,value);
            put_word(thread_address+0x160,0xdead);ctx.r3.u64=0xfb;ctx.r4.u64=0xb0006;ctx.r5.u64=0;ctx.r6.u64=0x70008000;ctx.r7.u64=24;
            __imp__XMsgStartIORequest(ctx,guest);return ctx.r3.u32;};
        expect(set_context(0,0x800b,1)==0x80070525 && load_word(thread_address+0x160)==0x525 && local_player_contexts.empty());
        local_player_signed_in=true;selected_local_player={};selected_local_player.xuid=0xe000000000001234ull;std::strcpy(selected_local_player.name,"Runner");
        expect(set_context(4,0x800b,1)==0x80070057 && load_word(thread_address+0x160)==87 && local_player_contexts.empty());
        expect(set_context(1,0x800b,1)==0x80070525 && local_player_contexts.empty());
        expect(set_context(0,0x800b,1)==0 && load_word(thread_address+0x160)==0);
        expect(set_context(0,0x800b,2)==0 && local_player_contexts.size()==1 && local_player_contexts[0].second==2);
        expect(set_context(0,0x8001,7)==0 && local_player_contexts.size()==2 && load_word(0x70008014)==7);
        // XUserWriteAchievements (0xB0008): {count, list}, list = {user, id}; synchronous here.
        const auto saved_unlocked=local_player_unlocked;const auto saved_times=local_player_unlock_times;
        achievement_store_disabled=true;  // self-test process: never read or write a profile store on disk
        local_player_unlocked.clear();local_player_unlock_times.clear();
        put_word(0x70008100,0);put_word(0x70008104,17);put_word(0x70008000,1);put_word(0x70008004,0x70008100);
        ctx.r3.u64=0xfb;ctx.r4.u64=0xb0008;ctx.r5.u64=0;ctx.r6.u64=0x70008000;ctx.r7.u64=8;__imp__XMsgStartIORequest(ctx,guest);
        expect(ctx.r3.u32==0 && local_player_unlocked.size()==1 && local_player_unlocked[0]==17);
        ctx.r3.u64=0xfb;ctx.r4.u64=0xb0008;ctx.r5.u64=0;ctx.r6.u64=0x70008000;ctx.r7.u64=8;__imp__XMsgStartIORequest(ctx,guest);
        expect(ctx.r3.u32==0 && local_player_unlocked.size()==1);  // same id again: no duplicate
        // Unknown XGI message: reported as not supported, the game keeps running.
        ctx.r3.u64=0xfb;ctx.r4.u64=0xb00ff;ctx.r5.u64=0;ctx.r6.u64=0x70008000;ctx.r7.u64=8;__imp__XMsgStartIORequest(ctx,guest);
        expect(ctx.r3.u32==0x80070032 && load_word(thread_address+0x160)==50);
        expect(local_unlock_filetime(17)>116444736000000000ull && local_unlock_filetime(18)==0);
        local_player_unlocked=saved_unlocked;local_player_unlock_times=saved_times;
        // Achievement metadata comes from the real title XDBF; nothing is reported as achieved.
        expect(load_title_achievements() && title_achievements.size()==44 && title_achievement_language==1);
        uint32_t score=0;for(const auto& a:title_achievements){score+=a.gamerscore;expect(!a.label.empty() && !(a.flags&0x30000));}
        expect(score==1000);
        auto create=[&](uint32_t flags,uint32_t offset,uint32_t count,uint64_t xuid=0){
            put_word(0x70001450,0x12345678);put_word(0x70001454,0x87654321);
            ctx.r3.u64=0;ctx.r4.u64=0;ctx.r5.u64=xuid;ctx.r6.u64=flags;ctx.r7.u64=offset;ctx.r8.u64=count;ctx.r9.u64=0x70001450;ctx.r10.u64=0x70001454;
            __imp__XamUserCreateAchievementEnumerator(ctx,guest);return ctx.r3.u32;};
        auto enumerate=[&](uint32_t handle,uint32_t capacity){
            put_word(0x70001458,0xffffffff);ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=0x70009000;ctx.r6.u64=capacity;ctx.r7.u64=0x70001458;ctx.r8.u64=0;
            __imp__XamEnumerate(ctx,guest);return ctx.r3.u32;};
        expect(create(7,45,2)==87 && load_word(0x70001450)==0x12345678 && load_word(0x70001454)==0x87654321);
        expect(create(7,0,2,0xe000000000001234ull)==0 && load_word(0x70001450)==2*500);
        const auto strings=load_word(0x70001454);expect(achievement_enumerators.find(strings)!=nullptr);
        put_word(0x70009000,0xa5a5a5a5);
        expect(enumerate(strings,999)==122 && load_word(0x70001458)==0 && load_word(0x70009000)==0xa5a5a5a5);
        expect(enumerate(strings,1000)==0 && load_word(0x70001458)==2);
        const auto& first=title_achievements[0];const auto label=load_word(0x70009004);
        expect(load_word(0x70009000)==first.id && label>=0x70009000+72 && label<0x70009000+1000);
        expect(load_word(0x70009014)==first.gamerscore && load_word(0x70009018)==0 && load_word(0x7000901c)==0 && load_word(0x70009020)==first.flags);
        bool same=true;for(size_t i=0;i<=first.label.size();++i){const char16_t c=i<first.label.size()?first.label[i]:0;
            same=same && guest[label+i*2]==uint8_t(c>>8) && guest[label+i*2+1]==uint8_t(c);}
        expect(same && load_word(0x70009008)>label && load_word(0x7000900c)>load_word(0x70009008));
        uint32_t total=2;while(enumerate(strings,1000)==0)total+=load_word(0x70001458);
        expect(ctx.r3.u32==18 && load_word(0x70001458)==0 && total==44);
        expect(create(0,40,50)==0 && load_word(0x70001450)==50*36);const auto plain=load_word(0x70001454);
        expect(enumerate(plain,1800)==0 && load_word(0x70001458)==4 && load_word(0x70009000)==title_achievements[40].id && load_word(0x70009004)==0);
        expect(enumerate(plain,1800)==18);
        ctx.r3.u64=strings;ctx.r4.u64=0;ctx.r5.u64=0x70009000;ctx.r6.u64=1000;ctx.r7.u64=0;ctx.r8.u64=0;__imp__XamEnumerate(ctx,guest);expect(ctx.r3.u32==87);
        expect(enumerate(0x60100,1000)==6);
        for(auto h:{strings,plain}){ctx.r3.u64=h;__imp__NtClose(ctx,guest);expect(ctx.r3.u32==0 && !achievement_enumerators.find(h));}
        expect(enumerate(strings,1000)==6);
        local_player_signed_in=false;expect(create(7,0,2)==1317 && load_word(0x70001454)==0x87654321);
        {   // UTF-8 decoding used for XDBF strings.
            std::u16string out;const uint8_t ok[]={'A',0xc3,0xa9,0xe2,0x82,0xac,0xf0,0x9f,0x98,0x80};
            expect(me_utf8_to_utf16(ok,sizeof(ok),out) && out==std::u16string({u'A',u'é',u'€',char16_t(0xd83d),char16_t(0xde00)}));
            const uint8_t truncated[]={0xe2,0x82},overlong[]={0xc0,0x80},surrogate[]={0xed,0xa0,0x80};
            expect(!me_utf8_to_utf16(truncated,2,out) && !me_utf8_to_utf16(overlong,2,out) && !me_utf8_to_utf16(surrogate,3,out));
        }
        local_player_signed_in=saved_signed;selected_local_player=saved_player;local_player_contexts=saved_contexts;gpu_explore_mode=saved;
    }
    {
        MeOfflineNetwork model;expect(model.start() && model.fd>=0 && model.port!=0);
        const auto fd=model.fd;const auto port=model.port;expect(model.start() && model.fd==fd && model.port==port);
        uint8_t params[13]{};params[0]=13;params[2]=7;expect(model.start(params) && model.params[2]==7);
        model.close();expect(model.fd==-1 && model.port==0);
        expect(model.start());model.close();
    }
    {
        MeNotifications model;const auto h=model.create(1ull<<5,0),other=model.create(1,0);uint32_t id=0,data=0;
        expect(h && other);expect(model.broadcast(0x0a000003,7)==1);
        expect(model.find(h)->queue.size()==1 && model.find(other)->queue.empty());
        expect(model.broadcast(0x0a010003,8)==0);expect(model.broadcast(0x0a000004,9)==1);
        expect(model.pop(h,0x0a000004,id,data) && id==0x0a000004 && data==9);
        expect(model.find(h)->queue.size()==1);expect(model.pop(h,0,id,data) && id==0x0a000003 && data==7);
        expect(!model.pop(h,0,id,data));expect(model.close(h) && !model.find(h));
    }
    {
        MeMutants model;uint32_t h=0,status=0;
        expect(model.create(11,h)==0 && model.find(h)->depth==1);
        expect(!model.wait(h,22,status));
        expect(model.wait(h,11,status) && status==0 && model.find(h)->depth==2);
        expect(model.release(h,22)==0xc0000046);
        expect(model.release(h,11)==0 && model.find(h)->depth==1);
        expect(model.release(h,11)==0 && model.find(h)->owner==0);
        expect(model.wait(h,22,status) && status==0);
        model.abandon(22);expect(model.find(h)->owner==0);
        expect(model.wait(h,33,status) && status==0x80);
        expect(model.wait(h,33,status) && status==0);
        expect(model.release(h,33)==0 && model.release(h,33)==0);
        expect(model.close(h)==0 && !model.find(h));
        expect(model.release(h,33)==0xc0000008);
        expect(model.wait(h,33,status) && status==0xc0000008);
    }
    {
        MeEvents model; uint32_t first=0, second=0, status=0;
        expect(model.create(1,true,first,"win32:Test",true)==0);
        expect(model.create(0,false,second,"win32:test",true)==0x40000000 && first==second);
        expect(!model.find(first)->manual && model.find(first)->signaled);
        expect(model.wait(second,true,status) && status==0);
        expect(model.wait(first,true,status) && status==0x102);
        expect(model.close(first)==0 && model.find(second));
        expect(model.close(second)==0 && !model.find(first));
        expect(model.create(0,false,second,"win32:Test",true)==0 && first!=second);
    }
    {
        MeExecutionGate gate;std::vector<int> order;gate.lock();
        auto queued=[&](uint64_t n){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(1);while(gate.queued_waiters()<n && std::chrono::steady_clock::now()<end)std::this_thread::yield();return gate.queued_waiters()==n;};
        std::thread first([&]{std::lock_guard<MeExecutionGate> held(gate);order.push_back(1);});
        expect(queued(1));
        std::thread second([&]{std::lock_guard<MeExecutionGate> held(gate);order.push_back(2);});
        expect(queued(2));gate.unlock();
        {std::lock_guard<MeExecutionGate> held(gate);order.push_back(3);}
        first.join();second.join();expect(order==std::vector<int>({1,2,3}));
        expect(gate.queued_waiters()==0);
    }
    {
        MeAudioOutput endpoint_test;
        expect(endpoint_test.open() && endpoint_test.stream && AAudioStream_getState(endpoint_test.stream) == AAUDIO_STREAM_STATE_STARTED);
        float silence[512]{};
        auto accepted_silence = me_pcm_write_all(silence, [&](const float* data, int frames) { return int(AAudioStream_write(endpoint_test.stream, data, frames, 20000000)); });
        expect(accepted_silence.frames == 256 && !accepted_silence.error);
        expect(!endpoint_test.open());
        endpoint_test.close(); endpoint_test.close(); expect(!endpoint_test.stream);
        expect(endpoint_test.open()); endpoint_test.close();
    }
    uint8_t pcm_input[6*256*4]{}; float pcm_output[512]{};
    auto set_pcm = [&](unsigned channel, unsigned index, float value) {
        uint32_t bits; std::memcpy(&bits, &value, 4); bits = __builtin_bswap32(bits);
        std::memcpy(pcm_input + (channel*256+index)*4, &bits, 4);
    };
    set_pcm(0,0,0.5f); set_pcm(1,0,-0.25f); set_pcm(2,1,0.5f); set_pcm(3,2,1.0f);
    set_pcm(4,3,0.5f); set_pcm(5,4,-0.5f); set_pcm(0,5,100.0f); set_pcm(1,5,-100.0f);
    set_pcm(0,6,std::numeric_limits<float>::quiet_NaN());
    me_pcm_stereo(pcm_input,pcm_output);
    expect(std::abs(pcm_output[0]-0.29289322f)<0.000001f && std::abs(pcm_output[1]+0.14644661f)<0.000001f);
    expect(pcm_output[2] == pcm_output[3] && pcm_output[2] > 0.2f && pcm_output[2] < 0.21f);
    expect(pcm_output[4] == 0 && pcm_output[5] == 0);
    expect(pcm_output[6] > 0.2f && pcm_output[7] == 0 && pcm_output[8] == 0 && pcm_output[9] < -0.2f);
    expect(pcm_output[10] == 1 && pcm_output[11] == -1 && pcm_output[12] == 0 && pcm_output[511] == 0);
    int pcm_calls = 0; bool pcm_offsets = true;
    auto partial_pcm = me_pcm_write_all(pcm_output, [&](const float* data, int frames) {
        pcm_offsets &= data == pcm_output + pcm_calls * 128 && frames == 256 - pcm_calls*64;
        ++pcm_calls; return 64;
    });
    expect(partial_pcm.frames == 256 && !partial_pcm.error && pcm_calls == 4 && pcm_offsets);
    pcm_calls = 0;
    auto failed_pcm = me_pcm_write_all(pcm_output, [&](const float*, int) { return ++pcm_calls == 1 ? 64 : -77; });
    expect(failed_pcm.frames == 64 && failed_pcm.error == -77);
    auto zero_pcm = me_pcm_write_all(pcm_output, [](const float*, int) { return 0; });
    expect(!zero_pcm.frames && zero_pcm.error);

    {
        char directory[]="/data/local/tmp/me_fs_test_XXXXXX";
        expect(mkdtemp(directory)!=nullptr);
        const std::string fixture=std::string(directory)+"/fixture.bin";
        FILE* out=std::fopen(fixture.c_str(),"wb");expect(out!=nullptr);
        expect(std::fwrite("ABCDE",1,5,out)==5);expect(std::fclose(out)==0);
        out=std::fopen((std::string(directory)+"/inventory.txt").c_str(),"wb");expect(out!=nullptr);
        const char index[]="fixture.bin\r\nnot-staged.bin\r\n";
        expect(std::fwrite(index,1,sizeof(index)-1,out)==sizeof(index)-1);expect(std::fclose(out)==0);
        MeReadOnlyFiles files(directory);std::string relative;
        expect(MeReadOnlyFiles::relative("D:\\FiXtUrE.bin",relative) && relative=="fixture.bin");
        expect(!MeReadOnlyFiles::relative("D:\\..\\escape",relative) && !MeReadOnlyFiles::relative("E:\\file",relative));
        const auto handle=files.open("D:\\fixture.bin");expect(handle && files.find(handle)->size==5);
        char content[8]{};expect(files.read(handle,content,3,1)==3 && std::memcmp(content,"BCD",3)==0 && files.find(handle)->position==4);
        expect(files.read(handle,content,8,4)==1 && content[0]=='E');
        expect(files.read(handle,content,8,5)==0);
        expect(pwrite(files.find(handle)->fd,"X",1,0)==-1 && errno==EBADF);
        expect(!files.open("D:\\missing.bin") && errno==ENOENT);
        expect(!files.open("D:\\not-staged.bin") && errno==ENODATA);
        expect(files.close(handle) && !files.find(handle) && !files.close(handle));
        expect(unlink(fixture.c_str())==0);
        expect(unlink((std::string(directory)+"/inventory.txt").c_str())==0 && rmdir(directory)==0);
    }
    {
        if (trace_checks) std::fprintf(stderr, "gate owned before fpu test: %d\n", int(execution_gate.owned()));
        const auto old=ctx.fpscr.getcsr();ctx.r3.u64=0;__imp__KeEnableFpuExceptions(ctx,guest);
        if (trace_checks) std::fprintf(stderr, "fpu test returned\n");
        expect(!(ctx.fpscr.getcsr()&0x9f00) && (ctx.fpscr.getcsr()&~0x9f00u)==(old&~0x9f00u));
        ctx.r3.u64=0;ctx.r4.u64=0x70001000;__imp__XexGetModuleHandle(ctx,guest);
        expect(!ctx.r3.u32 && load_word(0x70001000)==0x60001000);
        std::memcpy(guest+0x70001020,"HashSec",8);
        ctx.r3.u64=0x60001000;ctx.r4.u64=0x70001020;ctx.r5.u64=0x70001030;ctx.r6.u64=0x70001034;
        __imp__XexGetModuleSection(ctx,guest);expect(!ctx.r3.u32 && load_word(0x70001030)==0x83160000 && load_word(0x70001034)==451);
        std::memcpy(guest+0x70001020,"Absent",7);
        ctx.r3.u64=0x60001000;__imp__XexGetModuleSection(ctx,guest);
        expect(ctx.r3.u32==0xc0000225 && load_word(0x70001030)==0x83160000);
        guest[0x70001040]=0;guest[0x70001041]='A';guest[0x70001042]=0;guest[0x70001043]='B';
        ctx.r3.u64=0x70001050;ctx.r4.u64=1;ctx.r5.u64=0x70001054;ctx.r6.u64=0x70001040;ctx.r7.u64=4;
        __imp__RtlUnicodeToMultiByteN(ctx,guest);expect(!ctx.r3.u32 && guest[0x70001050]=='A' && load_word(0x70001054)==1);
        ctx.r3.u64=0x70001050;ctx.r4.u64=0;__imp__RtlUnicodeToMultiByteN(ctx,guest);expect(!ctx.r3.u32 && !load_word(0x70001054));
    }


    {
        const bool old_mode=gpu_explore_mode;gpu_explore_mode=true;
        put_word(0x70001100,100);ctx.r3.u64=0x70001100;__imp__MmQueryStatistics(ctx,guest);
        expect(ctx.r3.u32==0xc0000023 && load_word(0x70001100)==100);
        put_word(0x70001100,104);ctx.r3.u64=0x70001100;__imp__MmQueryStatistics(ctx,guest);
        expect(!ctx.r3.u32 && load_word(0x70001100)==104 && load_word(0x70001104)==0x20000 &&
            load_word(0x7000110c)<=0x1ec00 && load_word(0x70001164)==0x1ffff);
        gpu_explore_mode=old_mode;
    }

    MeXmaRegisters xma_test; uint64_t xma_value = 0;
    expect(!xma_test.read(0x7fea1800, 4, xma_value));
    xma_test.context_physical = 0x01fc8000;
    expect(xma_test.read(0x7fea1800, 4, xma_value) && xma_value == 0x0080fc01);
    expect(!xma_test.read(0x7fea1800, 8, xma_value) && !xma_test.read(0x7fea1801, 4, xma_value));
    expect(xma_test.write(0x7fea1804, 4, 0x03000000) && xma_test.control == 3);
    expect(xma_test.read(0x7fea1804, 4, xma_value) && xma_value == 0x03000000);
    expect(!xma_test.write(0x7fea1940, 4, 1) && !xma_test.read(0x7fea1820, 4, xma_value));
    expect(xma_test.read(0x7fea181c, 4, xma_value) && xma_value == 0x01000000);
    expect(xma_test.read(0x7fea1818, 4, xma_value) && xma_value == 0x01000000 && xma_test.next_context_index == 2);
    xma_test.next_context_index = 319;
    expect(xma_test.read(0x7fea1818, 4, xma_value) && xma_value == 0x3f010000 && xma_test.next_context_index == 0);
    expect(!xma_test.write(0x7fea1818, 4, 0));
    expect(!xma_test.write(0x7fea1804, 4, 0xffffffff) && xma_test.control == 3);
    // Test-only device callbacks. No register emulation is installed for game startup.
    struct MmioCapture { uint32_t address = 0, value = 0, reads = 0, writes = 0; } capture;
    expect(!gpu_mmio.read && !gpu_mmio.write);
    uint64_t mmio_value = 0;
    expect(!gpu_mmio.load(0x7fc80000, 4, mmio_value));
    gpu_mmio = {&capture,
        [](void* p, uint32_t a, uint32_t& v) { auto& c = *static_cast<MmioCapture*>(p); c.address = a; ++c.reads; v = c.value; return true; },
        [](void* p, uint32_t a, uint32_t v) { auto& c = *static_cast<MmioCapture*>(p); c.address = a; c.value = v; ++c.writes; return true; }};
    auto* base = guest;
    PPC_STORE_U32(0x7fc80714, 0x12345678);
    expect(capture.address == 0x7fc80714 && capture.value == 0x12345678 && capture.writes == 1);
    expect(PPC_LOAD_U32(0x7fc80714) == 0x12345678 && capture.reads == 1);
    expect(!gpu_mmio.store(0x7fc80001, 4, 1) && !gpu_mmio.store(0x7fc80000, 2, 1));
    expect(!gpu_mmio.load(0x7fc80000, 8, mmio_value) && !gpu_mmio.load(0x7fc90000, 4, mmio_value));
    expect(capture.writes == 1 && capture.reads == 1);
    expect(MeGpuMmio::overlaps(0x7fc7ffff, 2) && !MeGpuMmio::valid(0x7fc7ffff, 2));
    expect(!MeGpuMmio::overlaps(0x7fc90000, 4));
    // Real AOT object, synthetic destination: verifies the separately compiled game TU uses the hook.
    ctx.r3.u64 = 0x7fc80000;
    sub_82230088(ctx, guest);
    expect(capture.address == 0x7fc80000 && capture.value == uint32_t(-2112880640 + 31336) && capture.writes == 2);
    gpu_mmio = {};
    expect(!gpu_mmio.store(0x7fc80000, 4, 1));
    PPC_STORE_U8(0x70000400, 0xab);
    expect(PPC_LOAD_U8(0x70000400) == 0xab);
    PPC_STORE_U16(0x70000402, 0xcdef);
    expect(PPC_LOAD_U16(0x70000402) == 0xcdef && guest[0x70000402] == 0xcd);
    PPC_STORE_U32(0x70000404, 0x12345678);
    expect(PPC_LOAD_U32(0x70000404) == 0x12345678 && guest[0x70000404] == 0x12);
    PPC_STORE_U64(0x70000408, 0x0123456789abcdefull);
    expect(PPC_LOAD_U64(0x70000408) == 0x0123456789abcdefull && guest[0x70000408] == 1);
    auto xconfig = [&](uint32_t output, uint16_t capacity) {
        ctx.r3.u64 = 3; ctx.r4.u64 = 10; ctx.r5.u64 = output;
        ctx.r6.u64 = capacity; ctx.r7.u64 = 0x70000710;
        __imp__ExGetXConfigSetting(ctx, guest);
        return ctx.r3.u32;
    };
    expect(xconfig(0x70000700, 4) == 0 && load_word(0x70000700) == 0x40000);
    expect(MeXexHeaders::be32(guest + 0x70000710) >> 16 == 4);
    put_word(0x70000700, 0xabcdef01);
    expect(xconfig(0x70000700, 3) == 0xc0000023 && load_word(0x70000700) == 0xabcdef01);
    expect((load_word(0x70000710) >> 16) == 0);
    expect(xconfig(0, 0) == 0 && (load_word(0x70000710) >> 16) == 4);
    expect(xconfig(0, 4) == 0xc00000f1 && (load_word(0x70000710) >> 16) == 0);
    ctx.r3.u64 = thread_address; ctx.r4.s64 = 2; __imp__KeSetBasePriorityThread(ctx, guest);
    expect(ctx.r3.u32 == 0 && guest[thread_address + 0x70] == 2);
    ctx.r3.u64 = thread_address; ctx.r4.s64 = 0; __imp__KeSetBasePriorityThread(ctx, guest);
    expect(ctx.r3.u32 == 2 && guest[thread_address + 0x70] == 0);
    __imp__VdQueryVideoFlags(ctx, guest); expect(ctx.r3.u32 == 3);
    ctx.r3.u64 = 0x70000800; ctx.r4.u64 = 0x70000804; __imp__VdGetCurrentDisplayGamma(ctx, guest);
    expect(load_word(0x70000800) == 2);
    float gamma_test; auto gamma_bits = load_word(0x70000804); std::memcpy(&gamma_test, &gamma_bits, 4);
    expect(gamma_test > 2.22f && gamma_test < 2.23f);
    put_word(0x70000858, 0xdeadbeef);
    ctx.r3.u64 = 0x70000800; __imp__VdGetCurrentDisplayInformation(ctx, guest);
    expect(load_word(0x70000800) == 0x050002d0 && load_word(0x7000084c) == 0x42700000);
    expect(load_word(0x70000810) == 1280 && load_word(0x70000814) == 720);
    expect(load_word(0x70000858) == 0xdeadbeef);
    constexpr uint32_t cs = 0x70000100;
    ctx.r3.u64 = cs; __imp__RtlInitializeCriticalSection(ctx, guest);
    __imp__RtlEnterCriticalSection(ctx, guest);
    __imp__RtlEnterCriticalSection(ctx, guest);
    expect(load_word(cs + 20) == 2 && load_word(cs + 16) == 1 && load_word(cs + 24) == thread_address);
    __imp__RtlLeaveCriticalSection(ctx, guest);
    expect(load_word(cs + 20) == 1 && load_word(cs + 24) == thread_address);
    __imp__RtlLeaveCriticalSection(ctx, guest);
    expect(load_word(cs + 20) == 0 && load_word(cs + 16) == UINT32_MAX && !load_word(cs + 24));
    put_word(cs + 24, 0x61002000); put_word(cs + 16, 0); put_word(cs + 20, 1);
    ctx.r3.u64 = cs; __imp__RtlTryEnterCriticalSection(ctx, guest);
    expect(ctx.r3.u32 == 0 && load_word(cs + 24) == 0x61002000);
    for (unsigned i = 0; i < std::min(tls_slots, 256u); ++i) { __imp__KeTlsAlloc(ctx, guest); expect(ctx.r3.u32 == i); }
    __imp__KeTlsAlloc(ctx, guest); expect(ctx.r3.u32 == UINT32_MAX);
    ctx.r3.u64 = 17; ctx.r4.u64 = 0x1234abcd; __imp__KeTlsSetValue(ctx, guest);
    ctx.r3.u64 = 17; __imp__KeTlsGetValue(ctx, guest); expect(ctx.r3.u32 == 0x1234abcd);
    ctx.r3.u64 = 17; __imp__KeTlsFree(ctx, guest); __imp__KeTlsAlloc(ctx, guest); expect(ctx.r3.u32 == 17);
    __imp__KeTlsGetValue(ctx, guest); expect(ctx.r3.u32 == 0);
    guest[pcr_address + 0x18] = 1;
    __imp__KeRaiseIrqlToDpcLevel(ctx, guest);
    expect(ctx.r3.u32 == 1 && guest[pcr_address + 0x18] == 2);
    ctx.r3.u64 = 1; __imp__KfLowerIrql(ctx, guest); expect(guest[pcr_address + 0x18] == 1);
    ctx.r3.u64 = 0; __imp__KfLowerIrql(ctx, guest); expect(guest[pcr_address + 0x18] == 0);
    const uint32_t test_lock = 0x70000a00;
    put_word(test_lock, 0); guest[pcr_address + 0x18] = 0;
    ctx.r3.u64 = test_lock; __imp__KfAcquireSpinLock(ctx, guest);
    expect(ctx.r3.u32 == 0 && load_word(test_lock) == pcr_address && guest[pcr_address + 0x18] == 2);
    ctx.r3.u64 = test_lock; ctx.r4.u64 = 0; __imp__KfReleaseSpinLock(ctx, guest);
    expect(!load_word(test_lock) && guest[pcr_address + 0x18] == 0);
    guest[pcr_address + 0x18] = 2;
    ctx.r3.u64 = test_lock; __imp__KfAcquireSpinLock(ctx, guest);
    expect(ctx.r3.u32 == 2 && load_word(test_lock) == pcr_address);
    ctx.r3.u64 = test_lock; ctx.r4.u64 = 2; __imp__KfReleaseSpinLock(ctx, guest);
    expect(!load_word(test_lock) && guest[pcr_address + 0x18] == 2);
    guest[pcr_address + 0x18] = 0;
    for (const uint8_t level : {uint8_t(2), uint8_t(3)}) {
        guest[pcr_address + 0x18] = level;
        ctx.r3.u64 = test_lock; ctx.r4.u64 = 0x1234;
        __imp__KeAcquireSpinLockAtRaisedIrql(ctx, guest);
        expect(load_word(test_lock) == pcr_address && guest[pcr_address + 0x18] == level && ctx.r3.u32 == test_lock && ctx.r4.u32 == 0x1234);
        __imp__KeReleaseSpinLockFromRaisedIrql(ctx, guest);
        expect(!load_word(test_lock) && guest[pcr_address + 0x18] == level);
    }
    guest[pcr_address + 0x18] = 0;
    {
        uint32_t large=0, bytes=0x300000;
        expect(virtual_memory.allocate(large,bytes,0x3000)==0);
        expect(checked_file_buffer(large,bytes));
        ctx.r3.u64=large+0x1000; __imp__MmQueryAddressProtect(ctx,guest); expect(ctx.r3.u32==4);
        uint32_t hole=large+0x1000, page=0x1000;
        expect(virtual_memory.free(hole,page,0x4000)==0);
        expect(!checked_file_buffer(large,bytes));
        ctx.r3.u64=large+0x1000; __imp__MmQueryAddressProtect(ctx,guest); expect(ctx.r3.u32==1);
        ctx.r3.u64=large; __imp__MmQueryAddressProtect(ctx,guest); expect(ctx.r3.u32==4);
        bytes=0; expect(virtual_memory.free(large,bytes,0x8000)==0);
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        ctx.r3.u64=1;ctx.r4.u64=0;__imp__NetDll_XNetStartup(ctx,guest);expect(ctx.r3.u32==0 && offline_network.fd>=0);
        guest[0x70001400]=0x5a;ctx.r3.u64=1;ctx.r5.u64=0x70001400;
        __imp__NetDll_WSAStartup(ctx,guest);expect(ctx.r3.u32==10091 && guest[0x70001400]==0x5a);
        ctx.r3.u64=1;ctx.r4.u64=0x70001400;__imp__NetDll_XNetStartup(ctx,guest);expect(ctx.r3.u32==10022);
        gpu_explore_mode=saved;
    }
    uint32_t address = 0, length = 4096;
    expect(virtual_memory.allocate(address, length, 0x3000) == 0);
    guest[address] = 0x5a;
    uint32_t a = address, n = length; expect(virtual_memory.free(a, n, 0x4000) == 0);
    a = address; n = length; expect(virtual_memory.allocate(a, n, 0x1000) == 0 && guest[address] == 0);
    a = address; n = 0; expect(virtual_memory.free(a, n, 0x8000) == 0 && virtual_memory.regions.empty());
    timespec begin{}, end{}, delay{0, 20000000};
    clock_gettime(CLOCK_MONOTONIC, &begin); auto tick = __rdtsc();
    nanosleep(&delay, nullptr);
    auto elapsed_ticks = __rdtsc() - tick; clock_gettime(CLOCK_MONOTONIC, &end);
    const double seconds = double(end.tv_sec - begin.tv_sec) + double(end.tv_nsec - begin.tv_nsec) / 1e9;
    const double measured = elapsed_ticks / seconds;
    expect(measured > 49000000 && measured < 51000000);
    // End-to-end kernel-boundary scheduling: child signals while parent owns
    // a CS, waits for ownership, exits, and is joined through its guest handle.
    ctx.r3.u64 = 17; ctx.r4.u64 = 0x11111111; __imp__KeTlsSetValue(ctx, guest);
    ctx.r3.u64 = 0x70000300; __imp__RtlInitializeCriticalSection(ctx, guest);
    __imp__RtlEnterCriticalSection(ctx, guest);
    ctx.r3.u64 = 0x70000204; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 0;
    __imp__NtCreateEvent(ctx, guest); expect(ctx.r3.u32 == 0);
    auto original_entry = PPC_LOOKUP_FUNC(guest, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(guest, PPC_CODE_BASE) = thread_test_entry;
    ctx.r3.u64 = 0x70000208; ctx.r4.u64 = 32768; ctx.r5.u64 = 0x7000020c;
    ctx.r6.u64 = 0; ctx.r7.u64 = PPC_CODE_BASE; ctx.r8.u64 = load_word(0x70000204); ctx.r9.u64 = 0;
    __imp__ExCreateThread(ctx, guest); expect(ctx.r3.u32 == 0 && load_word(0x7000020c) == 2);
    expect(threads[1]->pcr != threads[0]->pcr && threads[1]->tls != threads[0]->tls);
    ctx.r3.u64 = load_word(0x70000204); ctx.r4.u64 = ctx.r5.u64 = ctx.r6.u64 = 0;
    __imp__NtWaitForSingleObjectEx(ctx, guest); expect(ctx.r3.u32 == 0);
    expect(!load_word(0x70000200) && load_word(0x70000318) == thread_address);
    ctx.r3.u64 = 0x70000300; __imp__RtlLeaveCriticalSection(ctx, guest);
    ctx.r3.u64 = load_word(0x70000208); ctx.r4.u64 = ctx.r5.u64 = ctx.r6.u64 = 0;
    __imp__NtWaitForSingleObjectEx(ctx, guest);
    expect(ctx.r3.u32 == 0 && threads[1]->done && threads[1]->exit_code == 7 && load_word(0x70000200) == 1);
    ctx.r3.u64 = 17; __imp__KeTlsGetValue(ctx, guest); expect(ctx.r3.u32 == 0x11111111);
    expect(load_word(threads[1]->tls + 17 * 4) == 0x22222222);
    ctx.r3.u64 = 17; __imp__KeTlsFree(ctx, guest);
    expect(!load_word(threads[0]->tls + 17 * 4) && !load_word(threads[1]->tls + 17 * 4));
    ctx.r3.u64 = load_word(0x70000208); ctx.r4.u64 = 0; ctx.r5.u64 = 0x70000210;
    __imp__ObReferenceObjectByHandle(ctx, guest);
    expect(!ctx.r3.u32 && load_word(0x70000210) == threads[1]->kthread && threads[1]->references == 1);
    ctx.r3.u64 = threads[1]->kthread; __imp__ObDereferenceObject(ctx, guest); expect(!threads[1]->references);
    ctx.r3.u64 = threads[1]->kthread; ctx.r4.u64 = 2; ctx.r5.u64 = 0x70000214;
    __imp__KeSetAffinityThread(ctx, guest);
    expect(!ctx.r3.u32 && load_word(0x70000214) == 1 && guest[threads[1]->pcr + 0x10c] == 1);
    ctx.r3.u64 = load_word(0x70000208); __imp__NtClose(ctx, guest); expect(!ctx.r3.u32);
    ctx.r3.u64 = load_word(0x70000208); ctx.r4.u64 = 0; ctx.r5.u64 = 0x70000210;
    __imp__ObReferenceObjectByHandle(ctx, guest); expect(ctx.r3.u32 == 0xc0000008);
    // A finite unsignaled wait times out after real elapsed time.
    put_word(0x70000218, UINT32_MAX); put_word(0x7000021c, uint32_t(-20000));
    ctx.r3.u64 = load_word(0x70000204); ctx.r4.u64 = ctx.r5.u64 = 0; ctx.r6.u64 = 0x70000218;
    tick = __rdtsc(); __imp__NtWaitForSingleObjectEx(ctx, guest);
    expect(ctx.r3.u32 == 0x102 && __rdtsc() - tick >= 50000);
    ctx.r3.u64 = 1; ctx.r4.u64 = 0; ctx.r5.u64 = 0x70000218;
    tick = __rdtsc(); __imp__KeDelayExecutionThread(ctx, guest); expect(ctx.r3.u32 == 0 && __rdtsc() - tick >= 50000);
    PPC_LOOKUP_FUNC(guest, PPC_CODE_BASE) = original_entry;
    std::memset(guest + 0x70000400, 0xcc, 56);
    ctx.r3.u64 = 0x70000404; __imp__XGetVideoMode(ctx, guest);
    expect(load_word(0x70000400) == 0xcccccccc && load_word(0x70000434) == 0xcccccccc);
    expect(load_word(0x70000404) == 1280 && load_word(0x70000408) == 720 && load_word(0x70000418) == 0x42700000);
    expect(!load_word(0x70000428) && !load_word(0x7000042c) && !load_word(0x70000430));
    ctx.r3.u64 = 0; ctx.r4.u64 = 4800; ctx.r5.u64 = 0x404;
    ctx.r6.u64 = 0; ctx.r7.u64 = UINT32_MAX; ctx.r8.u64 = 32;
    __imp__MmAllocatePhysicalMemoryEx(ctx, guest);
    const auto physical_alias = ctx.r3.u32, physical = MePhysicalMemory::physical(physical_alias);
    expect(physical_alias >= 0xe0000000 && physical_memory.allocations.size() == 1);
    expect(physical_memory.find(physical_alias)->size == 8192 && !(physical & 4095));
    put_word(physical_alias + 4, 0x12345678);
    expect(load_word(0xa0000000 + physical + 4) == 0x12345678 && load_word(0xc0000000 + physical + 4) == 0x12345678);
    put_word(0xc0000000 + physical, 0xabcdef01);
    expect(load_word(physical_alias) == 0xabcdef01);
    ctx.r3.u64 = physical_alias + 4; __imp__MmGetPhysicalAddress(ctx, guest); expect(ctx.r3.u32 == physical + 4);
    ctx.r3.u64 = physical_alias; __imp__MmQueryAllocationSize(ctx, guest); expect(ctx.r3.u32 == 8192);
    auto* shared_bytes = gpu_physical.resolve(physical, 8192);
    expect(shared_bytes && MeXexHeaders::be32(shared_bytes) == 0xabcdef01);
    put_word(0xa0000000 + physical + 8, 0x33445566);
    expect(MeXexHeaders::be32(shared_bytes + 8) == 0x33445566);
    const uint8_t from_gpu[4] = {0x77, 0x88, 0x99, 0xaa};
    std::memcpy(shared_bytes + 12, from_gpu, 4);
    expect(load_word(physical_alias + 12) == 0x778899aa);
    auto ring = gpu_physical.bind_ring(physical, 3); // 64 bytes.
    expect(ring && ring->capacity() == 64);
    put_word(physical_alias + 60, 0x80000000); put_word(physical_alias, 0xc0001000);
    ring->set_read_offset(60); ring->set_write_offset(4);
    expect(ring->read_count() == 8);
    expect(ring->ReadAndSwap<uint32_t>() == 0x80000000 && ring->read_offset() == 0);
    expect(ring->ReadAndSwap<uint32_t>() == 0xc0001000 && !ring->read_count());
    expect(gpu_physical.write_read_pointer(physical + 0x100, ring->read_offset() / 4, 16));
    expect(load_word(physical_alias + 0x100) == 1 && load_word(0xc0000000 + physical + 0x100) == 1);
    expect(!gpu_physical.write_read_pointer(physical + 0x100, 16, 16) && load_word(physical_alias + 0x100) == 1);
    // Exercise the SDK contract through its base type using this loader's memfd.
    {
        MeRexGpuMemory bridge(gpu_physical, gpu_mmio);
        rex::memory::GpuMemory& sdk_memory = bridge;
        expect(sdk_memory.physical_membase() == shared_bytes - physical);
        expect(sdk_memory.TranslatePhysical(physical) == shared_bytes);
        expect(sdk_memory.TranslatePhysical(physical | 0xe0000000) == shared_bytes);
        auto* sdk_word = sdk_memory.TranslatePhysical<uint32_t*>(physical + 0x104);
        *sdk_word = __builtin_bswap32(0xcafebabe);
        expect(load_word(physical_alias + 0x104) == 0xcafebabe);
        struct SdkCapture { uint32_t address = 0, value = 0; bool null_context = false; } sdk_capture;
        const auto read = +[](void* ppc, void* opaque, uint32_t a) -> uint32_t {
            auto& c = *static_cast<SdkCapture*>(opaque); c.address = a; c.null_context = !ppc; return c.value;
        };
        const auto write = +[](void* ppc, void* opaque, uint32_t a, uint32_t v) {
            auto& c = *static_cast<SdkCapture*>(opaque); c.address = a; c.value = v; c.null_context = !ppc;
        };
        expect(!sdk_memory.AddVirtualMappedRange(0x10000000, 0xffff0000, 0xffff, &sdk_capture, read, write));
        expect(sdk_memory.AddVirtualMappedRange(0x7fc80000, 0xffff0000, 0xffff, &sdk_capture, read, write));
        PPC_STORE_U32(0x7fc80714, 9);
        expect(sdk_capture.address == 0x7fc80714 && sdk_capture.value == 9 && sdk_capture.null_context);
        expect(PPC_LOAD_U32(0x7fc80714) == 9);
        expect(!sdk_memory.AddVirtualMappedRange(0x7fc80000, 0xffff0000, 0xffff, &sdk_capture, read, write));
        bool watch_rejected = false;
        try { sdk_memory.RegisterPhysicalMemoryInvalidationCallback(nullptr, nullptr); }
        catch (const std::runtime_error&) { watch_rejected = true; }
        expect(watch_rejected);
        struct Invalidation { uint32_t calls = 0, address = 0, length = 0; } invalidation;
        auto callback = +[](void* opaque, uint32_t address, uint32_t length, bool exact) {
            auto& state = *static_cast<Invalidation*>(opaque);
            ++state.calls; state.address = address; state.length = length;
            return std::pair<uint32_t, uint32_t>{address, length};
        };
        auto watch = sdk_memory.RegisterPhysicalMemoryInvalidationCallback(callback, &invalidation);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical + 3, 1, true, false);
        expect(bridge.watched_pages() == 1);
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(!invalidation.calls);
        PPC_STORE_U32(physical_alias + 16, 0x11223344);
        // Re-enabling must not hide the write by replacing the snapshot.
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites();
        expect(invalidation.calls == 1 && invalidation.address == physical && invalidation.length == 4096);
        expect(bridge.watched_pages() == 0);
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 1);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(guest + physical_alias + 32), simde_mm_set1_epi32(0x12345678));
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 2);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        __atomic_store_n(reinterpret_cast<uint32_t*>(guest + physical_alias + 64), 0x87654321, __ATOMIC_SEQ_CST);
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 3);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        std::memset(guest + physical_alias + 80, 0x5a, 16); // CPU write through a guest alias
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 4);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites();
        PPC_STORE_U32(physical_alias + 16, 0x55667788);
        sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 5 && bridge.watched_pages() == 0); // reported without a gate handover
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 5);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        PPC_STORE_U32(0xc0000000u + physical + 8, 1); // second physical alias faults too
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 6 && invalidation.address == physical);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        me_gpu_write_watch.host_write(physical_alias + 100, 8); // system-call writes (pread) are marked explicitly
        expect(!bridge.watched_pages());
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 7);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 8192, true, false);
        expect(bridge.watched_pages() == 2);
        MeRexGpuMemory::InvalidateReleasedRange(physical, 8192);
        expect(invalidation.calls == 8 && invalidation.length == 8192 && !bridge.watched_pages());
        auto release_alias = physical_memory.allocate(4096, 4, 0, UINT32_MAX, 4096);
        expect(release_alias != 0);
        const auto release_physical = MePhysicalMemory::physical(release_alias);
        // Allocation drops stale GPU copies of the new range (default ME_GPU_ALLOC_INVALIDATE).
        expect(invalidation.calls == 9 && invalidation.address == release_physical && invalidation.length == 4096);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(release_physical, 4096, true, false);
        expect(physical_memory.free(release_alias));
        expect(invalidation.calls == 10 && invalidation.address == release_physical && !bridge.watched_pages());
        sdk_memory.UnregisterPhysicalMemoryInvalidationCallback(watch);
        sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, false);
        PPC_STORE_U32(physical_alias + 16, 0);
        me_gate_epoch.fetch_add(1); sdk_memory.SynchronizePhysicalWrites(); expect(invalidation.calls == 10);
        bool rejected = false;
        try { sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 4096, true, true); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected);
        rejected = false;
        try { sdk_memory.EnablePhysicalMemoryAccessCallbacks(physical, 8193, true, false); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected && bridge.watched_pages() == 0);

    }
    expect(!gpu_mmio.read && !gpu_mmio.write);

    expect(!gpu_physical.resolve(physical, 8193) && !gpu_physical.resolve(0x1fffffff, 4));
    expect(!gpu_physical.bind_ring(physical + 1, 3) && !gpu_physical.bind_ring(physical, 22));
    ring.reset();
    expect(!physical_memory.allocate(8192, 4, physical, physical + 4095, 0));
    ctx.r3.u64 = 0; ctx.r4.u64 = physical_alias; __imp__MmFreePhysicalMemory(ctx, guest);
    expect(physical_memory.allocations.empty() && !physical_memory.find(physical_alias));
    if (!strict_physical_free()) {
        // Platform contract: the freed allocation's pages stay mapped in every
        // alias window, which is what makes the console's deferred audio-kick
        // race legal. Any stale read or write must not fault.
        put_word(physical_alias + 0x10, 0xdeadbeef);
        expect(load_word(physical_alias + 0x10) == 0xdeadbeef);
        expect(load_word(0xc0000000u + physical + 0x10) == 0xdeadbeef);
        PPC_STORE_U32(0xa0000000u + physical + 0x14, 5);
        expect(load_word(physical_alias + 0x14) == 5);
    }
    expect(!gpu_physical.resolve(physical, 4));
    auto reused = physical_memory.allocate(4800, 0x404, physical, physical + 8191, 4096);
    expect(reused == physical_alias && !load_word(reused) && !load_word(reused + 4));
    expect(physical_memory.free(reused));
    auto large = physical_memory.allocate(1, 0x20000004, 0, UINT32_MAX, 65536);
    expect(large >= 0xa0000000 && large < 0xc0000000 && !(MePhysicalMemory::physical(large) & 65535) && physical_memory.find(large)->size == 65536);
    expect(physical_memory.free(large));
    // Actual SDK HostWorker lifecycle, independent of the still-unconnected CP.
    {
        rex::graphics::HostWorker worker;
        std::promise<void> entered;
        auto ready = entered.get_future();
        std::atomic<unsigned> stage{0};
        worker.RequestPause();
        expect(worker.Start([&] {
            stage = worker.IsCurrentThread() ? 1 : 99;
            entered.set_value();
            worker.PauseCurrent();
            stage = 2;
        }));
        ready.wait();
        expect(stage == 1 && !worker.IsCurrentThread());
        expect(!worker.Start([] {}));
        worker.Resume(); worker.Join();
        expect(stage == 2 && !worker.failure());
        rex::graphics::HostWorker early_resume;
        early_resume.RequestPause(); early_resume.Resume();
        expect(early_resume.Start([&] { early_resume.PauseCurrent(); stage = 3; }));
        early_resume.Join(); expect(stage == 3);
        rex::graphics::HostWorker failing;
        expect(failing.Start([] { throw std::runtime_error("test worker failure"); }));
        failing.Join(); expect(bool(failing.failure()));
    }
    // Readiness handshake tests use SDK HostWorker, not a simulated Vulkan device.
    {
        std::atomic<unsigned> initialized{0}, ran{0};
        rex::graphics::HostWorker ready_worker;
        expect(ready_worker.StartReady([&] {
            initialized = ready_worker.IsCurrentThread() ? 1 : 99; return true;
        }, [&] { ++ran; }));
        expect(initialized == 1); // setup completion is guaranteed on return.
        ready_worker.Join(); expect(ran == 1 && !ready_worker.failure());
        rex::graphics::HostWorker rejected;
        expect(!rejected.StartReady([&] { initialized = 2; return false; }, [&] { ++ran; }));
        expect(initialized == 2 && ran == 1 && !rejected.failure());
        rex::graphics::HostWorker throwing_setup;
        expect(!throwing_setup.StartReady([]() -> bool { throw std::runtime_error("test setup failure"); }, [&] { ++ran; }));
        expect(bool(throwing_setup.failure()) && ran == 1);
        rex::graphics::HostWorker missing_setup;
        expect(!missing_setup.StartReady({}, [&] { ++ran; }));
    }
    {
        unsigned enters = 0, leaves = 0;
        rex::graphics::GuestExecutionHooks hooks{[&] { ++enters; }, [&] { ++leaves; }};
        rex::graphics::GuestExecutionGuard* current = nullptr;
        {
            rex::graphics::GuestExecutionBinding binding(current, hooks);
            expect(current && enters == 1 && !leaves);
            current->Release(); current->Release(); expect(leaves == 1);
            current->Acquire(); current->Acquire(); expect(enters == 2);
        }
        expect(!current && leaves == 2);
        try { rex::graphics::GuestExecutionBinding binding(current, hooks); throw std::runtime_error("gate unwind test"); }
        catch (const std::runtime_error&) {}
        expect(!current && enters == 3 && leaves == 3);
        bool rejected = false;
        rex::graphics::GuestExecutionHooks bad_hooks{[] {}, {}};
        try { rex::graphics::GuestExecutionGuard bad(bad_hooks); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected);
        // A real waiter remains blocked until the GPU scope releases the loader gate.
        std::promise<void> waiting, owned;
        auto waiting_future = waiting.get_future(), owned_future = owned.get_future();
        rex::graphics::HostWorker waiter;
        rex::graphics::GuestExecutionHooks gate_hooks{[] { execution_gate.lock(); }, [] { execution_gate.unlock(); }};
        { std::unique_lock<MeExecutionGate> release(execution_gate, std::adopt_lock);
          release.unlock();
          {
              rex::graphics::GuestExecutionGuard access(gate_hooks);
              expect(waiter.Start([&] { waiting.set_value(); std::lock_guard<MeExecutionGate> lock(execution_gate); owned.set_value(); }));
              waiting_future.wait();
              expect(owned_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
              access.Release(); owned_future.wait(); waiter.Join(); access.Acquire();
          }
          release.lock(); release.release(); }
        expect(!waiter.failure());
    }
    expect(!load_word(0x60003000) && !load_word(0x60003020));
    put_word(0x60003000, 0x12345678); expect(load_word(0x60003000) == 0x12345678); put_word(0x60003000, 0);
    put_word(0x70000600, 0x82230088); put_word(0x70000604, 12);
    ctx.r3.u64 = 0x70000600; ctx.r4.u64 = 1; __imp__ExRegisterTitleTerminateNotification(ctx, guest);
    expect(title_terminate_notifications.size() == 1 && title_terminate_notifications[0].priority == 12);
    ctx.r4.u64 = 0; __imp__ExRegisterTitleTerminateNotification(ctx, guest);
    expect(title_terminate_notifications.empty());
    __imp__ExRegisterTitleTerminateNotification(ctx, guest); expect(title_terminate_notifications.empty());
    expect(guest[0x60003080] == 1 && guest[0x60003081] == 40 && load_word(0x60003090) == UINT32_MAX);
    ctx.r3.u64 = 0x60003080; __imp__RtlTryEnterCriticalSection(ctx, guest);
    expect(ctx.r3.u32 == 1 && load_word(0x60003098) == thread_address);
    ctx.r3.u64 = 0x60003080; __imp__RtlLeaveCriticalSection(ctx, guest);
    __imp__KeEnterCriticalRegion(ctx, guest); __imp__KeEnterCriticalRegion(ctx, guest);
    expect(int32_t(load_word(thread_address + 0xb0)) == -2);
    __imp__KeLeaveCriticalRegion(ctx, guest); expect(int32_t(load_word(thread_address + 0xb0)) == -1);
    __imp__KeLeaveCriticalRegion(ctx, guest); expect(!load_word(thread_address + 0xb0));
    constexpr uint32_t embedded_event = 0x70000900, embedded_timeout = 0x70000940;
    auto init_embedded = [&](uint32_t type, uint32_t signal) {
        ctx.r3.u64 = embedded_event; ctx.r4.u64 = type; ctx.r5.u64 = signal; __imp__KeInitializeEvent(ctx, guest);
    };
    auto wait_embedded = [&](uint32_t address, uint32_t timeout) {
        ctx.r3.u64 = address; ctx.r4.u64 = 3; ctx.r5.u64 = 0; ctx.r6.u64 = 0; ctx.r7.u64 = timeout;
        __imp__KeWaitForSingleObject(ctx, guest); return ctx.r3.u32;
    };
    put_word(embedded_timeout, 0); put_word(embedded_timeout + 4, 0);
    init_embedded(1, 0); expect(guest[embedded_event] == 1 && !load_word(embedded_event + 4));
    expect(wait_embedded(embedded_event, embedded_timeout) == 0x102);
    ctx.r3.u64 = embedded_event; ctx.r4.u64 = 0; ctx.r5.u64 = 0; __imp__KeSetEvent(ctx, guest);
    expect(!ctx.r3.u32 && load_word(embedded_event + 4) == 1);
    expect(wait_embedded(embedded_event, embedded_timeout) == 0 && !load_word(embedded_event + 4));
    init_embedded(0, 1);
    expect(wait_embedded(embedded_event, embedded_timeout) == 0 && wait_embedded(embedded_event, embedded_timeout) == 0 && load_word(embedded_event + 4) == 1);
    ctx.r3.u64 = embedded_event; __imp__KeResetEvent(ctx, guest); expect(ctx.r3.u32 == 1 && !load_word(embedded_event + 4));
    init_embedded(1, 0);
    put_word(embedded_timeout, UINT32_MAX); put_word(embedded_timeout + 4, uint32_t(-10000));
    expect(wait_embedded(embedded_event, embedded_timeout) == 0x102);
    std::thread signal_embedded([&] {
        std::lock_guard<MeExecutionGate> gate(execution_gate);
        PPCContext setter{}; setter.r3.u64 = embedded_event;
        __imp__KeSetEvent(setter, guest);
    });
    expect(wait_embedded(embedded_event, 0) == 0 && !load_word(embedded_event + 4)); signal_embedded.join();
    expect(wait_embedded(threads[1]->kthread, 0) == 0);
    const uint32_t test_semaphore = 0x70000b00;
    ctx.r3.u64 = test_semaphore; ctx.r4.u64 = 2; ctx.r5.u64 = 3;
    __imp__KeInitializeSemaphore(ctx, guest);
    expect(guest[test_semaphore] == 5 && load_word(test_semaphore + 4) == 2 && load_word(test_semaphore + 16) == 3);
    expect(wait_embedded(test_semaphore, embedded_timeout) == 0 && load_word(test_semaphore + 4) == 1);
    expect(wait_embedded(test_semaphore, embedded_timeout) == 0 && !load_word(test_semaphore + 4));
    put_word(embedded_timeout, 0); put_word(embedded_timeout + 4, 0);
    expect(wait_embedded(test_semaphore, embedded_timeout) == 0x102 && !load_word(test_semaphore + 4));
    ctx.r3.u64 = test_semaphore; ctx.r4.u64 = 0; ctx.r5.u64 = 3; ctx.r6.u64 = 0;
    __imp__KeReleaseSemaphore(ctx, guest);
    expect(ctx.r3.u32 == 0 && load_word(test_semaphore + 4) == 3);
    for (unsigned i = 0; i < 3; ++i) expect(wait_embedded(test_semaphore, embedded_timeout) == 0 && load_word(test_semaphore + 4) == 2-i);
    std::thread release_semaphore([&] {
        std::lock_guard<MeExecutionGate> gate(execution_gate);
        PPCContext release{}; release.r3.u64 = test_semaphore; release.r5.u64 = 1;
        __imp__KeReleaseSemaphore(release, guest);
    });
    expect(wait_embedded(test_semaphore, 0) == 0 && !load_word(test_semaphore + 4));
    release_semaphore.join();
    put_word(embedded_timeout, UINT32_MAX); put_word(embedded_timeout + 4, uint32_t(-10000));
    expect(wait_embedded(test_semaphore, embedded_timeout) == 0x102 && !load_word(test_semaphore + 4));
    const uint32_t multi_event = 0x70000d00, multi_array = 0x70000d40;
    put_word(multi_array, multi_event); put_word(multi_array + 4, test_semaphore);
    std::memset(guest + multi_event, 0, 16); put_word(test_semaphore + 4, 1);
    put_word(embedded_timeout, 0); put_word(embedded_timeout + 4, 0);
    auto wait_multiple = [&](uint32_t type, uint32_t interval) {
        ctx.r3.u64 = 2; ctx.r4.u64 = multi_array; ctx.r5.u64 = type;
        ctx.r6.u64 = ctx.r7.u64 = ctx.r8.u64 = ctx.r10.u64 = 0; ctx.r9.u64 = interval;
        __imp__KeWaitForMultipleObjects(ctx, guest); return ctx.r3.u32;
    };
    expect(wait_multiple(0, embedded_timeout) == 0x102 && load_word(test_semaphore + 4) == 1);
    put_word(multi_event + 4, 1);
    expect(wait_multiple(0, embedded_timeout) == 0 && !load_word(test_semaphore + 4) && load_word(multi_event + 4) == 1);
    expect(wait_multiple(0, embedded_timeout) == 0x102 && load_word(multi_event + 4) == 1);
    put_word(multi_event + 4, 0); put_word(test_semaphore + 4, 2);
    expect(wait_multiple(1, embedded_timeout) == 1 && load_word(test_semaphore + 4) == 1);
    put_word(multi_event + 4, 1);
    expect(wait_multiple(1, embedded_timeout) == 0 && load_word(test_semaphore + 4) == 1);
    guest[multi_event] = 1;
    expect(wait_multiple(0, embedded_timeout) == 0 && !load_word(multi_event + 4) && !load_word(test_semaphore + 4));
    put_word(test_semaphore + 4, 1);
    std::thread multi_signal([&] {
        std::lock_guard<MeExecutionGate> gate(execution_gate);
        PPCContext signal{}; signal.r3.u64 = multi_event; __imp__KeSetEvent(signal, guest);
    });
    expect(wait_multiple(0, 0) == 0 && !load_word(multi_event + 4) && !load_word(test_semaphore + 4));
    multi_signal.join();
    put_word(embedded_timeout, UINT32_MAX); put_word(embedded_timeout + 4, uint32_t(-10000));
    expect(wait_multiple(1, embedded_timeout) == 0x102);
    put_word(multi_array, threads[1]->kthread);
    expect(wait_multiple(1, embedded_timeout) == 0);
    std::memcpy(guest + 0x70000e00, "abc", 4);
    ctx.r3.u64 = 0x70000e10; ctx.r4.u64 = 0x70000e00; __imp__RtlInitAnsiString(ctx, guest);
    expect(load_word(0x70000e10) == 0x00030004 && load_word(0x70000e14) == 0x70000e00);
    ctx.r4.u64 = 0; __imp__RtlInitAnsiString(ctx, guest);
    expect(!load_word(0x70000e10) && !load_word(0x70000e14));
    ctx.r4.u64 = 0x70000e03; __imp__RtlInitAnsiString(ctx, guest);
    expect(load_word(0x70000e10) == 1 && load_word(0x70000e14) == 0x70000e03);
    std::strcpy(reinterpret_cast<char*>(guest + 0x70000f00), "ShaderDumpxe:\\CompareBackEnds");
    ctx.r3.u64 = 0x70000f40; ctx.r4.u64 = 0x70000f00; __imp__RtlInitAnsiString(ctx, guest);
    put_word(0x70000f50, 0xfffffffd); put_word(0x70000f54, 0x70000f40); put_word(0x70000f58, 0);
    std::memset(guest + 0x70000f80, 0xaa, 56);
    ctx.r3.u64 = 0x70000f50; ctx.r4.u64 = 0x70000f80; __imp__NtQueryFullAttributesFile(ctx, guest);
    expect(ctx.r3.u32 == 0xc000000f && load_word(0x70000f80) == 0xaaaaaaaa && load_word(0x70000fb4) == 0xaaaaaaaa);
    ctx.r3.u64 = 0xc000000f; __imp__RtlNtStatusToDosError(ctx, guest); expect(ctx.r3.u32 == 2);
    ctx.r3.u64=0xc0000008;__imp__RtlNtStatusToDosError(ctx,guest);expect(ctx.r3.u32==6);
    ctx.r3.u64=0xc0000035;__imp__RtlNtStatusToDosError(ctx,guest);expect(ctx.r3.u32==183);
    guest[0x70000f00] = 1;
    ctx.r3.u64 = 0x70000f50; ctx.r4.u64 = 0x70000f80; __imp__NtQueryFullAttributesFile(ctx, guest);
    expect(ctx.r3.u32 == 0xc0000033 && load_word(0x70000f80) == 0xaaaaaaaa);
    const auto saved_suspended_entry = PPC_LOOKUP_FUNC(guest, PPC_CODE_BASE + 8);
    PPC_LOOKUP_FUNC(guest, PPC_CODE_BASE + 8) = suspended_thread_test_entry;
    put_word(0x70000c00, 0);
    ctx.r3.u64 = 0x70000c04; ctx.r4.u64 = 0x8000; ctx.r5.u64 = ctx.r6.u64 = ctx.r8.u64 = 0;
    ctx.r7.u64 = PPC_CODE_BASE + 8; ctx.r9.u64 = 0x08000001;
    __imp__ExCreateThread(ctx, guest);
    auto suspended = threads.back();
    expect(!ctx.r3.u32 && suspended->suspend_count == 1 && guest[suspended->kthread + 0xbc] == 1);
    { std::unique_lock<MeExecutionGate> release(execution_gate, std::adopt_lock);
      release.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); release.lock(); release.release(); }
    expect(!load_word(0x70000c00) && !suspended->done);
    expect(suspended->affinity == 8 && guest[suspended->pcr + 0x10c] == 3);
    ctx.r3.u64=suspended->handle;ctx.r4.u64=0x70001400;__imp__NtResumeThread(ctx,guest);
    expect(ctx.r3.u32==0 && load_word(0x70001400)==1);
    ctx.r3.u64=suspended->handle;ctx.r4.u64=0x70001400;__imp__NtResumeThread(ctx,guest);
    expect(ctx.r3.u32==0 && load_word(0x70001400)==0);
    ctx.r3.u64 = suspended->kthread; __imp__KeResumeThread(ctx, guest);
    expect(!ctx.r3.u32 && !suspended->suspend_count && !guest[suspended->kthread + 0xbc]);
    expect(wait_embedded(suspended->kthread, 0) == 0 && load_word(0x70000c00) == 4);
    ctx.r3.u64 = 0xdeadbeef; __imp__KeResumeThread(ctx, guest); expect(ctx.r3.u32 == 0xc0000008);
    PPC_LOOKUP_FUNC(guest, PPC_CODE_BASE + 8) = saved_suspended_entry;
    // Dedicated guest context + host delivery thread; no game IRQ is generated here.
    const auto main_pcr = pcr_address, main_thread = thread_address, main_tls = tls_dynamic;
    PPCContext irq_context{};
    initialize_thread(irq_context, unsigned(threads.size()), 0x8000);
    auto irq_record = threads.back();
    pcr_address = main_pcr; thread_address = main_thread; tls_dynamic = main_tls;
    const auto irq_stack = irq_context.r1.u64;
    const auto irq_tls_pointer = load_word(irq_record->pcr);
    constexpr uint32_t test_callback = PPC_CODE_BASE + 4;
    auto old_irq_mapping = PPC_LOOKUP_FUNC(guest, test_callback);
    PPC_LOOKUP_FUNC(guest, test_callback) = me_test_gpu_interrupt;
    MeGpuInterruptWorker irq_worker;
    expect(!irq_worker.submit({test_callback, 0, 0, 2}));
    unsigned delivered = 0;
    uint64_t irq_result = 0;
    expect(irq_worker.start([&](const MeGpuInterruptWorker::Request& request) {
        std::lock_guard<MeExecutionGate> lock(execution_gate);
        irq_result = dispatch_gpu_interrupt_locked(irq_context, *irq_record, request);
        ++delivered;
    }));
    expect(!irq_worker.start({}));
    expect(!irq_worker.submit({0, 0, 0, 2}) && !irq_worker.submit({test_callback + 1, 0, 0, 2}));
    expect(!irq_worker.submit({test_callback, 0, 0, 6}));
    expect(irq_worker.submit({test_callback, 0xaabbccdd, 7, UINT32_MAX}));
    expect(irq_worker.submit({0x82230088, 0, 0x70000540, 1}));
    // Release the gate before joining the worker, which needs it for guest code.
    { std::unique_lock<MeExecutionGate> release(execution_gate, std::adopt_lock);
      release.unlock(); irq_worker.stop_and_join(); release.lock(); release.release(); }
    expect(!irq_worker.failure() && delivered == 2 && irq_result == 0x70000540);
    expect(load_word(0x70000540) == uint32_t(-2112880640 + 31336));
    expect(load_word(0x70000520) == 7 && load_word(0x70000524) == 0xaabbccdd);
    expect(!load_word(0x70000528) && load_word(0x7000052c) == 2);
    expect(load_word(0x70000538) == 2 && guest[irq_record->pcr + 0x18] == 0);
    expect(load_word(0x70000530) == irq_record->pcr && load_word(0x70000534) == irq_record->kthread);
    expect(load_word(irq_record->pcr) == irq_tls_pointer && irq_context.r1.u64 == irq_stack && !irq_context.lr);
    expect(pcr_address == main_pcr && thread_address == main_thread && active_context == &ctx);
    expect(!irq_worker.submit({test_callback, 0, 0, 2}));
    bool rejected_irq = false;
    try { dispatch_gpu_interrupt_locked(irq_context, *irq_record, {PPC_CODE_BASE - 4, 0, 0, 2}); }
    catch (const std::runtime_error&) { rejected_irq = true; }
    expect(rejected_irq && load_word(irq_record->pcr) == irq_tls_pointer);
    MeGpuInterruptWorker failing_irq;
    expect(failing_irq.start([&](const MeGpuInterruptWorker::Request& request) {
        std::lock_guard<MeExecutionGate> lock(execution_gate);
        dispatch_gpu_interrupt_locked(irq_context, *irq_record, request);
    }));
    expect(failing_irq.submit({test_callback, UINT32_MAX, 0, 3}));
    { std::unique_lock<MeExecutionGate> release(execution_gate, std::adopt_lock);
      release.unlock(); failing_irq.stop_and_join(); release.lock(); release.release(); }
    expect(guest[irq_record->pcr + 0x18] == 0);
    expect(bool(failing_irq.failure()) && !failing_irq.submit({test_callback, 0, 0, 2}));
    expect(load_word(irq_record->pcr) == irq_tls_pointer && irq_context.r1.u64 == irq_stack && !irq_context.lr);
    expect(pcr_address == main_pcr && thread_address == main_thread && active_context == &ctx);
    PPC_LOOKUP_FUNC(guest, test_callback) = old_irq_mapping;
    irq_record->done = true;
    {
        const bool saved=polling_yield_experiment;polling_yield_experiment=true;
        for(const double input:{0.25,1.0,2.0,4.0,48000.0}){
            double seed=me_frsqrte_diagnostic(input,0x82ebbf18);
            expect(std::abs(seed*std::sqrt(input)-1.0)<0.000001);
            for(int i=0;i<3;++i)seed=0.5*seed*(3.0-input*seed*seed);
            expect(std::abs(seed*input-std::sqrt(input))<1e-10*std::sqrt(input));
        }
        polling_yield_experiment=saved;
    }
    {
        timestamp_bundle_address=physical_memory.allocate(24,4,0,0xffffffff,4096);
        expect(timestamp_bundle_address!=0);
        std::memset(guest+timestamp_bundle_address,0,24);
        timespec origin{};clock_gettime(CLOCK_MONOTONIC,&origin);
        timestamp_origin_ns=uint64_t(origin.tv_sec)*1000000000ull+uint64_t(origin.tv_nsec);
        const auto first=me_guest_load<uint32_t>(guest,uint64_t(timestamp_bundle_address)+16);
        timespec pause{0,5000000};nanosleep(&pause,nullptr);
        const auto second=me_guest_load<uint32_t>(guest,uint64_t(timestamp_bundle_address)+16);
        expect(second>=first+4 && second<1000);
        expect(load_word(timestamp_bundle_address)==0 && load_word(timestamp_bundle_address+20)==0);
        expect(physical_memory.free(timestamp_bundle_address));timestamp_bundle_address=0;
    }
    {
        // Execute the actual recompiled selection function against controlled
        // guest objects. No invented worker completion or altered getter value.
        const uint32_t pool=0x70002000,free_array=0x70002100,pending_array=0x70002200;
        const uint32_t normal=0x70002300,reserved=0x70002400,vtable=0x70002500,job=0x70002600,lock=0x70002700;
        const uint32_t callback=0x829fab98;
        auto old=PPC_LOOKUP_FUNC(guest,callback);PPC_LOOKUP_FUNC(guest,callback)=pool_test_assign;
        const bool saved=pool_scan_experiment;
        auto setup=[&](std::initializer_list<uint32_t> free){
            std::memset(guest+pool,0,0x800);put_word(pool+4,pending_array);put_word(pool+12,4);
            put_word(pool+16,free_array);put_word(pool+20,free.size());put_word(pool+24,4);put_word(pool+28,lock);
            uint32_t index=0;for(auto item:free)put_word(free_array+4*index++,item);
            put_word(normal,vtable);put_word(reserved,vtable);put_word(reserved+32,1);
            put_word(vtable+12,callback);put_word(vtable+16,0x826ccc08);
            ctx.r3.u64=lock+4;__imp__RtlInitializeCriticalSection(ctx,guest);
            pool_test_selected_worker=0;pool_test_selected_job=0;
        };
        auto submit=[&](uint32_t flag){ctx.r3.u64=pool;ctx.r4.u64=job;ctx.r5.u64=flag;sub_8266A880(ctx,guest);};
        pool_scan_experiment=false;setup({reserved,normal});submit(0);
        expect(pool_test_selected_worker==normal && pool_test_selected_job==job && load_word(pool+20)==1);
        pool_scan_experiment=true;setup({normal,reserved});submit(0);
        expect(pool_test_selected_worker==normal && pool_test_selected_job==job && load_word(pool+20)==1);
        expect(load_word(free_array)==reserved && load_word(reserved+32)==1 && load_word(pool+8)==0);
        setup({reserved,normal});submit(0);expect(pool_test_selected_worker==normal && load_word(pool+20)==1);
        setup({reserved});submit(0);expect(!pool_test_selected_worker && load_word(pool+8)==1 && load_word(pending_array)==job);
        expect(load_word(pool+20)==1 && load_word(reserved+32)==1);
        setup({normal,reserved});submit(1);expect(pool_test_selected_worker==reserved && load_word(job+4)==1 && load_word(pool+20)==1);
        setup({});submit(0);expect(!pool_test_selected_worker && load_word(pool+8)==1 && load_word(pending_array)==job);
        pool_scan_experiment=saved;PPC_LOOKUP_FUNC(guest,callback)=old;
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        put_word(0x70001400,0x10040002);put_word(0x70001410,16);std::memset(guest+0x70001420,0x5a,16);
        put_word(ctx.r1.u32+84,0);
        auto read=[&](uint32_t user,uint32_t count,uint32_t size){ctx.r3.u64=0;ctx.r4.u64=user;ctx.r5.u64=0;ctx.r6.u64=0;ctx.r7.u64=count;ctx.r8.u64=0x70001400;ctx.r9.u64=size;ctx.r10.u64=0x70001420;__imp__XamUserReadProfileSettings(ctx,guest);return ctx.r3.u32;};
        expect(read(0,1,0x70001410)==1317);expect(load_word(0x70001410)==16);
        bool unchanged=true;for(unsigned i=0;i<16;++i)unchanged&=guest[0x70001420+i]==0x5a;expect(unchanged);
        expect(read(4,1,0x70001410)==87);expect(read(0,0,0x70001410)==87);expect(read(0,33,0x70001410)==87);expect(read(0,1,0)==87);
        put_word(0x70001410,0);expect(read(0,1,0x70001410)==1317 && load_word(0x70001410)==0);
        auto name=[&](uint32_t user,uint32_t output,uint32_t capacity){ctx.r3.u64=user;ctx.r4.u64=output;ctx.r5.u64=capacity;__imp__XamUserGetName(ctx,guest);return ctx.r3.u32;};
        expect(name(0,0x70001420,16)==0x80070525);expect(name(3,0x70001420,16)==0x80070525);
        unchanged=true;for(unsigned i=0;i<16;++i)unchanged&=guest[0x70001420+i]==0x5a;expect(unchanged);
        expect(name(4,0x70001420,16)==0x80070057);expect(name(0,0,16)==0x80070057);expect(name(0,0x70001420,0)==0x80070057);
        gpu_explore_mode=saved;
    }
    {
        timespec before{},after{};clock_gettime(CLOCK_REALTIME,&before);
        put_word(0x7000141c,0x13572468);put_word(0x70001428,0x24681357);
        ctx.r3.u64=0x70001420;ctx.r4.u64=0x87654321;__imp__KeQuerySystemTime(ctx,guest);clock_gettime(CLOCK_REALTIME,&after);
        const uint64_t ticks=(uint64_t(load_word(0x70001420))<<32)|load_word(0x70001424);
        auto filetime=[](timespec t){return uint64_t(t.tv_sec+11644473600ll)*10000000ull+uint64_t(t.tv_nsec)/100;};
        expect(ticks>=filetime(before) && ticks<=filetime(after));
        expect(ctx.r3.u32==0x70001420 && ctx.r4.u32==0x87654321);
        expect(load_word(0x7000141c)==0x13572468 && load_word(0x70001428)==0x24681357);
        ctx.r3.u64=0;__imp__KeQuerySystemTime(ctx,guest);expect(ctx.r3.u64==0);
    }
    {
        auto query=[&](uint32_t setting,uint32_t output,uint32_t capacity){ctx.r3.u64=3;ctx.r4.u64=setting;ctx.r5.u64=output;ctx.r6.u64=capacity;ctx.r7.u64=0x70001430;__imp__ExGetXConfigSetting(ctx,guest);return ctx.r3.u32;};
        for(uint32_t setting=1;setting<=7;++setting){
            put_word(0x70001420,0xdeadbeef);put_word(0x70001430,0xaaaa5a5a);
            expect(query(setting,0x70001420,4)==0);
            expect(load_word(0x70001420)==((setting==2 || setting==3)?0x55544300u:0u) && load_word(0x70001430)==0x00045a5a);
        }
        put_word(0x70001420,0xdeadbeef);expect(query(1,0x70001420,3)==0xc0000023);
        expect(load_word(0x70001420)==0xdeadbeef && load_word(0x70001430)==0x00005a5a);
        expect(query(1,0,0)==0 && load_word(0x70001430)==0x00045a5a);
        expect(query(1,0,4)==0xc00000f1 && load_word(0x70001430)==0x00005a5a);
        expect(query(12,0x70001420,4)==0 && load_word(0x70001420)==0x40 && load_word(0x70001430)==0x00045a5a);
        expect(query(12,0,0)==0 && load_word(0x70001430)==0x00045a5a);
        expect(query(12,0x70001420,3)==0xc0000023 && load_word(0x70001420)==0x40);
    }
    {
        const std::pair<uint64_t,MeTimeFields::Fields> vectors[]={
            {0,{1601,1,1,0,0,0,0,1}},
            {116444736000000000ull,{1970,1,1,0,0,0,0,4}},
            {125963012967890000ull,{2000,2,29,12,34,56,789,2}},
            {133537247999990000ull,{2024,2,29,23,59,59,999,4}}};
        for(const auto& [ticks,expected]:vectors){
            put_word(0x70001400,uint32_t(ticks>>32));put_word(0x70001404,uint32_t(ticks));
            ctx.r3.u64=0x70001400;ctx.r4.u64=0x70001410;__imp__RtlTimeToTimeFields(ctx,guest);
            bool equal=true;for(unsigned i=0;i<8;++i){uint16_t value;std::memcpy(&value,guest+0x70001410+2*i,2);equal&=__builtin_bswap16(value)==expected[i];}expect(equal);
            expect(ctx.r3.u32==0x70001400 && ctx.r4.u32==0x70001410);
            ctx.r3.u64=0x70001410;ctx.r4.u64=0x70001430;__imp__RtlTimeFieldsToTime(ctx,guest);
            expect(ctx.r3.u32==1 && ((uint64_t(load_word(0x70001430))<<32)|load_word(0x70001434))==ticks);
        }
        MeTimeFields::Fields invalid={1900,2,29,0,0,0,0,0};uint64_t untouched=123;
        expect(!MeTimeFields::encode(invalid,untouched) && untouched==123);
        invalid={2024,2,29,24,0,0,0,0};expect(!MeTimeFields::encode(invalid,untouched));
        invalid={2024,258,29,0,0,0,0,0};expect(!MeTimeFields::encode(invalid,untouched));
        invalid={2024,1,1,0,0,0,1000,0};expect(!MeTimeFields::encode(invalid,untouched));
        MeTimeFields::Fields fields{};expect(!MeTimeFields::decode(UINT64_MAX,fields));
        expect(MeTimeFields::decode(116444736000009999ull,fields) && fields[6]==0);
        put_word(0x70001410,0x076c0002);put_word(0x70001414,0x001d0000);put_word(0x70001418,0);put_word(0x7000141c,0);
        put_word(0x70001430,0x12345678);put_word(0x70001434,0x87654321);
        ctx.r3.u64=0x70001410;ctx.r4.u64=0x70001430;__imp__RtlTimeFieldsToTime(ctx,guest);
        expect(ctx.r3.u32==0 && load_word(0x70001430)==0x12345678 && load_word(0x70001434)==0x87654321);
    }
    {
        uint32_t target=0,minimum=0;expect(module_headers.import_version("xboxkrnl.exe",target,minimum));
        expect(target==0x201ddd00 && minimum==0x201a3d00);
        expect(load_word(0x60003100)==0x00020000 && load_word(0x60003104)==0x1ddd0000);
        bool bound=false;for(unsigned i=0;i<guard_count;++i)if(!std::strcmp(guards[i].name,"XboxKrnlVersion"))bound=load_word(guards[i].iat)==0x60003100;expect(bound);
        expect(load_word(0x60003120)==500 && checked_span(0x60003120,4,true) && !checked_span(0x60003120,4));
        bool clock_bound=false;for(unsigned i=0;i<guard_count;++i)if(!std::strcmp(guards[i].name,"VdGpuClockInMHz"))clock_bound=load_word(guards[i].iat)==0x60003120;expect(clock_bound);
        expect(!module_headers.import_version("absent.exe",target,minimum) && !target && !minimum);
        uint32_t table=0;expect(module_headers.lookup(0x103ff,table));
        std::vector<uint8_t> corrupt(module_headers.bytes,module_headers.bytes+module_headers.size);
        const auto offset=table-module_headers.guest_base;std::memset(corrupt.data()+offset+4,0xff,4);
        MeXexHeaders invalid{corrupt.data(),corrupt.size(),module_headers.guest_base};expect(!invalid.import_version("xboxkrnl.exe",target,minimum));
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;put_word(0x70001400,0x12345678);
        for(uint32_t user:{0u,3u,255u}){ctx.r3.u64=user;ctx.r4.u64=0;ctx.r5.u64=0x70001400;__imp__XamInputSetState(ctx,guest);expect(ctx.r3.u32==1167);}
        expect(load_word(0x70001400)==0x12345678);
        ctx.r3.u64=0;ctx.r5.u64=0;__imp__XamInputSetState(ctx,guest);expect(ctx.r3.u32==160);
        gpu_explore_mode=saved;
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        const auto fd=offline_network.fd;const auto port=offline_network.port;
        put_word(thread_address+0x160,0x12345678);ctx.r3.u64=1;ctx.r4.u64=0x87654321;
        __imp__NetDll_XNetGetEthernetLinkStatus(ctx,guest);
        expect(ctx.r3.u64==0 && ctx.r4.u32==0x87654321);
        expect(offline_network.fd==fd && offline_network.port==port && load_word(thread_address+0x160)==0x12345678);
        gpu_explore_mode=saved;
    }
    {
        const uint32_t header=0x70001500,a=0x70001510,b=0x70001520;
        put_word(a,b);put_word(b,0);put_word(header,a);put_word(header+4,0x0002ffff);
        ctx.r3.u64=header;__imp__InterlockedPopEntrySList(ctx,guest);expect(ctx.r3.u32==a);
        expect(load_word(header)==b && load_word(header+4)==0x0001ffff && load_word(a)==b);
        ctx.r3.u64=header;__imp__InterlockedPopEntrySList(ctx,guest);expect(ctx.r3.u32==b && load_word(header)==0 && load_word(header+4)==0xffff);
        ctx.r3.u64=header;__imp__InterlockedPopEntrySList(ctx,guest);expect(ctx.r3.u32==0 && load_word(header+4)==0xffff);
        put_word(header,a);put_word(header+4,0x00021234);
        ctx.r3.u64=header;__imp__InterlockedFlushSList(ctx,guest);expect(ctx.r3.u32==a && load_word(header)==0 && load_word(header+4)==0);
        expect(load_word(a)==b && load_word(b)==0);
        ctx.r3.u64=header;__imp__InterlockedFlushSList(ctx,guest);expect(ctx.r3.u64==0);
        // Exercise real host CAS contention on stable guest nodes, with no
        // allocator/free or mutation of the address-space metadata in workers.
        const uint32_t base_nodes=0x70001900;constexpr unsigned node_count=64;
        for(unsigned i=0;i<node_count;++i)put_word(base_nodes+i*8,i+1<node_count?base_nodes+(i+1)*8:0);
        put_word(header,base_nodes);put_word(header+4,(node_count<<16)|0x4321);
        std::array<std::vector<uint32_t>,4> popped;std::array<std::thread,4> workers;
        for(unsigned i=0;i<4;++i)workers[i]=std::thread([&,i]{PPCContext local{};for(;;){local.r3.u64=header;__imp__InterlockedPopEntrySList_locked(local,guest); /* body without the kernel lock: real CAS contention */if(!local.r3.u32)break;popped[i].push_back(local.r3.u32);}});
        for(auto& worker:workers)worker.join();
        std::vector<uint32_t> all;for(const auto& items:popped)all.insert(all.end(),items.begin(),items.end());std::sort(all.begin(),all.end());
        expect(all.size()==node_count);bool exact=all.size()==node_count;for(unsigned i=0;i<all.size();++i)exact&=all[i]==base_nodes+i*8;expect(exact);
        expect(load_word(header)==0 && load_word(header+4)==0x4321);
    }
    {   // Lock-free critical sections under real host contention (parallel guest mode):
        // 4 threads x 20000 Enter/Leave, a plain counter inside must come out exact.
        const uint32_t cs=0x70001a00;ctx.r3.u64=cs;__imp__RtlInitializeCriticalSection(ctx,guest);
        uint64_t counter=0;std::array<std::thread,4> workers;
        { std::unique_lock<MeExecutionGate> release(execution_gate,std::adopt_lock);release.unlock();
          for(unsigned i=0;i<4;++i)workers[i]=std::thread([&,i]{thread_address=0x61f01000+i*0x1000;PPCContext local{};
              for(unsigned n=0;n<20000;++n){local.r3.u64=cs;if(n&1){__imp__RtlEnterCriticalSection(local,guest);}else{__imp__RtlTryEnterCriticalSection(local,guest);if(!local.r3.u32){local.r3.u64=cs;__imp__RtlEnterCriticalSection(local,guest);}}
                  ++counter;local.r3.u64=cs;__imp__RtlLeaveCriticalSection(local,guest);}});
          for(auto& w:workers)w.join();release.lock();release.release(); }
        expect(counter==80000 && load_word(cs+16)==UINT32_MAX && load_word(cs+20)==0 && load_word(cs+24)==0 && cs_handoffs.empty());
    }
    {
        const auto old_pcr=pcr_address,old_thread=thread_address,old_tls=tls_dynamic;
        const auto main_last=load_word(tls_dynamic+(tls_slots-1)*4);
        PPCContext boundary{};initialize_thread(boundary,max_guest_threads-1,0x100000);auto record=threads.back();
        expect(record->pcr==0x613f0000 && record->kthread==0x613f1000 && boundary.r13.u32==record->pcr);
        expect(record->low==0x78e01000 && record->high==0x78f01000 && boundary.r1.u32==record->high-0x100);
        expect(record->tls>=0x69e00000 && uint64_t(record->tls)+tls_slots*4<0x70000000 && checked_span(record->tls,tls_slots*4));
        put_word(record->tls+(tls_slots-1)*4,0x5a123456);
        expect(load_word(record->tls+(tls_slots-1)*4)==0x5a123456 && load_word(old_tls+(tls_slots-1)*4)==main_last);
        threads.pop_back();pcr_address=old_pcr;thread_address=old_thread;tls_dynamic=old_tls;
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        std::memset(guest+0x70001400,0x5a,44);const auto fd=offline_network.fd;const auto port=offline_network.port;
        ctx.r3.u64=1;ctx.r4.u64=0x70001404;ctx.r5.u64=0x87654321;
        __imp__NetDll_XNetGetTitleXnAddr(ctx,guest);expect(ctx.r3.u32==1 && ctx.r4.u32==0x70001404 && ctx.r5.u32==0x87654321);
        bool empty=true;for(unsigned i=0;i<36;++i)empty&=guest[0x70001404+i]==0;expect(empty);
        expect(load_word(0x70001400)==0x5a5a5a5a && load_word(0x70001428)==0x5a5a5a5a);
        expect(offline_network.fd==fd && offline_network.port==port);
        gpu_explore_mode=saved;
    }
    {
        const bool saved=gpu_explore_mode;gpu_explore_mode=true;
        std::array<uint32_t,MeXmaContextPool::context_count> contexts{};bool unique=true,zero=true;
        for(unsigned i=0;i<contexts.size();++i){
            ctx.r3.u64=0x70001400;__imp__XMACreateContext(ctx,guest);contexts[i]=load_word(0x70001400);
            unique&=ctx.r3.u32==0 && contexts[i]==0xa0000000u+xma_registers.context_physical+i*64;
            for(unsigned j=0;j<64;++j)zero&=guest[contexts[i]+j]==0;
        }
        expect(unique);expect(zero);
        std::memset(guest+contexts[0],0xff,64);me_xma_store(0x7fea1a80,4,0x01000000);
        expect(load_word(contexts[0])==0x07cfffff && load_word(contexts[0]+4)==0x7fffffff);
        expect(load_word(contexts[0]+8)==0xfc000020 && load_word(contexts[0]+36)==0xffffffe0);
        bool preserved=true;for(unsigned i=0;i<16;++i)if(i!=0 && i!=1 && i!=2 && i!=9)preserved&=load_word(contexts[0]+i*4)==0xffffffff;expect(preserved);
        std::memset(guest+contexts[319],0xff,64);me_xma_store(0x7fea1aa4,4,0x00000080);
        expect(load_word(contexts[319])==0x07cfffff && load_word(contexts[319]+8)==0xfc000020 && xma_context_pool.allocated[319]);
        bool neighbors_zero=true;for(unsigned i=0;i<64;++i)neighbors_zero&=guest[contexts[1]+i]==0 && guest[contexts[318]+i]==0;expect(neighbors_zero);
        ctx.r3.u64=0x70001400;__imp__XMACreateContext(ctx,guest);expect(ctx.r3.u32==0xc0000017 && load_word(0x70001400)==0);
        std::memset(guest+contexts[0],0x5a,64);ctx.r3.u64=contexts[0];__imp__XMAReleaseContext(ctx,guest);expect(ctx.r3.u32==0);
        ctx.r3.u64=0x70001400;__imp__XMACreateContext(ctx,guest);expect(ctx.r3.u32==0 && load_word(0x70001400)==contexts[0]);
        zero=true;for(unsigned i=0;i<64;++i)zero&=guest[contexts[0]+i]==0;expect(zero);
        expect(!xma_context_pool.release(xma_registers.context_physical,xma_registers.context_physical+1));
        // Kick/Lock through RexGlue XmaContext: a context without a valid output buffer is
        // consumed by one kick (enable flag cleared) and its data stays untouched.
        std::memset(guest+contexts[5],0,64);
        me_xma_store(0x7fea1940,4,__builtin_bswap32(1u<<5));
        expect(!me_xma_bridge_enabled(5));
        bool untouched=true;for(unsigned i=0;i<64;++i)untouched&=guest[contexts[5]+i]==0;expect(untouched);
        me_xma_store(0x7fea1964,4,__builtin_bswap32(1u<<31));expect(!me_xma_bridge_enabled(319));
        me_xma_store(0x7fea1a40,4,__builtin_bswap32(1u<<5));expect(!me_xma_bridge_enabled(5));
        untouched=true;for(unsigned i=0;i<64;++i)untouched&=guest[contexts[5]+i]==0;expect(untouched);
        bool released=true;for(auto address:contexts){ctx.r3.u64=address;__imp__XMAReleaseContext(ctx,guest);released&=ctx.r3.u32==0;}expect(released);
        expect(!xma_context_pool.release(xma_registers.context_physical,xma_registers.context_physical));
        uint64_t value=0;expect(xma_registers.read(0x7fea1800,4,value) && uint32_t(value)==__builtin_bswap32(xma_registers.context_physical));
        gpu_explore_mode=saved;
    }
    {
        uint32_t a=0,b=0;expect(events.create(1,false,a)==0 && events.create(0,true,b)==0);
        put_word(0x70001400,a);put_word(0x70001404,b);put_word(0x70001420,0);put_word(0x70001424,0);
        auto wait=[&](uint32_t count,uint32_t type,uint32_t timeout){ctx.r3.u64=count;ctx.r4.u64=0x70001400;ctx.r5.u64=type;ctx.r6.u64=0;ctx.r7.u64=0;ctx.r8.u64=timeout;__imp__NtWaitForMultipleObjectsEx(ctx,guest);return ctx.r3.u32;};
        expect(wait(2,1,0x70001420)==1 && events.find(b)->signaled);
        events.clear(b);uint32_t previous=0;events.set(a,previous);
        expect(wait(2,0,0x70001420)==0x102 && events.find(a)->signaled);
        events.set(b,previous);expect(wait(2,0,0x70001420)==0 && !events.find(a)->signaled && events.find(b)->signaled);
        events.set(a,previous);put_word(0x70001404,0xdeadbeec);
        expect(wait(2,1,0x70001420)==0xc0000008 && events.find(a)->signaled);
        put_word(0x70001404,b);events.clear(a);events.clear(b);
        put_word(0x70001420,UINT32_MAX);put_word(0x70001424,uint32_t(-50000));
        auto start=std::chrono::steady_clock::now();expect(wait(2,1,0x70001420)==0x102);
        expect(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(5));
        std::thread setter([&]{std::lock_guard<MeExecutionGate> gate(execution_gate);uint32_t old=0;events.set(b,old);state_changed.notify_all();});
        expect(wait(2,1,0)==1);setter.join();expect(!events.find(a)->signaled && events.find(b)->signaled);
        const auto listener=notifications.create(1,0);expect(listener!=0);
        notifications.find(listener)->queue.emplace_back(1,0x55);put_word(0x70001400,b);put_word(0x70001404,listener);
        put_word(0x70001420,0);put_word(0x70001424,0);
        expect(wait(2,0,0x70001420)==0 && notifications.find(listener)->queue.size()==1 && events.find(b)->signaled);
        uint32_t id=0,data=0;expect(notifications.pop(listener,0,id,data) && id==1 && data==0x55 && wait(2,0,0x70001420)==0x102);
        expect(notifications.close(listener));
        expect(events.close(a)==0 && events.close(b)==0);
        std::array<uint32_t,64> many{};bool created=true;
        for(unsigned i=0;i<many.size();++i){created&=events.create(1,i==63,many[i])==0;put_word(0x70001400+i*4,many[i]);}expect(created);
        // Timeout lives outside the 64-element array.
        put_word(0x70001600,0);put_word(0x70001604,0);
        expect(wait(64,1,0x70001600)==63 && !events.find(many[63])->signaled);
        for(auto h:many)events.close(h);
    }
    {
        constexpr uint32_t other=0x12345000;uint32_t a=0,b=0,event=0;
        expect(mutants.create(0,a)==0 && mutants.create(other,b)==0 && events.create(1,true,event)==0);
        auto array=[&](uint32_t first,uint32_t second){put_word(0x70001400,first);put_word(0x70001404,second);};
        put_word(0x70001420,0);put_word(0x70001424,0);
        auto wait=[&](uint32_t type,uint32_t timeout=0x70001420){ctx.r3.u64=2;ctx.r4.u64=0x70001400;ctx.r5.u64=type;ctx.r6.u64=0;ctx.r7.u64=0;ctx.r8.u64=timeout;__imp__NtWaitForMultipleObjectsEx(ctx,guest);return ctx.r3.u32;};
        array(a,b);expect(wait(0)==0x102 && !mutants.find(a)->owner && mutants.find(b)->owner==other);
        expect(mutants.release(b,other)==0);expect(wait(0)==0 && mutants.find(a)->depth==1 && mutants.find(b)->depth==1);
        expect(wait(0)==0 && mutants.find(a)->depth==2 && mutants.find(b)->depth==2);
        for(unsigned i=0;i<2;++i){mutants.release(a,thread_address);mutants.release(b,thread_address);}
        uint32_t status=0;mutants.wait(b,other,status);array(event,b);
        expect(wait(0)==0x102 && events.find(event)->signaled && mutants.find(b)->owner==other);
        mutants.abandon(other);expect(wait(0)==0x80 && !events.find(event)->signaled && mutants.find(b)->owner==thread_address && !mutants.find(b)->abandoned);
        mutants.release(b,thread_address);mutants.wait(b,other,status);mutants.abandon(other);
        expect(wait(1)==0x81 && mutants.find(b)->owner==thread_address);mutants.release(b,thread_address);
        mutants.wait(a,thread_address,status);mutants.find(a)->depth=0x7fffffff;
        uint32_t old=0;events.set(event,old);array(event,a);
        expect(wait(0)==0xc0000047 && events.find(event)->signaled && mutants.find(a)->depth==0x7fffffff);
        mutants.find(a)->depth=1;mutants.release(a,thread_address);
        mutants.wait(b,other,status);array(a,b);uint32_t released=UINT32_MAX;
        std::thread release_owner([&]{std::lock_guard<MeExecutionGate> gate(execution_gate);released=mutants.release(b,other);state_changed.notify_all();});
        expect(wait(0,0)==0);release_owner.join();expect(released==0 && mutants.find(a)->owner==thread_address && mutants.find(b)->owner==thread_address);
        mutants.release(a,thread_address);mutants.release(b,thread_address);
        expect(mutants.close(a)==0 && mutants.close(b)==0 && events.close(event)==0);
    }
    {
        constexpr auto period=MeVblankCadence::period_ns;MeVblankCadence cadence{period};
        expect(!cadence.due(0));expect(!cadence.due(period-1));expect(cadence.due(period) && cadence.next_ns==2*period);
        expect(!cadence.due(period));expect(cadence.due(10*period+5) && cadence.next_ns==11*period);
        expect(!cadence.due(10*period+5));
        MeGpuVblankWorker worker;std::mutex mutex;std::condition_variable changed;unsigned ticks=0;
        expect(worker.start([&]{std::lock_guard lock(mutex);++ticks;changed.notify_one();}));
        expect(!worker.start([]{}));
        {std::unique_lock lock(mutex);expect(changed.wait_for(lock,std::chrono::seconds(2),[&]{return ticks>=2;}));}
        worker.stop_and_join();expect(!worker.failure());expect(!worker.start([]{}));
    }

    {
        // Exercise guest ABI against an isolated host fixture, never real saves.
        expect(save_storage.root.empty());
        char fixture[]="/data/local/tmp/me-content-abi-XXXXXX";
        expect(mkdtemp(fixture)!=nullptr && save_storage.init(fixture));
        const bool old_signed=local_player_signed_in;const auto old_player=selected_local_player;
        local_player_signed_in=true;selected_local_player.xuid=0xe000000000001234ull;
        constexpr uint32_t base=0x70009000,ov=base+512,out=base+560,root=base+600;
        std::memset(guest+base,0,1024);uint32_t event=0;expect(events.create(1,false,event)==0);
        put_word(ov+12,event);ctx.r3.u64=0;ctx.r4.u64=1;ctx.r5.u64=0x200;ctx.r6.u64=0;ctx.r7.u64=out;ctx.r8.u64=ov;
        __imp__XamShowDeviceSelectorUI(ctx,guest);
        expect(ctx.r3.u32==997 && load_word(out)==1 && load_word(ov)==0 && events.find(event)->signaled);
        put_word(base,1);put_word(base+4,1);std::memcpy(guest+base+264,"AbiTest",8);std::memcpy(guest+root,"save",5);
        auto create=[&](uint32_t flags){ctx.r3.u64=0;ctx.r4.u64=root;ctx.r5.u64=base;ctx.r6.u64=flags;ctx.r7.u64=out;ctx.r8.u64=0;ctx.r9.u64=0;__imp__XamContentCreate(ctx,guest);return ctx.r3.u32;};
        expect(create(1)==0 && load_word(out)==1);
        // Build Xbox OBJECT_ATTRIBUTES and ANSI_STRING for NtOpenFile.
        constexpr uint32_t attrs=base+640,name=base+660,path=base+680,iosb=base+720,buf=base+760;
        std::memcpy(guest+path,"save:\\progress.bin",19);
        put_word(attrs,0);put_word(attrs+4,name);put_word(attrs+8,0);put_word(name,(18u<<16)|19);put_word(name+4,path);
        ctx.r3.u64=out;ctx.r4.u64=0x40000000;ctx.r5.u64=attrs;ctx.r6.u64=iosb;
        expect(save_open_guest(ctx,2,0x40) && ctx.r3.u32==0);const auto handle=load_word(out);
        const char payload[]="persisted guest bytes";std::memcpy(guest+buf,payload,sizeof(payload));
        ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;ctx.r7.u64=iosb;ctx.r8.u64=buf;ctx.r9.u64=sizeof(payload);ctx.r10.u64=0;
        __imp__NtWriteFile(ctx,guest);expect(ctx.r3.u32==0 && load_word(iosb+4)==sizeof(payload));
        auto allocation=[&](uint32_t h,uint64_t bytes){save_store64(base+800,bytes);ctx.r3.u64=h;ctx.r4.u64=iosb;ctx.r5.u64=base+800;ctx.r6.u64=8;ctx.r7.u64=19;__imp__NtSetInformationFile(ctx,guest);return ctx.r3.u32;};
        expect(allocation(handle,65536)==0);
        struct stat allocated{};expect(fstat(save_storage.find(handle)->fd,&allocated)==0 && allocated.st_size==sizeof(payload) && allocated.st_blocks*512>=65536);
        expect(allocation(handle,UINT64_MAX)==0xc000000d);
        ctx.r3.u64=handle;ctx.r4.u64=iosb;__imp__NtFlushBuffersFile(ctx,guest);expect(ctx.r3.u32==0);
        ctx.r3.u64=handle;__imp__NtClose(ctx,guest);expect(ctx.r3.u32==0);
        ctx.r3.u64=root;ctx.r4.u64=0;__imp__XamContentClose(ctx,guest);expect(ctx.r3.u32==0);
        expect(create(3)==0 && load_word(out)==2);
        ctx.r3.u64=out;ctx.r4.u64=0x80000000;ctx.r5.u64=attrs;ctx.r6.u64=iosb;ctx.r8.u64=0x40;
        __imp__NtOpenFile(ctx,guest);expect(ctx.r3.u32==0);const auto reopened=load_word(out);
        std::memset(guest+buf,0,sizeof(payload));ctx.r3.u64=reopened;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;ctx.r7.u64=iosb;ctx.r8.u64=buf;ctx.r9.u64=sizeof(payload);ctx.r10.u64=0;
        __imp__NtReadFile(ctx,guest);expect(ctx.r3.u32==0 && std::memcmp(guest+buf,payload,sizeof(payload))==0);
        expect(allocation(reopened,65536)==0xc0000022);
        ctx.r3.u64=reopened;__imp__NtClose(ctx,guest);expect(ctx.r3.u32==0);
        ctx.r3.u64=root;ctx.r4.u64=0;__imp__XamContentClose(ctx,guest);expect(ctx.r3.u32==0);
        expect(create(2)==50); // Existing data must survive unsupported replacement.
        ctx.r3.u64=0;ctx.r4.u64=1;ctx.r5.u64=1;ctx.r6.u64=0;ctx.r7.u64=1;ctx.r8.u64=out;ctx.r9.u64=out+4;
        __imp__XamContentCreateEnumerator(ctx,guest);expect(ctx.r3.u32==0 && load_word(out)==308);const auto enumeration=load_word(out+4);
        ctx.r3.u64=enumeration;ctx.r4.u64=0;ctx.r5.u64=base;ctx.r6.u64=308;ctx.r7.u64=out;ctx.r8.u64=0;
        __imp__XamEnumerate(ctx,guest);expect(ctx.r3.u32==0 && load_word(out)==1 && std::memcmp(guest+base+264,"AbiTest",7)==0);
        ctx.r3.u64=enumeration;__imp__NtClose(ctx,guest);expect(ctx.r3.u32==0);expect(events.close(event)==0);
        std::printf("{\"event\":\"save_abi_fixture\",\"path\":\"%s\"}\n",fixture);
        save_storage.root.clear();save_storage.files.clear();save_storage.mounts.clear();content_enumerators.clear();
        local_player_signed_in=old_signed;selected_local_player=old_player;
    }
    {
        // Guest changes a valid bit/pointer while hardware advances its offset.
        uint32_t word=__builtin_bswap32(0x00000007u);
        me_xma_merge_word(&word,0x00100007u,0x08100007u);
        expect(__builtin_bswap32(word)==0x08000007u);
        word=__builtin_bswap32(0x12345678u);
        me_xma_merge_word(&word,0x11223344u,0x11223344u);
        expect(__builtin_bswap32(word)==0x12345678u);
        word=__builtin_bswap32(0x00300007u);
        me_xma_merge_word(&word,0x00100007u,0x00000007u);
        expect(__builtin_bswap32(word)==0x00200007u);
    }
    std::printf("{\"event\":\"self_test_passed\",\"checks\":%u,\"measured_tick_hz\":%.0f}\n", checks, measured);
    return 0;
}
// Entered from exe_main.cpp (ADB executable) or jni_main.cpp (libmeruntime.so).
int me_runtime_main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // ME_GAME_ROOT: extracted game files (inventory.txt + files); the APK passes its
    // app-accessible copy, the ADB executable keeps /data/local/tmp/me_game_readonly.
    if (const char* game_root = std::getenv("ME_GAME_ROOT")) readonly_files.root = game_root;
    const bool testing = argc == 4 && std::strcmp(argv[3], "--self-test") == 0;
    pool_scan_experiment=argc==4 && std::strcmp(argv[3],"--gpu-explore-pool-scan")==0;
    if(pool_scan_experiment)std::puts("{\"event\":\"pool_scan_compatibility_experiment\",\"enabled\":true,\"original_game_bytes_modified\":false}");
    const bool long_probe=pool_scan_experiment || (argc==4 && std::strcmp(argv[3],"--gpu-explore-long-poll-yield")==0);
    diagnostic_timeout_rounds=pool_scan_experiment?48:(long_probe?24:4);
    // Optional longer game runs: ME_DIAGNOSTIC_SECONDS (240..3600) keeps 5 s snapshots
    // and only moves the bounded exit; the default limits stay unchanged.
    if(const char* seconds=std::getenv("ME_DIAGNOSTIC_SECONDS")){
        const long value=std::strtol(seconds,nullptr,10);
        if(value>=240 && value<=3600)diagnostic_timeout_rounds=sig_atomic_t(value/5);
        if(value==0)diagnostic_timeout_rounds=sig_atomic_t(0x7fffffff); // 0 = no bounded exit (APK play)
        std::printf("{\"event\":\"diagnostic_timeout\",\"seconds\":%d}\n",int(diagnostic_timeout_rounds)*5);
    }
    polling_yield_experiment=long_probe || (argc==4 && std::strcmp(argv[3],"--gpu-explore-poll-yield")==0);
    gpu_explore_mode = polling_yield_experiment || (argc == 4 && std::strcmp(argv[3], "--gpu-explore") == 0);
    const bool mmio_unbound_test = argc == 4 && std::strcmp(argv[3], "--mmio-unbound-test") == 0;
    if (argc != 2 && argc != 3 && !testing && !mmio_unbound_test && !gpu_explore_mode) { std::fprintf(stderr, "Usage: me_entry_probe private-sections.bin [original-xex-headers.bin] [--self-test|--mmio-unbound-test]\n"); return 2; }
    FILE* file = std::fopen(argv[1], "rb");
    if (!file) { std::perror("bundle"); return 2; }
    char magic[8]; read_exact(file, magic, 8);
    if (std::memcmp(magic, "MEBOOT01", 8)) return 2;
    page_size = size_t(sysconf(_SC_PAGESIZE));
    me_gate_label = gate_label;
    if (const char* serial = std::getenv("ME_GUEST_SERIAL"); serial && !std::strcmp(serial, "1")) guest_parallel = false;
    execution_gate.set_barging(guest_parallel && !testing); // short kernel lock in parallel mode
    guest = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (guest == MAP_FAILED) { std::perror("reserve guest space"); return 2; }
    unsigned sections = read32(file);
    if (!sections || sections > 32) return 2;
    for (unsigned i = 0; i < sections; ++i) {
        uint32_t address = read32(file), size = read32(file);
        if (address < 0x82000000 || uint64_t(address) + size > 0x83170000 || !size) return 2;
        writable(address, size);
        read_exact(file, guest + address, size);
        image_ranges[image_range_count++] = {address, size};
    }
    guard_count = read32(file);
    if (guard_count > 128) return 2;
    for (unsigned i = 0; i < guard_count; ++i) {
        auto& g = guards[i];
        g.iat = read32(file); g.address = read32(file); g.ordinal = read32(file);
        read_exact(file, g.name, sizeof(g.name)); g.name[95] = 0;
        if (g.iat < 0x82000000 || g.iat > 0x821D1400 - 4 ||
            g.address != 0x50000000u + i * 0x1000u) return 2;
        uint32_t encoded = __builtin_bswap32(g.address);
        std::memcpy(guest + g.iat, &encoded, 4);
    }
    std::fclose(file);
    if (argc >= 3) install_module(argv[2]);
    writable(uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE), PPC_CODE_SIZE * 2 + 16);
    unsigned mappings = 0;
    for (auto* p = PPCFuncMappings; p->host; ++p) {
        if (p->guest < PPC_CODE_BASE || p->guest >= PPC_CODE_BASE + PPC_CODE_SIZE) return 2;
        PPC_LOOKUP_FUNC(guest, p->guest) = p->host;
        ++mappings;
    }
    install_missing_jump_cases();
    if(polling_yield_experiment){original_stream_poll=PPC_LOOKUP_FUNC(guest,0x8238e850);PPC_LOOKUP_FUNC(guest,0x8238e850)=diagnostic_stream_poll;original_worker_poll=PPC_LOOKUP_FUNC(guest,0x826ccc08);PPC_LOOKUP_FUNC(guest,0x826ccc08)=diagnostic_worker_poll;}
    writable(0x70000000, 0x100000);
    physical_memory.map = map_physical;
    virtual_memory.commit_pages = commit_pages;
    virtual_memory.discard_pages = [](uint32_t address, uint32_t length) {
        if (address % page_size || length % page_size) return false;
        return mmap(guest + address, length, PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == guest + address;
    };
    alignas(64) PPCContext context{};
    active_context = &context;
    register_diagnostic_thread();
    context.r1.u64 = 0x700FF000;
    initialize_thread(context, 0, 0x100000);
    std::unique_lock<MeExecutionGate> run(execution_gate);
    if (mmio_unbound_test) {
        context.r3.u64 = 0x7fc80000;
        sub_82230088(context, guest); // Test-only destination; must exit 95 before touching PROT_NONE.
        return 1;
    }
    if (testing) return self_test(context);
    if(gpu_explore_mode){
        timestamp_bundle_address=physical_memory.allocate(24,4,0,0xffffffff,4096);
        if(!timestamp_bundle_address)return 2;
        std::memset(guest+timestamp_bundle_address,0,24);
        timespec origin{};clock_gettime(CLOCK_MONOTONIC,&origin);
        timestamp_origin_ns=uint64_t(origin.tv_sec)*1000000000ull+uint64_t(origin.tv_nsec);
        for(unsigned i=0;i<guard_count;++i)if(std::strcmp(guards[i].name,"KeTimeStampBundle")==0)put_word(guards[i].iat,timestamp_bundle_address);
        std::printf("{\"event\":\"timestamp_bundle_installed\",\"address\":\"%08x\",\"update_on_scalar_read\":true}\n",timestamp_bundle_address);
    }
    context.lr = 0;
    context.fpscr.csr = context.fpscr.getcsr();
    struct sigaction action{};
    action.sa_sigaction = fault; action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr); sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr); sigaction(SIGTRAP, &action, nullptr); sigaction(SIGALRM, &action, nullptr); sigaction(SIGUSR1, &action, nullptr);
    alarm(5);
    std::printf("{\"event\":\"entry_begin\",\"entry\":\"829fab98\",\"mappings\":%u,\"sections\":%u,\"page_size\":%zu}\n", mappings, sections, page_size);
    if (guest_parallel) run.unlock();
    _xstart(context, guest);
    std::printf("{\"event\":\"entry_returned\",\"r3\":%u}\n", context.r3.u32);
    return 44; // Unexpected return is not game startup success.
}
