/* OpenAL 1.1 (+ the EFX object API) implemented by the runtime, with its own software mixer.
   The game loads OpenAL32.dll with LoadLibrary and takes every entry point by name, so the HLE
   set below is all it sees; on Windows these functions are cdecl (the caller pops).

   Model: buffers hold 16-bit PCM in host memory; a source plays a queue of buffers (a static
   buffer is a queue of one) with gain, pitch (linear interpolation), looping, the distance models
   of the spec and a stereo pan for mono sources from their direction in listener space. Stereo
   buffers play as they are. EFX: effects, filters and auxiliary slots are kept as objects; a
   source's direct low-pass filter is applied (gain, and a one-pole low-pass for GAINHF);
   auxiliary sends (reverb and other effects) are accepted and not rendered. No EAX, no capture.
   The mix runs on a host audio thread (rt_host_*: waveOut on Windows). */
#include "rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARG(i) rt_arg(ctx, i)
#define RET(v) (ctx->eax = (uint32_t)(v))

/* host audio output (rt_host_win.c, later the Android layer) */
int host_audio_start(uint32_t rate, void (*mix)(int16_t *out, uint32_t frames));

enum {
    AL_NO_ERROR = 0, AL_INVALID_NAME = 0xA001, AL_INVALID_ENUM = 0xA002, AL_INVALID_VALUE = 0xA003,
    AL_INVALID_OPERATION = 0xA004,
    AL_SOURCE_RELATIVE = 0x202, AL_CONE_INNER_ANGLE = 0x1001, AL_CONE_OUTER_ANGLE = 0x1002, AL_PITCH = 0x1003,
    AL_POSITION = 0x1004, AL_DIRECTION = 0x1005, AL_VELOCITY = 0x1006, AL_LOOPING = 0x1007, AL_BUFFER = 0x1009,
    AL_GAIN = 0x100A, AL_MIN_GAIN = 0x100D, AL_MAX_GAIN = 0x100E, AL_ORIENTATION = 0x100F,
    AL_SOURCE_STATE = 0x1010, AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013, AL_STOPPED = 0x1014,
    AL_BUFFERS_QUEUED = 0x1015, AL_BUFFERS_PROCESSED = 0x1016,
    AL_REFERENCE_DISTANCE = 0x1020, AL_ROLLOFF_FACTOR = 0x1021, AL_CONE_OUTER_GAIN = 0x1022, AL_MAX_DISTANCE = 0x1023,
    AL_SEC_OFFSET = 0x1024, AL_SAMPLE_OFFSET = 0x1025, AL_BYTE_OFFSET = 0x1026, AL_SOURCE_TYPE = 0x1027,
    AL_STATIC = 0x1028, AL_STREAMING = 0x1029, AL_UNDETERMINED = 0x1030,
    AL_FORMAT_MONO8 = 0x1100, AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO8 = 0x1102, AL_FORMAT_STEREO16 = 0x1103,
    AL_FREQUENCY = 0x2001, AL_BITS = 0x2002, AL_CHANNELS = 0x2003, AL_SIZE = 0x2004,
    AL_VENDOR = 0xB001, AL_VERSION = 0xB002, AL_RENDERER = 0xB003, AL_EXTENSIONS = 0xB004,
    AL_DOPPLER_FACTOR = 0xC000, AL_DOPPLER_VELOCITY = 0xC001, AL_SPEED_OF_SOUND = 0xC003,
    AL_DISTANCE_MODEL = 0xD000, AL_INVERSE_DISTANCE = 0xD001, AL_INVERSE_DISTANCE_CLAMPED = 0xD002,
    AL_LINEAR_DISTANCE = 0xD003, AL_LINEAR_DISTANCE_CLAMPED = 0xD004, AL_EXPONENT_DISTANCE = 0xD005,
    AL_EXPONENT_DISTANCE_CLAMPED = 0xD006,
    AL_DIRECT_FILTER = 0x20005, AL_AUXILIARY_SEND_FILTER = 0x20006,
    AL_FILTER_TYPE = 0x8001, AL_FILTER_LOWPASS = 0x0001, AL_LOWPASS_GAIN = 0x0001, AL_LOWPASS_GAINHF = 0x0002,
    ALC_MAJOR_VERSION = 0x1000, ALC_MINOR_VERSION = 0x1001, ALC_ATTRIBUTES_SIZE = 0x1002, ALC_ALL_ATTRIBUTES = 0x1003,
    ALC_DEFAULT_DEVICE_SPECIFIER = 0x1004, ALC_DEVICE_SPECIFIER = 0x1005, ALC_EXTENSIONS = 0x1006,
    ALC_FREQUENCY = 0x1007, ALC_REFRESH = 0x1008, ALC_SYNC = 0x1009, ALC_MONO_SOURCES = 0x1010,
    ALC_STEREO_SOURCES = 0x1011, ALC_DEFAULT_ALL_DEVICES_SPECIFIER = 0x1012, ALC_ALL_DEVICES_SPECIFIER = 0x1013,
    ALC_CAPTURE_DEVICE_SPECIFIER = 0x310, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER = 0x311,
    ALC_EFX_MAJOR_VERSION = 0x20001, ALC_EFX_MINOR_VERSION = 0x20002, ALC_MAX_AUXILIARY_SENDS = 0x20003,
};

#define OUT_RATE 48000u
#define MAX_OBJ 1024
#define MAX_QUEUE 64

typedef struct { int used; int16_t *pcm; uint32_t frames, channels, freq, bytes, bits; } Buffer;
typedef struct {
    int used, state, looping, relative;
    float gain, min_gain, max_gain, pitch, ref_dist, max_dist, rolloff, pos[3], vel[3], dir[3];
    uint32_t queue[MAX_QUEUE], nqueue, cur, processed, type, filter;
    double frac;         /* position in the current buffer, in its frames */
    float lp[2];         /* low-pass state per output channel */
} Source;
typedef struct { int used; uint32_t type; float gain, gainhf; } Filter;
typedef struct { int used; uint32_t type; } Effect;

