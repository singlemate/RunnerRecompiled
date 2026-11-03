/* Unreal Engine 3 knowledge of Mirror's Edge (PC): the FName table and object layout, found at
   run time. Used for diagnostics (ME_PC_UE_DEBUG, ME_PC_UE_NAMES) and for player settings that the
   game has no ini key for (field of view). Layout facts were read from the running game:
   UObject +0x28 Outer, +0x2C Name (FName index); FNameEntry string at +16. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- UE3 diagnostics for an appErrorf throw (ME_PC_UE_DEBUG=1): finds the FName table by its
   first entry "None" and names the script function and object of the native that failed. ---- */
static uint32_t ue_names, ue_nnames, ue_name_off;

static int ue_ok(uint32_t a) { return a >= 0x10000 && (rt_vm_is_committed(a) || rt_module_at(a)); }

static void ue_find_names(void)
{
    RtModule *m = rt_module_at(0x400000);
    if (!m) return;
    for (uint32_t p = m->base + 0x1000; p + 12 <= m->base + m->size; p += 4) {
        uint32_t d = ld32(p), n = ld32(p + 4), mx = ld32(p + 8);
        if (n < 1000 || n > 4000000 || mx < n || (d & 3) || !ue_ok(d) || !ue_ok(d + 4 * n - 4) || (ld32(d) & 3) || !ue_ok(ld32(d))) continue;
        uint32_t e = ld32(d);
        if (!ue_ok(e + 48)) continue;
        for (uint32_t off = 0; off <= 32; off += 4)
            if (!memcmp(rt_ptr(e + off), "None", 5) || !memcmp(rt_ptr(e + off), "N\0o\0n\0e\0\0", 10)) {
                /* Several arrays start with "None" (name maps of loaded packages): the global
                   table is the largest. */
                if (n > ue_nnames) { ue_names = d; ue_nnames = n; ue_name_off = off; }
                break;
            }
    }
    if (ue_names) rt_log("UE: FName table %#x (%u names, string at +%u)", ue_names, ue_nnames, ue_name_off);
}

static const char *ue_name(uint32_t idx)
{
    static char buf[4][128]; static int k;
    char *b = buf[k++ & 3];
    if (!ue_names || idx >= ue_nnames || !ue_ok(ld32(ue_names + 4 * idx))) return NULL;
    uint32_t e = ld32(ue_names + 4 * idx) + ue_name_off;
    int wide = ld8(e + 1) == 0 && ld8(e) != 0;
    uint32_t i = 0;
    for (; i < 127; i++) { uint8_t c = wide ? (uint8_t)ld16(e + 2 * i) : ld8(e + i); if (!c) break; b[i] = (char)c; }
    b[i] = 0;
    return b;
}

/* Diagnostics: every FName containing pattern (case-insensitive). */
void rt_ue_dump_names(const char *pattern)
{
    ue_names = 0; ue_nnames = 0; /* the table grows while packages load: find it again for the current count */
    ue_find_names();
    if (!ue_names) { rt_log("UE: FName table not found"); return; }
    char line[1024]; size_t o = 0;
    uint32_t n = ue_nnames;
    for (uint32_t i = 0; i < n; i++) {
        const char *nm = ue_name(i);
        if (!nm || !*nm) continue;
        char low[128], pat[64]; size_t k;
        for (k = 0; nm[k] && k < 127; k++) low[k] = (char)(nm[k] | 0x20); low[k] = 0;
        for (k = 0; pattern[k] && k < 63; k++) pat[k] = (char)(pattern[k] | 0x20); pat[k] = 0;
        if (!strstr(low, pat)) continue;
        if (o > 900) { rt_log("UE names:%s", line); o = 0; }
        o += (size_t)snprintf(line + o, sizeof line - o, " %s", nm);
    }
    if (o) rt_log("UE names:%s", line);
}

static void ue_describe(const char *what, uint32_t obj)
{
    if (!ue_ok(obj) || !ue_ok(obj + 0x4c)) { rt_log("UE: %s %#x (not mapped)", what, obj); return; }
    char line[1024]; size_t o = 0;
    for (uint32_t off = 0x10; off <= 0x48; off += 4) {
        const char *n = ue_name(ld32(obj + off));
        if (n && *n) o += (size_t)snprintf(line + o, sizeof line - o, " +%#x=%s", off, n);
    }
    rt_log("UE: %s %#x:%s", what, obj, line);
}

