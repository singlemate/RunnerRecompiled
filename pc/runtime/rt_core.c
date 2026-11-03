/* Runtime core: guest memory and heap, PE modules, dispatch, HLE thunks, threads and TLS. */
#include "rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

uintptr_t x86_mem_base;

/* ------------------------------------------------------------------ diagnostics */

static FILE *rt_logfile;

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (!rt_logfile) rt_logfile = fopen("pc-runtime.log", "w");
    if (rt_logfile) {
        va_start(ap, fmt);
        vfprintf(rt_logfile, fmt, ap);
        va_end(ap);
        fputc('\n', rt_logfile);
        fflush(rt_logfile);
    }
}

/* Heuristic guest backtrace: words on the guest stack that point into a module's image. */
static void guest_backtrace(void)
{
    extern RtThread *rt_thread_self(void);
    RtThread *t = rt_thread_self();
    if (!t) return;
    uint32_t sp = t->ctx.esp, top = t->stack_base + t->stack_size, shown = 0;
    char line[1024];
    size_t o = 0;
    for (uint32_t p = sp; p + 4 <= top && shown < 40; p += 4) {
        uint32_t v = ld32(p);
        RtModule *m = rt_module_at(v);
        if (m && v - m->base > 0x1000) {
            o += (size_t)snprintf(line + o, sizeof line - o, " %#x", v);
            if (++shown % 10 == 0) { rt_log("  guest stack:%s", line); o = 0; }
        }
    }
    if (o) rt_log("  guest stack:%s", line);
    /* Frame-pointer chain: exact where functions keep an EBP frame (most of the game's do). */
    o = 0; shown = 0;
    for (uint32_t fp = t->ctx.ebp; fp >= sp && (uint64_t)fp + 8 <= top && shown < 24; fp = ld32(fp)) {
        o += (size_t)snprintf(line + o, sizeof line - o, " %#x", ld32(fp + 4));
        shown++;
        if (ld32(fp) <= fp) break;
    }
    if (o) rt_log("  ebp chain:%s", line);
}

static void rt_dump_hle_ring(void);

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    guest_backtrace();
    rt_dump_hle_ring();
    exit(3);
}

void x86_trap(X86Ctx *ctx, uint32_t addr, const char *why)
{
    rt_fatal("trap at guest %#x: %s (esp=%#x, [esp]=%#x)", addr, why, ctx->esp, ld32(ctx->esp));
}

/* The guest sees a processor with MMX and SSE but without SSE2, so the CRT keeps its x87 math
   paths (the lifter has no SSE2 double-precision instructions yet). */
void x86_cpuid(X86Ctx *ctx, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    (void)ctx;
    uint32_t leaf = *eax;
    *ebx = *ecx = *edx = 0;
    if (leaf == 0) {
        *eax = 1;
        memcpy(ebx, "Genu", 4);
        memcpy(edx, "ineI", 4);
        memcpy(ecx, "ntel", 4);
    } else if (leaf == 1) {
        *eax = 0x000006F2; /* family 6, model 15 */
        *edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25);
    } else if (leaf == 0x80000000u) {
        *eax = 0x80000000u;
    } else {
        *eax = 0;
    }
}

/* ------------------------------------------------------------------ guest memory */

#define NGRAN 65536u            /* 64 KB granules in 4 GB */
#define NPAGE (1u << 20)        /* 4 KB pages */
static uint32_t gran_region[NGRAN]; /* 0 = free, else region start + 1 */
static uint32_t region_size[NGRAN]; /* by granule index of the region start */
static uint8_t page_committed[NPAGE / 8];
static HostMutex *vm_lock;

void rt_mem_init(void)
{
    void *p = host_reserve(0x100000000ull);
    if (!p) rt_fatal("cannot reserve 4 GB of guest address space");
    x86_mem_base = (uintptr_t)p;
    vm_lock = host_mutex_new();
    /* Never hand out the null area or the HLE thunk range. */
    gran_region[0] = 1;
    region_size[0] = RT_GRANULE;
    rt_vm_reserve(RT_THUNK_BASE, 0x01000000);
}

static int range_free(uint32_t g0, uint32_t n)
{
    for (uint32_t g = g0; g < g0 + n; g++)
        if (g >= NGRAN || gran_region[g]) return 0;
    return 1;
}

