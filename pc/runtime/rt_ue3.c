/* Unreal Engine 3 knowledge of Mirror's Edge (PC): the FName table and object layout, found at
   run time. Used for diagnostics (ME_PC_UE_DEBUG, ME_PC_UE_NAMES) and for player settings that the
   game has no ini key for (field of view). Layout facts were read from the running game:
   UObject +0x28 Outer, +0x2C Name (FName index); FNameEntry string at +16. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (!ue_names || idx >= ue_nnames || !ue_ok(ue_names + 4 * idx) || !ue_ok(ld32(ue_names + 4 * idx))) return NULL;
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

/* The USoundNodeWave a sound buffer is being made from, found among the words on the guest stack
   (the engine's buffer code holds it): "Package.Name", or empty. Called from alBufferData. */
void rt_ue_find_sound(X86Ctx *ctx, char *out, size_t size)
{
    static uint64_t next_refresh;
    out[0] = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ue_names) ue_find_names();
        if (!ue_names) return;
        int unknown = 0;
        for (uint32_t k = 0; k < 160; k++) {
            uint32_t a = ctx->esp + 4 * k;
            if (!ue_ok(a)) break;
            uint32_t o = ld32(a);
            if (!o || (o & 3) || !ue_ok(o) || !ue_ok(o + 0x38)) continue;
            uint32_t cls = ld32(o + 0x34);
            if (!cls || (cls & 3) || !ue_ok(cls) || !ue_ok(cls + 0x30)) continue;
            uint32_t idx = ld32(cls + 0x2C);
            if (idx >= ue_nnames && idx < 4000000) unknown = 1;
            const char *c = ue_obj_name(cls);
            if (!c || strcmp(c, "SoundNodeWave")) continue;
            uint32_t top = o;
            for (int d = 0; d < 8 && ue_ok(top + 0x28) && ld32(top + 0x28) && ue_ok(ld32(top + 0x28)); d++) top = ld32(top + 0x28);
            const char *pkg = ue_obj_name(top), *nm = ue_obj_name(o);
            if (nm) snprintf(out, size, "%s.%s", pkg ? pkg : "?", nm);
            return;
        }
        /* The name table grows and moves while packages load. */
        uint64_t now = host_ticks_ns();
        if (!unknown || now < next_refresh) return;
        next_refresh = now + 2000000000ull;
        ue_names = 0; ue_nnames = 0;
    }
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
        /* Other arrays of objects exist (a package's export map): GObjObjects is the largest. */
        if (good >= 40 && bad <= 2 && n > ue_nobjs) {
            ue_objs = d;
            ue_nobjs = n;
            ue_objs_array = p;
        }
    }
    if (ue_objs_array) rt_log("UE: objects %#x (%u)", ue_objs, ue_nobjs);
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
    const char *find = getenv("ME_PC_UE_PROPS"); /* comma-separated parts of property or struct names */
    if (find) {
        int shown = 0;
        for (uint32_t i = 0; i < ue_nobjs && shown < 400; i++) {
            uint32_t d = ld32(ue_objs_array), n = ld32(ue_objs_array + 4);
            if (i >= n) break;
            if (!ue_ok(d + 4 * i)) continue;
            uint32_t o = ld32(d + 4 * i);
            const char *nm = ue_obj_name(o);
            const char *cn = nm ? ue_obj_name(ld32(o + 0x34)) : NULL;
            if (!nm || !cn || (!strstr(cn, "Property") && strcmp(cn, "ScriptStruct"))) continue;
            char list[256];
            snprintf(list, sizeof list, "%s", find);
            int hit = 0;
            for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) if (strstr(nm, t)) hit = 1;
            if (!hit) continue;
            const char *on = ue_obj_name(ld32(o + 0x28));
            char name[100], outer[100];
            snprintf(name, sizeof name, "%s", nm);
            snprintf(outer, sizeof outer, "%s", on ? on : "?");
            rt_log("UE prop: %s.%s (%s) offset %#x, +0x7c %#x +0x80 %#x +0x84 %#x", outer, name, cn, ld32(o + 0x64), ld32(o + 0x7c), ld32(o + 0x80), ld32(o + 0x84));
            shown++;
        }
    }
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
    fovs.game_thread = rt_thread_self();  /* also without a value: the app can set one later */
    if (!v) return;
    float fov = (float)atof(v);
    if (fov < 40.0f || fov > 140.0f) { rt_log("ME_PC_FOV=%s: outside 40..140, ignored", v); return; }
    fovs.fov = fov;
}