void rt_ue_debug_throw(X86Ctx *ctx)
{
    if (!getenv("ME_PC_UE_DEBUG")) return;
    if (!ue_names) ue_find_names();
    extern RtThread *rt_thread_self(void);
    RtThread *t = rt_thread_self();
    uint32_t top = t->stack_base + t->stack_size;
    rt_log("UE: ebp %#x/%#x esp %#x/%#x top %#x", ctx->ebp, t->ctx.ebp, ctx->esp, t->ctx.esp, top);
    for (uint32_t fp = ctx->ebp; fp >= ctx->esp && (uint64_t)fp + 8 <= top; fp = ld32(fp)) {
        rt_log("UE: frame %#x ret %#x", fp, ld32(fp + 4));
        if (ld32(fp + 4) == 0x114f39eu) { /* StaticLoadObject called from execDynamicLoadObject */
            char w[512]; size_t o = 0;
            for (uint32_t k = 8; k <= 96; k += 4) o += (size_t)snprintf(w + o, sizeof w - o, " %#x", ld32(fp + k));
            rt_log("UE: words after frame:%s", w);
            uint32_t stack = ld32(fp - 124); /* the exec function keeps Stack in esi, saved by StaticLoadObject */
            if (!ue_ok(stack) || !ue_ok(stack + 16)) { rt_log("UE: FFrame %#x not mapped", stack); break; }
            rt_log("UE: DynamicLoadObject FFrame %#x code %#x", stack, ld32(stack + 12));
            for (uint32_t fr = stack, lvl = 0; lvl < 6 && ue_ok(fr) && ue_ok(fr + 28); fr = ld32(fr + 20), lvl++) {
                uint32_t code = ld32(fr + 12), node = ld32(fr + 4);
                if (!ue_ok(node) || !ue_ok(node + 0x60)) break;
                uint32_t outer = ld32(node + 0x28);
                rt_log("UE: script frame %u: %s.%s code %#x", lvl, ue_ok(outer) ? ue_name(ld32(outer + 0x2c)) : "?", ue_name(ld32(node + 0x2c)), code);
                uint32_t script = 0, sn = 0;
                /* UStruct::Script (TArray<BYTE>) somewhere in the function: the array containing code. */
                for (uint32_t off = 0x54; off <= 0x54; off += 4) {
                    uint32_t d = ld32(node + off), n = ld32(node + off + 4);
                    if (ue_ok(d) && n < 0x100000 && code >= d && code <= d + n) { script = d; sn = n; rt_log("UE: Script at node+%#x: %#x (%u bytes)", off, d, n); break; }
                }
                if (script && lvl < 3) {
                    uint32_t lo = code - script > 160 ? code - 160 : script, hi = code + 64 < script + sn ? code + 64 : script + sn;
                    for (uint32_t q = lo; q < hi; q += 32) {
                        char hx[200]; size_t o = 0;
                        for (uint32_t k = 0; k < 32 && q + k < hi; k++) o += (size_t)snprintf(hx + o, sizeof hx - o, "%02x%s", ld8(q + k), q + k + 1 == code ? "|" : " ");
                        rt_log("UE: %04x %s", q - script, hx);
                    }
                    rt_log("UE: code offset %#x", code - script);
                    /* Object references in the bytecode are linked pointers: name what resolves. */
                    for (uint32_t q = lo; q + 4 <= hi; q++) {
                        uint32_t v = ld32(q);
                        if ((v & 3) == 0 && ue_ok(v) && ue_ok(v + 0x30)) {
                            const char *nm = ue_name(ld32(v + 0x2c));
                            uint32_t outer = ld32(v + 0x28);
                            const char *on = ue_ok(outer) && ue_ok(outer + 0x30) ? ue_name(ld32(outer + 0x2c)) : NULL;
                            if (nm && *nm) rt_log("UE:   ref @%04x %#x %s.%s", q - script, v, on ? on : "?", nm);
                        }
                    }
                }
            }
            ue_describe("Stack+4 (Node)", ld32(stack + 4));
            ue_describe("Stack+8 (Object)", ld32(stack + 8));
            ue_describe("Stack+16", ld32(stack + 16));
            uint32_t node = ld32(stack + 4);
            if (ue_ok(node)) for (uint32_t off = 0x10; off <= 0x48; off += 4) ue_describe("Node field", ld32(node + off));
            break;
        }
        if (ld32(fp) <= fp) break;
    }
}