static Buffer buffers[MAX_OBJ];
static Source sources[MAX_OBJ];
static Filter filters[MAX_OBJ];
static Effect effects[MAX_OBJ];
static int slots[MAX_OBJ];
static struct { float gain, pos[3], vel[3], at[3], up[3]; } listener = {1, {0}, {0}, {0, 0, -1}, {0, 1, 0}};
static uint32_t distance_model = AL_INVERSE_DISTANCE_CLAMPED, al_error, started;
static HostMutex *al_lock;
static uint32_t str_vendor, str_version, str_renderer, str_ext, str_dev, str_devs, str_alc_ext, str_empty;
static uint32_t device_obj, context_obj, current_ctx;

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u32f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void set_error(uint32_t e) { if (!al_error) al_error = e; }

static uint32_t guest_str(const char *s, size_t len)
{
    uint32_t g = rt_heap_alloc((uint32_t)len, 1);
    memcpy(rt_ptr(g), s, len);
    return g;
}

static void init_once(void)
{
    if (str_vendor) return;
    str_vendor = guest_str("RunnerRecompiled", 17);
    str_version = guest_str("1.1 RunnerRecompiled mixer", 27);
    str_renderer = guest_str("Software", 9);
    str_ext = guest_str("", 1);
    str_dev = guest_str("Generic Software", 17);
    str_devs = guest_str("Generic Software\0", 18); /* double-terminated list */
    str_alc_ext = guest_str("ALC_EXT_EFX", 12);
    str_empty = guest_str("\0", 2);
    device_obj = rt_heap_alloc(16, 1);
    context_obj = rt_heap_alloc(16, 1);
}

/* ---- the mixer (host audio thread) ---- */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float distance_gain(const Source *s, float dist)
{
    float ref = s->ref_dist, mx = s->max_dist, ro = s->rolloff;
    switch (distance_model) {
    case AL_INVERSE_DISTANCE_CLAMPED: dist = clampf(dist, ref, mx); /* fall through */
    case AL_INVERSE_DISTANCE: return ref + ro * (dist - ref) > 0 ? ref / (ref + ro * (dist - ref)) : 1;
    case AL_LINEAR_DISTANCE_CLAMPED: dist = clampf(dist, ref, mx); /* fall through */
    case AL_LINEAR_DISTANCE: return mx > ref ? clampf(1 - ro * (dist - ref) / (mx - ref), 0, 1) : 1;
    case AL_EXPONENT_DISTANCE_CLAMPED: dist = clampf(dist, ref, mx); /* fall through */
    case AL_EXPONENT_DISTANCE: return dist > 0 && ref > 0 ? powf(dist / ref, -ro) : 1;
    default: return 1; /* AL_NONE */
    }
}

static float mixbuf[2 * 4096];

static void mix_source(Source *s, uint32_t frames)
{
    Buffer *b = &buffers[s->queue[s->cur]];
    float gl, gr;
    /* gains: source gain clamped, distance, listener; pan for mono sources */
    float g = clampf(s->gain, s->min_gain, s->max_gain) * listener.gain;
    if (b->channels == 1) {
        float v[3];
        for (int k = 0; k < 3; k++) v[k] = s->pos[k] - (s->relative ? 0 : listener.pos[k]);
        float dist = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        g *= distance_gain(s, dist);
        /* right = at x up */
        const float *a = listener.at, *u = listener.up;
        float r[3] = {a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0]};
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        float pan = 0;
        if (dist > 1e-4f && rl > 1e-6f) pan = clampf((v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) / (dist * rl), -1, 1);
        gl = g * sqrtf((1 - pan) * 0.5f);
        gr = g * sqrtf((1 + pan) * 0.5f);
    } else {
        gl = gr = g;
    }
    float lpa = 1;
    if (s->filter && s->filter < MAX_OBJ && filters[s->filter].used && filters[s->filter].type == AL_FILTER_LOWPASS) {
        gl *= filters[s->filter].gain;
        gr *= filters[s->filter].gain;
        lpa = clampf(filters[s->filter].gainhf, 0.02f, 1);
    }
    for (uint32_t i = 0; i < frames; i++) {
        b = &buffers[s->queue[s->cur]];
        if (!b->used || !b->frames) { s->state = AL_STOPPED; return; }
        double step = (double)s->pitch * b->freq / OUT_RATE;
        uint32_t p = (uint32_t)s->frac;
        if (p >= b->frames) {
            /* end of this buffer: next in the queue, loop, or stop */
            s->frac -= b->frames;
            if (s->cur + 1 < s->nqueue) { s->cur++; s->processed++; i--; continue; }
            if (s->looping) { s->cur = 0; s->processed = 0; i--; continue; }
            s->processed = s->nqueue;
            s->state = AL_STOPPED;
            return;
        }
        float t = (float)(s->frac - p);
        uint32_t q = p + 1 < b->frames ? p + 1 : p;
        float l, r;
        if (b->channels == 1) {
            l = r = (b->pcm[p] + (b->pcm[q] - b->pcm[p]) * t) * (1.0f / 32768);
        } else {
            l = (b->pcm[2 * p] + (b->pcm[2 * q] - b->pcm[2 * p]) * t) * (1.0f / 32768);
            r = (b->pcm[2 * p + 1] + (b->pcm[2 * q + 1] - b->pcm[2 * p + 1]) * t) * (1.0f / 32768);
        }
        if (lpa < 1) { s->lp[0] += lpa * (l - s->lp[0]); s->lp[1] += lpa * (r - s->lp[1]); l = s->lp[0]; r = s->lp[1]; }
        mixbuf[2 * i] += l * gl;
        mixbuf[2 * i + 1] += r * gr;
        s->frac += step;
    }
}

