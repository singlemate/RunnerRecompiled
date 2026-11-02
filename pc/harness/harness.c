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

typedef struct { uint32_t entry; X86Fn fn; } TestFn;
extern const TestFn lifted_tests[];
extern const int lifted_tests_count;

int run_native(uint32_t target, uint32_t *regs);
int run_lifted(X86Fn fn, X86Ctx *ctx);
void native_ret(void);
void fault_landing(void);

#define SCRATCH_VA   0x30000000u
#define SCRATCH_SIZE 0x100000u  /* 1 MB: data area + stack at the top */
#define STACK_TOP    (SCRATCH_VA + SCRATCH_SIZE - 0x1000u)
#define DATA_AREA    0xC0000u   /* pointers are drawn from the first 768 KB */

static uint32_t data_va, data_size;
static uint8_t *snap_scratch, *snap_data, *res_scratch, *res_data;
static volatile LONG in_run, run_seq, timed_out;
static volatile DWORD fault_code, fault_eip, fault_addr;
static HANDLE test_thread;

void x86_dispatch(X86Ctx *ctx, uint32_t t) { (void)ctx; (void)t; fault_code = 0xD15; fault_landing(); for (;;) {} }
void x86_import(X86Ctx *ctx, uint32_t s) { (void)ctx; (void)s; fault_code = 0x1A7; fault_landing(); for (;;) {} }
void x86_trap(X86Ctx *ctx, uint32_t a, const char *w) { (void)ctx; (void)a; (void)w; fault_code = 0x7A9; fault_landing(); for (;;) {} }

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    if (!in_run)
        return EXCEPTION_CONTINUE_SEARCH;
    fault_code = ep->ExceptionRecord->ExceptionCode;
    fault_eip = ep->ContextRecord->Eip;
    fault_addr = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
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

/* A value that is a pointer into the scratch area, a small number, zero, or an address that
   always faults (the upper half is never mapped in a 32-bit process without LAA). */
static uint32_t rnd_value(void)
{
    uint32_t k = rnd() % 8;
    if (k < 4) return SCRATCH_VA + (rnd() % (DATA_AREA / 4)) * 4;
    if (k < 6) return rnd() % 64;
    if (k < 7) return 0;
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
    }
    free(file);
    return data_va != 0;
}

typedef struct { uint32_t r[8]; int faulted, hung; DWORD code, eip, addr; } Outcome;

static void prepare(uint32_t seed, uint32_t regs[8])
{
    rng = seed * 2654435761u + 1;
    uint32_t *sc = (uint32_t *)(uintptr_t)SCRATCH_VA;
    for (uint32_t i = 0; i < SCRATCH_SIZE / 4; i++) sc[i] = rnd_value();
    for (int i = 0; i < 8; i++) regs[i] = rnd_value();
    regs[4] = STACK_TOP - 4;                         /* esp: return address slot */
    regs[5] = STACK_TOP + 0x100 - (rnd() % 16) * 4;   /* ebp above the stack top (frame of a caller) */
    *(uint32_t *)(uintptr_t)(STACK_TOP - 4) = (uint32_t)(uintptr_t)native_ret;
}

static void finish(Outcome *o, int r)
{
    o->faulted = r && !timed_out;
    o->hung = timed_out;
    o->code = fault_code;
    o->eip = fault_eip;
    o->addr = fault_addr;
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
    snap_scratch = malloc(SCRATCH_SIZE); res_scratch = malloc(SCRATCH_SIZE);
    snap_data = malloc(data_size); res_data = malloc(data_size);
    memcpy(snap_data, (void *)(uintptr_t)data_va, data_size);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &test_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    AddVectoredExceptionHandler(1, on_exception);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    long agree = 0, both_fault = 0, both_hang = 0, mismatch = 0;
    int bad_fns = 0, tested_fns = 0, clean_fns = 0;
    for (int t = 0; t < lifted_tests_count; t++) {
        int fn_bad = 0, fn_ok = 0;
        for (int c = 0; c < cases && fn_bad < 3; c++) {
            uint32_t regs[8], start[8];
            memcpy((void *)(uintptr_t)data_va, snap_data, data_size);
            prepare(lifted_tests[t].entry ^ (uint32_t)c * 0x9E3779B9u, start);
            memcpy(snap_scratch, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE);

            Outcome a = {0}, b = {0};
            memcpy(regs, start, sizeof regs);
            fault_code = fault_eip = fault_addr = 0; timed_out = 0; InterlockedIncrement(&run_seq); in_run = 1;
            int r = run_native(lifted_tests[t].entry, regs);
            in_run = 0;
            finish(&a, r);
            memcpy(a.r, regs, sizeof regs);
            memcpy(res_scratch, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE);
            memcpy(res_data, (void *)(uintptr_t)data_va, data_size);

            memcpy((void *)(uintptr_t)SCRATCH_VA, snap_scratch, SCRATCH_SIZE);
            memcpy((void *)(uintptr_t)data_va, snap_data, data_size);
            X86Ctx ctx = {start[0], start[1], start[2], start[3], start[4], start[5], start[6], start[7], 0};
            fault_code = fault_eip = fault_addr = 0; timed_out = 0; InterlockedIncrement(&run_seq); in_run = 1;
            r = run_lifted(lifted_tests[t].fn, &ctx);
            in_run = 0;
            finish(&b, r);
            memcpy(b.r, &ctx, sizeof b.r);

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
                if (!why && memcmp(res_scratch, (void *)(uintptr_t)SCRATCH_VA, SCRATCH_SIZE)) {
                    uint32_t i = 0;
                    while (res_scratch[i] == ((uint8_t *)(uintptr_t)SCRATCH_VA)[i]) i++;
                    snprintf(buf, sizeof buf, "memory at %#x", SCRATCH_VA + i);
                    why = buf;
                }
                if (!why && memcmp(res_data, (void *)(uintptr_t)data_va, data_size)) {
                    uint32_t i = 0;
                    while (res_data[i] == ((uint8_t *)(uintptr_t)data_va)[i]) i++;
                    snprintf(buf, sizeof buf, ".data at %#x", data_va + i);
                    why = buf;
                }
            }
            if (why) {
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
    fprintf(rep, "cases: agree %ld, both fault %ld, both hang %ld, mismatch %ld\n", agree, both_fault, both_hang, mismatch);
    fclose(rep);
    printf("functions %d, clean (compared, no mismatch) %d, with mismatch %d\n", tested_fns, clean_fns, bad_fns);
    printf("cases: agree %ld, both fault %ld, both hang %ld, mismatch %ld\n", agree, both_fault, both_hang, mismatch);
    fflush(stdout);
    ExitProcess(mismatch ? 1 : 0);
}