/* ---- objects ---- */
static uint32_t ue_objs, ue_nobjs, ue_objs_array; /* data, count, and the TArray itself */

static const char *ue_obj_name(uint32_t o)
{
    return o && !(o & 3) && ue_ok(o) && ue_ok(o + 0x30) ? ue_name(ld32(o + 0x2C)) : NULL;
}

/* GObjObjects: a TArray<UObject*> in the image whose first entries are named objects. */
static void ue_find_objects(void)
{
    RtModule *m = rt_module_at(0x400000);
    if (!m || !ue_names) return;
    for (uint32_t p = m->base + 0x1000; p + 12 <= m->base + m->size; p += 4) {
        uint32_t d = ld32(p), n = ld32(p + 4), mx = ld32(p + 8);
        if (n < 10000 || n > 4000000 || mx < n || (d & 3) || !ue_ok(d) || !ue_ok(d + 4 * n - 4)) continue;
        /* Sample the whole array: GObjObjects holds named objects and freed (null) slots only. */
        int good = 0, bad = 0;
        for (uint32_t k = 0; k < 64; k++) {
            uint32_t a = d + 4 * (uint32_t)((uint64_t)k * (n - 1) / 63);
            if (!ue_ok(a)) { bad++; continue; }
            uint32_t o = ld32(a);
            const char *nm = o ? ue_obj_name(o) : "";
            if (!o) continue;
            if (nm && *nm) good++; else bad++;
        }
        if (good >= 40 && bad <= 2) {
            ue_objs = d;
            ue_nobjs = n;
            ue_objs_array = p;
            rt_log("UE: objects %#x (%u)", d, n);
            return;
        }
    }
}

static void ue_dump_words(const char *what, uint32_t o, uint32_t bytes)
{
    char line[1024];
    size_t k = 0;
    for (uint32_t off = 0; off < bytes && ue_ok(o + off + 3); off += 4)
        k += (size_t)snprintf(line + k, sizeof line - k, " %02x:%08x", off, ld32(o + off));
    rt_log("UE: %s %#x:%s", what, o, line);
}

/* Instances (and the default object) of a class, by class name: objects whose class (+0x34) is a
   UClass of that name (its own class is named "Class"). */
static float ue_f32(uint32_t a) { uint32_t u = ld32(a); float f; memcpy(&f, &u, 4); return f; }

void rt_ue_fov_instances(void)
{
    static const char *classes[] = {"Camera", "TdPlayerCamera", "PlayerController", "TdPlayerController",
                                    "GamePlayerController"};
    if (!ue_objs_array) return;
    uint32_t n = ld32(ue_objs_array + 4), unmapped = 0, null = 0, unnamed = 0, named = 0;
    char cl[1024]; size_t co = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = ld32(ue_objs_array);
        if (!ue_ok(d + 4 * i)) { unmapped++; continue; }
        uint32_t o = ld32(d + 4 * i);
        if (!o) { null++; continue; }
        const char *nm = ue_obj_name(o);
        if (!nm) {
            if (unnamed++ < 6)
                rt_log("UE: unnamed object %#x: mapped %d, name index %#x, words %08x %08x %08x", o, ue_ok(o) && ue_ok(o + 0x30),
                       ue_ok(o + 0x30) ? ld32(o + 0x2C) : 0, ue_ok(o) ? ld32(o) : 0, ue_ok(o + 8) ? ld32(o + 4) : 0,
                       ue_ok(o + 0x34) ? ld32(o + 0x30) : 0);
            continue;
        }
        named++;
        uint32_t cls = ld32(o + 0x34);
        const char *cn = ue_obj_name(cls);
        if (!cn) continue;
        if (!strcmp(cn, "Class") && strstr(nm, "Controller") && co < 900)
            co += (size_t)snprintf(cl + co, sizeof cl - co, " %s", nm);
        for (size_t k = 0; k < 1; k++) {
            (void)classes;
            if (strcmp(cn, "Camera") && !strstr(cn, "PlayerCamera") && !strstr(cn, "PlayerController")) continue;
            const char *meta = ue_obj_name(ld32(cls + 0x34));
            if (!meta || strcmp(meta, "Class")) continue;
            if (strstr(cn, "Camera"))
                rt_log("UE: %s %s_%u %#x: DefaultFOV(+1cc) %.2f flags(+1d0) %#x LockedFOV? (+1d4) %.2f", cn, nm,
                       ld32(o + 0x30), o, ue_f32(o + 0x1CC), ld32(o + 0x1D0), ue_f32(o + 0x1D4));
            else
                rt_log("UE: %s %s_%u %#x: FOVAngle(+30c) %.2f DesiredFOV(+310) %.2f DefaultFOV(+314) %.2f", cn, nm,
                       ld32(o + 0x30), o, ue_f32(o + 0x30C), ue_f32(o + 0x310), ue_f32(o + 0x314));
        }
    }
    rt_log("UE: walked %u objects: %u unmapped, %u null, %u unnamed, %u named; controller classes:%s",
           n, unmapped, null, unnamed, named, co ? cl : " none");
}

