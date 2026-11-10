/* Guest file system. The game sees itself installed in C:\MirrorsEdge. Reads come from the
   user's copy of the game; everything the game writes (logs, ini files, saves, caches) goes to a
   separate overlay, so the installed copy is never modified:

     C:\MirrorsEdge\<rel>        read: <user>/overlay/<rel> if present, else <game>/<rel>
                                 write: <user>/overlay/<rel> (copied from the game first if needed)
     any other path              <user>/other/<drive>/<path>; the game's "My Documents" and AppData
                                 are there (C:\Documents and Settings\Runner\...), outside the
                                 install folder as on Windows: the game rewrites paths under its
                                 install folder to its user folder, so one inside it would nest */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define ARG(i) rt_arg(ctx, i)
#define RET(v) (ctx->eax = (uint32_t)(v))
#define INVALID_HANDLE 0xFFFFFFFFu
#define GUEST_ROOT "C:\\MirrorsEdge"
#define GUEST_CWD "C:\\MirrorsEdge\\Binaries"
#define GUEST_PROFILE "C:\\Documents and Settings\\Runner"

static char game_root[1024], user_root[1024];
static int log_reads;

void rt_fs_init(const char *game, const char *user)
{
    snprintf(game_root, sizeof game_root, "%s", game);
    snprintf(user_root, sizeof user_root, "%s", user);
    host_mkdir(user_root);
    log_reads = getenv("ME_PC_LOG_FILES") != NULL;
}

static void set_error(uint32_t e) { st32(rt_thread_self()->teb + 0x34, e); }

/* ---- guest strings ---- */
static void guest_a(uint32_t p, char *out, size_t n) { snprintf(out, n, "%s", p ? rt_str(p) : ""); }

