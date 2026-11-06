/* Differential test: original x86 function vs. its lifted C version, in one 32-bit process.

   The game image is mapped at its own ImageBase, so guest addresses are host addresses
   (X86_MEM_BASE = 0). Every case starts both runs from the same registers, stack, scratch
   memory and .data; afterwards registers, scratch memory and .data must be identical. A run
   that faults or hangs is abandoned; then the other run must fault or hang as well.

   Usage: harness.exe <MirrorsEdge.exe> <cases per function> [report.txt] */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/x86rt.h"

/* flags: KNOWN_FSTSW the function reads the x87 status word (exception flags are not modelled),
   KNOWN_APPROX it uses rsqrtss/rcpss (hardware approximation vs. exact value),
   KNOWN_TRIG it uses x87 transcendentals (libm vs. x87 microcode in the last bits),
   KNOWN_NOCALL the entry is named only by a relocation (an __except block, a handler, the middle
   of a function): it is not entered by call, so its ret does not go back to the harness,
   KNOWN_MMX it uses MMX (MMX registers alias the x87 ones; only the tags are modelled),
   KNOWN_X87 it does x87 arithmetic (results are held as double: narrower exponent range than
   the x87, and at 24-bit precision a double rounding). */
enum { KNOWN_FSTSW = 1, KNOWN_APPROX = 2, KNOWN_TRIG = 4, KNOWN_NOCALL = 8, KNOWN_MMX = 16, KNOWN_X87 = 32 };
typedef struct { uint32_t entry; X86Fn fn; uint32_t flags; } TestFn;
extern const TestFn lifted_tests[];
extern const int lifted_tests_count;

/* Layout shared with trampoline.S. */
typedef struct {
    uint32_t r[8];
    uint32_t fcw, mxcsr, pad[2];
    X86Xmm xmm[8];
    uint8_t fsave[108];
} NativeState;

int run_native(uint32_t target, NativeState *st);
int run_lifted(X86Fn fn, X86Ctx *ctx);
void native_ret(void);
void fault_landing(void);

/* At 0x40000000 a pointer read as the high half of a double or as a float is a value of about
   2..4, so random data gives realistic magnitudes instead of 1e-77 (x87 keeps a wider exponent
   range than double, which would otherwise dominate the floating-point differences). */
#define SCRATCH_VA   0x40000000u
#define SCRATCH_SIZE 0x100000u  /* 1 MB: data area + stack at the top */
#define STACK_TOP    (SCRATCH_VA + SCRATCH_SIZE - 0x1000u)
#define DATA_AREA    0xC0000u   /* pointers are drawn from the first 768 KB */

static uint32_t data_va, data_size;
static uint8_t *snap_scratch, *snap_data, *res_scratch, *res_data;
static volatile LONG in_run, run_seq, timed_out;
static volatile DWORD fault_code, fault_eip, fault_addr, fault_rw;
static uint32_t text_va, text_end;
static HANDLE test_thread;

void x86_dispatch(X86Ctx *ctx, uint32_t t) { (void)ctx; (void)t; fault_code = 0xD15; fault_landing(); for (;;) {} }
void x86_trap(X86Ctx *ctx, uint32_t a, const char *w) { (void)ctx; (void)a; (void)w; fault_code = 0x7A9; fault_landing(); for (;;) {} }
void rt_ue_script_call(X86Ctx *ctx) { (void)ctx; }
uint64_t x86_rdtsc(X86Ctx *ctx) { (void)ctx; return 0; }
void x86_lock_enter(void) {}
void x86_lock_leave(void) {}
/* EH resume points are never reached in single-function runs. */
void x86_resume_push(X86Ctx *ctx, X86Resume *r, const uint32_t *t, uint32_t n) { (void)ctx; (void)r; (void)t; (void)n; }
void x86_resume_pop(X86Ctx *ctx, X86Resume *r) { (void)ctx; (void)r; }

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    if (!in_run)
        return EXCEPTION_CONTINUE_SEARCH;
    fault_code = ep->ExceptionRecord->ExceptionCode;
    fault_eip = ep->ContextRecord->Eip;
    fault_addr = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    fault_rw = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[0] : 0;
    ep->ContextRecord->Eip = (DWORD)(uintptr_t)fault_landing;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Abandons a run that takes longer than 200 ms (an endless loop on random inputs). */
