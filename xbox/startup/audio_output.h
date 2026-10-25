#pragma once
#include <aaudio/AAudio.h>
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
        return true;
    }
};
