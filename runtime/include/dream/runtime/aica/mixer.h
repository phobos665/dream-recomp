// AICA sound generation (WP2.5, second half): the 64 channels (PCM/ADPCM/noise streaming with
// loops, amplitude and filter envelopes, LFOs, panning and send levels), the effects DSP and the
// final mix to 16-bit stereo at 44.1 kHz. A port of Flycast's sgc_if.cpp and dsp_interp.cpp
// (GPL-2.0, ADR 1; the DSP interpreter descends from the Audio Overload SDK, R. Belmont and
// R. Bannister). Integer fixed-point throughout, with lookup tables generated once
// (tools/aica/gen_tables.py), so the output is bit-identical on every host (ADR 16).
//
// The mixer works directly on the AICA register bytes (channel slots at 0x0000, common block at
// 0x2800, DSP data at 0x3000) and on sound RAM; the Aica class calls it on register writes, on
// reads of the monitor registers and once per sample.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace dream::aica {

class Mixer {
public:
    Mixer(std::uint8_t* regs, std::uint8_t* aram) noexcept;
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;
    void reset();

    // A write landed in channel `channel`'s 0x80-byte slot at byte offset `reg` (size 1 or 2).
    void channel_reg_written(unsigned channel, unsigned reg, unsigned size);
    // RBP/RBL (0x2804) changed; DSP programme memory (0x3400..0x3BFF) changed.
    void ring_buffer_written();
    void dsp_program_written();
    // Refreshes the monitor registers (MIBUF at 0x2808, EG/SGC/LP at 0x2810, CA at 0x2814) before
    // a read of `offset` (0x2808..0x2817).
    void common_reg_read(unsigned offset, bool byte);
    // Produces one stereo sample.
    void sample(std::int16_t& left, std::int16_t& right);

    // Diagnostics
    std::uint64_t key_ons = 0, key_offs = 0, samples = 0, nonzero_samples = 0;
    unsigned active_channels() const noexcept;
    bool dsp_running() const noexcept { return !dsp_.stopped; }

    // ---- everything below is the ported model; public for the tests ----
    using SampleType = std::int32_t;
    enum EgState : unsigned { kAttack = 0, kDecay1 = 1, kDecay2 = 2, kRelease = 3 };
    enum LfoType : unsigned { kSawtooth = 0, kSquare = 1, kTriangle = 2, kRandom = 3 };
    enum PcmsType : unsigned { kPcm16 = 0, kPcm8 = 1, kAdpcm = 2, kAdpcmStream = 3, kNoise = 4 };

    union Fp22_10 {
        struct Parts {
            std::uint32_t fp : 10;
            std::uint32_t ip : 22;
        } p;
        std::uint32_t full;
    };

    struct DspState {
        std::int32_t TEMP[128];  // 24 bits
        std::int32_t MEMS[32];   // 24 bits
        std::int32_t MIXS[16];   // 20 bits
        std::uint32_t RBP, RBL, MDEC_CT;
        bool stopped, dirty;
    };

    struct Channel;  // defined in mixer.cpp

private:
    std::uint8_t* regs_;
    std::uint8_t* aram_;
    Channel* chans_;  // 64, allocated by the constructor
    DspState dsp_{};
    void dsp_step();
    void channels_init();
    void channels_free();
};

}  // namespace dream::aica
