#include <cstring>
#include <memory>

#include "dream/runtime/maple/maple.h"
#include "dream/runtime/system.h"

#include "doctest.h"

using namespace dream;

namespace {
struct Rig {
    System sys;
    maple::Bus bus{sys.sched, sys.holly, sys.memory};
    maple::Controller* pad = nullptr;
    Rig() {
        auto c = std::make_unique<maple::Controller>();
        pad = c.get();
        bus.attach(0, std::move(c));
        sys.memory.map_mmio(maple::Bus::kBase, maple::Bus::kEnd, &bus);
    }
    // One-frame descriptor list at `desc` with the reply going to `recv`.
    void descriptor(std::uint32_t desc, std::uint32_t recv, unsigned port, std::uint8_t cmd,
                    const std::vector<std::uint32_t>& args) {
        auto& m = sys.memory;
        const std::uint32_t words = static_cast<std::uint32_t>(1 + args.size());
        m.write32(desc, 0x80000000u | (port << 16) | (words - 1));
        m.write32(desc + 4, recv);
        const std::uint32_t reci = (port << 6) | 0x20u, send = port << 6;
        m.write32(desc + 8, cmd | (reci << 8) | (send << 16) |
                                (static_cast<std::uint32_t>(args.size()) << 24));
        for (std::size_t i = 0; i < args.size(); ++i)
            m.write32(desc + 12 + 4 * static_cast<std::uint32_t>(i), args[i]);
    }
    void kick(std::uint32_t desc) {
        auto& m = sys.memory;
        m.write32(0xA05F6C04u, desc);  // SB_MDSTAR
        m.write32(0xA05F6C14u, 1);     // SB_MDEN
        m.write32(0xA05F6C18u, 1);     // SB_MDST: software trigger
    }
};
}  // namespace

