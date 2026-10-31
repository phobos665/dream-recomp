#pragma once
// A look-ahead limiter for the AICA's final mix (Mixer::before_clamp). A game that mixes many loud
// voices goes past the 16-bit range, and the clamp then distorts the peaks. The limiter lowers the
// gain in the 64 samples before such a peak, so that the peak stays under the range, and raises it
// again over about 150 ms. A mix below the range passes unchanged, 64 samples (1.5 ms) later.

#include <array>
#include <cstdint>

namespace dream::aica {

class Limiter {
public:
    void process(std::int32_t& left, std::int32_t& right);

private:
    static constexpr int kAhead = 64;
    std::array<std::int32_t, kAhead * 2> delayed_{};
    std::array<float, kAhead> needs_{};  // the gain that each delayed sample needs
    int next_ = 0, lowered_ = 0;         // lowered_: the needs below 1
    float gain_ = 1.0f;
};

}  // namespace dream::aica