static void mix(int16_t *out, uint32_t frames)
{
    if (frames > 4096) frames = 4096;
    memset(mixbuf, 0, sizeof(float) * 2 * frames);
    host_lock(al_lock);
    for (int i = 1; i < MAX_OBJ; i++)
        if (sources[i].used && sources[i].state == AL_PLAYING && sources[i].nqueue) mix_source(&sources[i], frames);
    host_unlock(al_lock);
    for (uint32_t i = 0; i < 2 * frames; i++) {
        float v = clampf(mixbuf[i], -1, 1) * 32767;
        out[i] = (int16_t)lrintf(v);
    }
    /* ME_PC_AUDIO_DUMP=<file>: the first 10 minutes of output as raw 48 kHz stereo s16 (checking
       the mixer without listening). */
    static FILE *dump;
    static uint64_t dumped;
    static int dump_checked;
    if (!dump_checked) { dump_checked = 1; const char *p = getenv("ME_PC_AUDIO_DUMP"); if (p) dump = fopen(p, "wb"); }
    if (dump && dumped < (uint64_t)OUT_RATE * 600) { fwrite(out, 4, frames, dump); fflush(dump); dumped += frames; }
}

/* ---- object helpers ---- */
static Source *src(uint32_t id) { return id && id < MAX_OBJ && sources[id].used ? &sources[id] : NULL; }
static Buffer *buf(uint32_t id) { return id && id < MAX_OBJ && buffers[id].used ? &buffers[id] : NULL; }

static void gen(X86Ctx *ctx, int *used, size_t stride, void (*init)(uint32_t))
{
    uint32_t n = ARG(0), out = ARG(1);
    host_lock(al_lock);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t id = 1;
        while (id < MAX_OBJ && *(int *)((char *)used + stride * id)) id++;
        if (id == MAX_OBJ) { set_error(AL_INVALID_VALUE); break; }
        *(int *)((char *)used + stride * id) = 1;
        if (init) init(id);
        st32(out + 4 * k, id);
    }
    host_unlock(al_lock);
}

static void del(X86Ctx *ctx, int *used, size_t stride, void (*fini)(uint32_t))
{
    uint32_t n = ARG(0), in = ARG(1);
    host_lock(al_lock);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t id = ld32(in + 4 * k);
        if (id && id < MAX_OBJ) {
            if (fini) fini(id);
            *(int *)((char *)used + stride * id) = 0;
        }
    }
    host_unlock(al_lock);
}

static void source_init(uint32_t id)
{
    Source *s = &sources[id];
    memset(s, 0, sizeof *s);
    s->used = 1; s->state = AL_INITIAL; s->gain = 1; s->max_gain = 1; s->pitch = 1; s->ref_dist = 1;
    s->max_dist = 3.4e38f; s->rolloff = 1; s->type = AL_UNDETERMINED;
}

static void buffer_fini(uint32_t id) { free(buffers[id].pcm); memset(&buffers[id], 0, sizeof buffers[id]); }
static void filter_init(uint32_t id) { filters[id].type = 0; filters[id].gain = 1; filters[id].gainhf = 1; }

/* ---- AL: objects ---- */
static void a_GenSources(X86Ctx *ctx) { gen(ctx, &sources[0].used, sizeof(Source), source_init); }
static void a_DeleteSources(X86Ctx *ctx) { del(ctx, &sources[0].used, sizeof(Source), NULL); }
static void a_IsSource(X86Ctx *ctx) { RET(src(ARG(0)) != NULL); }
static void a_GenBuffers(X86Ctx *ctx) { gen(ctx, &buffers[0].used, sizeof(Buffer), NULL); }
static void a_DeleteBuffers(X86Ctx *ctx) { del(ctx, &buffers[0].used, sizeof(Buffer), buffer_fini); }
static void a_IsBuffer(X86Ctx *ctx) { RET(ARG(0) == 0 || buf(ARG(0)) != NULL); }
static void a_GenFilters(X86Ctx *ctx) { gen(ctx, &filters[0].used, sizeof(Filter), filter_init); }
static void a_DeleteFilters(X86Ctx *ctx) { del(ctx, &filters[0].used, sizeof(Filter), NULL); }
static void a_IsFilter(X86Ctx *ctx) { RET(ARG(0) && ARG(0) < MAX_OBJ && filters[ARG(0)].used); }
static void a_GenEffects(X86Ctx *ctx) { gen(ctx, &effects[0].used, sizeof(Effect), NULL); }
static void a_DeleteEffects(X86Ctx *ctx) { del(ctx, &effects[0].used, sizeof(Effect), NULL); }
static void a_IsEffect(X86Ctx *ctx) { RET(ARG(0) && ARG(0) < MAX_OBJ && effects[ARG(0)].used); }
static void a_GenSlots(X86Ctx *ctx) { gen(ctx, &slots[0], sizeof(int), NULL); }
static void a_DeleteSlots(X86Ctx *ctx) { del(ctx, &slots[0], sizeof(int), NULL); }
static void a_IsSlot(X86Ctx *ctx) { RET(ARG(0) && ARG(0) < MAX_OBJ && slots[ARG(0)]); }

/* alBufferData(buffer, format, data, size, freq) */
static void a_BufferData(X86Ctx *ctx)
{
    uint32_t fmt = ARG(1), data = ARG(2), size = ARG(3), freq = ARG(4);
    uint32_t ch = fmt == AL_FORMAT_STEREO8 || fmt == AL_FORMAT_STEREO16 ? 2 : 1;
    uint32_t bits = fmt == AL_FORMAT_MONO8 || fmt == AL_FORMAT_STEREO8 ? 8 : 16;
    if (fmt < AL_FORMAT_MONO8 || fmt > AL_FORMAT_STEREO16) { set_error(AL_INVALID_ENUM); return; }
    uint32_t frames = size / (ch * bits / 8);
    int16_t *pcm = malloc((size_t)frames * ch * sizeof(int16_t) + 4);
    if (bits == 16) memcpy(pcm, rt_ptr(data), (size_t)frames * ch * 2);
    else for (uint32_t i = 0; i < frames * ch; i++) pcm[i] = (int16_t)((ld8(data + i) - 128) << 8);
    if (getenv("ME_PC_LOG_AL")) {
        static int logged;
        int peak = 0;
        for (uint32_t i = 0; i < frames * ch; i++) peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
        if (logged++ < 40) rt_log("OpenAL: buffer %u: %u frames x%u at %u Hz, peak %d", ARG(0), frames, ch, freq, peak);
    }
    host_lock(al_lock);
    Buffer *b = buf(ARG(0));
    if (b) {
        free(b->pcm);
        b->pcm = pcm; b->frames = frames; b->channels = ch; b->freq = freq ? freq : 22050; b->bytes = size; b->bits = bits;
    } else {
        free(pcm);
        set_error(AL_INVALID_NAME);
    }
    host_unlock(al_lock);
}