TEST_CASE("maple: a device request returns the controller's fixed status block") {
    Rig r;
    auto& m = r.sys.memory;
    r.descriptor(0x8C100000u, 0x8C100100u, 0, maple::kDeviceRequest, {});
    r.kick(0x8C100000u);
    CHECK(m.read32(0xA05F6C18u) == 1u);  // busy until the transfer time has passed
    CHECK(m.read32(0x8C100100u) == 0u);
    r.sys.sched.advance(400'000);
    CHECK(m.read32(0xA05F6C18u) == 0u);
    // Reply header: DeviceStatus, sender = device, recipient = console, 28 words of payload.
    CHECK(m.read32(0x8C100100u) == (0x05u | (0x00u << 8) | (0x20u << 16) | (28u << 24)));
    CHECK(m.read32(0x8C100104u) == maple::kInput);
    CHECK(m.read32(0x8C100108u) == 0xFE060F00u);
    CHECK(m.read8(0x8C100114u) == 0xFFu);  // area code
    char name[31] = {};
    for (int i = 0; i < 30; ++i)
        name[i] = static_cast<char>(m.read8(0x8C100116u + static_cast<std::uint32_t>(i)));
    CHECK(std::string(name) == "Dreamcast Controller          ");
    CHECK(m.read16(0x8C100100u + 4 + 4 * 4 + 2 + 30 + 60) == 0x01AEu);
    CHECK(r.sys.holly.raised(holly::Irq::MapleDma));
    CHECK(r.bus.frames == 1);
}

TEST_CASE("maple: get-condition reports buttons and axes; absent ports time out") {
    Rig r;
    auto& m = r.sys.memory;
    r.pad->state.buttons = static_cast<std::uint16_t>(~(maple::kA | maple::kStart));
    r.pad->state.rtrigger = 200;
    r.pad->state.joy_x = 0x20;
    r.descriptor(0x8C100000u, 0x8C100100u, 0, maple::kGetCondition, {maple::kInput});
    r.kick(0x8C100000u);
    r.sys.sched.advance(400'000);
    CHECK(m.read32(0x8C100100u) == (0x08u | (0x20u << 16) | (3u << 24)));
    CHECK(m.read32(0x8C100104u) == maple::kInput);
    const std::uint16_t buttons = m.read16(0x8C100108u);
    CHECK((buttons & maple::kA) == 0);      // pressed
    CHECK((buttons & maple::kStart) == 0);  // pressed
    CHECK((buttons & maple::kB) != 0);      // released
    CHECK((buttons & maple::kZ) != 0);      // absent buttons read released
    CHECK(m.read8(0x8C10010Au) == 200u);    // right trigger
    CHECK(m.read8(0x8C10010Bu) == 0u);      // left trigger
    CHECK(m.read8(0x8C10010Cu) == 0x20u);   // stick x
    CHECK(m.read8(0x8C10010Du) == 0x80u);   // stick y centred
    CHECK(r.pad->condition_reads == 1);
    // Port 1 has nothing plugged in: the receive buffer gets the timeout marker.
    r.descriptor(0x8C100200u, 0x8C100300u, 1, maple::kDeviceRequest, {});
    r.kick(0x8C100200u);
    r.sys.sched.advance(400'000);
    CHECK(m.read32(0x8C100300u) == 0xFFFFFFFFu);
    CHECK(r.bus.no_device == 1);
    // Wrong function code is answered with an error reply.
    r.descriptor(0x8C100400u, 0x8C100500u, 0, maple::kGetCondition, {maple::kStorage});
    r.kick(0x8C100400u);
    r.sys.sched.advance(400'000);
    CHECK((m.read32(0x8C100500u) & 0xFFu) == maple::kUnknownFunction);
}

TEST_CASE("maple: the VBlank hardware trigger runs the list once per frame") {
    Rig r;
    auto& m = r.sys.memory;
    r.sys.spg.on_vblank_out = [&] { r.bus.vblank(); };
    r.descriptor(0x8C100000u, 0x8C100100u, 0, maple::kGetCondition, {maple::kInput});
    m.write32(0xA05F6C04u, 0x8C100000u);
    m.write32(0xA05F6C10u, 1);  // SB_MDTSEL: VBlank trigger
    m.write32(0xA05F6C14u, 1);  // SB_MDEN
    r.sys.sched.advance(r.sys.spg.frame_cycles());
    CHECK(r.bus.transfers >= 1);
    CHECK(m.read32(0x8C100104u) == maple::kInput);
    const std::uint64_t before = r.bus.transfers;
    r.sys.sched.advance(r.sys.spg.frame_cycles() * 3);
    CHECK(r.bus.transfers == before + 3);
    // Disabling the bus stops the trigger.
    m.write32(0xA05F6C14u, 0);
    r.sys.sched.advance(r.sys.spg.frame_cycles() * 2);
    CHECK(r.bus.transfers == before + 3);
}

// An analogue reading as the controller's byte. The centre value is the one that matters: a title
// reads 0x80 as "not steering", and a conversion that lands a resting stick one off it makes every
// car drift.
TEST_CASE("an axis converts to the byte the hardware calls centred") {
    using dream::maple::axis_byte;
    CHECK(axis_byte(0.0f, 0.0f) == 0x80);  // nothing held
    CHECK(axis_byte(1.0f, 1.0f) == 0x80);  // both halves: they cancel rather than fight
    // Two digital sources still give the extremes the keyboard always produced.
    CHECK(axis_byte(1.0f, 0.0f) == 0x00);
    CHECK(axis_byte(0.0f, 1.0f) == 0xFF);
    // A real stick's travel survives. Half travel is 191.25 and 63.75 rather than a round number
    // either side: centre is 127.5 on a byte that has no such value, so one side of it is a count
    // shorter. That is what the byte can express, not a rounding fault.
    CHECK(axis_byte(0.0f, 0.5f) == 191);
    CHECK(axis_byte(0.5f, 0.0f) == 64);
    // Out of range is clamped, not wrapped: a wrapped axis is full lock the wrong way.
    CHECK(axis_byte(0.0f, 9.0f) == 0xFF);
    CHECK(axis_byte(9.0f, 0.0f) == 0x00);
}

TEST_CASE("a trigger keeps its travel") {
    using dream::maple::trigger_byte;
    CHECK(trigger_byte(0.0f) == 0x00);
    CHECK(trigger_byte(1.0f) == 0xFF);
    CHECK(trigger_byte(0.5f) == 128);
    CHECK(trigger_byte(-1.0f) == 0x00);
    CHECK(trigger_byte(2.0f) == 0xFF);
}
