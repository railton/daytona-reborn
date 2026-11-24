// A short two-note chime, synthesised here (no recording, nothing from the
// game): the room's screens play it when a player joins. Its own SDL audio
// stream, opened the first time it plays.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace app {

class Chime {
public:
    Chime() = default;
    Chime(const Chime &) = delete;
    Chime &operator=(const Chime &) = delete;
    ~Chime() {
        if (stream_) SDL_DestroyAudioStream(stream_);
    }

    // volume: 0..1 (the settings' effects volume)
    void play(float volume) {
        if (volume <= 0.f) return;
        if (!stream_) {
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
            stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (!stream_) return; // no audio device: silently nothing
            SDL_ResumeAudioStreamDevice(stream_);
        }
        if (samples_.empty()) synthesize();
        std::vector<float> out(samples_);
        for (float &s : out) s *= volume * 0.45f;
        SDL_ClearAudioStream(stream_); // a new player cuts the last chime short
        SDL_PutAudioStreamData(stream_, out.data(), int(out.size() * sizeof(float)));
    }

private:
    static constexpr int kRate = 48000;
    SDL_AudioStream *stream_ = nullptr;
    std::vector<float> samples_;

    // Two bell-like notes rising a fifth (E6, then B6), each a sine with a
    // faint octave, a 4 ms attack and an exponential decay.
    void synthesize() {
        constexpr float pi = 3.14159265f;
        const struct {
            float freq, start, length;
        } notes[] = {{1318.5f, 0.f, 0.30f}, {1975.5f, 0.10f, 0.45f}};
        const int total = int(kRate * 0.56f);
        samples_.assign(size_t(total), 0.f);
        for (const auto &n : notes) {
            const int first = int(n.start * kRate), length = int(n.length * kRate);
            for (int i = 0; i < length && first + i < total; ++i) {
                const float t = float(i) / kRate;
                const float envelope = std::min(1.f, t / 0.004f) * std::exp(-t * 9.f);
                const float tone = std::sin(2 * pi * n.freq * t) + 0.25f * std::sin(2 * pi * n.freq * 2.f * t);
                samples_[size_t(first + i)] += 0.5f * envelope * tone;
            }
        }
    }
};

} // namespace app