static uint32_t buffer_param(Buffer *b, uint32_t p)
{
    switch (p) {
    case AL_FREQUENCY: return b->freq;
    case AL_BITS: return b->bits;
    case AL_CHANNELS: return b->channels;
    case AL_SIZE: return b->bytes;
    default: set_error(AL_INVALID_ENUM); return 0;
    }
}

static void a_GetBufferi(X86Ctx *ctx)
{
    Buffer *b = buf(ARG(0));
    if (!b) { set_error(AL_INVALID_NAME); return; }
    if (ARG(2)) st32(ARG(2), buffer_param(b, ARG(1)));
}

static void a_GetBufferf(X86Ctx *ctx) { if (ARG(2)) st32(ARG(2), u32f(0)); set_error(AL_INVALID_ENUM); }
static void a_Ignore(X86Ctx *ctx) { (void)ctx; }

/* ---- AL: sources ---- */
static void source_set_queue(Source *s, uint32_t b)
{
    s->nqueue = b ? 1 : 0;
    s->queue[0] = b;
    s->cur = 0;
    s->processed = 0;
    s->frac = 0;
    s->type = b ? AL_STATIC : AL_UNDETERMINED;
}

static void source_f(Source *s, uint32_t p, const float *v)
{
    switch (p) {
    case AL_GAIN: s->gain = v[0]; break;
    case AL_MIN_GAIN: s->min_gain = v[0]; break;
    case AL_MAX_GAIN: s->max_gain = v[0]; break;
    case AL_PITCH: s->pitch = v[0] > 0 ? v[0] : s->pitch; break;
    case AL_REFERENCE_DISTANCE: s->ref_dist = v[0]; break;
    case AL_MAX_DISTANCE: s->max_dist = v[0]; break;
    case AL_ROLLOFF_FACTOR: s->rolloff = v[0]; break;
    case AL_POSITION: memcpy(s->pos, v, 12); break;
    case AL_VELOCITY: memcpy(s->vel, v, 12); break;
    case AL_DIRECTION: memcpy(s->dir, v, 12); break;
    case AL_SEC_OFFSET: s->frac = s->nqueue ? v[0] * buffers[s->queue[s->cur]].freq : 0; break;
    case AL_CONE_INNER_ANGLE: case AL_CONE_OUTER_ANGLE: case AL_CONE_OUTER_GAIN: break;
    default: set_error(AL_INVALID_ENUM);
    }
}

static void source_i(Source *s, uint32_t p, const int32_t *v)
{
    switch (p) {
    case AL_LOOPING: s->looping = v[0] != 0; break;
    case AL_SOURCE_RELATIVE: s->relative = v[0] != 0; break;
    case AL_BUFFER:
        if (s->state == AL_PLAYING || s->state == AL_PAUSED) { set_error(AL_INVALID_OPERATION); break; }
        if (v[0] && !buf((uint32_t)v[0])) { set_error(AL_INVALID_VALUE); break; }
        source_set_queue(s, (uint32_t)v[0]);
        break;
    case AL_DIRECT_FILTER: s->filter = (uint32_t)v[0]; break;
    case AL_AUXILIARY_SEND_FILTER: break; /* sends are not rendered */
    case AL_POSITION: case AL_VELOCITY: case AL_DIRECTION: {
        float f[3] = {(float)v[0], (float)v[1], (float)v[2]};
        source_f(s, p, f);
        break;
    }
    case AL_SAMPLE_OFFSET: s->frac = v[0]; break;
    case AL_CONE_INNER_ANGLE: case AL_CONE_OUTER_ANGLE: break;
    default: { float f = (float)v[0]; source_f(s, p, &f); }
    }
}

#define WITH_SOURCE(body) \
    host_lock(al_lock); \
    Source *s = src(ARG(0)); \
    if (!s) set_error(AL_INVALID_NAME); else { body; } \
    host_unlock(al_lock)

static void a_Sourcef(X86Ctx *ctx) { float v = f32(ARG(2)); WITH_SOURCE(source_f(s, ARG(1), &v)); }
static void a_Source3f(X86Ctx *ctx) { float v[3] = {f32(ARG(2)), f32(ARG(3)), f32(ARG(4))}; WITH_SOURCE(source_f(s, ARG(1), v)); }
static void a_Sourcefv(X86Ctx *ctx) { float v[3]; memcpy(v, rt_ptr(ARG(2)), 12); WITH_SOURCE(source_f(s, ARG(1), v)); }
static void a_Sourcei(X86Ctx *ctx) { int32_t v = (int32_t)ARG(2); WITH_SOURCE(source_i(s, ARG(1), &v)); }
static void a_Source3i(X86Ctx *ctx) { int32_t v[3] = {(int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4)}; WITH_SOURCE(source_i(s, ARG(1), v)); }
static void a_Sourceiv(X86Ctx *ctx) { int32_t v[3]; memcpy(v, rt_ptr(ARG(2)), 12); WITH_SOURCE(source_i(s, ARG(1), v)); }

static int source_get_i(Source *s, uint32_t p, int32_t *out)
{
    switch (p) {
    case AL_SOURCE_STATE: *out = s->state; return 1;
    case AL_BUFFERS_QUEUED: *out = (int32_t)s->nqueue; return 1;
    case AL_BUFFERS_PROCESSED: *out = (int32_t)(s->state == AL_STOPPED ? s->nqueue : s->processed); return 1;
    case AL_BUFFER: *out = s->nqueue ? (int32_t)s->queue[s->cur] : 0; return 1;
    case AL_LOOPING: *out = s->looping; return 1;
    case AL_SOURCE_RELATIVE: *out = s->relative; return 1;
    case AL_SOURCE_TYPE: *out = (int32_t)s->type; return 1;
    case AL_SAMPLE_OFFSET: *out = (int32_t)s->frac; return 1;
    case AL_BYTE_OFFSET:
        *out = s->nqueue ? (int32_t)s->frac * (int32_t)(buffers[s->queue[s->cur]].channels * 2) : 0;
        return 1;
    case AL_DIRECT_FILTER: *out = (int32_t)s->filter; return 1;
    default: return 0;
    }
}

