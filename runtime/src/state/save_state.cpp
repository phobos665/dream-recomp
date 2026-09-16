// Every subsystem's contribution to a save state, in one file.
//
// The alternative was a save_state/load_state pair in each device's own .cpp, next to the fields it
// serialises. This is the better trade for a format: what a state contains is then one file that
// can be read top to bottom and checked against docs/design/save-states.md, instead of fourteen
// fragments where an omission looks exactly like a device that has nothing to save. The cost is
// that the declarations live in the headers away from their bodies, which is the smaller cost.
//
// Rules for anything added here:
//   - write fields explicitly, never a struct blit: a state outlives the build that wrote it, and
//     a blit silently changes meaning when a member is inserted;
//   - bump that section's version in the caller when its contents change;
//   - a load must leave the object in a state the device's own code would have produced, which for
//     anything holding a scheduler event id means re-arming through the scheduler, not restoring
//     the id.
#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/arm7.h"
#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/hle/bios.h"
#include "dream/runtime/holly/g2dma.h"
#include "dream/runtime/holly/intc.h"
#include "dream/runtime/holly/sysblock.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/pvr/spg.h"
#include "dream/runtime/sched/scheduler.h"
#include "dream/runtime/sh4/dmac.h"
#include "dream/runtime/sh4/intc.h"
#include "dream/runtime/sh4/tmu.h"
#include "dream/runtime/state/state.h"

namespace {
// Small helpers: a bool is written as a byte, because sizeof(bool) is not fixed by the standard and
// a state file is read by a build that need not be this one.
void put_bool(dream::state::Writer& w, bool v) {
    w.pod(static_cast<std::uint8_t>(v ? 1 : 0));
}
bool get_bool(dream::state::Reader& r) {
    std::uint8_t v = 0;
    r.pod(v);
    return v != 0;
}
}  // namespace

// ---------------------------------------------------------------- memory (CPU-side registers)

namespace dream::mem {

void DcMemory::save_state(state::Writer& w) {
    for (std::uint32_t v : ccn_) w.u32(v);
    for (std::uint32_t v : qacr_) w.u32(v);
    for (const auto& q : sq_)
        for (std::uint32_t v : q) w.u32(v);
    w.u32(tra);
    w.u32(expevt);
    w.u32(intevt);
}

void DcMemory::load_state(state::Reader& r) {
    for (std::uint32_t& v : ccn_) v = r.u32();
    for (std::uint32_t& v : qacr_) v = r.u32();
    for (auto& q : sq_)
        for (std::uint32_t& v : q) v = r.u32();
    tra = r.u32();
    expevt = r.u32();
    intevt = r.u32();
}

}  // namespace dream::mem

// ---------------------------------------------------------------- scheduler

namespace dream::sched {

// Events are keyed by name rather than by id. Ids are assigned in construction order, and the
// launcher's own events (the watchdog, the register sampler) exist only when the flags that create
// them were passed -- so a state saved by a run with --sample and loaded by one without it would
// otherwise shift every id after the sampler's by one and arm the wrong device.
void Scheduler::save_state(state::Writer& w) {
    w.u64(now_);
    w.u64(events_.size());
    for (const auto& e : events_) {
        w.str(e.name);
        w.u64(e.deadline);
    }
}

void Scheduler::load_state(state::Reader& r) {
    now_ = r.u64();
    const std::uint64_t n = r.u64();
    for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
        const std::string name = r.str();
        const std::uint64_t deadline = r.u64();
        for (auto& e : events_)
            if (e.name == name) {
                e.deadline = deadline;
                break;
            }
    }
}

}  // namespace dream::sched

// ---------------------------------------------------------------- SH-4 on-chip