/* ME_PC_UE_FOV=<tick>: camera and controller instances with their FOV fields; with
   ME_PC_UE_FOV_PROPS also the field-of-view properties, their owners and the default objects of the
   controller and camera classes, to find where the game keeps its FOV. */
void rt_ue_fov_probe(void)
{
    ue_names = 0; ue_nnames = 0;
    ue_find_names();
    ue_objs = 0;
    ue_find_objects();
    if (!ue_objs) { rt_log("UE: object list not found"); return; }
    rt_ue_fov_instances();
    if (getenv("ME_PC_UE_FOV_PROPS") == NULL) return;
    static const char *props[] = {"DefaultFOV", "FOVAngle", "DesiredFOV", "bLockedFOV", "LockedFOV"};
    for (uint32_t i = 0; i < ue_nobjs; i++) {
        /* The game keeps running: the array may be reallocated while we walk it. */
        uint32_t d = ld32(ue_objs_array), n = ld32(ue_objs_array + 4);
        if (i >= n) break;
        if (!ue_ok(d + 4 * i)) continue;
        uint32_t o = ld32(d + 4 * i);
        const char *nm = ue_obj_name(o);
        if (!nm) continue;
        for (size_t k = 0; k < sizeof props / sizeof *props; k++)
            if (!strcmp(nm, props[k])) {
                uint32_t outer = ld32(o + 0x28);
                const char *on = ue_obj_name(outer);
                char what[160];
                snprintf(what, sizeof what, "property %s.%s", on ? on : "?", nm);
                ue_dump_words(what, o, 0x90);
            }
        if (!strncmp(nm, "Default__", 9) && (strstr(nm, "PlayerController") || strstr(nm, "Camera")))
            ue_dump_words(nm, o, 0x40);
    }
}

/* ---- field of view (ME_PC_FOV=<degrees>) ----
   The PC game has no setting for it. The player's view comes from the controller (FOVAngle,
   DesiredFOV, DefaultFOV of PlayerController) and its camera (DefaultFOV of Camera). The chosen
   value is written into the default objects and live instances of TdPlayerController and
   TdPlayerCamera. FOVAngle/DesiredFOV are only replaced while they hold that controller's own
   default, so the game's temporary changes (zoom, effects) still happen. Property offsets are
   read from the property objects (UProperty +0x64 = Offset), not assumed.
   It runs on the game thread between frames (its Sleep in the main loop): objects are only
   created and destroyed by that thread, so none can disappear while we walk them. */
static uint32_t ue_prop_offset(const char *owner, const char *prop)
{
    uint32_t n = ld32(ue_objs_array + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = ld32(ue_objs_array);
        if (!ue_ok(d + 4 * i)) continue;
        uint32_t o = ld32(d + 4 * i);
        const char *nm = ue_obj_name(o);
        if (!nm || strcmp(nm, prop)) continue;
        const char *on = ue_obj_name(ld32(o + 0x28));
        const char *cn = ue_obj_name(ld32(o + 0x34));
        if (on && !strcmp(on, owner) && cn && !strcmp(cn, "FloatProperty") && ue_ok(o + 0x68)) return ld32(o + 0x64);
    }
    return 0;
}

/* The UClass object of that name (an object whose own class is named "Class"). */
static uint32_t ue_class(const char *name)
{
    uint32_t n = ld32(ue_objs_array + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = ld32(ue_objs_array);
        if (!ue_ok(d + 4 * i)) continue;
        uint32_t o = ld32(d + 4 * i);
        const char *nm = ue_obj_name(o);
        if (!nm || strcmp(nm, name)) continue;
        const char *cn = ue_obj_name(ld32(o + 0x34));
        if (cn && !strcmp(cn, "Class")) return o;
    }
    return 0;
}