static DWORD WINAPI watchdog(void *arg)
{
    (void)arg;
    LONG last = -1;
    int ticks = 0;
    for (;;) {
        Sleep(50);
        LONG s = run_seq;
        if (!in_run || s != last) { last = s; ticks = 0; continue; }
        if (++ticks < 4) continue;
        SuspendThread(test_thread);
        if (in_run && run_seq == s) {
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(test_thread, &c);
            c.Eip = (DWORD)(uintptr_t)fault_landing;
            SetThreadContext(test_thread, &c);
            timed_out = 1;
        }
        ResumeThread(test_thread);
        ticks = 0;
    }
    return 0;
}

static uint32_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* A float with an ordinary value (most of them) or a special one. */
static uint32_t rnd_float(void)
{
    uint32_t k = rnd() % 16;
    float f;
    if (k == 0) return 0;
    if (k == 1) return 0x80000000u;          /* -0 */
    if (k == 2) return 0x3F800000u;          /* 1 */
    if (k == 3) return 0x7FC00000u;          /* NaN */
    f = (float)((int32_t)(rnd() % 20001) - 10000) / (float)(1 + rnd() % 256);
    return f2u(f);
}

/* A value that is a pointer into the scratch area, a small number, zero, a float, or an address
   that always faults (the upper half is never mapped in a 32-bit process without LAA). */
static uint32_t rnd_value(void)
{
    uint32_t k = rnd() % 10;
    if (k < 4) return SCRATCH_VA + (rnd() % (DATA_AREA / 4)) * 4;
    if (k < 6) return rnd() % 64;
    if (k < 7) return 0;
    if (k < 9) return rnd_float();
    return 0x80000000u | rnd();
}

static int load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    uint32_t base = nt->OptionalHeader.ImageBase, size = nt->OptionalHeader.SizeOfImage;
    /* The launcher reserved this range before the process initialized (see spawn_child). */
    uint8_t *img = VirtualAlloc((void *)(uintptr_t)base, size, MEM_COMMIT, PAGE_READWRITE);
    if ((uintptr_t)img != base) { printf("cannot map image at %#x\n", base); return 0; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(img + s[i].VirtualAddress, file + s[i].PointerToRawData,
               s[i].SizeOfRawData < s[i].Misc.VirtualSize ? s[i].SizeOfRawData : s[i].Misc.VirtualSize);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD old, prot = (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ? PAGE_EXECUTE_READ
                        : (s[i].Characteristics & IMAGE_SCN_MEM_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        VirtualProtect(img + s[i].VirtualAddress, s[i].Misc.VirtualSize, prot, &old);
        if (!memcmp(s[i].Name, ".data", 6)) { data_va = base + s[i].VirtualAddress; data_size = s[i].Misc.VirtualSize; }
        if (!memcmp(s[i].Name, ".text", 6)) { text_va = base + s[i].VirtualAddress; text_end = text_va + s[i].Misc.VirtualSize; }
    }
    free(file);
    return data_va != 0;
}

typedef struct {
    uint32_t r[8];
    int faulted, hung;
    DWORD code, eip, addr, rw;
    uint32_t ftop, ftag, fcc, fcw, mxcsr;
    double st[8];
    X86Xmm xmm[8];
} Outcome;

/* Even cases run the FPU at 53-bit precision, odd ones at 24-bit (what Direct3D 9 selects). */
static void prepare(uint32_t seed, int c, NativeState *st)
{
    rng = seed * 2654435761u + 1;
    uint32_t *sc = (uint32_t *)(uintptr_t)SCRATCH_VA;
    for (uint32_t i = 0; i < SCRATCH_SIZE / 4; i++) sc[i] = rnd_value();
    memset(st, 0, sizeof *st);
    for (int i = 0; i < 8; i++) st->r[i] = rnd_value();
    st->r[4] = STACK_TOP - 4;                          /* esp: return address slot */
    st->r[5] = STACK_TOP + 0x100 - (rnd() % 16) * 4;   /* ebp above the stack top (frame of a caller) */
    *(uint32_t *)(uintptr_t)(STACK_TOP - 4) = (uint32_t)(uintptr_t)native_ret;
    st->fcw = (c & 1) ? 0x007F : 0x027F;
    st->mxcsr = 0x1F80;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) st->xmm[i].u[k] = rnd_float();
}

