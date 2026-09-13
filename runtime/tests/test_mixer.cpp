// AICA sound generation (WP2.5 second half): a looping 16-bit PCM channel keyed on through the
// registers produces output that follows the envelope, a noise channel produces output, the DSP
// idles on an empty programme, and the mix is deterministic.
#include <cstdint>
#include <cstdlib>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/system.h"

#include "doctest.h"

namespace {

using A = dream::aica::Aica;

struct Rig {
    dream::System sys;
    A aica{sys.sched, sys.holly, sys.memory};
    void w(std::uint32_t off, std::uint32_t v, unsigned size = 2) {
        aica.write(A::kRegBase + off, v, size);
    }
    void square_wave(std::uint32_t at, unsigned period, std::int16_t amp) {
        std::uint8_t* ram = sys.memory.aram() + at;
        for (unsigned i = 0; i < period; ++i) {
            const std::int16_t v = i < period / 2 ? amp : static_cast<std::int16_t>(-amp);
            ram[2 * i] = static_cast<std::uint8_t>(v);
            ram[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
        }
    }
    // Channel 0: 16-bit PCM at `sa`, loop [0, len), pitch 1:1, fast attack, no decay, fast
    // release, full send level, centre pan, TL 0, filter bypassed (all-zero FLV registers would
    // close it). `ctrl` is the first register word without the key bits; drivers write KYONEX,
    // KYONB, LPCTL, PCMS and SA_hi together, as key_on()/key_off() do.
    std::uint32_t ctrl = 0;
    void program_channel(std::uint32_t sa, unsigned len, bool noise = false) {
        ctrl = ((sa >> 16) & 0x7Fu) | (1u << 9) | (noise ? (1u << 10) : 0u);  // LPCTL, SSCTL
        w(0x04, sa & 0xFFFFu);
        w(0x00, ctrl);
        w(0x08, 0);
        w(0x0C, len);
        w(0x10, 31);        // AR 31, D1R 0, D2R 0
        w(0x14, 31);        // RR 31, DL 0, KRS 0
        w(0x18, 0);         // FNS 0, OCT 0
        w(0x24, 15u << 8);  // DISDL 15, DIPAN 0
        w(0x28, 1u << 5);   // TL 0, LPOFF
        w(0x2800, 15);      // MVOL 15
    }
    void key_on() { w(0x00, ctrl | (1u << 14) | (1u << 15)); }  // KYONB | KYONEX
    void key_off() { w(0x00, ctrl | (1u << 15)); }              // KYONEX, KYONB clear
};

}  // namespace

TEST_CASE("mixer: a keyed-on looping PCM channel sounds, then releases to silence") {
    Rig r;
    r.square_wave(0x1000, 64, 8000);
    r.program_channel(0x1000, 64);
    CHECK(r.aica.mixer.active_channels() == 0);
    r.key_on();
    CHECK(r.aica.mixer.key_ons == 1);
    CHECK(r.aica.mixer.active_channels() == 1);
    int peak = 0;
    for (int i = 0; i < 4000; ++i) {
        r.aica.sample_tick();
        peak = std::max(peak, std::abs(static_cast<int>(r.aica.last_left)));
    }
    CHECK(r.aica.mixer.nonzero_samples > 3000);
    CHECK(peak > 1000);
    CHECK(r.aica.last_left == r.aica.last_right);  // centre pan
    // Monitor registers follow the channel: CA advances, SGC reports the envelope state.
    r.w(0x280C, 0);  // MSLC 0
    CHECK(r.aica.read(A::kRegBase + 0x2814, 2) < 64);
    r.key_off();
    CHECK(r.aica.mixer.key_offs == 1);
    for (int i = 0; i < 20000; ++i) r.aica.sample_tick();
    CHECK(r.aica.mixer.active_channels() == 0);
    CHECK(r.aica.last_left == 0);
    CHECK(r.aica.last_right == 0);
}

TEST_CASE("mixer: pan moves the signal between the channels") {
    Rig r;
    r.square_wave(0x1000, 64, 8000);
    r.program_channel(0x1000, 64);
    r.w(0x24, (15u << 8) | 0x1Fu);  // DIPAN 0x1F: right fully attenuated
    r.key_on();
    long long left = 0, right = 0;
    for (int i = 0; i < 4000; ++i) {
        r.aica.sample_tick();
        left += std::abs(static_cast<int>(r.aica.last_left));
        right += std::abs(static_cast<int>(r.aica.last_right));
    }
    CHECK(left > 0);
    CHECK(right == 0);
}

TEST_CASE("mixer: noise source and idle DSP") {
    Rig r;
    r.program_channel(0x1000, 64, /*noise=*/true);
    r.key_on();
    for (int i = 0; i < 2000; ++i) r.aica.sample_tick();
    CHECK(r.aica.mixer.nonzero_samples > 1000);
    CHECK_FALSE(r.aica.mixer.dsp_running());
    // A non-empty DSP programme starts the DSP on the next sample.
    r.w(0x3400, 0x0100);  // MPRO[0]: TWT
    r.aica.sample_tick();
    CHECK(r.aica.mixer.dsp_running());
}

TEST_CASE("mixer: identical inputs give identical output") {
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        r->square_wave(0x1000, 64, 8000);
        r->program_channel(0x1000, 64);
        r->key_on();
    }
    for (int i = 0; i < 3000; ++i) {
        a.aica.sample_tick();
        b.aica.sample_tick();
        REQUIRE(a.aica.last_left == b.aica.last_left);
    }
}