static int source_get_f(Source *s, uint32_t p, float *out)
{
    switch (p) {
    case AL_GAIN: out[0] = s->gain; return 1;
    case AL_MIN_GAIN: out[0] = s->min_gain; return 1;
    case AL_MAX_GAIN: out[0] = s->max_gain; return 1;
    case AL_PITCH: out[0] = s->pitch; return 1;
    case AL_REFERENCE_DISTANCE: out[0] = s->ref_dist; return 1;
    case AL_MAX_DISTANCE: out[0] = s->max_dist; return 1;
    case AL_ROLLOFF_FACTOR: out[0] = s->rolloff; return 1;
    case AL_POSITION: memcpy(out, s->pos, 12); return 3;
    case AL_VELOCITY: memcpy(out, s->vel, 12); return 3;
    case AL_DIRECTION: memcpy(out, s->dir, 12); return 3;
    case AL_SEC_OFFSET:
        out[0] = s->nqueue && buffers[s->queue[s->cur]].freq ? (float)(s->frac / buffers[s->queue[s->cur]].freq) : 0;
        return 1;
    default: return 0;
    }
}

static void a_GetSourcei(X86Ctx *ctx)
{
    int32_t v = 0;
    WITH_SOURCE(if (!source_get_i(s, ARG(1), &v)) { float f; if (source_get_f(s, ARG(1), &f)) v = (int32_t)f; else set_error(AL_INVALID_ENUM); });
    if (ARG(2)) st32(ARG(2), (uint32_t)v);
}

static void a_GetSourceiv(X86Ctx *ctx) { a_GetSourcei(ctx); }

static void a_GetSourcef(X86Ctx *ctx)
{
    float v[3] = {0};
    WITH_SOURCE(if (!source_get_f(s, ARG(1), v)) { int32_t i; if (source_get_i(s, ARG(1), &i)) v[0] = (float)i; else set_error(AL_INVALID_ENUM); });
    if (ARG(2)) st32(ARG(2), u32f(v[0]));
}

static void a_GetSourcefv(X86Ctx *ctx)
{
    float v[3] = {0};
    int n = 0;
    WITH_SOURCE(n = source_get_f(s, ARG(1), v); if (!n) set_error(AL_INVALID_ENUM));
    if (ARG(2)) for (int k = 0; k < (n ? n : 1); k++) st32(ARG(2) + 4 * (uint32_t)k, u32f(v[k]));
}

static void a_GetSource3f(X86Ctx *ctx)
{
    float v[3] = {0};
    WITH_SOURCE(if (!source_get_f(s, ARG(1), v)) set_error(AL_INVALID_ENUM));
    for (int k = 0; k < 3; k++) if (ARG(2 + k)) st32(ARG(2 + k), u32f(v[k]));
}

static void a_GetSource3i(X86Ctx *ctx)
{
    float v[3] = {0};
    WITH_SOURCE(if (!source_get_f(s, ARG(1), v)) set_error(AL_INVALID_ENUM));
    for (int k = 0; k < 3; k++) if (ARG(2 + k)) st32(ARG(2 + k), (uint32_t)(int32_t)v[k]);
}

static void play(Source *s)
{
    if (!s->nqueue) { s->state = AL_STOPPED; return; }
    if (s->state != AL_PAUSED) { s->cur = 0; s->processed = 0; s->frac = 0; s->lp[0] = s->lp[1] = 0; }
    s->state = AL_PLAYING;
}

static void stop(Source *s) { if (s->state != AL_INITIAL) { s->state = AL_STOPPED; s->processed = s->nqueue; } }
static void rewind_src(Source *s) { s->state = AL_INITIAL; s->cur = 0; s->processed = 0; s->frac = 0; }
static void pause_src(Source *s) { if (s->state == AL_PLAYING) s->state = AL_PAUSED; }

static void a_SourcePlay(X86Ctx *ctx) { WITH_SOURCE(play(s)); }
static void a_SourceStop(X86Ctx *ctx) { WITH_SOURCE(stop(s)); }
static void a_SourceRewind(X86Ctx *ctx) { WITH_SOURCE(rewind_src(s)); }
static void a_SourcePause(X86Ctx *ctx) { WITH_SOURCE(pause_src(s)); }

static void each_source(X86Ctx *ctx, void (*fn)(Source *))
{
    host_lock(al_lock);
    for (uint32_t k = 0; k < ARG(0); k++) {
        Source *s = src(ld32(ARG(1) + 4 * k));
        if (s) fn(s); else set_error(AL_INVALID_NAME);
    }
    host_unlock(al_lock);
}

static void a_SourcePlayv(X86Ctx *ctx) { each_source(ctx, play); }
static void a_SourceStopv(X86Ctx *ctx) { each_source(ctx, stop); }
static void a_SourceRewindv(X86Ctx *ctx) { each_source(ctx, rewind_src); }
static void a_SourcePausev(X86Ctx *ctx) { each_source(ctx, pause_src); }

/* alSourceQueueBuffers(source, n, buffers) */
static void a_SourceQueueBuffers(X86Ctx *ctx)
{
    WITH_SOURCE(
        for (uint32_t k = 0; k < ARG(1); k++) {
            uint32_t b = ld32(ARG(2) + 4 * k);
            if (!buf(b) || s->nqueue >= MAX_QUEUE) { set_error(AL_INVALID_VALUE); break; }
            if (s->type == AL_STATIC) { set_error(AL_INVALID_OPERATION); break; }
            s->queue[s->nqueue++] = b;
            s->type = AL_STREAMING;
            /* a stopped stream that gets new data resumes from the new buffers on Play */
        });
}