/* x87 state of the native run from its fnsave image, in the lifted layout (physical registers). */
static void native_fp(const NativeState *st, Outcome *o)
{
    uint16_t fcw, fsw, ftw;
    memcpy(&fcw, st->fsave, 2);
    memcpy(&fsw, st->fsave + 4, 2);
    memcpy(&ftw, st->fsave + 8, 2);
    o->fcw = fcw;
    o->ftop = (fsw >> 11) & 7;
    o->fcc = fsw & 0x4700u;
    o->ftag = 0;
    for (int phys = 0; phys < 8; phys++) {
        if (((ftw >> (2 * phys)) & 3) == 3) { o->st[phys] = 0; continue; }
        o->ftag |= 1u << phys;
        int i = (phys - (int)o->ftop) & 7; /* fnsave stores ST(0)..ST(7) */
        o->st[phys] = x87_ld80((uint32_t)(uintptr_t)(st->fsave + 28 + 10 * i));
    }
    o->mxcsr = st->mxcsr;
    memcpy(o->xmm, st->xmm, sizeof o->xmm);
}

static int same_double(double a, double b)
{
    if (a != a && b != b) return 1;
    return !memcmp(&a, &b, 8);
}

static int both_nan(uint32_t a, uint32_t b)
{
    return (a & 0x7F800000u) == 0x7F800000u && (a & 0x7FFFFF) && (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFF);
}

/* Normal double range: values outside it are held differently by the x87 (wider exponent). */
static int in_double_range(double v)
{
    double m = __builtin_fabs(v);
    return v != v || m == 0 || (m >= 2.2250738585072014e-308 && m <= 1e300);
}

static int close_rel(double a, double b, double tol)
{
    if (a == b) return 1;
    double m = __builtin_fabs(a) > __builtin_fabs(b) ? __builtin_fabs(a) : __builtin_fabs(b);
    return __builtin_fabs(a - b) <= tol * m;
}

/* Is the first difference one of the documented, expected kinds for this function? */
static const char *known_kind(uint32_t flags, const Outcome *a, const Outcome *b, const char *why)
{
    int native_only_fault = a->faulted && !b->faulted && !b->hung && a->code == 0xC0000005;
    if (native_only_fault && (flags & KNOWN_NOCALL) && (a->eip < text_va || a->eip >= text_end))
        return "entered not by call: ret left the harness";
    /* Random inputs can make a function write past a stack array over its own return address;
       the original then returns into garbage, the lifted code does not read the return address. */
    if (native_only_fault && a->eip == a->addr && (a->eip < text_va || a->eip >= text_end))
        return "return address overwritten by random input";
    /* A read fault inside the game's code that the optimized C does not repeat: the loaded value
       is unused and the compiler dropped the load (CHECKLIST). */
    if (native_only_fault && a->rw == 0 && a->eip >= text_va && a->eip < text_end)
        return "dead load";
    /* A write that changes nothing (x += 0) is dropped together with its load; a real lost write
       would show as a memory difference in the cases that do not fault. */
    if (native_only_fault && a->rw == 1 && a->eip >= text_va && a->eip < text_end)
        return "no-op store optimized away";
    if ((flags & KNOWN_FSTSW) && !strncmp(why, "eax ", 4) && !((a->r[0] ^ b->r[0]) & ~0xFFu))
        return "x87 exception flags";
    if ((flags & KNOWN_MMX) && !strncmp(why, "x87 ST(", 7))
        return "MMX/x87 register aliasing";
    if (flags & KNOWN_X87) {
        unsigned x, y, addr;
        if (!strncmp(why, "x87 ST(", 7)) {
            for (int p = 0; p < 8; p++)
                if ((a->ftag >> p & 1) && !same_double(a->st[p], b->st[p])) {
                    if (!in_double_range(a->st[p]) || !in_double_range(b->st[p])) return "x87 exponent range";
                    if (close_rel(a->st[p], b->st[p], 1.0 / (1 << 22))) return "x87 double rounding";
                    break;
                }
        } else if (sscanf(why, "memory at %x native=%x lifted=%x", &addr, &x, &y) == 3) {
            /* The differing dword is the low or the high half of a double, or a float. */
            for (int off = -4; off <= 0; off += 4) {
                double dn, dl;
                memcpy(&dn, res_scratch + (addr - SCRATCH_VA) + off, 8);
                memcpy(&dl, (void *)(uintptr_t)(addr + off), 8);
                if (dn != 0 && !in_double_range(dn)) return "x87 exponent range";
                if (dn != 0 && close_rel(dn, dl, 1.0 / (1 << 22))) return "x87 double rounding";
            }
            if (close_rel(u2f(x), u2f(y), 1.0 / (1 << 22))) return "x87 double rounding";
        }
    }
    if (flags & (KNOWN_APPROX | KNOWN_TRIG)) {
        double tol = (flags & KNOWN_APPROX) ? 1.0 / 1024 : 1.0 / (1 << 20);
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 4; k++)
                if (a->xmm[i].u[k] != b->xmm[i].u[k])
                    return close_rel(a->xmm[i].f[k], b->xmm[i].f[k], tol) ? "approximation" : NULL;
        for (int p = 0; p < 8; p++)
            if ((a->ftag >> p & 1) && a->st[p] != b->st[p])
                return close_rel(a->st[p], b->st[p], tol) ? "approximation" : NULL;
        unsigned x, y;
        if (sscanf(why, "memory at %*x native=%x lifted=%x", &x, &y) == 2) {
            if ((flags & KNOWN_TRIG) && (x > y ? x - y : y - x) <= 64) return "last bits of a double";
            if (close_rel(u2f(x), u2f(y), tol)) return "approximation";
        }
    }
    return NULL;
}