/* From the app while the game runs: the next tick writes it. The running game keeps its controller's
   FOVAngle in step because the old value counts as that controller's default (see the tick). */
void rt_ue_set_fov(float degrees) { if (degrees >= 40.0f && degrees <= 140.0f) fovs.fov = degrees; }

/* ---- ME_PC_UE_BONES=1: diagnostics of skeletal poses (game thread, every 2 s). For every
   SkeletalMeshComponent: bones whose SpaceBases matrix is not finite or far out, LocalAtoms with
   zero scale; for the components of the player's pawn the first bones in full. ---- */
static uint32_t ue_prop_offset_typed(const char *owner, const char *prop, const char *type)
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
        if (on && !strcmp(on, owner) && cn && !strcmp(cn, type) && ue_ok(o + 0x68)) return ld32(o + 0x64);
    }
    return 0;
}

static struct { int on, ready; uint32_t space, atoms; uint64_t next_ns; void *game_thread; } bones;

void rt_ue_start_bones(void)
{
    bones.on = getenv("ME_PC_UE_BONES") != NULL;
    bones.game_thread = rt_thread_self();
}

static int f_bad(float f) { return !(f == f) || f > 1e6f || f < -1e6f; }

static uint32_t watch_comp[8], watch_val[8], nwatch, watch_logs;
/* Pitch of the player's controller, pawn and camera (Actor.Rotation), logged every frame it jumps. */
static uint32_t rot_off, pc_obj, cam_obj, pawn_obj, pitch_logs;
static int32_t last_cam_pitch;
static int32_t pitch16(uint32_t o) { return ue_ok(o + rot_off + 8) ? (int16_t)(ld32(o + rot_off) & 0xFFFF) : 0; }