uint32_t rt_vm_reserve(uint32_t addr, uint32_t size)
{
    if (!size) return 0;
    uint32_t n = (uint32_t)(((uint64_t)size + RT_GRANULE - 1) / RT_GRANULE);
    host_lock(vm_lock);
    uint32_t g0 = 0xFFFFFFFFu;
    if (addr) {
        if (range_free(addr / RT_GRANULE, n)) g0 = addr / RT_GRANULE;
    } else {
        /* Above the executable, below the thunks; the game is large-address aware. */
        for (uint32_t g = 0x0300; g + n < RT_THUNK_BASE / RT_GRANULE; g++)
            if (range_free(g, n)) { g0 = g; break; }
    }
    if (g0 == 0xFFFFFFFFu) { host_unlock(vm_lock); return 0; }
    for (uint32_t g = g0; g < g0 + n; g++) gran_region[g] = g0 * RT_GRANULE + 1;
    region_size[g0] = n * RT_GRANULE;
    host_unlock(vm_lock);
    return g0 * RT_GRANULE;
}

int rt_vm_commit(uint32_t addr, uint32_t size)
{
    uint32_t p0 = addr / RT_PAGE, p1 = (uint32_t)(((uint64_t)addr + size + RT_PAGE - 1) / RT_PAGE);
    host_lock(vm_lock);
    for (uint32_t p = p0; p < p1; p++) {
        if (!gran_region[p / 16]) { host_unlock(vm_lock); return 0; }
    }
    for (uint32_t p = p0; p < p1; p++) {
        if (!(page_committed[p / 8] & (1u << (p % 8)))) {
            uint32_t q = p;
            while (q < p1 && !(page_committed[q / 8] & (1u << (q % 8)))) q++;
            if (!host_commit(rt_ptr(p * RT_PAGE), (q - p) * RT_PAGE)) { host_unlock(vm_lock); return 0; }
            for (uint32_t r = p; r < q; r++) page_committed[r / 8] |= 1u << (r % 8);
            p = q - 1;
        }
    }
    host_unlock(vm_lock);
    return 1;
}

int rt_vm_release(uint32_t addr)
{
    host_lock(vm_lock);
    uint32_t g0 = addr / RT_GRANULE;
    if (addr % RT_GRANULE || gran_region[g0] != addr + 1) { host_unlock(vm_lock); return 0; }
    uint32_t n = region_size[g0] / RT_GRANULE;
    host_decommit(rt_ptr(addr), n * RT_GRANULE);
    for (uint32_t g = g0; g < g0 + n; g++) {
        gran_region[g] = 0;
        for (uint32_t p = g * 16; p < g * 16 + 16; p++) page_committed[p / 8] &= ~(1u << (p % 8));
    }
    region_size[g0] = 0;
    host_unlock(vm_lock);
    return 1;
}

int rt_vm_is_committed(uint32_t addr)
{
    uint32_t p = addr / RT_PAGE;
    return (page_committed[p / 8] >> (p % 8)) & 1;
}

uint32_t rt_vm_region_size(uint32_t addr, uint32_t *base, int *committed)
{
    uint32_t g = addr / RT_GRANULE;
    if (!gran_region[g]) { if (base) *base = g * RT_GRANULE; if (committed) *committed = 0; return 0; }
    uint32_t start = gran_region[g] - 1;
    if (base) *base = start;
    if (committed) *committed = rt_vm_is_committed(addr);
    return region_size[start / RT_GRANULE];
}

/* ------------------------------------------------------------------ guest heap
   Size classes: multiples of 16 up to 1 KB, then powers of two up to 64 KB, carved from 1 MB
   arenas. Larger blocks get their own VM region. Every block has an 8-byte header
   {size, class}; free blocks of a class form a singly linked list through guest memory. */

#define HEAP_SMALL_MAX 1024u
#define HEAP_MAX_CLASS 64u + 7u /* 64 classes of 16 bytes, then 2K..64K */
static uint32_t heap_free_list[HEAP_MAX_CLASS + 1];
static uint32_t arena_cur, arena_end;
static HostMutex *heap_lock;

