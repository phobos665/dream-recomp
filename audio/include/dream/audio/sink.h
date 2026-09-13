// Audio output (WP2.5).
//
// The AICA produces one stereo sample per 44.1 kHz tick of the guest's virtual clock. The host's
// sound device consumes samples on its own real clock and never waits. Those two clocks are not the
// same clock, and everything awkward about audio output follows from that: run the guest faster
// than real time and samples pile up, slower and the device runs dry.
//
// This is deliberately a queue and not a resampler. The launcher already paces the guest against
// the wall clock in windowed mode, so the two rates agree to within the jitter of a frame; the sink
// only has to absorb that jitter and say plainly when it could not. A title that needs more than
// that will show up as a non-zero dropped or starved count, which is the signal to do something
// cleverer rather than a reason to do it now.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace dream::audio {

class Sink {
public:
    Sink() = default;
    ~Sink();
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    // Opens the default playback device as 16-bit stereo at `rate`. False with error() set when
    // there is no device, which is an ordinary outcome on a build machine and not a failure worth
    // stopping a run for.
    bool open(unsigned rate = 44100);
    void close();
    bool is_open() const noexcept { return stream_ != nullptr; }

    // One stereo sample from the AICA. Buffered until a block is worth sending.
    void push(std::int16_t left, std::int16_t right);
    // Sends whatever is buffered, whether or not it fills a block. For the end of a run.
    void flush();

    // How many samples the device still has to play. The launcher uses it to tell "the guest is
    // ahead" from "the guest is behind" when it reports.
    unsigned queued_samples() const;

    // How full the queue is allowed to get before samples are dropped. Roughly a fifth of a second
    // at 44.1 kHz: enough to ride out a slow frame, short enough that sound stays in step with the
    // picture.
    unsigned high_water = 8192;

    std::uint64_t pushed = 0, dropped = 0, blocks = 0;
    const std::string& error() const noexcept { return error_; }

private:
    static constexpr std::size_t kBlockFrames = 512;  // stereo frames per send

    SDL_AudioStream* stream_ = nullptr;
    std::vector<std::int16_t> block_;
    unsigned rate_ = 44100;
    std::string error_;
};

}  // namespace dream::audio