static void guest_w(uint32_t p, char *out, size_t n)
{
    size_t o = 0;
    for (uint32_t i = 0; p && o + 4 < n; i++) {
        uint16_t c = ld16(p + 2 * i);
        if (!c) break;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

static void put_a(uint32_t p, const char *s) { memcpy(rt_ptr(p), s, strlen(s) + 1); }

static void put_w(uint32_t p, const char *s)
{
    uint32_t i = 0;
    for (; s[i]; i++) st16(p + 2 * i, (uint8_t)s[i]);
    st16(p + 2 * i, 0);
}

/* ---- paths ---- */

/* Canonical guest path: drive letter upper case, backslashes, no "." / "..", no trailing slash. */
static void canonical(const char *in, char *out, size_t n)
{
    char tmp[1024];
    if (in[0] && in[1] == ':') snprintf(tmp, sizeof tmp, "%s", in);
    else if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof tmp, "C:%s", in);
    else snprintf(tmp, sizeof tmp, "%s\\%s", GUEST_CWD, in);
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    char *parts[128];
    int np = 0;
    /* split in place on backslashes (strtok is not thread-safe: guest threads open files at once) */
    char *p = tmp + 2;
    for (char *tok = p; tok; tok = p) {
        p = strchr(tok, '\\');
        if (p) *p++ = 0;
        if (!strcmp(tok, ".") || !*tok) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        if (np < 128) parts[np++] = tok;
    }
    size_t o = (size_t)snprintf(out, n, "%c:", toupper((unsigned char)tmp[0]));
    for (int i = 0; i < np && o < n; i++) o += (size_t)snprintf(out + o, n - o, "\\%s", parts[i]);
    if (!np) snprintf(out + o, n - o, "\\");
}

/* Relative part under C:\MirrorsEdge (with leading separator), or NULL. */
static const char *under_root(const char *g)
{
    size_t l = strlen(GUEST_ROOT);
    if (!strncmp(g, "C:", 2) && !_strnicmp(g, GUEST_ROOT, l) && (g[l] == '\\' || !g[l])) return g + l;
    return NULL;
}

static void overlay_path(const char *g, char *out, size_t n)
{
    const char *rel = under_root(g);
    if (rel) snprintf(out, n, "%s/overlay%s", user_root, rel);
    else snprintf(out, n, "%s/other/%c%s", user_root, g[0], g + 2);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
}

static int game_path(const char *g, char *out, size_t n)
{
    const char *rel = under_root(g);
    if (!rel) return 0;
    snprintf(out, n, "%s%s", game_root, rel);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return 1;
}

/* Host path to read g: the overlay wins over the game. Returns the kind (0 missing, 1 file, 2 dir). */
static int read_path(const char *g, char *out, size_t n)
{
    char gp[1024];
    overlay_path(g, out, n);
    int k = host_stat(out, NULL, NULL);
    if (k) return k;
    if (game_path(g, gp, sizeof gp)) {
        k = host_stat(gp, NULL, NULL);
        if (k) { snprintf(out, n, "%s", gp); return k; }
    }
    overlay_path(g, out, n);
    return 0;
}

static void mkdirs_for(const char *host_file)
{
    char d[1024];
    snprintf(d, sizeof d, "%s", host_file);
    for (char *p = d + strlen(user_root) + 1; *p; p++)
        if (*p == '/') { *p = 0; host_mkdir(d); *p = '/'; }
}

static int copy_file(const char *from, const char *to)
{
    int a = host_open(from, 0, 0, 0);
    if (a < 0) return 0;
    mkdirs_for(to);
    int b = host_open(to, 1, 1, 1);
    if (b < 0) { host_close(a); return 0; }
    char *buf = malloc(1 << 20);
    int64_t r;
    while ((r = host_read(a, buf, 1 << 20)) > 0) host_write(b, buf, (uint32_t)r);
    free(buf);
    host_close(a);
    host_close(b);
    return 1;
}

/* ---- file objects ---- */
typedef struct { int fd, is_dir, fresh; char host[1024], guest[512]; } RtFile;

/* ME_PC_INI_SET="Section|Key=Value;...": when the game writes TdEngine.ini for the first time,
   these keys are set in it, so settings chosen before the first run apply on that run. The ini
   is ASCII. Later rewrites (the game's own options) are left alone. */
static void ini_first_write(const char *path)
{
    const char *set = getenv("ME_PC_INI_SET");
    static int done;
    if (!set || done) return;
    done = 1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t cap = (size_t)n + strlen(set) * 2 + 64;
    char *b = malloc(cap + 1);
    n = (long)fread(b, 1, (size_t)n, f);
    fclose(f);
    b[n] = 0;
    char *copy = strdup(set);
    for (char *item = strtok(copy, ";"); item; item = strtok(NULL, ";")) {
        char *bar = strchr(item, '|'), *eq = bar ? strchr(bar, '=') : NULL;
        if (!eq) continue;
        char head[128], key[128];
        snprintf(head, sizeof head, "[%.*s]", (int)(bar - item), item);
        snprintf(key, sizeof key, "%.*s=", (int)(eq - (bar + 1)), bar + 1);
        char *sec = strstr(b, head);
        if (!sec) { rt_log("ini: no section %s", head); continue; }
        char *body = sec + strlen(head), *end = strstr(body, "\n["), *p = body, *found = NULL;
        size_t klen = strlen(key);
        while ((p = strstr(p, key)) && (!end || p < end)) {
            if (p[-1] == '\n') { found = p; break; }
            p += klen;
        }
        if (!found) { rt_log("ini: no key %s", key); continue; }
        char *value = found + klen, *stop = value;
        while (*stop && *stop != '\r' && *stop != '\n') stop++;
        size_t vlen = strlen(eq + 1);
        memmove(value + vlen, stop, strlen(stop) + 1);
        memcpy(value, eq + 1, vlen);
        rt_log("ini: %s %s%s", head, key, eq + 1);
    }
    free(copy);
    f = fopen(path, "wb");
    if (f) { fwrite(b, 1, strlen(b), f); fclose(f); }
    free(b);
}

void rt_file_close(uint32_t h)
{
    RtFile *f = rt_handle_data(h, RT_H_FILE);
    if (f) {
        if (f->fd >= 0) host_close(f->fd);
        size_t glen = strlen(f->guest);
        if (f->fresh && glen >= 12 && !_stricmp(f->guest + glen - 12, "TdEngine.ini")) ini_first_write(f->host);
        free(f);
    }
    void *fd = rt_handle_data(h, RT_H_FIND);
    if (fd) free(fd);
}

#define GENERIC_WRITE 0x40000000u

/* ME_PC_PAD_GLYPHS=nintendo: the Xbox glyphs with the letters of the buttons in those positions. The
   game files are left as they are, the guest opens a changed copy of TdGameUI.<language> next to the
   log. A returned path is a copy, or the original on any failure. ("ps" is handled in rt_ue3.c.) */
static int unit_match(const unsigned char *p, size_t left, int unit, const char *s)
{
    size_t n = strlen(s);
    if (left < n * (size_t)unit) return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i * (size_t)unit] != (unsigned char)s[i] || (unit == 2 && p[i * 2 + 1])) return 0;
    return 1;
}

static const char *glyph_file(const char *guest, const char *host, char *out, size_t cap)
{
    const char *mode = getenv("ME_PC_PAD_GLYPHS"), *base = strrchr(guest, '\\'), *log = getenv("ME_PC_LOG_PATH");
    char prefix[10];
    base = base ? base + 1 : guest;
    snprintf(prefix, sizeof prefix, "%.9s", base);
    if (!mode || strcmp(mode, "nintendo") || _stricmp(prefix, "TdGameUI.")) return host;
    FILE *f = fopen(host, "rb");
    if (!f) return host;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *in = malloc((size_t)n + 1), *o = malloc((size_t)n * 2 + 16);
    n = (long)fread(in, 1, (size_t)n, f);
    fclose(f);
    int unit = n >= 2 && in[0] == 0xFF && in[1] == 0xFE ? 2 : 1, swap = 0, line_start = 1;
    size_t i = unit == 2 ? 2 : 0, w = 0;
    if (unit == 2) { o[w++] = 0xFF; o[w++] = 0xFE; }
#define EMIT(c) do { o[w++] = (unsigned char)(c); if (unit == 2) o[w++] = 0; } while (0)
    while (i < (size_t)n) {
        size_t left = (size_t)n - i;
        if (line_start)
            swap = unit_match(in + i, left, unit, "Xenon_A=") || unit_match(in + i, left, unit, "Xenon_B=")
                || unit_match(in + i, left, unit, "Xenon_X=") || unit_match(in + i, left, unit, "Xenon_Y=");
        line_start = 0;
        if (swap && unit_match(in + i, left, unit, "> ") && left >= 3 * (size_t)unit) {
            int c = in[i + 2 * (size_t)unit];
            int to = c == 'A' ? 'B' : c == 'B' ? 'A' : c == 'X' ? 'Y' : c == 'Y' ? 'X' : c;
            EMIT('>'); EMIT(' '); EMIT(to);
            i += 3 * (size_t)unit;
            swap = 0;
            continue;
        }
        if (in[i] == '\n' && (unit == 1 || !in[i + 1])) line_start = 1;
        o[w++] = in[i++];
        if (unit == 2 && i < (size_t)n) o[w++] = in[i++];
    }
#undef EMIT
    const char *cut = log ? strrchr(log, '/') : NULL;
    snprintf(out, cap, "%.*s%s%s", cut ? (int)(cut - log + 1) : 0, cut ? log : "", "glyphs-", base);
    f = fopen(out, "wb");
    int ok = f && fwrite(o, 1, w, f) == w;
    if (f) fclose(f);
    free(in);
    free(o);
    rt_log("button glyphs (%s): %s", mode, ok ? out : "could not write the copy");
    return ok ? out : host;
}