namespace dream::sh4 {

void Intc::save_state(state::Writer& w) {
    w.u32(pending_);
    w.u32(ipra);
    w.u32(iprb);
    w.u32(iprc);
    w.u32(icr);
}

void Intc::load_state(state::Reader& r) {
    pending_ = r.u32();
    ipra = static_cast<std::uint16_t>(r.u32());
    iprb = static_cast<std::uint16_t>(r.u32());
    iprc = static_cast<std::uint16_t>(r.u32());
    icr = static_cast<std::uint16_t>(r.u32());
}

// The counter is derived from the virtual clock rather than stored (base_ is the clock value the
// count is measured from), so restoring base_ alongside the scheduler's now_ restores the count.
// Katana's syTmrGetCount busy-waits on TMU0 between interrupt polls: a title resumed with a stopped
// or wrongly-based timer does not fault, it spins, which is the worse failure of the two.
void Tmu::save_state(state::Writer& w) {
    for (unsigned i = 0; i < 3; ++i) {
        w.u32(tcor_[i]);
        w.u32(tcr_mode_[i]);
        w.u32(tcr_[i]);
        w.u32(shift_[i]);
        put_bool(w, running_[i]);
        w.pod(base_[i]);
    }
    w.u32(tstr_);
    w.u32(tocr_);
}

void Tmu::load_state(state::Reader& r) {
    for (unsigned i = 0; i < 3; ++i) {
        tcor_[i] = r.u32();
        tcr_mode_[i] = r.u32();
        tcr_[i] = static_cast<std::uint16_t>(r.u32());
        shift_[i] = r.u32();
        running_[i] = get_bool(r);
        r.pod(base_[i]);
    }
    tstr_ = static_cast<std::uint8_t>(r.u32());
    tocr_ = static_cast<std::uint8_t>(r.u32());
    // The underflow events are re-armed from the restored counters rather than taken from the
    // state: the scheduler section restores deadlines by name, and schedule() derives the same
    // ones from the same inputs, so this only matters when the two disagree -- in which case the
    // device's own derivation is the one to trust.
    for (unsigned i = 0; i < 3; ++i) schedule(i);
}

void Dmac::save_state(state::Writer& w) {
    for (const auto& c : ch_) {
        w.u32(c.sar);
        w.u32(c.dar);
        w.u32(c.tcr);
        w.u32(c.chcr);
    }
    w.u32(dmaor_);
}

void Dmac::load_state(state::Reader& r) {
    for (auto& c : ch_) {
        c.sar = r.u32();
        c.dar = r.u32();
        c.tcr = r.u32();
        c.chcr = r.u32();
    }
    dmaor_ = r.u32();
}

}  // namespace dream::sh4

// ---------------------------------------------------------------- Holly

namespace dream::holly {

void Intc::save_state(state::Writer& w) {
    for (std::uint32_t v : {istnrm, istext, isterr, iml2nrm, iml2ext, iml2err, iml4nrm, iml4ext,
                            iml4err, iml6nrm, iml6ext, iml6err})
        w.u32(v);
}

void Intc::load_state(state::Reader& r) {
    for (std::uint32_t* p : {&istnrm, &istext, &isterr, &iml2nrm, &iml2ext, &iml2err, &iml4nrm,
                             &iml4ext, &iml4err, &iml6nrm, &iml6ext, &iml6err})
        *p = r.u32();
    // The masks decide which SH-4 IRL lines are asserted, and nothing else recomputes them: a
    // title resumed without this never sees another VBlank.
    recompute();
}

void SysBlock::save_state(state::Writer& w) {
    for (std::uint32_t v : regs_) w.u32(v);
}

void SysBlock::load_state(state::Reader& r) {
    for (std::uint32_t& v : regs_) v = r.u32();
}

// Channel transfers complete inside the write that starts them (as in Flycast), so nothing is ever
// mid-transfer at a capture. What can be outstanding is the end-of-DMA interrupt, which is a
// scheduler event; the scheduler section carries its deadline and the launcher re-arms it.
void G2Dma::save_state(state::Writer& w) {
    for (std::uint32_t v : regs_) w.u32(v);
    w.u64(transfers);
    w.u64(bytes);
    w.u64(refused);
    w.u64(to_aica_ram);
}

void G2Dma::load_state(state::Reader& r) {
    for (std::uint32_t& v : regs_) v = r.u32();
    transfers = r.u64();
    bytes = r.u64();
    refused = r.u64();
    to_aica_ram = r.u64();
}

}  // namespace dream::holly

