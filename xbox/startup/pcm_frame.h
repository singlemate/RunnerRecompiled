#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
// Fixed SDK-compatible diagnostic mix: FL FR FC LFE BL BR, 256 samples each.
inline void me_pcm_stereo(const uint8_t* source, float* stereo) {
    auto sample = [&](unsigned channel, unsigned index) {
        uint32_t bits; std::memcpy(&bits, source + (channel * 256 + index) * 4, 4);
        bits = __builtin_bswap32(bits); float value; std::memcpy(&value, &bits, 4);
        return std::isfinite(value) ? value : 0.0f;
    };
    for (unsigned i = 0; i < 256; ++i) {
        const float mid = sample(2,i) * 0.70710678f;
        stereo[i*2] = std::clamp((sample(0,i) + mid + sample(4,i)*0.70710678f)*0.58578644f, -1.0f, 1.0f);
        stereo[i*2+1] = std::clamp((sample(1,i) + mid + sample(5,i)*0.70710678f)*0.58578644f, -1.0f, 1.0f);
    }
}
struct MePcmWriteResult { int frames = 0; int error = 0; };
template<class Writer> MePcmWriteResult me_pcm_write_all(const float* stereo, Writer write) {
    MePcmWriteResult result;
    while (result.frames < 256) {
        const int remaining = 256 - result.frames;
        const int wrote = write(stereo + result.frames * 2, remaining);
        if (wrote <= 0 || wrote > remaining) { result.error = wrote < 0 ? wrote : -1; break; }
        result.frames += wrote;
    }
    return result;
}