static void ue_store_f32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); st32(a, u); }

static struct {
    float fov;
    RtThread *game_thread;
    uint64_t next_ns;
    uint32_t cam_default, pc_angle, pc_desired, pc_default, cam_class, pc_class;
    struct { uint32_t obj; float base; } seen[64];
    int nseen, logged;
} fovs;

/* Called from Sleep: does its work on the game thread, at most twice a second. */
void rt_ue_fov_tick(void)
{
    if (fovs.fov == 0 || rt_thread_self() != fovs.game_thread) return;
    uint64_t now = host_ticks_ns();
    if (now < fovs.next_ns) return;
    fovs.next_ns = now + 500000000ull;
    if (!ue_objs_array) {
        /* Early on only small name tables exist: find the names again until the objects are found. */
        fovs.next_ns = now + 5000000000ull;
        ue_names = 0;
        ue_nnames = 0;
        ue_find_names();
        if (ue_nnames) ue_find_objects();
        if (!ue_objs_array) return;
    }
    if (!fovs.cam_class || !fovs.pc_class) {
        fovs.cam_default = ue_prop_offset("Camera", "DefaultFOV");
        fovs.pc_angle = ue_prop_offset("PlayerController", "FOVAngle");
        fovs.pc_desired = ue_prop_offset("PlayerController", "DesiredFOV");
        fovs.pc_default = ue_prop_offset("PlayerController", "DefaultFOV");
        fovs.cam_class = ue_class("TdPlayerCamera");
        fovs.pc_class = ue_class("TdPlayerController");
        if (!fovs.cam_default || !fovs.pc_angle || !fovs.pc_desired || !fovs.pc_default || !fovs.cam_class ||
            !fovs.pc_class) {
            fovs.cam_class = fovs.pc_class = 0; /* the game's packages are not loaded yet */
            return;
        }
        rt_log("FOV %.1f: Camera.DefaultFOV +%#x, PlayerController FOVAngle +%#x DesiredFOV +%#x DefaultFOV +%#x",
               fovs.fov, fovs.cam_default, fovs.pc_angle, fovs.pc_desired, fovs.pc_default);
    }
    uint32_t n = ld32(ue_objs_array + 4), d = ld32(ue_objs_array), cams = 0, pcs = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = ld32(d + 4 * i);
        if (!o) continue;
        uint32_t cls = ld32(o + 0x34);
        if (cls == fovs.cam_class) {
            ue_store_f32(o + fovs.cam_default, fovs.fov);
            cams++;
        } else if (cls == fovs.pc_class) {
            float cur = ue_f32(o + fovs.pc_default), base = cur;
            int k = 0;
            while (k < fovs.nseen && fovs.seen[k].obj != o) k++;
            if (k == fovs.nseen && fovs.nseen < 64) { fovs.seen[k].obj = o; fovs.seen[k].base = cur; fovs.nseen++; }
            if (k < fovs.nseen) {
                if (cur != fovs.fov) fovs.seen[k].base = cur; /* the game set a new default */
                base = fovs.seen[k].base;
            }
            if (ue_f32(o + fovs.pc_angle) == base) ue_store_f32(o + fovs.pc_angle, fovs.fov);
            if (ue_f32(o + fovs.pc_desired) == base) ue_store_f32(o + fovs.pc_desired, fovs.fov);
            ue_store_f32(o + fovs.pc_default, fovs.fov);
            pcs++;
        }
    }
    if (fovs.logged != (int)(cams * 100 + pcs)) {
        rt_log("FOV %.1f set on %u cameras and %u controllers (default objects included)", fovs.fov, cams, pcs);
        fovs.logged = (int)(cams * 100 + pcs);
    }
}

/* From the main thread before the game starts: it is the game thread. */
void rt_ue_start_fov(void)
{
    const char *v = getenv("ME_PC_FOV");
    if (!v) return;
    float fov = (float)atof(v);
    if (fov < 40.0f || fov > 140.0f) { rt_log("ME_PC_FOV=%s: outside 40..140, ignored", v); return; }
    fovs.fov = fov;
    fovs.game_thread = rt_thread_self();
}