static uint32_t heap_class(uint32_t size, uint32_t *csize)
{
    if (size <= HEAP_SMALL_MAX) {
        uint32_t c = (size + 15) / 16;
        if (!c) c = 1;
        *csize = c * 16;
        return c - 1;
    }
    uint32_t c = 64, s = 2048;
    while (s < size) { s <<= 1; c++; }
    *csize = s;
    return c;
}

uint32_t rt_heap_alloc(uint32_t size, int zero)
{
    if (!heap_lock) heap_lock = host_mutex_new();
    uint32_t p;
    if (size > 65536u) {
        uint32_t total = size + 16;
        uint32_t r = rt_vm_reserve(0, total);
        if (!r || !rt_vm_commit(r, total)) return 0;
        st32(r + 8, size);
        st32(r + 12, 0xFFu);
        p = r + 16;
        if (zero) memset(rt_ptr(p), 0, size); /* fresh pages are zero already; kept for clarity */
        return p;
    }
    uint32_t csize, c = heap_class(size, &csize);
    host_lock(heap_lock);
    if (heap_free_list[c]) {
        uint32_t b = heap_free_list[c];
        heap_free_list[c] = ld32(b);
        host_unlock(heap_lock);
        st32(b - 8, size);
        if (zero) memset(rt_ptr(b), 0, csize);
        return b;
    }
    if (arena_cur + csize + 8 > arena_end) {
        uint32_t r = rt_vm_reserve(0, 0x100000);
        if (!r || !rt_vm_commit(r, 0x100000)) { host_unlock(heap_lock); return 0; }
        arena_cur = r;
        arena_end = r + 0x100000;
    }
    uint32_t h = arena_cur;
    arena_cur += csize + 8;
    host_unlock(heap_lock);
    st32(h, size);
    st32(h + 4, c);
    p = h + 8;
    if (zero) memset(rt_ptr(p), 0, csize);
    return p;
}

void rt_heap_free(uint32_t p)
{
    if (!p) return;
    uint32_t c = ld32(p - 4);
    if (c == 0xFFu) { rt_vm_release(p - 16); return; }
    host_lock(heap_lock);
    st32(p, heap_free_list[c]);
    heap_free_list[c] = p;
    host_unlock(heap_lock);
}

uint32_t rt_heap_size(uint32_t p) { return p ? ld32(p - 8) : 0; }

uint32_t rt_heap_realloc(uint32_t p, uint32_t size, int zero)
{
    if (!p) return rt_heap_alloc(size, zero);
    uint32_t old = rt_heap_size(p);
    uint32_t c = ld32(p - 4), csize;
    if (c != 0xFFu && heap_class(size, &csize) == c) {
        if (zero && size > old) memset(rt_ptr(p + old), 0, size - old);
        st32(p - 8, size);
        return p;
    }
    uint32_t q = rt_heap_alloc(size, zero);
    if (!q) return 0;
    memcpy(rt_ptr(q), rt_ptr(p), old < size ? old : size);
    rt_heap_free(p);
    return q;
}

/* ------------------------------------------------------------------ dispatch */

#define FN_HASH_BITS 20
static struct { uint32_t addr; X86Fn fn; } fn_hash[1u << FN_HASH_BITS];

static void fn_add(uint32_t addr, X86Fn fn)
{
    uint32_t h = (addr * 2654435761u) >> (32 - FN_HASH_BITS);
    while (fn_hash[h].fn && fn_hash[h].addr != addr) h = (h + 1) & ((1u << FN_HASH_BITS) - 1);
    fn_hash[h].addr = addr;
    fn_hash[h].fn = fn;
}

static X86Fn fn_find(uint32_t addr)
{
    uint32_t h = (addr * 2654435761u) >> (32 - FN_HASH_BITS);
    while (fn_hash[h].fn) {
        if (fn_hash[h].addr == addr) return fn_hash[h].fn;
        h = (h + 1) & ((1u << FN_HASH_BITS) - 1);
    }
    return NULL;
}

typedef struct { char dll[64], name[512]; HleFn fn; uint8_t nargs, stub, called; uint32_t calls; } Thunk;
int rt_stub_for(const char *dll, const char *name, HleFn *fn, uint8_t *nargs, int *is_data);
static Thunk *thunks;
static uint32_t nthunks, cap_thunks;
static const HleDef **hle_sets;
static size_t *hle_set_len, n_hle_sets;
static HostMutex *thunk_lock;

