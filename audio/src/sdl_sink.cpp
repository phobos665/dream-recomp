// See sink.h.
#include "dream/audio/sink.h"

#include <SDL3/SDL.h>

namespace dream::audio {

Sink::~Sink() {
    close();
}

bool Sink::open(unsigned rate) {
    close();
    rate_ = rate;
    // SDL_Init is safe to call again when the window has already initialised the video subsystem;
    // each subsystem is counted separately.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_ = std::string("SDL_InitSubSystem(audio): ") + SDL_GetError();
        return false;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        error_ = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        return false;
    }
    // A device opened this way starts paused, so that an application can queue something before
    // the first sample is due.
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        error_ = std::string("SDL_ResumeAudioStreamDevice: ") + SDL_GetError();
        close();
        return false;
    }
    block_.reserve(kBlockFrames * 2);
    return true;
}

void Sink::close() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    block_.clear();
}

unsigned Sink::queued_samples() const {
    if (!stream_)
        return 0;
    const int bytes = SDL_GetAudioStreamQueued(stream_);
    if (bytes <= 0)
        return 0;
    return static_cast<unsigned>(bytes) / 4u;  // 16-bit stereo
}

void Sink::push(std::int16_t left, std::int16_t right) {
    if (!stream_)
        return;
    block_.push_back(left);
    block_.push_back(right);
    ++pushed;
    if (block_.size() < kBlockFrames * 2)
        return;
    // The guest is ahead of the device: throw this block away rather than let the delay between
    // what is on screen and what is heard grow without limit. Dropping is audible once, whereas a
    // queue that only grows is audible for the rest of the run.
    if (queued_samples() > high_water) {
        dropped += kBlockFrames;
        block_.clear();
        return;
    }
    flush();
}

void Sink::flush() {
    if (!stream_ || block_.empty())
        return;
    SDL_PutAudioStreamData(stream_, block_.data(),
                           static_cast<int>(block_.size() * sizeof(std::int16_t)));
    ++blocks;
    block_.clear();
}

}  // namespace dream::audio