static uint32_t create_file(const char *name, uint32_t access, uint32_t disp, uint32_t flags)
{
    char g[512], hp[1024];
    canonical(name, g, sizeof g);
    /* ME_PC_NO_INTRO=1: the logo movie before the menu counts as missing, the game goes on without it. */
    size_t glen = strlen(g);
    if (glen >= 16 && !_stricmp(g + glen - 16, "StartupMovie.bik") && getenv("ME_PC_NO_INTRO")) {
        rt_log("CreateFile %s: hidden (ME_PC_NO_INTRO)", g);
        set_error(2);
        return INVALID_HANDLE;
    }
    int kind = read_path(g, hp, sizeof hp);
    int write = (access & GENERIC_WRITE) != 0;
    if (kind == 2) {
        if (!(flags & 0x02000000u)) { set_error(5); return INVALID_HANDLE; } /* ACCESS_DENIED */
        RtFile *f = calloc(1, sizeof *f);
        f->fd = -1;
        f->is_dir = 1;
        snprintf(f->host, sizeof f->host, "%s", hp);
        snprintf(f->guest, sizeof f->guest, "%s", g);
        return rt_handle_new(RT_H_FILE, f);
    }
    int exists = kind == 1;
    if ((disp == 3 || disp == 5) && !exists) {
        if (log_reads) rt_log("CreateFile %s: not found", g);
        set_error(2);
        return INVALID_HANDLE;
    }
    if (disp == 1 && exists) { set_error(80); return INVALID_HANDLE; }
    int fd;
    char op[1024];
    overlay_path(g, op, sizeof op);
    if (!write && exists) {
        char glyphs[1024];
        fd = host_open(glyph_file(g, hp, glyphs, sizeof glyphs), 0, 0, 0);
        if (log_reads) rt_log("CreateFile %s (read): %s", g, hp);
    } else {
        int truncate = disp == 2 || disp == 5;
        if (exists && strcmp(hp, op) && !truncate) copy_file(hp, op); /* copy on write */
        mkdirs_for(op);
        fd = host_open(op, 1, 1, truncate);
        snprintf(hp, sizeof hp, "%s", op);
        rt_log("CreateFile %s (write, disposition %u): %s", g, disp, op);
    }
    if (fd < 0) { set_error(5); return INVALID_HANDLE; }
    set_error((disp == 2 || disp == 4) && exists ? 183 : 0); /* ERROR_ALREADY_EXISTS is informational */
    RtFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    f->fresh = write && !exists;
    snprintf(f->host, sizeof f->host, "%s", hp);
    snprintf(f->guest, sizeof f->guest, "%s", g);
    return rt_handle_new(RT_H_FILE, f);
}

static void k_CreateFileA(X86Ctx *ctx)
{
    char n[512];
    guest_a(ARG(0), n, sizeof n);
    RET(create_file(n, ARG(1), ARG(4), ARG(5)));
}

static void k_CreateFileW(X86Ctx *ctx)
{
    char n[512];
    guest_w(ARG(0), n, sizeof n);
    RET(create_file(n, ARG(1), ARG(4), ARG(5)));
}

void rt_event_set(uint32_t h);

static void k_ReadFile(X86Ctx *ctx)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread = ARG(3), ovl = ARG(4);
    RtFile *f = rt_handle_data(h, RT_H_FILE);
    if (h == 0x10) { if (pread) st32(pread, 0); RET(1); return; } /* stdin: empty */
    if (!f || f->fd < 0) { set_error(6); RET(0); return; }
    if (ovl) host_seek(f->fd, (int64_t)(((uint64_t)ld32(ovl + 12) << 32) | ld32(ovl + 8)), 0);
    static const char *hitch;
    static int hitch_read;
    if (!hitch_read) { hitch = getenv("ME_PC_HITCH"); hitch_read = 1; }
    uint64_t io_start = hitch ? host_ticks_ns() : 0;
    int64_t r = n ? host_read(f->fd, rt_ptr(buf), n) : 0;
    if (hitch) {
        /* Per second, not synchronised: a rough picture of the streaming load. */
        static uint64_t window, bytes, longest, total;
        static uint32_t reads;
        uint64_t end = host_ticks_ns(), took = end - io_start;
        bytes += r > 0 ? (uint64_t)r : 0; reads++; total += took;
        if (took > longest) longest = took;
        if (end - window >= 1000000000ull) {
            if (window && bytes) rt_log("io %.1f MB in %u reads, read time %.0f ms, longest %.1f ms", bytes / 1e6, reads, total / 1e6, longest / 1e6);
            window = end; bytes = longest = total = 0; reads = 0;
        }
    }
    if (r < 0) { set_error(30); RET(0); return; }
    if (pread) st32(pread, (uint32_t)r);
    if (ovl) {
        st32(ovl, 0);
        st32(ovl + 4, (uint32_t)r);
        if (ld32(ovl + 16)) rt_event_set(ld32(ovl + 16));
    }
    RET(1);
}