/* alSourceUnqueueBuffers(source, n, out): only processed buffers come off the front */
static void a_SourceUnqueueBuffers(X86Ctx *ctx)
{
    WITH_SOURCE(
        uint32_t n = ARG(1);
        uint32_t done = s->state == AL_STOPPED ? s->nqueue : s->processed;
        if (n > done) { set_error(AL_INVALID_VALUE); }
        else {
            for (uint32_t k = 0; k < n; k++) st32(ARG(2) + 4 * k, s->queue[k]);
            memmove(s->queue, s->queue + n, (s->nqueue - n) * sizeof s->queue[0]);
            s->nqueue -= n;
            if (s->state == AL_STOPPED) { s->cur = 0; s->processed = 0; s->frac = 0; }
            else { s->cur -= n; s->processed -= n; }
            if (!s->nqueue) s->type = AL_UNDETERMINED;
        });
}

/* ---- AL: listener and state ---- */
static void listener_f(uint32_t p, const float *v)
{
    switch (p) {
    case AL_GAIN: listener.gain = v[0]; break;
    case AL_POSITION: memcpy(listener.pos, v, 12); break;
    case AL_VELOCITY: memcpy(listener.vel, v, 12); break;
    case AL_ORIENTATION: memcpy(listener.at, v, 12); memcpy(listener.up, v + 3, 12); break;
    default: set_error(AL_INVALID_ENUM);
    }
}

static void a_Listenerf(X86Ctx *ctx) { float v = f32(ARG(1)); host_lock(al_lock); listener_f(ARG(0), &v); host_unlock(al_lock); }
static void a_Listener3f(X86Ctx *ctx)
{
    float v[3] = {f32(ARG(1)), f32(ARG(2)), f32(ARG(3))};
    host_lock(al_lock); listener_f(ARG(0), v); host_unlock(al_lock);
}
static void a_Listenerfv(X86Ctx *ctx)
{
    float v[6];
    memcpy(v, rt_ptr(ARG(1)), ARG(0) == AL_ORIENTATION ? 24 : ARG(0) == AL_GAIN ? 4 : 12);
    host_lock(al_lock); listener_f(ARG(0), v); host_unlock(al_lock);
}
static void a_Listeneri(X86Ctx *ctx) { float v = (float)(int32_t)ARG(1); host_lock(al_lock); listener_f(ARG(0), &v); host_unlock(al_lock); }

static void a_GetListenerf(X86Ctx *ctx) { if (ARG(0) == AL_GAIN && ARG(1)) st32(ARG(1), u32f(listener.gain)); else set_error(AL_INVALID_ENUM); }
static void a_GetListenerfv(X86Ctx *ctx)
{
    const float *v = ARG(0) == AL_POSITION ? listener.pos : ARG(0) == AL_VELOCITY ? listener.vel : listener.at;
    uint32_t n = ARG(0) == AL_ORIENTATION ? 6 : ARG(0) == AL_GAIN ? 1 : 3;
    if (ARG(0) == AL_GAIN) v = &listener.gain;
    for (uint32_t k = 0; k < n && ARG(1); k++) st32(ARG(1) + 4 * k, u32f(k < 3 ? v[k] : listener.up[k - 3]));
}
static void a_GetListener3f(X86Ctx *ctx)
{
    const float *v = ARG(0) == AL_POSITION ? listener.pos : listener.vel;
    for (int k = 0; k < 3; k++) if (ARG(1 + k)) st32(ARG(1 + k), u32f(v[k]));
}

static void a_DistanceModel(X86Ctx *ctx) { host_lock(al_lock); distance_model = ARG(0); host_unlock(al_lock); }

static uint32_t get_state(uint32_t p)
{
    switch (p) {
    case AL_DISTANCE_MODEL: return distance_model;
    case AL_DOPPLER_FACTOR: return 1;
    case AL_SPEED_OF_SOUND: return 343;
    default: set_error(AL_INVALID_ENUM); return 0;
    }
}

static void a_GetInteger(X86Ctx *ctx) { RET(get_state(ARG(0))); }
static void a_GetIntegerv(X86Ctx *ctx) { if (ARG(1)) st32(ARG(1), get_state(ARG(0))); }
static void a_GetFloat(X86Ctx *ctx) { x87_push(ctx, 0.0); set_error(AL_INVALID_ENUM); } /* float results come back in st(0) */
static void a_GetBoolean(X86Ctx *ctx) { RET(0); }
static void a_GetError(X86Ctx *ctx) { RET(al_error); al_error = 0; }
static void a_IsEnabled(X86Ctx *ctx) { RET(0); }

static void a_GetString(X86Ctx *ctx)
{
    init_once();
    switch (ARG(0)) {
    case AL_VENDOR: RET(str_vendor); break;
    case AL_VERSION: RET(str_version); break;
    case AL_RENDERER: RET(str_renderer); break;
    case AL_EXTENSIONS: RET(str_ext); break;
    default: set_error(AL_INVALID_ENUM); RET(0);
    }
}

static void a_IsExtensionPresent(X86Ctx *ctx) { RET(0); } /* no AL extensions, no EAX */

static const struct { const char *name; uint32_t v; } enums[] = {
    {"AL_DIRECT_FILTER", AL_DIRECT_FILTER}, {"AL_AUXILIARY_SEND_FILTER", AL_AUXILIARY_SEND_FILTER},
    {"AL_FILTER_TYPE", AL_FILTER_TYPE}, {"AL_FILTER_LOWPASS", AL_FILTER_LOWPASS}, {"AL_LOWPASS_GAIN", AL_LOWPASS_GAIN},
    {"AL_LOWPASS_GAINHF", AL_LOWPASS_GAINHF}, {"AL_EFFECT_TYPE", 0x8001}, {"AL_EFFECT_REVERB", 0x0001},
    {"AL_EFFECT_EAXREVERB", 0x8000}, {"AL_EFFECTSLOT_EFFECT", 0x0001}, {"AL_EFFECTSLOT_GAIN", 0x0002},
    {"AL_EFFECTSLOT_AUXILIARY_SEND_AUTO", 0x0003}, {"ALC_MAX_AUXILIARY_SENDS", ALC_MAX_AUXILIARY_SENDS},
    {"ALC_EFX_MAJOR_VERSION", ALC_EFX_MAJOR_VERSION}, {"ALC_EFX_MINOR_VERSION", ALC_EFX_MINOR_VERSION},
    {"AL_FORMAT_MONO16", AL_FORMAT_MONO16}, {"AL_FORMAT_STEREO16", AL_FORMAT_STEREO16},
};

