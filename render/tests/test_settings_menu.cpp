// The settings screen: tabs, the memory card slots, the picture files and what draws them.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dream/render/image.h"
#include "dream/render/settings_menu.h"

#include "doctest.h"

using dream::render::Bindings;
using dream::render::PadControl;
using dream::render::SettingsMenu;
using Kind = SettingsMenu::Choice::Kind;

namespace {

SettingsMenu memory_tab(std::vector<std::string> files, const std::string& in_a1) {
    SettingsMenu m;
    m.open(Bindings::defaults(), SettingsMenu::Tab::Memory);
    m.set_card_files(std::move(files));
    dream::render::SlotView a1;
    a1.file = in_a1;
    m.set_slot(0, a1);
    return m;
}

}  // namespace

TEST_CASE("hotspot files name controls by their bindings keys") {
    std::string warnings;
    const auto h = dream::render::parse_hotspots(
        "# a comment\n"
        "a 0.5 0.25 0.05\n"
        "dpad_up 0.1 0.2 0.03   # trailing comment\n"
        "nonsense 0.1 0.1 0.1\n"
        "b 2 0.5 0.1\n",
        &warnings);
    const auto& a = h[static_cast<unsigned>(PadControl::A)];
    CHECK(a.set);
    CHECK(a.x == doctest::Approx(0.5));
    CHECK(a.r == doctest::Approx(0.05));
    CHECK(h[static_cast<unsigned>(PadControl::Up)].set);
    CHECK_FALSE(h[static_cast<unsigned>(PadControl::B)].set);  // out of range
    CHECK(warnings.find("nonsense") != std::string::npos);
    CHECK(warnings.find("line 5") != std::string::npos);
}

TEST_CASE("the card's screen placement is a rectangle and two optional flips") {
    const auto p = dream::render::parse_lcd_placement("lcd 0.2 0.3 0.8 0.6\nflip_y\n");
    CHECK(p.set);
    CHECK(p.x1 == doctest::Approx(0.8));
    CHECK_FALSE(p.flip_x);
    CHECK(p.flip_y);
    CHECK_FALSE(dream::render::parse_lcd_placement("lcd 0.8 0.3 0.2 0.6").set);  // inside out
}

TEST_CASE("tabs cycle, but not while a binding waits for its input") {
    SettingsMenu m;
    m.open(Bindings::defaults());
    CHECK(m.tab() == SettingsMenu::Tab::Controller);
    m.switch_tab(1);
    CHECK(m.tab() == SettingsMenu::Tab::Memory);
    m.switch_tab(1);
    CHECK(m.tab() == SettingsMenu::Tab::Controller);
    m.switch_tab(-1);
    CHECK(m.tab() == SettingsMenu::Tab::Memory);
    m.switch_tab(1);
    // Down to the first binding row and start a capture: the tab must stay put.
    m.activate();
    REQUIRE(m.awaiting());
    m.switch_tab(1);
    CHECK(m.tab() == SettingsMenu::Tab::Controller);
    m.back();  // leaves the capture, not the screen
    CHECK_FALSE(m.awaiting());
    CHECK(m.is_open());
}

TEST_CASE("return on the editing row steps through the devices") {
    SettingsMenu m;
    m.open(Bindings::defaults());
    m.move(-3);  // from the first binding row up to the editing row
    CHECK(m.editing_device() == 0);
    m.activate(2);  // keyboard -> the pad
    CHECK(m.editing_device() == 1);
    m.activate(2);  // and round again
    CHECK(m.editing_device() == 0);
    m.activate(1);  // with no pad there is nowhere to go
    CHECK(m.editing_device() == 0);
    CHECK_FALSE(m.awaiting());
}

TEST_CASE("a slot choice is only a choice until it is confirmed") {
    auto m = memory_tab({"a.bin", "b.bin"}, "a.bin");
    CHECK(m.shown_choice(0).kind == Kind::Card);
    m.adjust(1, 1);  // a.bin -> b.bin
    CHECK(m.shown_choice(0).file == "b.bin");
    CHECK(m.slot(0).file == "a.bin");  // nothing happened yet
    m.back();                          // drops the choice, keeps the screen
    CHECK(m.is_open());
    CHECK(m.shown_choice(0).file == "a.bin");
    m.adjust(1, 1);
    CHECK(m.activate() == SettingsMenu::Action::Swap);
    CHECK(m.swap_slot() == 0);
    CHECK(m.swap_choice().file == "b.bin");
    // Confirming the slot's own card again asks for nothing.
    CHECK(m.activate() == SettingsMenu::Action::None);
}