static void k_WriteFile(X86Ctx *ctx)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3), ovl = ARG(4);
    if (h == 0x11 || h == 0x12) {
        char line[2048];
        uint32_t c = n < sizeof line - 1 ? n : (uint32_t)sizeof line - 1;
        memcpy(line, rt_ptr(buf), c);
        line[c] = 0;
        rt_log("guest %s: %s", h == 0x11 ? "stdout" : "stderr", line);
        if (pw) st32(pw, n);
        RET(1);
        return;
    }
    RtFile *f = rt_handle_data(h, RT_H_FILE);
    if (!f || f->fd < 0) { set_error(6); RET(0); return; }
    if (ovl) host_seek(f->fd, (int64_t)(((uint64_t)ld32(ovl + 12) << 32) | ld32(ovl + 8)), 0);
    int64_t r = n ? host_write(f->fd, rt_ptr(buf), n) : 0;
    if (r < 0) { set_error(29); RET(0); return; }
    if (pw) st32(pw, (uint32_t)r);
    if (ovl) { st32(ovl, 0); st32(ovl + 4, (uint32_t)r); if (ld32(ovl + 16)) rt_event_set(ld32(ovl + 16)); }
    RET(1);
}

static void k_SetFilePointer(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    uint32_t phi = ARG(2);
    if (!f || f->fd < 0) { set_error(6); RET(0xFFFFFFFFu); return; }
    int64_t d = phi ? (int64_t)(((uint64_t)ld32(phi) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1);
    int64_t r = host_seek(f->fd, d, (int)ARG(3));
    if (r < 0) { set_error(131); RET(0xFFFFFFFFu); return; }
    if (phi) st32(phi, (uint32_t)((uint64_t)r >> 32));
    set_error(0);
    RET((uint32_t)r);
}

static void k_SetFilePointerEx(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    if (!f || f->fd < 0) { set_error(6); RET(0); return; }
    int64_t d = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    int64_t r = host_seek(f->fd, d, (int)ARG(4));
    if (r < 0) { set_error(131); RET(0); return; }
    if (ARG(3)) { st32(ARG(3), (uint32_t)r); st32(ARG(3) + 4, (uint32_t)((uint64_t)r >> 32)); }
    RET(1);
}

static int64_t file_size(RtFile *f)
{
    int64_t cur = host_seek(f->fd, 0, 1), end = host_seek(f->fd, 0, 2);
    host_seek(f->fd, cur, 0);
    return end;
}

static void k_GetFileSize(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    if (!f || f->fd < 0) { set_error(6); RET(0xFFFFFFFFu); return; }
    int64_t s = file_size(f);
    if (ARG(1)) st32(ARG(1), (uint32_t)((uint64_t)s >> 32));
    RET((uint32_t)s);
}

static void k_GetFileSizeEx(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    if (!f || f->fd < 0) { set_error(6); RET(0); return; }
    int64_t s = file_size(f);
    st32(ARG(1), (uint32_t)s);
    st32(ARG(1) + 4, (uint32_t)((uint64_t)s >> 32));
    RET(1);
}

static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)tolower((unsigned char)*s)) * 16777619u;
    return h;
}

static void k_GetFileInformationByHandle(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    uint32_t p = ARG(1);
    if (!f) { set_error(6); RET(0); return; }
    int64_t size = 0;
    uint64_t ft = 0;
    host_stat(f->host, &size, &ft);
    if (f->fd >= 0) size = file_size(f);
    memset(rt_ptr(p), 0, 52);
    st32(p, f->is_dir ? 0x10 : 0x20);
    for (int k = 0; k < 3; k++) { st32(p + 4 + 8 * k, (uint32_t)ft); st32(p + 8 + 8 * k, (uint32_t)(ft >> 32)); }
    st32(p + 28, 0x4D452050u);     /* volume serial */
    st32(p + 32, (uint32_t)((uint64_t)size >> 32));
    st32(p + 36, (uint32_t)size);
    st32(p + 40, 1);               /* links */
    st32(p + 48, path_hash(f->guest));
    RET(1);
}

static void k_GetFileTime(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    if (!f) { set_error(6); RET(0); return; }
    uint64_t ft = 0;
    host_stat(f->host, NULL, &ft);
    for (int k = 1; k <= 3; k++)
        if (ARG(k)) { st32(ARG(k), (uint32_t)ft); st32(ARG(k) + 4, (uint32_t)(ft >> 32)); }
    RET(1);
}

static void k_FlushFileBuffers(X86Ctx *ctx) { RET(1); }

static void k_SetEndOfFile(X86Ctx *ctx)
{
    RtFile *f = rt_handle_data(ARG(0), RT_H_FILE);
    if (!f || f->fd < 0) { set_error(6); RET(0); return; }
    RET(host_truncate(f->fd, host_seek(f->fd, 0, 1)));
}