void rt_ue_bones_tick(void)
{
    if (!bones.on || rt_thread_self() != bones.game_thread) return;
    if (rot_off && cam_obj && pitch_logs < 600) {
        static uint32_t cache_off;
        if (!cache_off) { cache_off = ue_prop_offset_typed("Camera", "CameraCache", "StructProperty"); rt_log("pitch: CameraCache +%#x", cache_off); }
        int32_t c = cache_off ? (int16_t)(ld32(cam_obj + cache_off + 16) & 0xFFFF) : 0;
        int32_t d = c - last_cam_pitch;
        if (d > 300 || d < -300) {
            pitch_logs++;
            rt_log("pitch: camera %d (was %d) controller %d pawn %d", c, last_cam_pitch, pitch16(pc_obj), pawn_obj ? pitch16(pawn_obj) : 0);
        }
        last_cam_pitch = c;
    }
    /* Flags word +0xFC of the player's mesh components, checked every frame: changes are logged. */
    for (uint32_t w = 0; w < nwatch; w++) {
        if (!ue_ok(watch_comp[w] + 0x100)) continue;
        uint32_t v = ld32(watch_comp[w] + 0xFC);
        if (v != watch_val[w] && watch_logs < 400) {
            watch_logs++;
            rt_log("bones: component %#x flags %08x -> %08x (OwnerNoSee %d, OnlyOwnerSee %d, HiddenGame %d)", watch_comp[w],
                   watch_val[w], v, !!(v & 0x400), !!(v & 0x800), !!(v & 0x4));
        }
        watch_val[w] = v;
    }
    uint64_t now = host_ticks_ns();
    if (now < bones.next_ns) return;
    bones.next_ns = now + 2000000000ull;
    /* The name table grows (and moves) while packages load: look it up again every time. */
    ue_names = 0;
    ue_nnames = 0;
    ue_find_names();
    ue_objs_array = 0;
    ue_nobjs = 0;
    if (ue_nnames) ue_find_objects();
    if (!ue_objs_array) return;
    if (!bones.ready) {
        bones.space = ue_prop_offset_typed("SkeletalMeshComponent", "SpaceBases", "ArrayProperty");
        bones.atoms = ue_prop_offset_typed("SkeletalMeshComponent", "LocalAtoms", "ArrayProperty");
        if (!bones.space || !bones.atoms) return;
        bones.ready = 1;
        rt_log("bones: SpaceBases +%#x LocalAtoms +%#x", bones.space, bones.atoms);
    }
    uint32_t n = ld32(ue_objs_array + 4), d = ld32(ue_objs_array), comps = 0, badc = 0;
    if (!rot_off) rot_off = ue_prop_offset_typed("Actor", "Rotation", "StructProperty");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = ld32(d + 4 * i);
        if (!o || !ue_ok(o) || !ue_ok(o + 0x40)) continue;
        const char *cn = ue_obj_name(ld32(o + 0x34));
        if (!cn) continue;
        if (!strcmp(cn, "TdPlayerController")) pc_obj = o;
        else if (!strcmp(cn, "TdPlayerCamera")) cam_obj = o;
        else if (!strcmp(cn, "TdTutorialPawn") || !strcmp(cn, "TdPlayerPawn")) pawn_obj = o;
    }
    uint32_t old_n = nwatch, old_comp[8];
    memcpy(old_comp, watch_comp, sizeof old_comp);
    nwatch = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = ld32(d + 4 * i);
        if (!o || !ue_ok(o) || !ue_ok(o + 0x40)) continue;
        const char *cn = ue_obj_name(ld32(o + 0x34));
        if (!cn || !strstr(cn, "SkeletalMeshComponent") || !ue_ok(o + bones.atoms + 8)) continue;
        const char *on = ue_obj_name(ld32(o + 0x28)), *nm = ue_obj_name(o);
        if (on && !strncmp(on, "Default__", 9)) continue;
        uint32_t sb = ld32(o + bones.space), sn = ld32(o + bones.space + 4);
        uint32_t la = ld32(o + bones.atoms), ln = ld32(o + bones.atoms + 4);
        if (!sn || sn > 1000 || !ue_ok(sb) || !ue_ok(sb + 64 * sn - 4)) continue;
        comps++;
        uint32_t bad = 0, first_bad = ~0u, zero_scale = 0;
        for (uint32_t b = 0; b < sn; b++) {
            int any = 0;
            for (int k = 0; k < 16; k++) any |= f_bad(ue_f32(sb + 64 * b + 4 * k));
            if (any) { bad++; if (first_bad == ~0u) first_bad = b; }
        }
        if (ln && ln <= 1000 && ue_ok(la) && ue_ok(la + 32 * ln - 4))
            for (uint32_t b = 0; b < ln; b++) zero_scale += ue_f32(la + 32 * b + 28) == 0.0f;
        int player = on && (strstr(on, "Player") || strstr(on, "Pawn"));
        if (player) {
            uint32_t w = 0;
            while (w < nwatch && watch_comp[w] != o) w++;
            if (w == nwatch && nwatch < 8) { watch_comp[nwatch] = o; watch_val[nwatch++] = ld32(o + 0xFC); }
        }
        if (bad) badc++;
        if (bad || player)
            rt_log("bones: %s %s.%s: %u bones, %u bad (first %d), %u atoms, %u with scale 0", cn, on ? on : "?",
                   nm ? nm : "?", sn, bad, (int)first_bad, ln, zero_scale);
        if (player && ue_ok(la) && getenv("ME_PC_UE_BONES_FULL")) {
            for (uint32_t b = 0; b < ln && b < 64; b++) {
                uint32_t a = la + 32 * b, m = sb + 64 * b;
                rt_log("  bone %2u: q %.3f %.3f %.3f %.3f t %.1f %.1f %.1f s %.3f | pos %.1f %.1f %.1f", b,
                       ue_f32(a), ue_f32(a + 4), ue_f32(a + 8), ue_f32(a + 12), ue_f32(a + 16), ue_f32(a + 20),
                       ue_f32(a + 24), ue_f32(a + 28), ue_f32(m + 48), ue_f32(m + 52), ue_f32(m + 56));
            }
        }
    }
    /* The natives that hide a component from its owner: their C++ function is the word of the
       UFunction that points into the game's code (logged once, then watched in x86_dispatch). */
    static int natives_done;
    if (!natives_done) {
        static const char *names[] = {"SetOwnerNoSee", "SetHidden", "SetOnlyOwnerSee"};
        extern void rt_watch_call(uint32_t addr, const char *label);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t f = ld32(d + 4 * i);
            if (!f || !ue_ok(f) || !ue_ok(f + 0xC0)) continue;
            const char *fn = ue_obj_name(f);
            if (!fn) continue;
            for (int k = 0; k < 3; k++) {
                if (strcmp(fn, names[k])) continue;
                const char *fc = ue_obj_name(ld32(f + 0x34)), *fo = ue_obj_name(ld32(f + 0x28));
                if (!fc || strcmp(fc, "Function")) continue;
                for (uint32_t w = 0x40; w < 0xC0; w += 4) {
                    uint32_t v = ld32(f + w);
                    if (v >= 0x401000 && v < 0x1A00000) {
                        static char lbl[3][8][64]; static int nl[3];
                        if (nl[k] < 8) {
                            snprintf(lbl[k][nl[k]], 64, "%s.%s", fo ? fo : "?", names[k]);
                            rt_log("bones: native %s at UFunction %#x +%#x -> %#x", lbl[k][nl[k]], f, w, v);
                            rt_watch_call(v, lbl[k][nl[k]]);
                            nl[k]++;
                        }
                        break;
                    }
                }
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t f = ld32(d + 4 * i);
            if (!f || !ue_ok(f) || !ue_ok(f + 0x40)) continue;
            const char *fn = ue_obj_name(f);
            if (!fn || (!strstr(fn, "chievement") && !strstr(fn, "GrantAward"))) continue;
            char name[96];
            snprintf(name, sizeof name, "%s", fn);
            const char *fc = ue_obj_name(ld32(f + 0x34));
            char cls[64];
            snprintf(cls, sizeof cls, "%s", fc ? fc : "?");
            const char *fo = ue_obj_name(ld32(f + 0x28));
            rt_log("bones: object %s %s.%s", cls, fo ? fo : "?", name);
        }
        natives_done = 1;
    }
    if (nwatch != old_n || memcmp(old_comp, watch_comp, sizeof old_comp))
        for (uint32_t w = 0; w < nwatch; w++)
            rt_log("bones: watching component %#x (%u bones) flags %08x", watch_comp[w], ld32(watch_comp[w] + bones.space + 4), watch_val[w]);
}