volatile uint32_t rt_last_dispatch, rt_last_dispatch_ret;

/* Ring of the last HLE calls (thunk index, esp before the call, return address), for fatal reports. */
static struct { uint32_t thunk, esp, ret, eax_after; } hle_ring[64];
static volatile uint32_t hle_ring_pos;

static void resume_jump(RtThread *t, X86Ctx *ctx, uint32_t target);

void x86_dispatch(X86Ctx *ctx, uint32_t target)
{
    rt_last_dispatch = target;
    rt_last_dispatch_ret = ld32(ctx->esp);
    RtThread *self = rt_thread_self();
    if (self) { self->last_target = target; self->last_ret = ld32(ctx->esp); }
    if (target >= RT_THUNK_BASE && target < RT_THUNK_BASE + 0x01000000u) {
        uint32_t i = (target - RT_THUNK_BASE) / RT_THUNK_STEP;
        if (i >= nthunks) rt_fatal("call to unknown thunk %#x", target);
        Thunk *t = &thunks[i];
        if (!t->fn)
            rt_fatal("missing import %s!%s called (return address %#x)", t->dll, t->name, ld32(ctx->esp));
        if (t->stub && !t->called) {
            t->called = 1;
            rt_log("stub %s!%s (return address %#x)", t->dll, t->name, ld32(ctx->esp));
        }
        uint32_t slot = __atomic_fetch_add(&hle_ring_pos, 1, __ATOMIC_RELAXED) & 63;
        hle_ring[slot].thunk = i;
        hle_ring[slot].esp = ctx->esp;
        hle_ring[slot].ret = ld32(ctx->esp);
        if (self) self->in_hle = i + 1;
        __atomic_fetch_add(&t->calls, 1, __ATOMIC_RELAXED);
        t->fn(ctx);
        if (self) self->in_hle = 0;
        hle_ring[slot].eax_after = ctx->eax;
        ctx->esp += 4 + 4u * t->nargs; /* stdcall: the callee pops the return address and its arguments */
        return;
    }
    X86Fn fn = fn_find(target);
    if (!fn && self) resume_jump(self, ctx, target);
    if (!fn) rt_fatal("no lifted function at %#x (return address %#x)", target, ld32(ctx->esp));
    fn(ctx);
}

static void rt_dump_hle_ring(void)
{
    uint32_t pos = hle_ring_pos;
    for (uint32_t k = pos > 64 ? pos - 64 : 0; k < pos; k++) {
        uint32_t sl = k & 63;
        rt_log("  hle %s!%s esp %#x ret %#x -> %#x", thunks[hle_ring[sl].thunk].dll, thunks[hle_ring[sl].thunk].name,
               hle_ring[sl].esp, hle_ring[sl].ret, hle_ring[sl].eax_after);
    }
}

void rt_hle_register(const HleDef *defs, size_t n)
{
    hle_sets = realloc(hle_sets, (n_hle_sets + 1) * sizeof *hle_sets);
    hle_set_len = realloc(hle_set_len, (n_hle_sets + 1) * sizeof *hle_set_len);
    hle_sets[n_hle_sets] = defs;
    hle_set_len[n_hle_sets] = n;
    n_hle_sets++;
}

static void lower(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) d[i] = (char)tolower((unsigned char)s[i]);
    d[i] = 0;
}

uint32_t rt_hle_thunk(const char *dll, const char *name)
{
    char ldll[64];
    lower(ldll, dll, sizeof ldll);
    if (!thunk_lock) thunk_lock = host_mutex_new();
    host_lock(thunk_lock);
    for (uint32_t i = 0; i < nthunks; i++)
        if (!strcmp(thunks[i].dll, ldll) && !strcmp(thunks[i].name, name)) {
            host_unlock(thunk_lock);
            return RT_THUNK_BASE + i * RT_THUNK_STEP;
        }
    if (nthunks == cap_thunks) {
        cap_thunks = cap_thunks ? cap_thunks * 2 : 1024;
        thunks = realloc(thunks, cap_thunks * sizeof *thunks);
    }
    Thunk *t = &thunks[nthunks];
    memset(t, 0, sizeof *t);
    snprintf(t->dll, sizeof t->dll, "%s", ldll);
    snprintf(t->name, sizeof t->name, "%s", name);
    for (size_t s = 0; s < n_hle_sets && !t->fn; s++)
        for (size_t k = 0; k < hle_set_len[s]; k++) {
            char d2[64];
            lower(d2, hle_sets[s][k].dll, sizeof d2);
            if (!strcmp(d2, ldll) && !strcmp(hle_sets[s][k].name, name)) {
                t->fn = hle_sets[s][k].fn;
                t->nargs = hle_sets[s][k].nargs;
                break;
            }
        }
    if (!t->fn) {
        int is_data = 0;
        if (rt_stub_for(ldll, name, &t->fn, &t->nargs, &is_data) && !is_data) t->stub = 1;
    }
    uint32_t a = RT_THUNK_BASE + nthunks * RT_THUNK_STEP;
    nthunks++;
    host_unlock(thunk_lock);
    return a;
}