static uint32_t enum_value(const char *name)
{
    for (size_t k = 0; name && k < sizeof enums / sizeof *enums; k++)
        if (!strcmp(name, enums[k].name)) return enums[k].v;
    return 0; /* unknown: e.g. AL_FORMAT_QUAD16, multichannel formats are not supported */
}

static void a_GetEnumValue(X86Ctx *ctx) { RET(enum_value(rt_str(ARG(0)))); }

/* ---- EFX parameters: filters are used by the mixer; effects and slots only stored ---- */
static void a_Filteri(X86Ctx *ctx)
{
    uint32_t id = ARG(0);
    if (!id || id >= MAX_OBJ || !filters[id].used) { set_error(AL_INVALID_NAME); return; }
    if (ARG(1) == AL_FILTER_TYPE) filters[id].type = ARG(2);
}

static void a_Filterf(X86Ctx *ctx)
{
    uint32_t id = ARG(0);
    if (!id || id >= MAX_OBJ || !filters[id].used) { set_error(AL_INVALID_NAME); return; }
    if (ARG(1) == AL_LOWPASS_GAIN) filters[id].gain = clampf(f32(ARG(2)), 0, 1);
    else if (ARG(1) == AL_LOWPASS_GAINHF) filters[id].gainhf = clampf(f32(ARG(2)), 0, 1);
}

static void a_Effecti(X86Ctx *ctx) { if (ARG(0) && ARG(0) < MAX_OBJ && ARG(1) == 0x8001) effects[ARG(0)].type = ARG(2); }
static void a_GetZero(X86Ctx *ctx) { if (ARG(2)) st32(ARG(2), 0); }

/* ---- ALC ---- */
static void c_OpenDevice(X86Ctx *ctx)
{
    init_once();
    if (!started) {
        started = host_audio_start(OUT_RATE, mix);
        rt_log("OpenAL: device \"%s\" opened, host output %s", ARG(0) ? rt_str(ARG(0)) : "(default)",
               started ? "started (48 kHz stereo)" : "not available: silent");
        started = 1;
    }
    RET(device_obj);
}

static void c_CloseDevice(X86Ctx *ctx) { RET(1); }
static void c_CreateContext(X86Ctx *ctx) { RET(context_obj); }
static void c_MakeContextCurrent(X86Ctx *ctx) { current_ctx = ARG(0); RET(1); }
static void c_GetCurrentContext(X86Ctx *ctx) { RET(current_ctx); }
static void c_GetContextsDevice(X86Ctx *ctx) { RET(device_obj); }
static void c_DestroyContext(X86Ctx *ctx) { if (current_ctx == ARG(0)) current_ctx = 0; }
static void c_GetError(X86Ctx *ctx) { RET(0); }
static void c_Null(X86Ctx *ctx) { RET(0); }

static void c_IsExtensionPresent(X86Ctx *ctx)
{
    const char *n = rt_str(ARG(1));
    RET(n && (!strcmp(n, "ALC_EXT_EFX") || !strcmp(n, "ALC_ENUMERATION_EXT") || !strcmp(n, "ALC_ENUMERATE_ALL_EXT")));
}

static void c_GetString(X86Ctx *ctx)
{
    init_once();
    switch (ARG(1)) {
    case ALC_DEFAULT_DEVICE_SPECIFIER: case ALC_DEFAULT_ALL_DEVICES_SPECIFIER: RET(str_dev); break;
    case ALC_DEVICE_SPECIFIER: case ALC_ALL_DEVICES_SPECIFIER: RET(ARG(0) ? str_dev : str_devs); break;
    case ALC_EXTENSIONS: RET(str_alc_ext); break;
    case ALC_CAPTURE_DEVICE_SPECIFIER: case ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER: RET(str_empty); break;
    default: RET(str_empty);
    }
}

/* alcGetIntegerv(device, param, size, out) */
static void c_GetIntegerv(X86Ctx *ctx)
{
    uint32_t p = ARG(1), n = ARG(2), out = ARG(3), v;
    if (!out || !n) return;
    switch (p) {
    case ALC_MAJOR_VERSION: v = 1; break;
    case ALC_MINOR_VERSION: v = 1; break;
    case ALC_EFX_MAJOR_VERSION: v = 1; break;
    case ALC_EFX_MINOR_VERSION: v = 0; break;
    case ALC_MAX_AUXILIARY_SENDS: v = 1; break;
    case ALC_FREQUENCY: v = OUT_RATE; break;
    case ALC_MONO_SOURCES: v = 255; break;
    case ALC_STEREO_SOURCES: v = 16; break;
    case ALC_ATTRIBUTES_SIZE: v = 1; break;
    case ALC_ALL_ATTRIBUTES: v = 0; break;
    default: v = 0;
    }
    st32(out, v);
}

static void c_GetEnumValue(X86Ctx *ctx) { RET(enum_value(rt_str(ARG(1)))); }

/* alGetProcAddress(name) / alcGetProcAddress(device, name): implemented functions only */
static void a_GetProcAddress(X86Ctx *ctx);
static void c_GetProcAddress(X86Ctx *ctx);