/* ---- Achievements. The game keeps all the logic (TdPlayerController.UnlockAchievement with the
   ETdAchievements ids), but the PC online layer has nowhere to put them. Script calls go directly to
   UObject::ProcessInternal (0x01148D50 in the GOG exe); the lifter puts rt_ue_script_call at its
   entry (lift.py SCRIPT_CALL). It records the id, the first parameter, in ME_PC_ACH_PATH ("<id>
   <unix seconds>" lines). FFrame: Node +0x04, Locals +0x10 (read from live calls).
   ME_PC_ACH_TEST=<owner>.<function> logs calls of another function instead (layout check). ---- */
#define UFUNCTION_FUNC 0xAC
#define FFRAME_NODE 0x04
#define FFRAME_LOCALS 0x10
static struct {
    const char *path, *test;
    int on, installed, logged;
    uint32_t fn[2];
    uint8_t seen[64];
    uint64_t next_ns;
    void *game_thread;
} ach;

static void ach_record(uint32_t id)
{
    if (id == 0 || id >= sizeof ach.seen || ach.seen[id]) return;
    ach.seen[id] = 1;
    rt_log("achievement %u unlocked", id);
    FILE *f = fopen(ach.path, "a");
    if (!f) { rt_log("achievements: cannot write %s", ach.path); return; }
    fprintf(f, "%u %lld\n", id, (long long)time(NULL));
    fclose(f);
}