// ---------------------------------------------------------------- PowerVR

namespace dream::pvr {

void Spg::save_state(state::Writer& w) {
    for (std::uint32_t v : {spg_hblank_int, spg_vblank_int, spg_control, spg_hblank, spg_load,
                            spg_vblank, spg_width})
        w.u32(v);
    w.u32(scanline_);
    put_bool(w, vclk_full_);
    put_bool(w, vsync_);
    put_bool(w, fieldnum_);
    w.u64(line_cycles_);
    w.u64(frames_);
}

void Spg::load_state(state::Reader& r) {
    for (std::uint32_t* p : {&spg_hblank_int, &spg_vblank_int, &spg_control, &spg_hblank, &spg_load,
                             &spg_vblank, &spg_width})
        *p = r.u32();
    scanline_ = r.u32();
    vclk_full_ = get_bool(r);
    vsync_ = get_bool(r);
    fieldnum_ = get_bool(r);
    line_cycles_ = r.u64();
    frames_ = r.u64();
    // recalc() would re-derive line_cycles_ and re-arm the per-scanline event from the restored
    // registers, but it also resets the scanline to zero. The event is re-armed from the scheduler
    // section instead, which keeps the capture's position within the frame.
}

void Core::save_state(state::Writer& w) {
    for (std::uint32_t v : regs_) w.u32(v);
    for (std::uint32_t v : fifo_buf_) w.u32(v);
    w.u32(fifo_fill_);
    w.bytes(yuv_mb_.data(), yuv_mb_.size());
    w.u32(yuv_fill_);
    w.u32(yuv_index_);
    put_bool(w, seen_ta_data_);
    // Tile Accelerator: the parser's state machine and the parameter words of the list being built.
    // A capture taken between a list init and the render that consumes it would otherwise resume
    // with the first half of a display list missing, and draw a half-frame.
    w.u32(ta.current_list);
    w.u64(ta.chunks);
    w.u64(ta.invalid);
    w.u64(ta.stream.size());
    if (!ta.stream.empty())
        w.bytes(ta.stream.data(), ta.stream.size() * sizeof(std::uint32_t));
    for (const auto& l : ta.lists) {
        w.u32(l.polygons);
        w.u32(l.sprites);
        w.u32(l.modvols);
        w.u32(l.vertices);
        w.u32(l.chunks);
        w.u32(l.ends);
    }
    w.u64(renders);
    w.u64(renders_done);
    w.u64(frame_swaps);
    w.u64(yuv_words);
    w.u64(texture_words);
    w.u64(list_inits);
}

void Core::load_state(state::Reader& r) {
    for (std::uint32_t& v : regs_) v = r.u32();
    for (std::uint32_t& v : fifo_buf_) v = r.u32();
    fifo_fill_ = r.u32();
    r.bytes(yuv_mb_.data(), yuv_mb_.size());
    yuv_fill_ = r.u32();
    yuv_index_ = r.u32();
    seen_ta_data_ = get_bool(r);
    ta.current_list = r.u32();
    ta.chunks = r.u64();
    ta.invalid = r.u64();
    const std::uint64_t n = r.u64();
    ta.stream.assign(n, 0);
    if (n)
        r.bytes(ta.stream.data(), n * sizeof(std::uint32_t));
    for (auto& l : ta.lists) {
        l.polygons = r.u32();
        l.sprites = r.u32();
        l.modvols = r.u32();
        l.vertices = r.u32();
        l.chunks = r.u32();
        l.ends = r.u32();
    }
    renders = r.u64();
    renders_done = r.u64();
    frame_swaps = r.u64();
    yuv_words = r.u64();
    texture_words = r.u64();
    list_inits = r.u64();
    // Ta::state_ and pending_v64_ are deliberately not carried: they are the position within one
    // 32-byte parameter, and a capture taken by the launcher is never inside one, because the
    // capture point is a guest instruction boundary and a parameter arrives as a single burst.
}

}  // namespace dream::pvr

