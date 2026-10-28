#pragma once
#include <aaudio/AAudio.h>
#include <algorithm>
#include <cstdint>
// Blocking-mode output, owned by the diagnostic process. No audio frames are
// acknowledged here: PCM submission/conversion remains a separate operation.
struct MeAudioOutput {
    AAudioStream* stream = nullptr;
    ~MeAudioOutput() { close(); }
    void close() { if (stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = nullptr; } }
    bool open() {
        if (stream) return false;
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, 48000);
        auto result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK) { stream = nullptr; return false; }
        if (AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_FLOAT ||
            AAudioStream_getChannelCount(stream) != 2 || AAudioStream_getSampleRate(stream) != 48000 ||
            AAudioStream_requestStart(stream) != AAUDIO_OK) { close(); return false; }
        auto state = AAudioStream_getState(stream);
        for (unsigned attempt = 0; attempt < 8 && state != AAUDIO_STREAM_STATE_STARTED; ++attempt) {
            if (state != AAUDIO_STREAM_STATE_STARTING && state != AAUDIO_STREAM_STATE_OPEN) break;
            aaudio_stream_state_t next = state;
            const auto waited = AAudioStream_waitForStateChange(stream, state, &next, 100000000);
            state = next;
            if (waited != AAUDIO_OK && waited != AAUDIO_ERROR_TIMEOUT) break;
        }
        if (state != AAUDIO_STREAM_STATE_STARTED) { close(); return false; }
        // Blocking writes pace the guest audio callback to real device consumption;
        // keep the queue short (about 4 guest frames of 256 samples) so the guest's
        // audio clock stays close to what is actually heard.
        const int32_t burst = AAudioStream_getFramesPerBurst(stream);
        AAudioStream_setBufferSizeInFrames(stream, burst > 512 ? burst * 2 : 1024);
        xruns = AAudioStream_getXRunCount(stream);
        return true;
    }
    // Underruns (crackling) when the guest audio thread is late, e.g. on slower devices: grow
    // the queue by one burst per new underrun, up to ~85 ms (4096 frames). Returns the new
    // buffer size when it grew, 0 otherwise. Called after writes, under the writer lock.
    int32_t xruns = 0;
    int32_t adapt() {
        const int32_t count = AAudioStream_getXRunCount(stream);
        if (count <= xruns) return 0;
        xruns = count;
        const int32_t size = AAudioStream_getBufferSizeInFrames(stream);
        const int32_t limit = std::min<int32_t>(4096, AAudioStream_getBufferCapacityInFrames(stream));
        if (size >= limit) return 0;
        const int32_t burst = std::max<int32_t>(AAudioStream_getFramesPerBurst(stream), 256);
        const int32_t grown = AAudioStream_setBufferSizeInFrames(stream, std::min(limit, size + burst));
        return grown > size ? grown : 0;
    }
};