/* ---- names ---- */
static uint32_t attributes(const char *name)
{
    char g[512], hp[1024];
    canonical(name, g, sizeof g);
    int k = read_path(g, hp, sizeof hp);
    if (!k) { set_error(2); return 0xFFFFFFFFu; }
    return k == 2 ? 0x10u : 0x20u;
}

static void k_GetFileAttributesA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(attributes(n)); }
static void k_GetFileAttributesW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(attributes(n)); }

static uint32_t attributes_ex(const char *name, uint32_t p)
{
    char g[512], hp[1024];
    canonical(name, g, sizeof g);
    int k = read_path(g, hp, sizeof hp);
    if (!k) { set_error(2); return 0; }
    int64_t size = 0;
    uint64_t ft = 0;
    host_stat(hp, &size, &ft);
    memset(rt_ptr(p), 0, 36);
    st32(p, k == 2 ? 0x10 : 0x20);
    for (int i = 0; i < 3; i++) { st32(p + 4 + 8 * i, (uint32_t)ft); st32(p + 8 + 8 * i, (uint32_t)(ft >> 32)); }
    st32(p + 28, (uint32_t)((uint64_t)size >> 32));
    st32(p + 32, (uint32_t)size);
    return 1;
}

static void k_GetFileAttributesExA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(attributes_ex(n, ARG(2))); }
static void k_GetFileAttributesExW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(attributes_ex(n, ARG(2))); }
static void k_SetFileAttributes(X86Ctx *ctx) { RET(1); }

static uint32_t create_dir(const char *name)
{
    char g[512], hp[1024];
    canonical(name, g, sizeof g);
    if (read_path(g, hp, sizeof hp)) { set_error(183); return 0; }
    overlay_path(g, hp, sizeof hp);
    mkdirs_for(hp);
    if (!host_mkdir(hp)) { set_error(3); return 0; }
    rt_log("CreateDirectory \"%s\" -> %s: %s", name, g, hp);
    return 1;
}

static void k_CreateDirectoryA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(create_dir(n)); }
static void k_CreateDirectoryW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(create_dir(n)); }

static uint32_t remove_name(const char *name, int dir)
{
    char g[512], op[1024];
    canonical(name, g, sizeof g);
    overlay_path(g, op, sizeof op);
    if (host_stat(op, NULL, NULL)) return dir ? host_rmdir(op) : host_unlink(op);
    char gp[1024];
    if (game_path(g, gp, sizeof gp) && host_stat(gp, NULL, NULL)) {
        rt_log("delete of %s inside the game copy ignored", g);
        return 1;
    }
    set_error(2);
    return 0;
}

static void k_DeleteFileA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(remove_name(n, 0)); }
static void k_DeleteFileW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(remove_name(n, 0)); }
static void k_RemoveDirectoryA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(remove_name(n, 1)); }
static void k_RemoveDirectoryW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(remove_name(n, 1)); }

static uint32_t move_or_copy(const char *from, const char *to, int move, int fail_if_exists)
{
    char gf[512], gt[512], hf[1024], ht[1024];
    canonical(from, gf, sizeof gf);
    canonical(to, gt, sizeof gt);
    if (!read_path(gf, hf, sizeof hf)) { set_error(2); return 0; }
    if (fail_if_exists && read_path(gt, ht, sizeof ht)) { set_error(80); return 0; }
    overlay_path(gt, ht, sizeof ht);
    mkdirs_for(ht);
    char of[1024];
    overlay_path(gf, of, sizeof of);
    if (move && !strcmp(hf, of)) return host_rename(hf, ht);
    return copy_file(hf, ht);
}

static void k_MoveFileA(X86Ctx *ctx) { char a[512], b[512]; guest_a(ARG(0), a, sizeof a); guest_a(ARG(1), b, sizeof b); RET(move_or_copy(a, b, 1, 1)); }
static void k_MoveFileW(X86Ctx *ctx) { char a[512], b[512]; guest_w(ARG(0), a, sizeof a); guest_w(ARG(1), b, sizeof b); RET(move_or_copy(a, b, 1, 1)); }
static void k_CopyFileA(X86Ctx *ctx) { char a[512], b[512]; guest_a(ARG(0), a, sizeof a); guest_a(ARG(1), b, sizeof b); RET(move_or_copy(a, b, 0, ARG(2) != 0)); }
static void k_CopyFileW(X86Ctx *ctx) { char a[512], b[512]; guest_w(ARG(0), a, sizeof a); guest_w(ARG(1), b, sizeof b); RET(move_or_copy(a, b, 0, ARG(2) != 0)); }

/* ---- find ---- */
typedef struct { char dir_game[1024], dir_overlay[1024], pattern[256]; char (*names)[260]; int n, cap, pos; } RtFind;

static int wild(const char *p, const char *s)
{
    if (!strcmp(p, "*.*") || !strcmp(p, "*")) return 1;
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char *t = s;; t++) { if (wild(p + 1, t)) return 1; if (!*t) return 0; }
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}