// ---------------------------------------------------------------- Maple

namespace dream::maple {

// The devices themselves (controller, memory card) hold no state that survives a frame: the pad's
// buttons come from the host every VBlank, and the card's contents are the image file. What has to
// come back is the DMA command-table pointer, because the transfer the guest starts on the next
// VBlank walks it, and a zero there is a read of address zero.
//
// And the reply frames a started transfer has built but not yet written back. Dropping those was
// the first version of this function, on the reasoning that the guest polls for completion and the
// next VBlank starts another transfer -- true, and it still cost seven guest writes at the seam,
// measured: one controller reply of four words at 0x0c1a7780 and the three 0xffffffff no-device
// markers for the empty ports. Seven writes is the whole difference between a resumed run that
// matches the uninterrupted one and one that does not, so they are carried.
void Bus::save_state(state::Writer& w) {
    for (std::uint32_t v : {mdstar, mdtsel, mden, mdst, msys, mshtcl, mdapro, mmsel}) w.u32(v);
    put_bool(w, trigger_pending_reset_);
    w.u64(out_.size());
    for (const auto& pending : out_) {
        w.u32(pending.addr);
        w.u64(pending.words.size());
        if (!pending.words.empty())
            w.bytes(pending.words.data(), pending.words.size() * sizeof(std::uint32_t));
    }
}

void Bus::load_state(state::Reader& r) {
    for (std::uint32_t* p : {&mdstar, &mdtsel, &mden, &mdst, &msys, &mshtcl, &mdapro, &mmsel})
        *p = r.u32();
    trigger_pending_reset_ = get_bool(r);
    out_.clear();
    const std::uint64_t n = r.u64();
    for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
        Pending pending;
        pending.addr = r.u32();
        const std::uint64_t words = r.u64();
        pending.words.assign(words, 0);
        if (words)
            r.bytes(pending.words.data(), words * sizeof(std::uint32_t));
        out_.push_back(std::move(pending));
    }
}

}  // namespace dream::maple

// ---------------------------------------------------------------- AICA

namespace dream::aica {

void Arm7::save_state(state::Writer& w) {
    for (const auto& r : r_) w.u32(r.I);
    put_bool(w, n_flag_);
    put_bool(w, z_flag_);
    put_bool(w, c_flag_);
    put_bool(w, v_flag_);
    put_bool(w, irq_enable_);
    put_bool(w, fiq_enable_);
    put_bool(w, enabled_);
    w.u32(static_cast<std::uint32_t>(mode_));
    w.u32(static_cast<std::uint32_t>(ticks_));
    put_bool(w, aica_interr_);
    put_bool(w, e68k_out_);
    w.u32(aica_reg_l_);
    w.u32(e68k_reg_l_);
    w.u32(e68k_reg_m_);
}

void Arm7::load_state(state::Reader& rd) {
    for (auto& r : r_) r.I = rd.u32();
    n_flag_ = get_bool(rd);
    z_flag_ = get_bool(rd);
    c_flag_ = get_bool(rd);
    v_flag_ = get_bool(rd);
    irq_enable_ = get_bool(rd);
    fiq_enable_ = get_bool(rd);
    enabled_ = get_bool(rd);
    mode_ = static_cast<int>(rd.u32());
    ticks_ = static_cast<int>(rd.u32());
    aica_interr_ = get_bool(rd);
    e68k_out_ = get_bool(rd);
    aica_reg_l_ = rd.u32();
    e68k_reg_l_ = rd.u32();
    e68k_reg_m_ = rd.u32();
}

void Aica::save_state(state::Writer& w) {
    w.bytes(regs_.data(), regs_.size());
    for (int v : timer_step_) w.u32(static_cast<std::uint32_t>(v));
    for (int v : timer_left_) w.u32(static_cast<std::uint32_t>(v));
    w.u64(samples);
    arm.save_state(w);
    mixer.save_dsp_state(w);
}

void Aica::load_state(state::Reader& r) {
    r.bytes(regs_.data(), regs_.size());
    for (int& v : timer_step_) v = static_cast<int>(r.u32());
    for (int& v : timer_left_) v = static_cast<int>(r.u32());
    samples = r.u64();
    arm.load_state(r);
    // The mixer's own state is not carried (see docs/design/save-states.md): the per-channel
    // playback cursors and envelope phases live in mixer.cpp and are audio only. What it does need
    // is to be told that the registers and sound RAM underneath it have been replaced, so it drops
    // the channels it was part-way through and re-derives the ring buffer from what was restored.
    // Without this it kept cursors into sound RAM that no longer says what it did -- audible as
    // crackling -- and left the DSP pointed at the bottom of sound RAM, on top of the ARM7's code.
    // Order matters: resync first, so RBP/RBL come from the restored registers, then the DSP's
    // own working state on top -- MDEC_CT especially, which is where in the delay line the effects
    // path is reading and writing. Losing it left the reverb reading from the wrong offset, which
    // is audible as a hollow, phasey version of the right sound rather than as silence.
    mixer.resync_after_load();
    if (r.section_version() >= 2)
        mixer.load_dsp_state(r);
    update_arm_interrupts();
    update_sh4_interrupts();
}

void Rtc::save_state(state::Writer& w) {
    w.u32(seconds_);
    w.u32(enable_);
}

void Rtc::load_state(state::Reader& r) {
    seconds_ = r.u32();
    enable_ = r.u32();
}

}  // namespace dream::aica