const char *rt_str(uint32_t a) { return a ? (const char *)rt_ptr(a) : NULL; }

const char *rt_thunk_name(uint32_t t)
{
    uint32_t i = (t - RT_THUNK_BASE) / RT_THUNK_STEP;
    return t >= RT_THUNK_BASE && i < nthunks ? thunks[i].name : "";
}

int rt_thunk_implemented(uint32_t t)
{
    uint32_t i = (t - RT_THUNK_BASE) / RT_THUNK_STEP;
    return t >= RT_THUNK_BASE && i < nthunks && thunks[i].fn != NULL;
}

/* ------------------------------------------------------------------ modules */

static RtModule modules[32];
static int nmodules;

RtModule *rt_find_module(const char *name)
{
    char l[64];
    lower(l, name, sizeof l);
    if (!strchr(l, '.')) strncat(l, ".dll", sizeof l - strlen(l) - 1);
    for (int i = 0; i < nmodules; i++)
        if (!strcmp(modules[i].name, l)) return &modules[i];
    return NULL;
}

RtModule *rt_module_at(uint32_t addr)
{
    for (int i = 0; i < nmodules; i++)
        if (addr >= modules[i].base && addr - modules[i].base < modules[i].size) return &modules[i];
    return NULL;
}

uint32_t rt_export(RtModule *m, const char *name)
{
    if (name[0] == '#') {
        unsigned ord = (unsigned)atoi(name + 1);
        for (uint32_t i = 0; i < m->nexports; i++)
            if (m->exports[i].ordinal == ord) return m->exports[i].addr;
        return 0;
    }
    for (uint32_t i = 0; i < m->nexports; i++)
        if (!strcmp(m->exports[i].name, name)) return m->exports[i].addr;
    return 0;
}

RtModule *rt_load_module(const char *path, const char *name, const X86FnEntry *fns, uint32_t nfns)
{
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)n);
    if (fread(file, 1, (size_t)n, f) != (size_t)n) rt_fatal("cannot read %s", path);
    fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3C);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), optsz = *(uint16_t *)(file + pe + 20);
    uint16_t chars = *(uint16_t *)(file + pe + 22);
    uint8_t *opt = file + pe + 24;
    uint32_t entry = *(uint32_t *)(opt + 16), base = *(uint32_t *)(opt + 28);
    uint32_t size = *(uint32_t *)(opt + 56), hdrs = *(uint32_t *)(opt + 60);
    if (rt_vm_reserve(base, size) != base || !rt_vm_commit(base, size))
        rt_fatal("cannot place %s at its base %#x (rebasing is not implemented)", name, base);
    memcpy(rt_ptr(base), file, hdrs);
    uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = *(uint32_t *)(sec + 8), va = *(uint32_t *)(sec + 12);
        uint32_t rawsize = *(uint32_t *)(sec + 16), raw = *(uint32_t *)(sec + 20);
        memcpy(rt_ptr(base + va), file + raw, rawsize < vsize ? rawsize : vsize);
    }
    free(file);
    RtModule *m = &modules[nmodules++];
    memset(m, 0, sizeof *m);
    lower(m->name, name, sizeof m->name);
    m->base = base;
    m->size = size;
    m->entry = entry ? base + entry : 0;
    m->is_dll = (chars & 0x2000) != 0;
    m->fns = fns;
    m->nfns = nfns;
    for (uint32_t i = 0; i < nfns; i++) fn_add(fns[i].addr, fns[i].fn);
    /* Exports. */
    uint32_t erva = ld32(base + pe + 24 + 96), esize = ld32(base + pe + 24 + 100);
    if (erva) {
        uint32_t d = base + erva, obase = ld32(d + 16), nf = ld32(d + 20), nn = ld32(d + 24);
        uint32_t af = base + ld32(d + 28), an = base + ld32(d + 32), ao = base + ld32(d + 36);
        m->exports = calloc(nf ? nf : 1, sizeof *m->exports);
        for (uint32_t i = 0; i < nf; i++) {
            uint32_t a = ld32(af + 4 * i);
            if (!a || (a >= erva && a < erva + esize)) continue; /* forwarders are not followed */
            struct RtExport *x = &m->exports[m->nexports++];
            x->addr = base + a;
            x->ordinal = (uint16_t)(obase + i);
            for (uint32_t k = 0; k < nn; k++)
                if (ld16(ao + 2 * k) == i) {
                    snprintf(x->name, sizeof x->name, "%s", rt_str(base + ld32(an + 4 * k)));
                    break;
                }
        }
    }
    rt_log("loaded %s at %#x (%u bytes, %u lifted functions, %u exports)", m->name, base, size, nfns, m->nexports);
    return m;
}