static void add_name(void *u, const char *name)
{
    RtFind *f = u;
    if (!wild(f->pattern, name)) return;
    for (int i = 0; i < f->n; i++) if (!_stricmp(f->names[i], name)) return;
    if (f->n == f->cap) { f->cap = f->cap ? 2 * f->cap : 64; f->names = realloc(f->names, (size_t)f->cap * 260); }
    snprintf(f->names[f->n++], 260, "%s", name);
}

static int fill_find(RtFind *f, uint32_t data, int wide)
{
    if (f->pos >= f->n) return 0;
    const char *name = f->names[f->pos++];
    char hp[1200];
    snprintf(hp, sizeof hp, "%s/%s", f->dir_overlay, name);
    int64_t size = 0;
    uint64_t ft = 0;
    int k = host_stat(hp, &size, &ft);
    if (!k) { snprintf(hp, sizeof hp, "%s/%s", f->dir_game, name); k = host_stat(hp, &size, &ft); }
    memset(rt_ptr(data), 0, wide ? 592 : 320);
    st32(data, k == 2 ? 0x10 : 0x20);
    for (int i = 0; i < 3; i++) { st32(data + 4 + 8 * i, (uint32_t)ft); st32(data + 8 + 8 * i, (uint32_t)(ft >> 32)); }
    st32(data + 28, (uint32_t)((uint64_t)size >> 32));
    st32(data + 32, (uint32_t)size);
    if (wide) put_w(data + 44, name);
    else put_a(data + 44, name);
    return 1;
}

static uint32_t find_first(const char *pattern, uint32_t data, int wide)
{
    char g[512];
    canonical(pattern, g, sizeof g);
    char *slash = strrchr(g, '\\');
    RtFind *f = calloc(1, sizeof *f);
    snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1);
    *slash = 0;
    if (!strchr(g, '\\')) strcat(g, "\\");
    overlay_path(g, f->dir_overlay, sizeof f->dir_overlay);
    if (!game_path(g, f->dir_game, sizeof f->dir_game)) f->dir_game[0] = 0;
    host_list_dir(f->dir_overlay, add_name, f);
    if (f->dir_game[0]) host_list_dir(f->dir_game, add_name, f);
    if (log_reads) rt_log("FindFirstFile %s\\%s: %d entries", g, f->pattern, f->n);
    if (!fill_find(f, data, wide)) { free(f->names); free(f); set_error(2); return INVALID_HANDLE; }
    return rt_handle_new(RT_H_FIND, f);
}

static void k_FindFirstFileA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(find_first(n, ARG(1), 0)); }
static void k_FindFirstFileW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(find_first(n, ARG(1), 1)); }

static void find_next(X86Ctx *ctx, int wide)
{
    RtFind *f = rt_handle_data(ARG(0), RT_H_FIND);
    if (!f || !fill_find(f, ARG(1), wide)) { set_error(18); RET(0); return; } /* ERROR_NO_MORE_FILES */
    RET(1);
}

static void k_FindNextFileA(X86Ctx *ctx) { find_next(ctx, 0); }
static void k_FindNextFileW(X86Ctx *ctx) { find_next(ctx, 1); }

static void k_FindClose(X86Ctx *ctx)
{
    RtFind *f = rt_handle_data(ARG(0), RT_H_FIND);
    if (f) { free(f->names); free(f); rt_handle_close(ARG(0)); }
    RET(1);
}

/* ---- full paths, disks, folders ---- */
static uint32_t full_path(const char *name, uint32_t n, uint32_t buf, uint32_t filepart, int wide)
{
    char g[512];
    canonical(name, g, sizeof g);
    uint32_t len = (uint32_t)strlen(g);
    if (n <= len) return len + 1;
    if (wide) put_w(buf, g);
    else put_a(buf, g);
    if (filepart) {
        const char *last = strrchr(g, '\\');
        st32(filepart, last && last[1] ? buf + (uint32_t)(last + 1 - g) * (wide ? 2u : 1u) : 0);
    }
    return len;
}

static void k_GetFullPathNameA(X86Ctx *ctx) { char n[512]; guest_a(ARG(0), n, sizeof n); RET(full_path(n, ARG(1), ARG(2), ARG(3), 0)); }
static void k_GetFullPathNameW(X86Ctx *ctx) { char n[512]; guest_w(ARG(0), n, sizeof n); RET(full_path(n, ARG(1), ARG(2), ARG(3), 1)); }

static void k_GetDiskFreeSpaceEx(X86Ctx *ctx)
{
    uint64_t v = 20ull << 30;
    for (int k = 1; k <= 3; k++)
        if (ARG(k)) { st32(ARG(k), (uint32_t)v); st32(ARG(k) + 4, (uint32_t)(v >> 32)); }
    RET(1);
}

static void k_GetDriveType(X86Ctx *ctx) { RET(3); /* DRIVE_FIXED */ }

static void k_GetTempPathA(X86Ctx *ctx)
{
    const char *d = GUEST_ROOT "\\Temp\\";
    uint32_t len = (uint32_t)strlen(d);
    if (ARG(0) <= len) { RET(len + 1); return; }
    put_a(ARG(1), d);
    RET(len);
}