/* Returns a description of the first difference in the FP state, or NULL. */
static const char *fp_diff(const Outcome *a, const Outcome *b, char *buf, size_t n)
{
    if (a->ftop != b->ftop) { snprintf(buf, n, "x87 top native=%u lifted=%u", a->ftop, b->ftop); return buf; }
    if (a->ftag != b->ftag) { snprintf(buf, n, "x87 tags native=%#x lifted=%#x", a->ftag, b->ftag); return buf; }
    for (int p = 0; p < 8; p++)
        if ((a->ftag >> p & 1) && !same_double(a->st[p], b->st[p])) {
            snprintf(buf, n, "x87 ST(%d) native=%.17g lifted=%.17g", (p - (int)a->ftop) & 7, a->st[p], b->st[p]);
            return buf;
        }
    /* C1 (0x200) is not modelled: it reports round-up and stack faults. */
    if ((a->fcc ^ b->fcc) & 0x4500u) { snprintf(buf, n, "x87 C0/C2/C3 native=%#x lifted=%#x", a->fcc, b->fcc); return buf; }
    if ((a->fcw & 0x1F3F) != (b->fcw & 0x1F3F)) { snprintf(buf, n, "fcw native=%#x lifted=%#x", a->fcw, b->fcw); return buf; }
    if ((a->mxcsr & ~0x3Fu) != (b->mxcsr & ~0x3Fu)) { snprintf(buf, n, "mxcsr native=%#x lifted=%#x", a->mxcsr, b->mxcsr); return buf; }
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++)
            if (a->xmm[i].u[k] != b->xmm[i].u[k] && !both_nan(a->xmm[i].u[k], b->xmm[i].u[k])) {
                snprintf(buf, n, "xmm%d[%d] native=%#x (%g) lifted=%#x (%g)", i, k, a->xmm[i].u[k], a->xmm[i].f[k],
                         b->xmm[i].u[k], b->xmm[i].f[k]);
                return buf;
            }
    return NULL;
}

/* The harness's own snapshot buffers. A test function can compute a wild address that lands in
   them; they are read-only while a function runs, so such a write faults in both runs alike
   instead of silently corrupting a snapshot. */