/* A quick turn the game does not have (it only knows the 180): the app asks for degrees, and
   TdPlayerController.UpdateRotation, which runs once per tick, gets its yaw moved over ~0.14 s
   before it reads it. A request made outside the game (nothing calls UpdateRotation) expires. */
#define FFRAME_OBJECT 0x08
#define TURN_UNITS_PER_DEG (65536.0f / 360.0f)
static struct {
    volatile int request;
    volatile uint64_t asked_ns;
    uint32_t fn, rotation;
    int left, step;
} turn;

void rt_ue_request_turn(int degrees)
{
    turn.asked_ns = host_ticks_ns();
    __atomic_store_n(&turn.request, degrees, __ATOMIC_RELEASE);
}

static void turn_tick(uint32_t frame, uint32_t locals)
{
    int asked = __atomic_exchange_n(&turn.request, 0, __ATOMIC_ACQUIRE);
    if (asked && host_ticks_ns() - turn.asked_ns < 300000000ull) {
        turn.left += (int)((float)asked * TURN_UNITS_PER_DEG);
        turn.step = turn.left > 0 ? turn.left : -turn.left;
    }
    if (!turn.left) return;
    int step = (int)((float)turn.step * ue_f32(locals) / 0.14f), mag = turn.left > 0 ? turn.left : -turn.left;
    if (step > mag) step = mag;
    if (step < 1) step = 1;
    if (turn.left < 0) step = -step;
    uint32_t yaw = ld32(frame + FFRAME_OBJECT) + turn.rotation + 4; /* Rotator: Pitch, Yaw, Roll */
    st32(yaw, ld32(yaw) + (uint32_t)step);
    turn.left -= step;
}

void rt_ue_script_call(X86Ctx *ctx)
{
    if (!ach.installed) return;
    uint32_t frame = ld32(ctx->esp + 4);
    uint32_t node = ld32(frame + FFRAME_NODE);
    if (node == turn.fn && node) { turn_tick(frame, ld32(frame + FFRAME_LOCALS)); return; }
    if (!node || (node != ach.fn[0] && node != ach.fn[1])) return;
    uint32_t locals = ld32(frame + FFRAME_LOCALS);
    if (ach.logged < 3) {
        ach.logged++;
        rt_log("achievements: call of %#x, locals %#x, first float %f, first byte %u", node, locals, ue_f32(locals), ld8(locals));
    }
    if (!ach.test) ach_record(ld8(locals));
}

/* UMultiFont::GetResolutionTestTableIndex(float, thiscall): takes the page nearest at or above the
   height test (width * 0.5625) and, when every page is below it, page 0, which is 480p. On screens
   above 1920 wide all text then came from that page, enlarged. Here that one case answers with the
   tallest page instead; the text scale is test / page height, so layout stays the same.
   The idea comes from the high-res UI fix of Mirror's Edge Tweaks (softsoundd). */
int rt_ue_font_page(X86Ctx *ctx)
{
    uint32_t count = ld32(ctx->ecx + 0x154), table = ld32(ctx->ecx + 0x150);
    float test = ue_f32(ctx->esp + 4), best = 0;
    uint32_t page = 0;
    for (uint32_t i = 0; i < count; i++) {
        float height = ue_f32(table + 4 * i);
        if (height > best) { best = height; page = i; }
    }
    if (!count || best >= test - 0.0001f) return 0;
    ctx->eax = page;
    ctx->esp += 8; /* return address and the float argument */
    return 1;
}