static void k_SHGetFolderPathW(X86Ctx *ctx)
{
    uint32_t csidl = ARG(1) & 0xFF, out = ARG(4);
    char g[512];
    switch (csidl) {
    case 0x05: snprintf(g, sizeof g, "%s\\My Documents", GUEST_PROFILE); break;      /* CSIDL_PERSONAL */
    case 0x1A: snprintf(g, sizeof g, "%s\\Application Data", GUEST_PROFILE); break;  /* CSIDL_APPDATA */
    case 0x1C: snprintf(g, sizeof g, "%s\\Local Settings\\Application Data", GUEST_PROFILE); break;
    default: snprintf(g, sizeof g, "%s\\Folder%02X", GUEST_PROFILE, csidl); break;
    }
    char hp[1024];
    overlay_path(g, hp, sizeof hp);
    mkdirs_for(hp);
    host_mkdir(hp);
    put_w(out, g);
    rt_log("SHGetFolderPathW(%#x) = %s", csidl, g);
    RET(0);
}

/* ---- file mappings: guest memory; a file's contents are read in, writes are not flushed back ---- */
typedef struct { char name[128]; uint32_t size, view; uint32_t handle; int fd; int64_t file_size; } RtMapping;
static RtMapping *mappings[64];

static uint32_t create_mapping(uint32_t file, uint32_t size_hi, uint32_t size_lo, const char *name)
{
    if (name && *name)
        for (int i = 0; i < 64; i++)
            if (mappings[i] && !strcmp(mappings[i]->name, name)) { set_error(183); return mappings[i]->handle; }
    RtMapping *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name ? name : "");
    m->fd = -1;
    m->size = size_lo;
    if (size_hi) { free(m); set_error(8); return 0; }
    if (file != INVALID_HANDLE) {
        RtFile *f = rt_handle_data(file, RT_H_FILE);
        if (!f || f->fd < 0) { free(m); set_error(6); return 0; }
        m->fd = f->fd;
        m->file_size = file_size(f);
        if (!m->size) m->size = (uint32_t)m->file_size;
    }
    m->handle = rt_handle_new(RT_H_MAPPING, m);
    for (int i = 0; i < 64; i++) if (!mappings[i]) { mappings[i] = m; break; }
    rt_log("CreateFileMapping \"%s\" %u bytes%s -> %#x", m->name, m->size, m->fd >= 0 ? " (file)" : "", m->handle);
    set_error(0);
    return m->handle;
}

static void k_CreateFileMappingA(X86Ctx *ctx)
{
    char n[128];
    guest_a(ARG(5), n, sizeof n);
    RET(create_mapping(ARG(0), ARG(3), ARG(4), ARG(5) ? n : NULL));
}

static void k_CreateFileMappingW(X86Ctx *ctx)
{
    char n[128];
    guest_w(ARG(5), n, sizeof n);
    RET(create_mapping(ARG(0), ARG(3), ARG(4), ARG(5) ? n : NULL));
}

static void k_OpenFileMapping(X86Ctx *ctx) { set_error(2); RET(0); }

static void k_MapViewOfFile(X86Ctx *ctx)
{
    RtMapping *m = rt_handle_data(ARG(0), RT_H_MAPPING);
    if (!m) { set_error(6); RET(0); return; }
    if (!m->view) {
        m->view = rt_vm_reserve(0, m->size);
        if (!m->view || !rt_vm_commit(m->view, m->size)) { set_error(8); RET(0); return; }
        if (m->fd >= 0) {
            int64_t cur = host_seek(m->fd, 0, 1);
            host_seek(m->fd, 0, 0);
            host_read(m->fd, rt_ptr(m->view), (uint32_t)(m->file_size < m->size ? m->file_size : m->size));
            host_seek(m->fd, cur, 0);
        }
    }
    RET(m->view + ARG(2)); /* offset low */
}

static void k_UnmapViewOfFile(X86Ctx *ctx) { RET(1); } /* the view stays until the process ends */

/* ---- registry: empty; the game falls back to its defaults ---- */
static void a_RegOpenKeyEx(X86Ctx *ctx) { RET(2); }
static void a_RegCloseKey(X86Ctx *ctx) { RET(0); }
static void a_RegQueryValueEx(X86Ctx *ctx) { RET(2); }
static void a_RegSetValueEx(X86Ctx *ctx) { RET(0); }

static void a_GetUserNameW(X86Ctx *ctx)
{
    put_w(ARG(0), "Runner");
    st32(ARG(1), 7);
    RET(1);
}

static void a_GetUserNameA(X86Ctx *ctx)
{
    put_a(ARG(0), "Runner");
    st32(ARG(1), 7);
    RET(1);
}

static void k_GetEnvironmentVariableW(X86Ctx *ctx) { set_error(203); RET(0); }

/* The guest's Windows folders (nothing is read from them: system DLLs are HLE). Returns the
   length, or the size needed when the buffer is too small. */
static void dir_result(X86Ctx *ctx, const char *path)
{
    uint32_t buf = rt_arg(ctx, 0), n = rt_arg(ctx, 1), len = (uint32_t)strlen(path);
    if (!buf || n <= len) { ctx->eax = len + 1; return; }
    memcpy(rt_ptr(buf), path, len + 1);
    ctx->eax = len;
}

static void k_GetWindowsDirectoryA(X86Ctx *ctx) { dir_result(ctx, "C:\\WINDOWS"); }
static void k_GetSystemDirectoryA(X86Ctx *ctx) { dir_result(ctx, "C:\\WINDOWS\\system32"); }