static void *buffer(size_t n)
{
    return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static void guard_buffers(int on)
{
    DWORD old, prot = on ? PAGE_READONLY : PAGE_READWRITE;
    VirtualProtect(snap_scratch, SCRATCH_SIZE, prot, &old);
    VirtualProtect(res_scratch, SCRATCH_SIZE, prot, &old);
    VirtualProtect(snap_data, data_size, prot, &old);
    VirtualProtect(res_data, data_size, prot, &old);
}

static void finish(Outcome *o, int r)
{
    o->faulted = r && !timed_out;
    o->hung = timed_out;
    o->code = fault_code;
    o->eip = fault_eip;
    o->addr = fault_addr;
    o->rw = fault_rw;
}

/* Low addresses of a 32-bit process fill up with heaps, stacks and mapped files during start-up,
   so the image range is reserved in a suspended copy of this process before it initializes. */
static int spawn_child(const char *game_exe)
{
    FILE *f = fopen(game_exe, "rb");
    if (!f) { printf("cannot open %s\n", game_exe); return 2; }
    uint8_t hdr[4096];
    fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(hdr + ((IMAGE_DOS_HEADER *)hdr)->e_lfanew);
    char *cmd = malloc(strlen(GetCommandLineA()) + 16);
    sprintf(cmd, "%s --child", GetCommandLineA());
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 2;
    }
    void *a = VirtualAllocEx(pi.hProcess, (void *)(uintptr_t)nt->OptionalHeader.ImageBase,
                             nt->OptionalHeader.SizeOfImage, MEM_RESERVE, PAGE_NOACCESS);
    void *b = VirtualAllocEx(pi.hProcess, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!a || !b) {
        printf("cannot reserve image/scratch in child (%p %p, error %lu)\n", a, b, GetLastError());
        TerminateProcess(pi.hProcess, 2);
        return 2;
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[argc - 1], "--child")) return spawn_child(argv[1]);
    if (argc >= 3) argc--;
    if (argc < 3) { printf("usage: harness <MirrorsEdge.exe> <cases> [report]\n"); return 2; }
    int cases = atoi(argv[2]);
    FILE *rep = fopen(argc > 3 ? argv[3] : "harness-report.txt", "w");
    if (!load_image(argv[1])) return 2;
    if (VirtualAlloc((void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE, MEM_COMMIT, PAGE_READWRITE) !=
        (void *)(uintptr_t)SCRATCH_VA) { printf("cannot map scratch\n"); return 2; }
    snap_scratch = buffer(SCRATCH_SIZE); res_scratch = buffer(SCRATCH_SIZE);
    snap_data = buffer(data_size); res_data = buffer(data_size);
    memcpy(snap_data, (void *)(uintptr_t)data_va, data_size);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &test_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    AddVectoredExceptionHandler(1, on_exception);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    long agree = 0, both_fault = 0, both_hang = 0, mismatch = 0, known = 0;
    int bad_fns = 0, tested_fns = 0, clean_fns = 0;
    for (int t = 0; t < lifted_tests_count; t++) {
        int fn_bad = 0, fn_ok = 0;
        for (int c = 0; c < cases && fn_bad < 3; c++) {
            static NativeState start_state, nat;
            uint32_t *start = start_state.r;
            memcpy((void *)(uintptr_t)data_va, snap_data, data_size);
            prepare(lifted_tests[t].entry ^ (uint32_t)c * 0x9E3779B9u, c, &start_state);
            memcpy(snap_scratch, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE);

            Outcome a = {0}, b = {0};
            nat = start_state;
            fault_code = fault_eip = fault_addr = fault_rw = 0; timed_out = 0; InterlockedIncrement(&run_seq); in_run = 1;
            guard_buffers(1);
            int r = run_native(lifted_tests[t].entry, &nat);
            in_run = 0;
            guard_buffers(0);
            finish(&a, r);
            memcpy(a.r, nat.r, sizeof a.r);
            if (!r) native_fp(&nat, &a);
            memcpy(res_scratch, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE);
            memcpy(res_data, (void *)(uintptr_t)data_va, data_size);

            memcpy((void *)(uintptr_t)SCRATCH_VA, snap_scratch, SCRATCH_SIZE);
            memcpy((void *)(uintptr_t)data_va, snap_data, data_size);
            static X86Ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            memcpy(&ctx, start, 32);
            x87_init(&ctx);
            ctx.fcw = start_state.fcw;
            ctx.mxcsr = start_state.mxcsr;
            memcpy(ctx.xmm, start_state.xmm, sizeof ctx.xmm);
            fault_code = fault_eip = fault_addr = fault_rw = 0; timed_out = 0; InterlockedIncrement(&run_seq); in_run = 1;
            guard_buffers(1);
            r = run_lifted(lifted_tests[t].fn, &ctx);
            in_run = 0;
            guard_buffers(0);
            finish(&b, r);
            memcpy(b.r, &ctx, sizeof b.r);
            b.ftop = ctx.ftop; b.ftag = ctx.ftag; b.fcc = ctx.fsw & 0x4700u; b.fcw = ctx.fcw; b.mxcsr = ctx.mxcsr;
            memcpy(b.st, ctx.st, sizeof b.st);
            memcpy(b.xmm, ctx.xmm, sizeof b.xmm);

            if (getenv("HARNESS_DEBUG"))
                fprintf(rep, "f_%08X case %d in  %08x %08x %08x %08x %08x %08x %08x %08x\n"
                             "  native %d/%d %08x %08x %08x %08x %08x %08x %08x %08x\n"
                             "  lifted %d/%d %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        lifted_tests[t].entry, c, start[0], start[1], start[2], start[3], start[4], start[5],
                        start[6], start[7], a.faulted, a.hung, a.r[0], a.r[1], a.r[2], a.r[3], a.r[4], a.r[5],
                        a.r[6], a.r[7], b.faulted, b.hung, b.r[0], b.r[1], b.r[2], b.r[3], b.r[4], b.r[5],
                        b.r[6], b.r[7]);
            const char *why = NULL;
            char buf[160];
            if (a.hung || b.hung) {
                if (a.hung && b.hung) { both_hang++; continue; }
                why = a.hung ? "native hung, lifted did not" : "lifted hung, native did not";
            } else if (a.faulted || b.faulted) {
                if (a.faulted && b.faulted) { both_fault++; continue; }
                snprintf(buf, sizeof buf, "fault mismatch native=%d(%#lx at eip %#lx, addr %#lx) lifted=%d(%#lx addr %#lx)",
                         a.faulted, (unsigned long)a.code, (unsigned long)a.eip, (unsigned long)a.addr, b.faulted,
                         (unsigned long)b.code, (unsigned long)b.addr);
                why = buf;
            } else {
                static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
                for (int i = 0; i < 8 && !why; i++)
                    if (a.r[i] != b.r[i]) {
                        snprintf(buf, sizeof buf, "%s native=%#x lifted=%#x", rn[i], a.r[i], b.r[i]);
                        why = buf;
                    }
                if (!why)
                    why = fp_diff(&a, &b, buf, sizeof buf);
                if (!why && memcmp(res_scratch, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE)) {
                    uint32_t i = 0;
                    while (res_scratch[i] == ((uint8_t *)(uintptr_t)SCRATCH_VA)[i]) i++;
                    i &= ~3u;
                    snprintf(buf, sizeof buf, "memory at %#x native=%#x lifted=%#x", SCRATCH_VA + i,
                             *(uint32_t *)(res_scratch + i), *(uint32_t *)(uintptr_t)(SCRATCH_VA + i));
                    if (getenv("HARNESS_DEBUG"))
                        for (uint32_t j = 0; j < SCRATCH_SIZE; j += 4) {
                            uint32_t nv = *(uint32_t *)(res_scratch + j), lv = *(uint32_t *)(uintptr_t)(SCRATCH_VA + j),
                                     sv = *(uint32_t *)(snap_scratch + j);
                            if (nv != lv || nv != sv)
                                fprintf(rep, "  %#x start=%08x native=%08x lifted=%08x\n", SCRATCH_VA + j, sv, nv, lv);
                        }
                    why = buf;
                }
                if (!why && memcmp(res_data, (void *)(uintptr_t)data_va, data_size)) {
                    uint32_t i = 0;
                    while (res_data[i] == ((uint8_t *)(uintptr_t)data_va)[i]) i++;
                    i &= ~3u;
                    snprintf(buf, sizeof buf, ".data at %#x native=%#x lifted=%#x", data_va + i,
                             *(uint32_t *)(res_data + i), *(uint32_t *)(uintptr_t)(data_va + i));
                    why = buf;
                }
            }
            const char *kk = why ? known_kind(lifted_tests[t].flags, &a, &b, why) : NULL;
            if (kk) {
                known++;
                fprintf(rep, "KNOWN f_%08X case %d (%s): %s\n", lifted_tests[t].entry, c, kk, why);
            } else if (why) {
                mismatch++;
                fn_bad++;
                fprintf(rep, "MISMATCH f_%08X case %d: %s\n", lifted_tests[t].entry, c, why);
            } else {
                agree++;
                fn_ok++;
            }
        }
        tested_fns++;
        if (fn_bad) bad_fns++;
        else if (fn_ok) clean_fns++;
        if ((t + 1) % 500 == 0) { printf("%d/%d functions\n", t + 1, lifted_tests_count); fflush(stdout); }
    }
    fprintf(rep, "functions %d, with compared cases and no mismatch %d, with mismatch %d\n", tested_fns, clean_fns, bad_fns);
    fprintf(rep, "cases: agree %ld, both fault %ld, both hang %ld, known differences %ld, mismatch %ld\n", agree,
            both_fault, both_hang, known, mismatch);
    fclose(rep);
    printf("functions %d, clean (compared, no mismatch) %d, with mismatch %d\n", tested_fns, clean_fns, bad_fns);
    printf("cases: agree %ld, both fault %ld, both hang %ld, known differences %ld, mismatch %ld\n", agree, both_fault,
           both_hang, known, mismatch);
    fflush(stdout);
    ExitProcess(mismatch ? 1 : 0);
}