static uint32_t ue_function(const char *owner, const char *name)
{
    uint32_t n = ld32(ue_objs_array + 4), d = ld32(ue_objs_array);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = ld32(d + 4 * i);
        if (!o || !ue_ok(o) || !ue_ok(o + UFUNCTION_FUNC + 4)) continue;
        const char *nm = ue_obj_name(o);
        if (!nm || strcmp(nm, name)) continue;
        const char *cn = ue_obj_name(ld32(o + 0x34));
        if (!cn || strcmp(cn, "Function")) continue;
        const char *on = ue_obj_name(ld32(o + 0x28));
        if (on && !strcmp(on, owner)) return o;
    }
    return 0;
}

void rt_ue_start_achievements(void)
{
    extern void rt_ue_font_start(void);
    rt_ue_font_start();
    ach.path = getenv("ME_PC_ACH_PATH");
    ach.test = getenv("ME_PC_ACH_TEST");
    if (!ach.path) return;
    ach.on = 1;
    ach.game_thread = rt_thread_self();
    FILE *f = fopen(ach.path, "r");
    if (f) {
        unsigned id; long long t;
        while (fscanf(f, "%u %lld", &id, &t) == 2) if (id < sizeof ach.seen) ach.seen[id] = 1;
        fclose(f);
    }
}

void rt_ue_achievements_tick(void)
{
    if (!ach.on || ach.installed || rt_thread_self() != ach.game_thread) return;
    uint64_t now = host_ticks_ns();
    if (now < ach.next_ns) return;
    ach.next_ns = now + 5000000000ull;
    ue_names = 0;
    ue_nnames = 0;
    ue_find_names();
    ue_objs_array = 0;
    ue_nobjs = 0;
    if (ue_nnames) ue_find_objects();
    if (!ue_objs_array) return;
    if (ach.test) {
        char owner[64], name[64];
        if (sscanf(ach.test, "%63[^.].%63s", owner, name) != 2) return;
        ach.fn[0] = ue_function(owner, name);
    } else {
        ach.fn[0] = ue_function("TdPlayerController", "UnlockAchievement");
        ach.fn[1] = ue_function("TdPlayerController", "ClientUnlockAchievement");
    }
    if (!ach.fn[0]) return;
    rt_log("achievements: watching script functions %#x %#x", ach.fn[0], ach.fn[1]);
    turn.rotation = ue_prop_offset_typed("Actor", "Rotation", "StructProperty");
    if (turn.rotation) turn.fn = ue_function("TdPlayerController", "UpdateRotation");
    rt_log("quick turn: UpdateRotation %#x, Rotation +%#x", turn.fn, turn.rotation);
    ach.installed = 1;
}

/* ME_PC_PAD_GLYPHS=ps: the button font is the game's own PS3 one. The UI style that should select it
   draws nothing in this build, so the Xbox font's table and pages become those of the PS3 font (both
   are MultiFonts of 3 x 256 characters, loaded with the front end). The third size of the PS3 table
   holds small cells, the second one is used for it as well. Runs on the game thread. */
static struct { void *thread; uint64_t next_ns; int tries, done; } pad_font;

static uint32_t ue_multifont(const char *wanted)
{
    uint32_t n = ld32(ue_objs_array + 4), d = ld32(ue_objs_array);
    for (uint32_t i = 0; i < n; i++) {
        if (!ue_ok(d + 4 * i)) continue;
        uint32_t o = ld32(d + 4 * i);
        const char *nm = ue_obj_name(o);
        if (!nm || strcmp(nm, wanted)) continue;
        const char *cn = ue_obj_name(ld32(o + 0x34));
        if (cn && !strcmp(cn, "MultiFont")) return o;
    }
    return 0;
}

