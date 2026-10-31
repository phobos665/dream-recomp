#include "dream/runtime/aica/limiter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dream::aica {

void Limiter::process(std::int32_t& left, std::int32_t& right) {
    constexpr float kCeiling = 32000.0f;
    // 20 dB in 150 ms, multiplied up to the gain needed, which it then holds exactly.
    static const float release = std::pow(10.0f, 20.0f / 20.0f / (0.15f * 44100.0f));
    const float peak = static_cast<float>(std::max(std::abs(left), std::abs(right)));
    // The sample that leaves now came in 64 samples ago, and its need still counts.
    const std::int32_t out_left = delayed_[2 * next_], out_right = delayed_[2 * next_ + 1];
    float target = needs_[next_] > 0.0f ? needs_[next_] : 1.0f;
    lowered_ -= needs_[next_] > 0.0f && needs_[next_] < 1.0f;
    delayed_[2 * next_] = left;
    delayed_[2 * next_ + 1] = right;
    needs_[next_] = peak > kCeiling ? kCeiling / peak : 1.0f;
    lowered_ += needs_[next_] < 1.0f;
    next_ = (next_ + 1) % kAhead;
    // Most of the time no sample in the window needs less than the full gain.
    if (lowered_ > 0)
        for (float need : needs_)
            if (need > 0.0f)
                target = std::min(target, need);
    gain_ = target < gain_ ? gain_ + (target - gain_) * 0.1f : std::min(target, gain_ * release);
    left = static_cast<std::int32_t>(std::lround(static_cast<float>(out_left) * gain_));
    right = static_cast<std::int32_t>(std::lround(static_cast<float>(out_right) * gain_));
}

}  // namespace dream::aica