void rt_module_init(RtModule *m, X86Ctx *ctx)
{
    if (!m || m->initialized || !m->is_dll || !m->entry) return;
    m->initialized = 1;
    uint32_t args[3] = {m->base, 1, 0};
    uint32_t r = rt_call(ctx, m->entry, 3, args);
    rt_log("%s DllMain(PROCESS_ATTACH) = %u", m->name, r);
}

void rt_module_init_tree(RtModule *m, X86Ctx *ctx)
{
    if (!m || m->initialized) return;
    m->initialized = -1; /* visiting: breaks import cycles */
    for (int i = 0; i < m->ndeps; i++)
        if (!m->deps[i]->initialized) rt_module_init_tree(m->deps[i], ctx);
    m->initialized = 0;
    if (m->is_dll) rt_module_init(m, ctx);
    else m->initialized = 1;
}

void rt_resolve_imports(RtModule *m)
{
    uint32_t pe = ld32(m->base + 0x3C);
    uint32_t irva = ld32(m->base + pe + 24 + 104);
    if (!irva) return;
    uint32_t missing = 0, hle = 0, lifted = 0;
    for (uint32_t d = m->base + irva; ld32(d + 12); d += 20) {
        const char *dll = rt_str(m->base + ld32(d + 12));
        uint32_t lookup = ld32(d) ? ld32(d) : ld32(d + 16), iat = ld32(d + 16);
        RtModule *target = rt_find_module(dll);
        if (target && target != m && m->ndeps < 16) {
            int known = 0;
            for (int k = 0; k < m->ndeps; k++) known |= m->deps[k] == target;
            if (!known) m->deps[m->ndeps++] = target;
        }
        for (uint32_t i = 0;; i++) {
            uint32_t ent = ld32(m->base + lookup + 4 * i);
            if (!ent) break;
            char name[512];
            if (ent & 0x80000000u) snprintf(name, sizeof name, "#%u", ent & 0xFFFF);
            else snprintf(name, sizeof name, "%s", rt_str(m->base + ent + 2));
            uint32_t addr = target ? rt_export(target, name) : 0;
            HleFn sfn;
            uint8_t sn;
            int is_data = 0;
            if (!addr && rt_stub_for(dll, name, &sfn, &sn, &is_data) && is_data) {
                addr = rt_heap_alloc(256, 1); /* imported variable of a stubbed DLL: zeroed */
                hle++;
            } else if (addr) lifted++;
            else {
                addr = rt_hle_thunk(dll, name);
                Thunk *t = &thunks[(addr - RT_THUNK_BASE) / RT_THUNK_STEP];
                if (t->fn) hle++;
                else missing++;
            }
            st32(m->base + iat + 4 * i, addr);
        }
    }
    rt_log("%s imports: %u to lifted modules, %u HLE, %u not implemented yet", m->name, lifted, hle, missing);
}

/* ------------------------------------------------------------------ threads and TLS */

static __thread RtThread *self_thread;
static uint32_t peb_addr;
static uint8_t tls_used[RT_TLS_SLOTS];
static HostMutex *tls_lock;