TEST_CASE("one file cannot be in both slots, and the second slot offers the rumble pack") {
    auto m = memory_tab({"a.bin", "b.bin"}, "a.bin");
    m.set_rumble_available(true);
    m.move(1);  // slot A2, empty
    std::vector<SettingsMenu::Choice> seen;
    for (int i = 0; i < 6; ++i) {
        m.adjust(1, 1);
        seen.push_back(m.shown_choice(1));
    }
    bool rumble = false, a = false, b = false, fresh = false;
    for (const auto& c : seen) {
        rumble |= c.kind == Kind::Rumble;
        a |= c.kind == Kind::Card && c.file == "a.bin";
        b |= c.kind == Kind::Card && c.file == "b.bin";
        fresh |= c.kind == Kind::NewCard;
    }
    CHECK(rumble);
    CHECK_FALSE(a);  // already in A1
    CHECK(b);
    CHECK(fresh);
}

TEST_CASE("moving off a slot row abandons its choice, and the last row closes") {
    auto m = memory_tab({"a.bin"}, "");
    m.adjust(1, 1);
    CHECK(m.shown_choice(0).kind != Kind::Empty);
    m.move(1);
    m.move(-1);
    CHECK(m.shown_choice(0).kind == Kind::Empty);
    m.move(-1);  // wraps to the last row
    CHECK(m.activate() == SettingsMenu::Action::Close);
    m.back();
    CHECK_FALSE(m.is_open());
}

TEST_CASE("card file names become labels") {
    CHECK(dream::render::card_label("rush2049-vmu.bin") == "RUSH2049-VMU");
    CHECK(dream::render::card_label("C:/saves/card.bin") == "CARD");
    CHECK(dream::render::card_label("noext") == "NOEXT");
}

TEST_CASE("shrinking a cut-out picture keeps its shape and does not darken its edge") {
    dream::render::Image img{4, 2, std::vector<std::uint32_t>(8, 0u)};
    // Left half opaque white, right half fully transparent.
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 2; ++x) img.rgba[y * 4 + x] = 0xFFFFFFFFu;
    const auto half = dream::render::scale_to_fit(img, 2, 100);
    CHECK(half.width == 2);
    CHECK(half.height == 1);
    // Averaging 2x2 blocks: the left one stays white, the right one stays clear.
    CHECK(half.rgba[0] == 0xFFFFFFFFu);
    CHECK((half.rgba[1] >> 24) == 0u);
    const auto mixed = dream::render::scale_to_fit(img, 1, 1);
    CHECK((mixed.rgba[0] & 0xFFFFFFu) == 0xFFFFFFu);  // white, half covered: never grey
    CHECK((mixed.rgba[0] >> 24) == 127u);
}

TEST_CASE(
    "the card's screen is decoded the right way up for a pad, where the card is upside down") {
    std::array<std::uint8_t, 192> lcd{};
    lcd[5] = 0x01;       // the first row sent, last byte, low bit: the bottom-left pixel
    lcd[6 * 31] = 0x80;  // the last row sent, first byte, high bit: the top-right pixel
    CHECK(dream::render::lcd_pixel(lcd, 0, 31));
    CHECK_FALSE(dream::render::lcd_pixel(lcd, 1, 31));
    CHECK(dream::render::lcd_pixel(lcd, 47, 0));
    CHECK_FALSE(dream::render::lcd_pixel(lcd, 0, 0));
    CHECK_FALSE(dream::render::lcd_pixel(lcd, 48, 0));
}

TEST_CASE("the screen draws on any frame, with or without pictures") {
    SettingsMenu m;
    m.open(Bindings::defaults());
    const std::vector<std::string> devices{"KEYBOARD", "SOME PAD WITH A VERY LONG NAME INDEED"};
    dream::render::PadPreview pad{};
    pad.held[static_cast<unsigned>(PadControl::A)] = true;
    pad.value[static_cast<unsigned>(PadControl::RightTrigger)] = 0.5f;
    dream::render::SettingsArt art;
    art.controller = dream::render::Image{8, 8, std::vector<std::uint32_t>(64, 0xFF808080u)};
    art.front = dream::render::parse_hotspots("a 0.5 0.5 0.2\n");
    std::vector<std::uint32_t> buf(640 * 480, 0);
    m.draw(buf.data(), 640, 640, 480, devices, pad, art);
    m.draw(buf.data(), 64, 64, 48, devices, pad, art);
    m.draw(buf.data(), 1, 1, 1, devices, pad, art);
    m.switch_tab(1);
    dream::render::SlotView v;
    v.file = "a.bin";
    v.lcd_drawn = true;
    v.lcd.fill(0xFF);
    m.set_slot(0, v);
    m.draw(buf.data(), 640, 640, 480, devices, pad, dream::render::SettingsArt{});
    m.draw(nullptr, 640, 640, 480, devices, pad, art);
    // A closed screen leaves the frame alone.
    std::vector<std::uint32_t> clean(64 * 48, 0x12345678u);
    SettingsMenu closed;
    closed.draw(clean.data(), 64, 64, 48, devices, pad, art);
    for (std::uint32_t p : clean) CHECK(p == 0x12345678u);
}
