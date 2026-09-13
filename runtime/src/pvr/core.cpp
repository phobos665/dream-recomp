#include "dream/runtime/pvr/core.h"

#include <cstring>

namespace dream::pvr {

// ---- Ta ----------------------------------------------------------------------------------------

namespace {
inline unsigned para_type(std::uint32_t pcw) {
    return (pcw >> 29) & 7u;
}
inline unsigned list_type(std::uint32_t pcw) {
    return (pcw >> 24) & 7u;
}
inline bool pcw_texture(std::uint32_t pcw) {
    return (pcw >> 3) & 1u;
}
inline bool pcw_offset(std::uint32_t pcw) {
    return (pcw >> 2) & 1u;
}
inline unsigned pcw_col_type(std::uint32_t pcw) {
    return (pcw >> 4) & 3u;
}
inline bool pcw_volume(std::uint32_t pcw) {
    return (pcw >> 6) & 1u;
}
inline bool is_modvol_list(unsigned list) {
    return (list & 1u) != 0;
}
}  // namespace

bool Ta::header_is_64(std::uint32_t pcw) noexcept {
    const unsigned col = pcw_col_type(pcw);
    if (!pcw_volume(pcw))
        return col == 2 && pcw_texture(pcw) && pcw_offset(pcw);  // polygon type 2
    return col == 2;                                             // polygon type 4
}

bool Ta::vertex_is_64(std::uint32_t pcw) noexcept {
    if (!pcw_texture(pcw))
        return false;
    return pcw_col_type(pcw) == 1 || pcw_volume(pcw);  // vertex types 5, 6, 11..14
}

void Ta::reset() noexcept {
    state_ = kNs;
    current_list = 7;
    pending_v64_ = false;
    lists = {};
    stream.clear();
}

void Ta::feed(const std::uint32_t words[8]) {
    ++chunks;
    stream.insert(stream.end(), words, words + 8);
    // Second halves of 64-byte parameters carry no control word.
    switch (state_) {
        case kPlhv32:
            state_ = kPlv32;
            return;
        case kPlhv64:
            state_ = kPlv64;
            return;
        case kPlv64H:
            state_ = kPlv64;
            return;
        case kMlv64H:
            state_ = kMlv64;
            return;
        default:
            break;
    }
    const std::uint32_t pcw = words[0];
    const unsigned type = para_type(pcw);
    switch (type) {
        case kEndOfList: {
            const unsigned list = current_list == 7 ? list_type(pcw) : current_list;
            if (list < 5) {
                ++lists[list].ends;
                if (on_list_end)
                    on_list_end(list);
            }
            current_list = 7;
            state_ = kNs;
            return;
        }
        case kUserTileClip:
        case kObjectListSet:
            if (current_list < 5)
                ++lists[current_list].chunks;
            return;  // 32 bytes, no state change
        case kPolygonOrModVol: {
            if (current_list == 7)
                current_list = list_type(pcw);
            if (current_list < 5)
                ++lists[current_list].chunks;
            if (is_modvol_list(current_list)) {
                if (current_list < 5)
                    ++lists[current_list].modvols;
                state_ = kMlv64;
                return;
            }
            if (current_list < 5)
                ++lists[current_list].polygons;
            const bool v64 = vertex_is_64(pcw);
            if (header_is_64(pcw))
                state_ = v64 ? kPlhv64 : kPlhv32;
            else
                state_ = v64 ? kPlv64 : kPlv32;
            return;
        }
        case kSprite:
            if (current_list == 7)
                current_list = list_type(pcw);
            if (current_list < 5) {
                ++lists[current_list].sprites;
                ++lists[current_list].chunks;
            }
            state_ = kPlv64;  // sprite vertices are 64 bytes
            return;
        case kVertex:
            if (current_list < 5) {
                ++lists[current_list].vertices;
                ++lists[current_list].chunks;
            }
            switch (state_) {
                case kPlv32:
                    return;
                case kPlv64:
                    state_ = kPlv64H;
                    return;
                case kMlv64:
                    state_ = kMlv64H;
                    return;
                default:
                    ++invalid;
                    return;  // vertex outside a list
            }
        default:
            ++invalid;
            return;
    }
}

// ---- Core --------------------------------------------------------------------------------------

Core::Core(sched::Scheduler& sched, holly::Intc& holly, Spg& spg, mem::DcMemory& memory)
    : sched_(sched), holly_(holly), spg_(spg), memory_(memory) {
    render_event_ = sched_.add("pvr-render", [this](std::uint64_t, std::uint64_t) {
        ++renders_done;
        holly_.raise(holly::Irq::RenderDone);
        holly_.raise(holly::Irq::RenderDoneIsp);
        holly_.raise(holly::Irq::RenderDoneVideo);
    });
    ta.on_list_end = [this](unsigned list) {
        static const holly::Irq irqs[5] = {holly::Irq::OpaqueDone, holly::Irq::OpaqueModDone,
                                           holly::Irq::TransDone, holly::Irq::TransModDone,
                                           holly::Irq::PunchThruDone};
        holly_.raise(irqs[list]);
    };
    reset();
}

void Core::reset() {
    regs_.fill(0);
    regs_[kId >> 2] = 0x17FD11DBu;
    regs_[kRevision >> 2] = 0x00000011u;
    regs_[kSoftReset >> 2] = 0x00000007u;
    ta.reset();
    fifo_fill_ = 0;
    seen_ta_data_ = false;
}

void Core::start_render() {
    ++renders;
    if (on_render)
        on_render(ta.stream);
    const std::uint64_t cycles = render_base_cycles + render_cycles_per_word * ta.stream.size();
    sched_.request(render_event_, cycles);
}

std::uint32_t Core::read(std::uint32_t addr, unsigned) {
    if (addr >= kRegBase && addr < kRegEnd) {
        const std::uint32_t off = addr - kRegBase;
        if (off >= Spg::kRegLo - kRegBase && off < Spg::kRegHi - kRegBase)
            return spg_.read(addr, 4);
        if (addr == Spg::kStatus)
            return spg_.read(addr, 4);
        return regs_[off >> 2];
    }
    return 0;  // the FIFO paths are write-only
}

void Core::fifo_word(std::uint32_t addr, std::uint32_t value) {
    // Individual 32-bit writes assemble a 32-byte chunk in address order.
    fifo_buf_[(addr >> 2) & 7u] = value;
    if (((addr >> 2) & 7u) == 7u) {
        if (!seen_ta_data_) {
            seen_ta_data_ = true;
            if (on_first_ta_data)
                on_first_ta_data();
        }
        ta.feed(fifo_buf_);
        regs_[kTaItpCurrent >> 2] += 32;
    }
}

void Core::write_burst(std::uint32_t addr, const std::uint32_t* words) {
    if (addr >= kFifoBase && addr < kYuvBase) {
        if (!seen_ta_data_) {
            seen_ta_data_ = true;
            if (on_first_ta_data)
                on_first_ta_data();
        }
        ta.feed(words);
        regs_[kTaItpCurrent >> 2] += 32;
        return;
    }
    for (unsigned i = 0; i < 8; ++i) write(addr + 4 * i, words[i], 4);
}

void Core::write(std::uint32_t addr, std::uint32_t value, unsigned size) {
    if (addr >= kFifoBase && addr < kYuvBase) {
        fifo_word(addr, value);
        return;
    }
    if (addr >= kYuvBase &&
        addr < kTexBase) {  // YUV converter input: counted only (WP2.3 renderer)
        ++yuv_words;
        return;
    }
    if (addr >= kTexBase && addr < kFifoEnd) {  // texture path: lands in the 64-bit VRAM view
        ++texture_words;
        memory_.write32(0x04000000u | (addr & 0x00FFFFFCu), value);
        return;
    }
    if (addr < kRegBase || addr >= kRegEnd)
        return;
    const std::uint32_t off = addr - kRegBase;
    if ((off >= Spg::kRegLo - kRegBase && off < Spg::kRegHi - kRegBase) || addr == Spg::kStatus) {
        spg_.write(addr, value, size);
        return;
    }
    switch (off) {
        case kId:
        case kRevision:
        case kTaYuvTexCnt:
            return;  // read-only
        case kStartRender:
            start_render();
            return;
        case kTaListInit:
            if (value >> 31) {
                ++list_inits;
                ta.reset();
                regs_[kTaNextOpb >> 2] = regs_[kTaNextOpbInit >> 2];
                regs_[kTaItpCurrent >> 2] = regs_[kTaIspBase >> 2];
            }
            return;
        case kTaListCont:
            ta.reset();
            return;
        case kSoftReset:
            regs_[off >> 2] = value;
            if (value & 1u)
                ta.reset();
            return;
        case kFbRCtrl: {
            const bool changed = ((regs_[off >> 2] ^ value) >> 23) & 1u;
            regs_[off >> 2] = value;
            if (changed)
                spg_.set_vclk_div((value >> 23) & 1u);
            return;
        }
        case kFbRSof1:
            regs_[off >> 2] = value & 0x00FFFFFCu;
            ++frame_swaps;
            return;
        case kFbRSof2:
            regs_[off >> 2] = value & 0x00FFFFFCu;
            return;
        case kFbWSof1:
        case kFbWSof2:
            regs_[off >> 2] = value & 0x01FFFFFCu;
            return;
        default:
            regs_[off >> 2] = value;
            return;
    }
}

}  // namespace dream::pvr
