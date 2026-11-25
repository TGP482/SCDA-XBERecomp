// Linux: the mixer's output through SDL3 audio (PipeWire, PulseAudio or ALSA underneath), in place of XAudio2. The
// mix is 5.1 in Speaker order, which is SDL's 5.1 order too. A device with 5.1 speakers gets the six channels; any
// other gets the console's stereo downmix, as audio/xaudio2.cpp folds it.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "audio/output.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr float kCentreFold = 0.707f;
constexpr float kBackFold = 0.707f;
constexpr uint32_t kChunk = kRate / 100;  // the mixer renders 10 ms at a time

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_device_surround{false};

struct Output {
    SDL_AudioStream* stream = nullptr;
    int channels = 2;
    std::function<void(float*, uint32_t)> render;
    std::vector<float> mix;  // kChunk frames of kChannels
    std::vector<float> out;
    bool paused = false;
};
Output g_out;

void ApplyMode() {
    uint32_t mode = g_mode;
    bool want = mode == kOutputAuto ? g_device_surround.load() : mode == kOutputSurround;
    bool was = g_surround.exchange(want);
    bool mono = g_mono.exchange(mode == kOutputMono);
    if (was != want || mono != (mode == kOutputMono))
        LOGI("audio: %s output, %d device channels", mode == kOutputMono ? "mono" : want ? "5.1" : "stereo",
             g_out.channels);
}

// one chunk of the mix into the device's channels
void Convert(const float* mix, float* out, int channels) {
    bool surround = g_surround && channels >= 6;
    bool mono = g_mono || channels == 1;
    for (uint32_t f = 0; f < kChunk; f++) {
        const float* s = mix + size_t(f) * kChannels;
        float* d = out + size_t(f) * size_t(channels);
        if (surround) {
            for (int c = 0; c < 6; c++) d[c] = s[c];
            for (int c = 6; c < channels; c++) d[c] = 0;
            continue;
        }
        float left = s[FrontLeft] + kCentreFold * s[Centre] + kBackFold * s[BackLeft];
        float right = s[FrontRight] + kCentreFold * s[Centre] + kBackFold * s[BackRight];
        if (mono) left = right = (left + right) * 0.5f;
        d[0] = left;
        if (channels > 1) d[1] = right;
        for (int c = 2; c < channels; c++) d[c] = 0;
    }
}

void SDLCALL Pull(void*, SDL_AudioStream* stream, int additional, int) {
    Output& o = g_out;
    if (g_paused) {
        // the voices keep their place: silence goes out, the mixer is not run
        std::fill(o.out.begin(), o.out.end(), 0.0f);
        int bytes = int(o.out.size() * sizeof(float));
        for (; additional > 0; additional -= bytes) SDL_PutAudioStreamData(stream, o.out.data(), bytes);
        return;
    }
    int chunk_bytes = int(kChunk * uint32_t(o.channels) * sizeof(float));
    while (additional > 0) {
        o.render(o.mix.data(), kChunk);
        Convert(o.mix.data(), o.out.data(), o.channels);
        SDL_PutAudioStreamData(stream, o.out.data(), chunk_bytes);
        additional -= chunk_bytes;
    }
}

}  // namespace

bool IsSurround() { return g_surround; }

bool IsMono() { return g_mono; }

void SetOutputMode(uint32_t mode) {
    if (g_mode.exchange(mode) != mode) ApplyMode();
}

void SetPaused(bool paused) { g_paused = paused; }

void StartOutput(std::function<void(float*, uint32_t)> render) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec device{};
    int frames = 0;
    int channels = 2;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, &frames)) channels = device.channels;
    g_device_surround = channels >= 6;
    g_out.channels = channels >= 6 ? 6 : 2;
    g_out.render = std::move(render);
    g_out.mix.resize(size_t(kChunk) * kChannels);
    g_out.out.resize(size_t(kChunk) * size_t(g_out.channels));
    ApplyMode();
    SDL_AudioSpec spec{SDL_AUDIO_F32, g_out.channels, int(kRate)};
    g_out.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Pull, nullptr);
    if (!g_out.stream) {
        LOGE("audio: no output device, no sound: %s", SDL_GetError());
        return;
    }
    const char* driver = SDL_GetCurrentAudioDriver();
    LOGI("audio: SDL %s, %d device channels, %d frame buffer", driver ? driver : "?", channels, frames);
    SDL_ResumeAudioStreamDevice(g_out.stream);
}

}  // namespace audio