RtThread *rt_thread_self(void) { return self_thread; }

/* ---- EH resume points (x86rt.h) ---- */
void x86_resume_push(X86Ctx *ctx, X86Resume *r, const uint32_t *targets, uint32_t n)
{
    RtThread *t = self_thread;
    r->targets = targets;
    r->ntargets = n;
    r->entry_esp = ctx->esp;
    r->target = 0;
    r->prev = t->resume_top;
    t->resume_top = r;
}

void x86_resume_pop(X86Ctx *ctx, X86Resume *r)
{
    (void)ctx;
    RtThread *t = self_thread;
    if (t->resume_top == r) t->resume_top = r->prev;
}

/* An indirect jump to a non-entry: the EH runtime resuming a frame at a catch continuation or an
   __except block, with that frame's ebp in ctx. The innermost live frame that owns the target and
   lies above ebp is the one (deeper invocations of the same function have entry esp <= ebp). */
static void resume_jump(RtThread *t, X86Ctx *ctx, uint32_t target)
{
    if (getenv("ME_PC_LOG_EH")) {
        int depth = 0;
        for (X86Resume *r = t->resume_top; r; r = r->prev) depth++;
        rt_log("EH resume %#x: ebp %#x esp %#x, %d live frames", target, ctx->ebp, ctx->esp, depth);
        for (X86Resume *r = t->resume_top; r; r = r->prev) rt_log("  frame entry esp %#x first target %#x", r->entry_esp, r->targets[0]);
    }
    for (X86Resume *r = t->resume_top; r; r = r->prev) {
        if (r->entry_esp <= ctx->ebp) continue;
        for (uint32_t i = 0; i < r->ntargets; i++)
            if (r->targets[i] == target) {
                t->resume_top = r;
                t->in_hle = 0;
                r->target = target;
                longjmp(r->jb, 1);
            }
    }
}

static RtThread *all_threads[64];
static int nall_threads;

static void register_thread(RtThread *t)
{
    if (nall_threads < 64) all_threads[nall_threads++] = t;
}

/* ME_PC_STALL_DUMP=1: every 10 s, what each guest thread is doing. */
static void stall_watch(void *arg)
{
    (void)arg;
    for (;;) {
        host_sleep_ms(10000);
        for (int i = 0; i < nall_threads; i++) {
            RtThread *t = all_threads[i];
            uint32_t h = t->in_hle;
            rt_log("watch: thread %#x last dispatch %#x (return %#x)%s%s", t->id, t->last_target, t->last_ret,
                   h ? " in HLE " : "", h ? thunks[h - 1].name : "");
        }
        /* The busiest HLE functions of the last interval (Present, Sleep, ... tell the loop). */
        static uint32_t *prev;
        if (!prev) prev = calloc(65536, sizeof *prev);
        uint32_t top[8] = {0}, topn[8] = {0};
        for (uint32_t k = 0; k < nthunks && k < 65536; k++) {
            uint32_t d = thunks[k].calls - prev[k];
            prev[k] = thunks[k].calls;
            for (int j = 0; j < 8; j++)
                if (d > topn[j]) {
                    memmove(top + j + 1, top + j, (7 - j) * sizeof *top); memmove(topn + j + 1, topn + j, (7 - j) * sizeof *topn);
                    top[j] = k; topn[j] = d; break;
                }
        }
        char line[1024]; size_t o = 0;
        for (int j = 0; j < 8 && topn[j]; j++) o += (size_t)snprintf(line + o, sizeof line - o, " %s=%u", thunks[top[j]].name, topn[j]);
        rt_log("watch: busiest HLE in 10 s:%s", o ? line : " none");
        o = 0;
        for (uint32_t k = 0; k < nthunks && k < 65536; k++)
            if (strstr(thunks[k].name, "::Present") || strstr(thunks[k].name, "::EndScene"))
                o += (size_t)snprintf(line + o, sizeof line - o, " %s=%u", thunks[k].name, thunks[k].calls);
        if (o) rt_log("watch: frames so far:%s", line);
    }
}

void rt_start_stall_watch(void)
{
    if (getenv("ME_PC_STALL_DUMP")) host_thread_start(stall_watch, NULL);
}

uint32_t rt_peb(void) { return peb_addr; }