// ---------------------------------------------------------------- BIOS HLE

namespace dream::hle {

// The GD-ROM state machine, including a read in flight. This is the one subsystem where leaving
// state out does not degrade the resumed run but stops it: a title that was streaming from disc
// when the capture was taken waits for a completion that, without this, is never scheduled.
void Bios::save_state(state::Writer& w) {
    w.u32(static_cast<std::uint32_t>(gd_.status));
    w.u32(gd_.command);
    for (std::uint32_t v : gd_.params) w.u32(v);
    for (std::uint32_t v : gd_.result) w.u32(v);
    w.u32(gd_.last_request);
    w.u32(gd_.next_request);
    w.u32(gd_.read_sector);
    w.u32(gd_.read_remaining);
    w.u32(gd_.read_total);
    w.u32(gd_.read_dest);
    w.u32(gd_.speed);
    w.u32(gd_.standby);
    w.u32(gd_.read_flags);
    w.u32(gd_.read_retry);
    w.u32(gd_.callback);
    w.u32(gd_.callback_arg);
    for (std::uint32_t v : gd_.sector_mode) w.u32(v);
    w.u64(sectors_read);
}

void Bios::load_state(state::Reader& r) {
    gd_.status = static_cast<std::int32_t>(r.u32());
    gd_.command = r.u32();
    for (std::uint32_t& v : gd_.params) v = r.u32();
    for (std::uint32_t& v : gd_.result) v = r.u32();
    gd_.last_request = r.u32();
    gd_.next_request = r.u32();
    gd_.read_sector = r.u32();
    gd_.read_remaining = r.u32();
    gd_.read_total = r.u32();
    gd_.read_dest = r.u32();
    gd_.speed = r.u32();
    gd_.standby = r.u32();
    gd_.read_flags = r.u32();
    gd_.read_retry = r.u32();
    gd_.callback = r.u32();
    gd_.callback_arg = r.u32();
    for (std::uint32_t& v : gd_.sector_mode) v = r.u32();
    sectors_read = r.u64();
    // The drive event is deliberately NOT re-armed here. The scheduler section has already restored
    // its deadline, and asking for it again would replace the capture's remaining delay with a full
    // fresh one -- the sectors would then land later than they did in the uninterrupted run, which
    // is a divergence manufactured by the loader. It is only re-armed when the scheduler had
    // nothing for it, which means the state predates the scheduler section.
    if (gd_.read_remaining && gd_event_ >= 0 && !sys_.sched.armed(gd_event_))
        sys_.sched.request(gd_event_, gd_ticks());
}

}  // namespace dream::hle
