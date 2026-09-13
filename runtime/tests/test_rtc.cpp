#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/system.h"

#include "doctest.h"

using namespace dream;

TEST_CASE("rtc: seconds read as two halves, writes need the enable, tick advances") {
    System sys;
    aica::Rtc rtc(0x12345678u);
    sys.memory.map_mmio(aica::Rtc::kBase, aica::Rtc::kEnd, &rtc);
    auto& m = sys.memory;
    CHECK(m.read32(0xA0710000u) == 0x1234u);
    CHECK(m.read32(0xA0710004u) == 0x5678u);
    m.write32(0xA0710004u, 0xFFFFu);  // locked: ignored
    CHECK(m.read32(0xA0710004u) == 0x5678u);
    m.write32(0xA0710008u, 1);
    m.write32(0xA0710004u, 0x0001u);
    m.write32(0xA0710000u, 0x0002u);  // high half re-locks
    CHECK(rtc.seconds() == 0x00020001u);
    CHECK(m.read32(0xA0710008u) == 0);
    m.write32(0xA0710004u, 0x9999u);
    CHECK(rtc.seconds() == 0x00020001u);
    rtc.tick();
    CHECK(m.read32(0xA0710004u) == 0x0002u);
    // 2000-01-01 00:00 UTC in Dreamcast seconds
    CHECK(946684800u + aica::Rtc::kUnixEpochOffset == 1577836800u);
}