static void thread_setup(RtThread *t, uint32_t stack_size)
{
    t->stack_size = stack_size;
    t->stack_base = rt_vm_reserve(0, stack_size);
    if (!t->stack_base || !rt_vm_commit(t->stack_base, stack_size)) rt_fatal("cannot allocate a guest stack");
    t->teb = rt_heap_alloc(0x1000, 1);
    uint32_t teb = t->teb;
    st32(teb + 0x00, 0xFFFFFFFFu);                    /* SEH chain: empty */
    st32(teb + 0x04, t->stack_base + stack_size);     /* stack base (top) */
    st32(teb + 0x08, t->stack_base);                  /* stack limit */
    st32(teb + 0x18, teb);                            /* self */
    st32(teb + 0x20, 1);                              /* process id */
    st32(teb + 0x24, t->id);                          /* thread id */
    st32(teb + 0x30, peb_addr);
    t->ctx.fs_base = teb;
    t->ctx.esp = t->stack_base + stack_size - 16;
    x87_init(&t->ctx);
    t->ctx.mxcsr = 0x1F80;
    self_thread = t;
    register_thread(t);
}

RtThread *rt_thread_new(uint32_t stack_size)
{
    RtThread *t = calloc(1, sizeof *t);
    static uint32_t next_id = 0x1000;
    t->id = __atomic_add_fetch(&next_id, 4, __ATOMIC_SEQ_CST);
    RtThread *prev = self_thread;
    thread_setup(t, stack_size);   /* sets self_thread: restored below */
    self_thread = prev;
    return t;
}

void rt_thread_bind(RtThread *t) { self_thread = t; }

RtThread *rt_thread_create_main(uint32_t stack_size)
{
    if (!peb_addr) {
        peb_addr = rt_heap_alloc(0x1000, 1);
        st32(peb_addr + 0x08, 0x400000);   /* ImageBaseAddress (patched by the loader caller if needed) */
        st32(peb_addr + 0xA4, 5);          /* OSMajorVersion: Windows XP */
        st32(peb_addr + 0xA8, 1);          /* OSMinorVersion */
        st32(peb_addr + 0xAC, 2600);       /* OSBuildNumber */
        st32(peb_addr + 0xB0, 2);          /* OSPlatformId: VER_PLATFORM_WIN32_NT */
    }
    RtThread *t = calloc(1, sizeof *t);
    t->id = host_thread_id();
    thread_setup(t, stack_size);
    return t;
}

uint32_t rt_tls_alloc(void)
{
    if (!tls_lock) tls_lock = host_mutex_new();
    host_lock(tls_lock);
    for (uint32_t i = 0; i < RT_TLS_SLOTS; i++)
        if (!tls_used[i]) { tls_used[i] = 1; host_unlock(tls_lock); return i; }
    host_unlock(tls_lock);
    return 0xFFFFFFFFu; /* TLS_OUT_OF_INDEXES */
}

int rt_tls_free(uint32_t slot)
{
    if (slot >= RT_TLS_SLOTS || !tls_used[slot]) return 0;
    tls_used[slot] = 0;
    return 1;
}

static uint32_t tls_cell(uint32_t slot)
{
    uint32_t teb = self_thread->teb;
    if (slot < 64) return teb + 0xE10 + 4 * slot;
    uint32_t ext = ld32(teb + 0xF94);
    if (!ext) {
        ext = rt_heap_alloc(1024 * 4, 1);
        st32(teb + 0xF94, ext);
    }
    return ext + 4 * (slot - 64);
}

uint32_t rt_tls_get(uint32_t slot) { return slot < RT_TLS_SLOTS ? ld32(tls_cell(slot)) : 0; }

int rt_tls_set(uint32_t slot, uint32_t v)
{
    if (slot >= RT_TLS_SLOTS) return 0;
    st32(tls_cell(slot), v);
    return 1;
}

uint32_t rt_call(X86Ctx *ctx, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t saved = ctx->esp;
    for (int i = nargs - 1; i >= 0; i--) { ctx->esp -= 4; st32(ctx->esp, args[i]); }
    ctx->esp -= 4;
    st32(ctx->esp, 0xFEFFFFF0u); /* return address: back to the host */
    x86_dispatch(ctx, fn);
    ctx->esp = saved;
    return ctx->eax;
}
