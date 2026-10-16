// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <atomic>
#include <future>
#include <optional>
#include <thread>
#include <vector>

#include "audio/output.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr float kCentreFold = 0.707f;
constexpr DWORD kFront = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY;
constexpr DWORD kBack = SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
constexpr DWORD kSide = SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
// the speaker of each source channel, in Speaker order
constexpr DWORD kSourceSpeaker[kChannels] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
                                             SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT};

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_mode_changed{false};
std::atomic<bool> g_paused{false};

struct Output {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
};

// the master's speakers have a centre, a sub and a back or side pair
bool HasSurround(DWORD mask) {
    bool back = (mask & kBack) == kBack;
    bool side = (mask & kSide) == kSide;
    return (mask & kFront) == kFront && (back || side);
}

bool WantSurround(uint32_t mode, DWORD mask) {
    if (mode == kOutputAuto) return HasSurround(mask);
    return mode == kOutputSurround;
}

WAVEFORMATEXTENSIBLE MixFormat() {
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: ksmedia.h needs ks.h, which breaks GUID_NULL
    static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = kChannels;
    wf.Format.nSamplesPerSec = kRate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = kChannels * sizeof(float);
    wf.Format.nAvgBytesPerSec = kRate * kChannels * sizeof(float);
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = kFront | kBack;
    wf.SubFormat = kFloat;
    return wf;
}

// output index of a speaker in a channel mask, outputs follow the mask's bit order
int OutputOf(DWORD mask, DWORD speaker) {
    if (!(mask & speaker)) return -1;
    DWORD below = mask & (speaker - 1);
    int index = 0;
    for (; below; below &= below - 1) index++;
    return index;
}

// every source channel on its own speaker, the back pair on the side pair when the master has no back pair
std::vector<float> SurroundMatrix(DWORD mask, uint32_t outputs) {
    std::vector<float> m(outputs * kChannels, 0.0f);
    bool no_back = (mask & kBack) != kBack;
    for (uint32_t s = 0; s < kChannels; s++) {
        DWORD speaker = kSourceSpeaker[s];
        if (no_back && speaker == SPEAKER_BACK_LEFT) speaker = SPEAKER_SIDE_LEFT;
        if (no_back && speaker == SPEAKER_BACK_RIGHT) speaker = SPEAKER_SIDE_RIGHT;
        int o = OutputOf(mask, speaker);
        if (o >= 0) m[o * kChannels + s] = 1.0f;
    }
    return m;
}

// L = FL + kCentreFold C + BL and R = FR + kCentreFold C + BR without the sub, on the first two outputs.
// Mono, or a mono master, hears the average of the two.
std::vector<float> FoldToStereo(uint32_t outputs, bool mono) {
    static const float kLeft[kChannels] = {1, 0, kCentreFold, 0, 1, 0};
    static const float kRight[kChannels] = {0, 1, kCentreFold, 0, 0, 1};
    std::vector<float> m(outputs * kChannels, 0.0f);
    bool average = mono || outputs == 1;
    for (uint32_t s = 0; s < kChannels; s++) {
        float left = kLeft[s];
        float right = kRight[s];
        if (average) {
            left = (kLeft[s] + kRight[s]) * 0.5f;
            right = left;
        }
        m[s] = left;
        if (outputs > 1) m[kChannels + s] = right;
    }
    return m;
}

uint32_t MasterOutputs(IXAudio2MasteringVoice* master) {
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    return details.InputChannels;
}

DWORD MasterMask(IXAudio2MasteringVoice* master, uint32_t outputs) {
    DWORD mask = 0;
    master->GetChannelMask(&mask);
    // a stereo device that names no speakers is the front pair
    if (!mask && outputs == 2) mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    return mask;
}

const char* OutputName() {
    if (g_mono) return "mono";
    return g_surround ? "5.1" : "stereo";
}

void ApplyMatrix(Output& out) {
    uint32_t outputs = MasterOutputs(out.master);
    DWORD mask = MasterMask(out.master, outputs);
    uint32_t mode = g_mode;
    g_surround = WantSurround(mode, mask);
    g_mono = mode == kOutputMono;
    // a 5.1 mix on a device without 5.1 speakers is folded to stereo
    bool own_speakers = g_surround && HasSurround(mask);
    std::vector<float> m = own_speakers ? SurroundMatrix(mask, outputs) : FoldToStereo(outputs, g_mono);
    out.voice->SetOutputMatrix(out.master, kChannels, outputs, m.data());
    LOGI("audio: %s output, %u device channels", OutputName(), outputs);
}

std::optional<Output> Open() {
    Output out;
    if (FAILED(XAudio2Create(&out.xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return std::nullopt;
    if (FAILED(out.xa->CreateMasteringVoice(&out.master))) return std::nullopt;
    WAVEFORMATEXTENSIBLE wf = MixFormat();
    if (FAILED(out.xa->CreateSourceVoice(&out.voice, &wf.Format))) return std::nullopt;
    ApplyMatrix(out);
    return out;
}

void Pump(Output& out, const std::function<void(float*, uint32_t)>& render) {
    out.voice->Start();
    constexpr uint32_t kFrames = kRate / 100;
    // 100 ms queued: a 30 ms queue starves under short stalls, such as x86 emulation on ARM under Proton
    constexpr uint32_t kQueued = 10;
    constexpr uint32_t kSlots = 16;  // more than kQueued, so the slot being rendered is never queued
    std::vector<float> bufs(kSlots * kFrames * kChannels);
    bool stopped = false;
    for (uint32_t n = 0;;) {
        if (g_mode_changed.exchange(false)) ApplyMatrix(out);
        if (g_paused != stopped) {
            stopped = !stopped;
            if (stopped) out.voice->Stop();
            else out.voice->Start();
        }
        if (stopped) {
            Sleep(10);
            continue;
        }
        XAUDIO2_VOICE_STATE st;
        out.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued >= kQueued) {
            Sleep(2);
            continue;
        }
        float* b = &bufs[(n % kSlots) * kFrames * kChannels];
        render(b, kFrames);
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = kFrames * kChannels * sizeof(float);
        xb.pAudioData = reinterpret_cast<const BYTE*>(b);
        out.voice->SubmitSourceBuffer(&xb);
        n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
    }
}

}  // namespace

bool IsSurround() { return g_surround; }

bool IsMono() { return g_mono; }

void SetOutputMode(uint32_t mode) {
    if (g_mode.exchange(mode) != mode) g_mode_changed = true;
}

void SetPaused(bool paused) { g_paused = paused; }

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::promise<void> opened;
    std::future<void> known = opened.get_future();
    std::thread([render, opened = std::move(opened)]() mutable {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::optional<Output> out = Open();
        opened.set_value();
        if (!out) {
            LOGE("audio: XAudio2 unavailable, no sound");
            return;
        }
        Pump(*out, render);
    }).detach();
    known.wait();
}

}  // namespace audio