void rt_ue_font_tick(void)
{
    const char *mode = getenv("ME_PC_PAD_GLYPHS");
    if (pad_font.done || !mode || strcmp(mode, "ps") || (void *)rt_thread_self() != pad_font.thread) return;
    uint64_t now = host_ticks_ns();
    if (now < pad_font.next_ns) return;
    pad_font.next_ns = now + 2000000000ull;
    if (++pad_font.tries > 150) { pad_font.done = 1; rt_log("button font: PS3 font not found"); return; }
    ue_names = 0;
    ue_nnames = 0;
    ue_find_names();
    ue_objs_array = 0;
    ue_nobjs = 0;
    if (ue_nnames) ue_find_objects();
    if (!ue_objs_array) return;
    uint32_t chars = ue_prop_offset_typed("Font", "Characters", "ArrayProperty");
    uint32_t pages = ue_prop_offset_typed("Font", "Textures", "ArrayProperty");
    uint32_t xbox = ue_multifont("UI_Fonts_XboxButtons"), ps3 = ue_multifont("UI_Fonts_PS3Buttons");
    if (!chars || !pages || !xbox || !ps3) return;
    enum { TIER = 256, SIZE = 24 };  /* FFontCharacter: 4 ints, the page byte, the vertical offset */
    uint32_t xc = ld32(xbox + chars), pc = ld32(ps3 + chars), xt = ld32(xbox + pages), pt = ld32(ps3 + pages);
    if (ld32(xbox + chars + 4) != 3 * TIER || ld32(ps3 + chars + 4) != 3 * TIER
        || ld32(xbox + pages + 4) != 3 || ld32(ps3 + pages + 4) != 3) {
        pad_font.done = 1;
        rt_log("button font: unexpected tables, left as they are");
        return;
    }
    memcpy(rt_ptr(xc), rt_ptr(pc), 2 * TIER * SIZE);
    memcpy(rt_ptr(xc + 2 * TIER * SIZE), rt_ptr(pc + TIER * SIZE), TIER * SIZE);
    memcpy(rt_ptr(xt), rt_ptr(pt), 3 * 4);
    pad_font.done = 1;
    rt_log("button font: PS3 glyphs in place");
}

void rt_ue_font_start(void) { pad_font.thread = (void *)rt_thread_self(); }

/* Diagnostics, ME_PC_PHYS=log|off[,rigid=<hz>]: what the scene's physics costs. Once a second on the
   game thread: counts the cloth components and logs the timing of each PhysX compartment; "off" stops
   cloth simulation. */
static void ue_physics_probe(const char *mode)
{
    static uint64_t next;
    static int logged;
    uint64_t now = host_ticks_ns();
    if (now < next || !ue_objs_array) return;
    next = now + 1000000000ull;
    uint32_t n = ld32(ue_objs_array + 4), d = ld32(ue_objs_array);
    int comps = 0, withverts = 0, enabled = 0, turned = 0;
    float rigid = 0;
    const char *r = strstr(mode, "rigid=");
    if (r) rigid = 1.0f / (float)atof(r + 6);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = ld32(d + 4 * i);
        const char *cn = o && ue_ok(o) ? ue_obj_name(ld32(o + 0x34)) : NULL;
        if (!cn) continue;
        if (strstr(cn, "SkeletalMeshComponent")) {
            comps++;
            uint32_t flags = ld32(o + 0x324), verts = ld32(o + 0x388);
            if (verts) withverts++;
            if (flags & 1) enabled++;
            if ((flags & 1) && !strncmp(mode, "off", 3)) { st32(o + 0x324, flags & ~1u); turned++; }
        } else if (!strcmp(cn, "WorldInfo") || !strcmp(cn, "TdWorldInfo")) {
            uint32_t t = o + 0xda8;
            if (r) st32(t + 0xC + 4, f2u(rigid));
            if (logged < 12) rt_log("physics: %s rigid fixed %u step %.4f sub %u; fluid %u %.4f %u; cloth %u %.4f %u", cn,
                ld32(t + 0xC) & 1, u2f(ld32(t + 0xC + 4)), ld32(t + 0xC + 8), ld32(t + 0x18) & 1, u2f(ld32(t + 0x18 + 4)), ld32(t + 0x18 + 8),
                ld32(t + 0x24) & 1, u2f(ld32(t + 0x24 + 4)), ld32(t + 0x24 + 8));
        }
    }
    if (logged++ < 12) rt_log("physics: %d skeletal components, %d with cloth vertices, %d with cloth on, %d turned off", comps, withverts, enabled, turned);
}