#define F(n, fn) {"openal32.dll", n, fn, 0}
static const HleDef al_defs[] = {
    F("alGenSources", a_GenSources), F("alDeleteSources", a_DeleteSources), F("alIsSource", a_IsSource),
    F("alGenBuffers", a_GenBuffers), F("alDeleteBuffers", a_DeleteBuffers), F("alIsBuffer", a_IsBuffer),
    F("alBufferData", a_BufferData), F("alGetBufferi", a_GetBufferi), F("alGetBufferf", a_GetBufferf),
    F("alBufferi", a_Ignore), F("alBufferf", a_Ignore),
    F("alSourcef", a_Sourcef), F("alSource3f", a_Source3f), F("alSourcefv", a_Sourcefv),
    F("alSourcei", a_Sourcei), F("alSource3i", a_Source3i), F("alSourceiv", a_Sourceiv),
    F("alGetSourcei", a_GetSourcei), F("alGetSourceiv", a_GetSourceiv), F("alGetSourcef", a_GetSourcef),
    F("alGetSourcefv", a_GetSourcefv), F("alGetSource3f", a_GetSource3f), F("alGetSource3i", a_GetSource3i),
    F("alSourcePlay", a_SourcePlay), F("alSourceStop", a_SourceStop), F("alSourceRewind", a_SourceRewind),
    F("alSourcePause", a_SourcePause), F("alSourcePlayv", a_SourcePlayv), F("alSourceStopv", a_SourceStopv),
    F("alSourceRewindv", a_SourceRewindv), F("alSourcePausev", a_SourcePausev),
    F("alSourceQueueBuffers", a_SourceQueueBuffers), F("alSourceUnqueueBuffers", a_SourceUnqueueBuffers),
    F("alListenerf", a_Listenerf), F("alListener3f", a_Listener3f), F("alListenerfv", a_Listenerfv),
    F("alListeneri", a_Listeneri), F("alListener3i", a_Ignore), F("alListeneriv", a_Ignore),
    F("alGetListenerf", a_GetListenerf), F("alGetListenerfv", a_GetListenerfv), F("alGetListener3f", a_GetListener3f),
    F("alGetListeneri", a_GetZero), F("alGetListener3i", a_Ignore), F("alGetListeneriv", a_GetZero),
    F("alDistanceModel", a_DistanceModel), F("alDopplerFactor", a_Ignore), F("alDopplerVelocity", a_Ignore),
    F("alSpeedOfSound", a_Ignore), F("alEnable", a_Ignore), F("alDisable", a_Ignore), F("alIsEnabled", a_IsEnabled),
    F("alGetInteger", a_GetInteger), F("alGetIntegerv", a_GetIntegerv), F("alGetFloat", a_GetFloat),
    F("alGetFloatv", a_GetIntegerv), F("alGetBoolean", a_GetBoolean), F("alGetBooleanv", a_GetIntegerv),
    F("alGetDouble", a_GetFloat), F("alGetDoublev", a_GetIntegerv),
    F("alGetError", a_GetError), F("alGetString", a_GetString), F("alIsExtensionPresent", a_IsExtensionPresent),
    F("alGetProcAddress", a_GetProcAddress), F("alGetEnumValue", a_GetEnumValue),
    /* EFX */
    F("alGenFilters", a_GenFilters), F("alDeleteFilters", a_DeleteFilters), F("alIsFilter", a_IsFilter),
    F("alFilteri", a_Filteri), F("alFilterf", a_Filterf), F("alFilteriv", a_Ignore), F("alFilterfv", a_Ignore),
    F("alGetFilteri", a_GetZero), F("alGetFilterf", a_GetZero), F("alGetFilteriv", a_GetZero), F("alGetFilterfv", a_GetZero),
    F("alGenEffects", a_GenEffects), F("alDeleteEffects", a_DeleteEffects), F("alIsEffect", a_IsEffect),
    F("alEffecti", a_Effecti), F("alEffectf", a_Ignore), F("alEffectiv", a_Ignore), F("alEffectfv", a_Ignore),
    F("alGetEffecti", a_GetZero), F("alGetEffectf", a_GetZero), F("alGetEffectiv", a_GetZero), F("alGetEffectfv", a_GetZero),
    F("alGenAuxiliaryEffectSlots", a_GenSlots), F("alDeleteAuxiliaryEffectSlots", a_DeleteSlots),
    F("alIsAuxiliaryEffectSlot", a_IsSlot), F("alAuxiliaryEffectSloti", a_Ignore), F("alAuxiliaryEffectSlotf", a_Ignore),
    F("alAuxiliaryEffectSlotiv", a_Ignore), F("alAuxiliaryEffectSlotfv", a_Ignore),
    F("alGetAuxiliaryEffectSloti", a_GetZero), F("alGetAuxiliaryEffectSlotf", a_GetZero),
    F("alGetAuxiliaryEffectSlotiv", a_GetZero), F("alGetAuxiliaryEffectSlotfv", a_GetZero),
    /* ALC */
    F("alcOpenDevice", c_OpenDevice), F("alcCloseDevice", c_CloseDevice), F("alcCreateContext", c_CreateContext),
    F("alcMakeContextCurrent", c_MakeContextCurrent), F("alcProcessContext", a_Ignore),
    F("alcSuspendContext", a_Ignore), F("alcDestroyContext", c_DestroyContext),
    F("alcGetCurrentContext", c_GetCurrentContext), F("alcGetContextsDevice", c_GetContextsDevice),
    F("alcGetError", c_GetError), F("alcIsExtensionPresent", c_IsExtensionPresent),
    F("alcGetProcAddress", c_GetProcAddress), F("alcGetEnumValue", c_GetEnumValue), F("alcGetString", c_GetString),
    F("alcGetIntegerv", c_GetIntegerv),
    F("alcCaptureOpenDevice", c_Null), F("alcCaptureCloseDevice", c_Null), F("alcCaptureStart", a_Ignore),
    F("alcCaptureStop", a_Ignore), F("alcCaptureSamples", a_Ignore),
};
#undef F

static uint32_t proc_address(const char *name)
{
    if (!name) return 0;
    for (size_t k = 0; k < sizeof al_defs / sizeof *al_defs; k++)
        if (!strcmp(al_defs[k].name, name)) return rt_hle_thunk("openal32.dll", name);
    rt_log("OpenAL: GetProcAddress(%s): not implemented", name);
    return 0;
}

static void a_GetProcAddress(X86Ctx *ctx) { RET(proc_address(rt_str(ARG(0)))); }
static void c_GetProcAddress(X86Ctx *ctx) { RET(proc_address(rt_str(ARG(1)))); }

void rt_openal_register(void)
{
    al_lock = host_mutex_new();
    rt_hle_register(al_defs, sizeof al_defs / sizeof *al_defs);
}