static const HleDef file_defs[] = {
    {"kernel32.dll", "CreateFileA", k_CreateFileA, 7},
    {"kernel32.dll", "CreateFileW", k_CreateFileW, 7},
    {"kernel32.dll", "ReadFile", k_ReadFile, 5},
    {"kernel32.dll", "WriteFile", k_WriteFile, 5},
    {"kernel32.dll", "SetFilePointer", k_SetFilePointer, 4},
    {"kernel32.dll", "SetFilePointerEx", k_SetFilePointerEx, 5},
    {"kernel32.dll", "GetFileSize", k_GetFileSize, 2},
    {"kernel32.dll", "GetFileSizeEx", k_GetFileSizeEx, 2},
    {"kernel32.dll", "GetFileInformationByHandle", k_GetFileInformationByHandle, 2},
    {"kernel32.dll", "GetFileTime", k_GetFileTime, 4},
    {"kernel32.dll", "FlushFileBuffers", k_FlushFileBuffers, 1},
    {"kernel32.dll", "SetEndOfFile", k_SetEndOfFile, 1},
    {"kernel32.dll", "GetFileAttributesA", k_GetFileAttributesA, 1},
    {"kernel32.dll", "GetFileAttributesW", k_GetFileAttributesW, 1},
    {"kernel32.dll", "GetFileAttributesExA", k_GetFileAttributesExA, 3},
    {"kernel32.dll", "GetFileAttributesExW", k_GetFileAttributesExW, 3},
    {"kernel32.dll", "SetFileAttributesA", k_SetFileAttributes, 2},
    {"kernel32.dll", "SetFileAttributesW", k_SetFileAttributes, 2},
    {"kernel32.dll", "CreateDirectoryA", k_CreateDirectoryA, 2},
    {"kernel32.dll", "CreateDirectoryW", k_CreateDirectoryW, 2},
    {"kernel32.dll", "RemoveDirectoryA", k_RemoveDirectoryA, 1},
    {"kernel32.dll", "RemoveDirectoryW", k_RemoveDirectoryW, 1},
    {"kernel32.dll", "DeleteFileA", k_DeleteFileA, 1},
    {"kernel32.dll", "DeleteFileW", k_DeleteFileW, 1},
    {"kernel32.dll", "MoveFileA", k_MoveFileA, 2},
    {"kernel32.dll", "MoveFileW", k_MoveFileW, 2},
    {"kernel32.dll", "CopyFileA", k_CopyFileA, 3},
    {"kernel32.dll", "CopyFileW", k_CopyFileW, 3},
    {"kernel32.dll", "FindFirstFileA", k_FindFirstFileA, 2},
    {"kernel32.dll", "FindFirstFileW", k_FindFirstFileW, 2},
    {"kernel32.dll", "FindNextFileA", k_FindNextFileA, 2},
    {"kernel32.dll", "FindNextFileW", k_FindNextFileW, 2},
    {"kernel32.dll", "FindClose", k_FindClose, 1},
    {"kernel32.dll", "GetFullPathNameA", k_GetFullPathNameA, 4},
    {"kernel32.dll", "GetFullPathNameW", k_GetFullPathNameW, 4},
    {"kernel32.dll", "GetDiskFreeSpaceExA", k_GetDiskFreeSpaceEx, 4},
    {"kernel32.dll", "GetDiskFreeSpaceExW", k_GetDiskFreeSpaceEx, 4},
    {"kernel32.dll", "GetDriveTypeA", k_GetDriveType, 1},
    {"kernel32.dll", "GetDriveTypeW", k_GetDriveType, 1},
    {"kernel32.dll", "GetTempPathA", k_GetTempPathA, 2},
    {"kernel32.dll", "GetWindowsDirectoryA", k_GetWindowsDirectoryA, 2},
    {"kernel32.dll", "GetSystemDirectoryA", k_GetSystemDirectoryA, 2},
    {"kernel32.dll", "GetEnvironmentVariableW", k_GetEnvironmentVariableW, 3},
    {"shell32.dll", "SHGetFolderPathW", k_SHGetFolderPathW, 5},
    {"kernel32.dll", "CreateFileMappingA", k_CreateFileMappingA, 6},
    {"kernel32.dll", "CreateFileMappingW", k_CreateFileMappingW, 6},
    {"kernel32.dll", "OpenFileMappingA", k_OpenFileMapping, 3},
    {"kernel32.dll", "OpenFileMappingW", k_OpenFileMapping, 3},
    {"kernel32.dll", "MapViewOfFile", k_MapViewOfFile, 5},
    {"kernel32.dll", "UnmapViewOfFile", k_UnmapViewOfFile, 1},
    {"advapi32.dll", "RegOpenKeyExA", a_RegOpenKeyEx, 5},
    {"advapi32.dll", "RegOpenKeyExW", a_RegOpenKeyEx, 5},
    {"advapi32.dll", "RegCloseKey", a_RegCloseKey, 1},
    {"advapi32.dll", "RegQueryValueExA", a_RegQueryValueEx, 6},
    {"advapi32.dll", "RegQueryValueExW", a_RegQueryValueEx, 6},
    {"advapi32.dll", "RegSetValueExA", a_RegSetValueEx, 6},
    {"advapi32.dll", "GetUserNameW", a_GetUserNameW, 2},
    {"advapi32.dll", "GetUserNameA", a_GetUserNameA, 2},
};

void rt_files_register(void) { rt_hle_register(file_defs, sizeof file_defs / sizeof *file_defs); }