/* ME_PC_FILL_SCREEN=1: the picture on the whole screen. Two things hold the game at 16:9, both changed
   in memory only:
   - the script Camera.UpdateCamera sets bConstrainAspectRatio to True every frame: at byte 0x66 of its
     bytecode (739 bytes) True (0x27) becomes False (0x28);
   - fixed cameras (CameraActor: the menus, in-engine scenes) have the same flag of their own, cleared
     once a second on the instances and the default object;
   - the scene buffers get the height width * 9/16 (float at 0x01C54ED0, read by two functions), which
     leaves a white picture on screens narrower than 16:9: the factor follows the back buffer. */
void rt_ue_aspect_tick(void)
{
    static uint64_t next_ns;
    static int tries, done;
    static uint32_t actor_class, actor_flag, actor_mask;
    if (!getenv("ME_PC_FILL_SCREEN") || (void *)rt_thread_self() != pad_font.thread) return;
    uint64_t now = host_ticks_ns();
    if (now < next_ns) return;
    next_ns = now + (done ? 1000000000ull : 2000000000ull);
    if (done) {
        if (!ue_objs_array) return;
        uint32_t n = ld32(ue_objs_array + 4), d = ld32(ue_objs_array);
        if (!actor_class) {
            actor_class = ue_class("CameraActor");
            for (uint32_t i = 0; i < n && !actor_flag; i++) {
                uint32_t o = ld32(d + 4 * i);
                const char *nm = o ? ue_obj_name(o) : NULL;
                if (!nm || strcmp(nm, "bConstrainAspectRatio")) continue;
                const char *on = ue_obj_name(ld32(o + 0x28));
                if (on && !strcmp(on, "CameraActor")) { actor_flag = ld32(o + 0x64); actor_mask = ld32(o + 0x84); }
            }
            if (!actor_class || !actor_flag) { actor_class = 0; return; }
            rt_log("fill screen: CameraActor flag +%#x mask %#x", actor_flag, actor_mask);
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t o = ld32(d + 4 * i);
            if (o && ld32(o + 0x34) == actor_class) st32(o + actor_flag, ld32(o + actor_flag) & ~actor_mask);
        }
        if (getenv("ME_PC_PHYS")) ue_physics_probe(getenv("ME_PC_PHYS"));
        return;
    }
    if (++tries > 120) { done = 1; rt_log("fill screen: UpdateCamera not found"); return; }
    ue_names = 0;
    ue_nnames = 0;
    ue_find_names();
    ue_objs_array = 0;
    ue_nobjs = 0;
    if (ue_nnames) ue_find_objects();
    if (!ue_objs_array) return;
    uint32_t fn = ue_function("Camera", "UpdateCamera");
    if (!fn) return;
    for (uint32_t off = 0x30; off + 12 <= 0x100 && !done; off += 4) {
        uint32_t data = ld32(fn + off), num = ld32(fn + off + 4), max = ld32(fn + off + 8);
        if (num != 739 || max < num || !ue_ok(data) || !ue_ok(data + num - 1)) continue;
        uint32_t flag = data + 0x66;
        if (ld8(flag) != 0x27) { rt_log("fill screen: byte 0x66 is %#x, not True; left as it is", ld8(flag)); done = 1; return; }
        st8(flag, 0x28);
        rt_log("fill screen: UpdateCamera at %#x patched (script at +%#x)", fn, off);
        done = 1;
    }
    if (!done) { ue_dump_words("UpdateCamera", fn, 0x100); done = tries > 20; }
}

/* From CreateDevice and Reset: the scene buffers get the back buffer's height (the result is truncated). */
void rt_ue_set_buffer_size(uint32_t width, uint32_t height)
{
    static int valid = -1;
    if (!getenv("ME_PC_FILL_SCREEN") || !width || !height) return;
    if (valid < 0) valid = ld32(0x01C54ED0u) == 0x3F100000u;
    if (valid) stf(0x01C54ED0u, ((float)height + 0.5f) / (float)width);
}
