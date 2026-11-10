/* Generic stubs for imports of DLLs that are not part of the port (wxWidgets of the built-in
   editor). The MSVC-decorated name tells the calling convention and the parameter types, so a
   stub can pop the right number of argument bytes: constructors return `this`, everything else
   returns 0. Imported data symbols get a zeroed guest block instead of a thunk. */
#include "rt.h"

#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int fail; } Dm;

static int dm_type(Dm *d);

/* Skips a qualified name: fragments up to the terminating '@'. */
static void dm_name(Dm *d)
{
    while (*d->p && *d->p != '@') {
        if (*d->p >= '0' && *d->p <= '9') { d->p++; continue; }       /* name back reference */
        if (d->p[0] == '?' && d->p[1] == '$') {                        /* template */
            d->p += 2;
            while (*d->p && *d->p != '@') d->p++;
            if (*d->p) d->p++;
            while (*d->p && *d->p != '@' && !d->fail) {
                if (*d->p == '$') { d->p++; if (*d->p == '0' || *d->p == '1') { d->p++; d->p++; } continue; }
                dm_type(d);
            }
            if (*d->p) d->p++;
            continue;
        }
        while (*d->p && *d->p != '@') d->p++;
        if (*d->p) d->p++;
    }
    if (*d->p == '@') d->p++;
}

/* Parses one type and returns its size on the argument stack (4 or 8); -1 if unknown. */
static int dm_type(Dm *d)
{
    char c = *d->p++;
    switch (c) {
    case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I': case 'J': case 'K': case 'M':
        return 4;
    case 'N': case 'O':
        return 8;
    case 'X':
        return 0;
    case '_': {
        char e = *d->p++;
        return (e == 'J' || e == 'K') ? 8 : 4;
    }
    case 'P': case 'Q': case 'R': case 'S': case 'A': case 'B':
        if (*d->p == '6') { /* function pointer: convention, return type, parameters */
            d->p += 2;
            dm_type(d);
            while (*d->p && *d->p != '@' && *d->p != 'Z' && !d->fail) dm_type(d);
            if (*d->p == '@') d->p++;
            if (*d->p == 'Z') d->p++;
            return 4;
        }
        if (*d->p == '8') { /* pointer to member function: class, cv of this, convention, return, parameters */
            d->p++;
            dm_name(d);
            d->p += 2;
            dm_type(d);
            while (*d->p && *d->p != '@' && *d->p != 'Z' && !d->fail) dm_type(d);
            if (*d->p == '@') d->p++;
            if (*d->p == 'Z') d->p++;
            return 4; /* single inheritance (wxObject): a plain code pointer */
        }
        if (*d->p == 'E') d->p++; /* __ptr64 / modifiers are not used on x86 */
        d->p++;                   /* cv qualifier of the pointee */
        dm_type(d);
        return 4;
    case 'V': case 'U': case 'T':
        dm_name(d);
        return -2; /* a class: fine behind a pointer, unknown size by value */
    case 'W':
        d->p++;
        dm_name(d);
        return 4;
    case '?':
        d->p++;   /* cv of a by-value parameter */
        return dm_type(d);
    case '$':
        if (*d->p == '$') { d->p += 2; return dm_type(d); }
        d->fail = 1;
        return -1;
    default:
        if (c >= '0' && c <= '9') return 4; /* parameter back reference: pointers in practice */
        d->fail = 1;
        return -1;
    }
}

typedef struct { int is_data, is_ctor, hidden_ret, callee_pops, arg_bytes, ok; } Sig;

static Sig demangle(const char *name)
{
    Sig s = {0};
    if (name[0] != '?') return s;
    int ctor = name[1] == '?' && name[2] == '0';
    Dm d = {name + 1, 0};
    if (d.p[0] == '?') d.p += 2; /* operator / ctor / dtor code */
    dm_name(&d);
    char a = *d.p++;
    if (a >= '0' && a <= '4') { s.is_data = 1; s.ok = 1; return s; }
    char conv;
    if (a == 'Y' || a == 'Z') conv = *d.p++;
    else if (strchr("CDKLST", a)) conv = *d.p++;
    else { d.p++; conv = *d.p++; } /* member: cv of this, then convention */
    int ret = 0;
    if (*d.p == '@') d.p++;        /* constructors and destructors have no return type */
    else if (*d.p == '?') { d.p++; ret = dm_type(&d); }
    else ret = dm_type(&d);
    /* A class returned by value comes back through a hidden pointer argument. */
    int bytes = ret == -2 ? 4 : 0;
    s.hidden_ret = ret == -2;
    if (*d.p == 'X') d.p++;
    else
        while (*d.p && *d.p != '@' && *d.p != 'Z' && !d.fail) {
            int n = dm_type(&d);
            if (n == -2) d.fail = 1; /* class by value: size unknown */
            if (n > 0) bytes += n;
        }
    if (d.fail) return s;
    s.is_ctor = ctor;
    s.callee_pops = conv == 'E' || conv == 'G'; /* __thiscall, __stdcall */
    s.arg_bytes = bytes;
    s.ok = 1;
    return s;
}

static void stub_ret0(X86Ctx *ctx) { ctx->eax = 0; }
static void stub_ctor(X86Ctx *ctx) { ctx->eax = ctx->ecx; }
static void stub_hidden(X86Ctx *ctx) { ctx->eax = rt_arg(ctx, 0); } /* returns the result buffer */

/* wxWidgets (the editor): signature-derived stubs. Bink is lifted (binkw32 module). */
int rt_stub_dll(const char *dll) { return !strncmp(dll, "wxmsw28u", 8); }

/* Fills a stub for an unimplemented import of a stubbed DLL; returns 0 if the name cannot be
   understood (then the import stays "missing" and stops the run when called). */
int rt_stub_for(const char *dll, const char *name, HleFn *fn, uint8_t *nargs, int *is_data)
{
    if (!rt_stub_dll(dll)) return 0;
    /* __stdcall C names carry the argument size: _Name@N. */
    const char *at = strrchr(name, '@');
    if (name[0] == '_' && at && at[1] >= '0' && at[1] <= '9') {
        *is_data = 0;
        *fn = stub_ret0;
        *nargs = (uint8_t)(atoi(at + 1) / 4);
        return 1;
    }
    Sig s = demangle(name);
    if (!s.ok) return 0;
    *is_data = s.is_data;
    if (s.is_data) return 1;
    *fn = s.is_ctor ? stub_ctor : s.hidden_ret ? stub_hidden : stub_ret0;
    *nargs = (uint8_t)(s.callee_pops ? s.arg_bytes / 4 : 0);
    return 1;
}
