// The binding screen's behaviour, without a window. The states that matter are the modal ones:
// while it waits for an input, nothing else may move, and a physical input must never end up bound
// to two controls at once.
#include <vector>

#include "dream/render/input_menu.h"

#include "doctest.h"

using namespace dream::render;

namespace {
InputMenu opened() {
    InputMenu m;
    m.open(Bindings::defaults());
    return m;
}
// The row index of a control, which the menu keeps private; the tests navigate rather than assume.
void go_to(InputMenu& m, PadControl c) {
    // From a freshly opened menu the cursor is on the first binding row.
    for (unsigned i = 0; i < static_cast<unsigned>(c); ++i) m.move(1);
}
}  // namespace

TEST_CASE("opening takes a copy and starts clean") {
    Bindings b = Bindings::defaults();
    b.deadzone_percent = 33;
    InputMenu m;
    CHECK_FALSE(m.is_open());
    m.open(b);
    CHECK(m.is_open());
    CHECK_FALSE(m.dirty());
    CHECK(m.bindings().deadzone_percent == 33);
}

TEST_CASE("binding a control stores what was captured") {
    InputMenu m = opened();
    go_to(m, PadControl::B);
    CHECK(m.activate() == InputMenu::Action::None);
    CHECK(m.awaiting());
    m.apply_capture(Binding{BindSource::Key, "Space", 1}, false);
    CHECK_FALSE(m.awaiting());
    CHECK(m.bindings().keyboard[PadControl::B].text() == "key:Space");
    CHECK(m.dirty());
}

TEST_CASE("the screen is modal while it waits") {
    // Moving or adjusting mid-capture would bind the input to whatever row it landed on.
    InputMenu m = opened();
    go_to(m, PadControl::B);
    m.activate();
    REQUIRE(m.awaiting());
    m.move(3);
    m.adjust(1, 2);
    m.apply_capture(Binding{BindSource::Key, "Space", 1}, false);
    CHECK(m.bindings().keyboard[PadControl::B].text() == "key:Space");
    CHECK(m.bindings().keyboard[PadControl::A].text() == "key:Z");  // nothing else moved
}

TEST_CASE("a cancelled capture changes nothing") {
    InputMenu m = opened();
    go_to(m, PadControl::B);
    m.activate();
    m.apply_capture(Binding{}, true);
    CHECK_FALSE(m.awaiting());
    CHECK(m.bindings().keyboard[PadControl::B].text() == "key:X");
    CHECK_FALSE(m.dirty());
}

TEST_CASE("one input cannot drive two controls") {
    // Otherwise pressing it does both, which reads as a broken pad rather than as a bad binding.
    InputMenu m = opened();
    go_to(m, PadControl::B);
    m.activate();
    m.apply_capture(Binding{BindSource::Key, "Z", 1}, false);  // Z is A's default
    CHECK(m.bindings().keyboard[PadControl::B].text() == "key:Z");
    CHECK_FALSE(m.bindings().keyboard[PadControl::A].bound());  // and visibly so
}

TEST_CASE("escape leaves the capture first and the screen second") {
    InputMenu m = opened();
    go_to(m, PadControl::B);
    m.activate();
    REQUIRE(m.awaiting());
    m.back();
    CHECK_FALSE(m.awaiting());
    CHECK(m.is_open());
    m.back();
    CHECK_FALSE(m.is_open());
}

TEST_CASE("the device row cycles and the dead zone clamps") {
    InputMenu m = opened();
    m.move(-3);  // from the first binding row up to the device row
    CHECK(m.device() == 0);
    m.adjust(1, 2);
    CHECK(m.device() == 1);
    m.adjust(1, 2);
    CHECK(m.device() == 0);  // wraps rather than sticking at the end
    m.move(1);               // dead zone
    for (int i = 0; i < 40; ++i) m.adjust(1, 2);
    CHECK(m.bindings().deadzone_percent == 90);
    for (int i = 0; i < 40; ++i) m.adjust(-1, 2);
    CHECK(m.bindings().deadzone_percent == 0);
}

TEST_CASE("editing follows the selected device") {
    InputMenu m = opened();
    m.move(-3);
    m.adjust(1, 2);  // gamepad
    go_to(m, PadControl::A);
    m.move(3);  // back down to the first binding row
    m.activate();
    m.apply_capture(Binding{BindSource::Button, "back", 1}, false);
    CHECK(m.bindings().gamepad[PadControl::A].text() == "button:back");
    CHECK(m.bindings().keyboard[PadControl::A].text() == "key:Z");  // the keyboard is untouched
}

TEST_CASE("reset restores one device, not both") {
    InputMenu m = opened();
    go_to(m, PadControl::B);
    m.activate();
    m.apply_capture(Binding{BindSource::Key, "Space", 1}, false);
    // Walk to the reset row: it sits directly after the last binding. Activating a binding row on
    // the way starts a capture, and the screen is modal until that capture ends: leave it before
    // moving on, or move() is ignored and the walk never advances.
    while (m.activate() != InputMenu::Action::Save) {
        if (m.awaiting())
            m.back();
        m.move(1);
    }
    CHECK(m.bindings().keyboard[PadControl::B].text() == "key:X");
    CHECK(m.bindings().gamepad[PadControl::A].text() == "button:a");
}

TEST_CASE("drawing clips and does not care about tiny frames") {
    // The card is sized from the frame, and a window being dragged to nothing must not fall over.
    InputMenu m = opened();
    std::vector<std::uint32_t> buf(64 * 48, 0xFF000000u);
    const std::vector<std::string> devices{"KEYBOARD", "TEST PAD"};
    m.draw(buf.data(), 64, 64, 48, devices);
    m.draw(buf.data(), 1, 1, 1, devices);
    m.draw(nullptr, 64, 64, 48, devices);
    m.draw(buf.data(), 64, 0, 0, devices);
    // A closed menu draws nothing at all.
    InputMenu closed;
    std::vector<std::uint32_t> clean(64 * 48, 0xFF000000u);
    const auto before = clean;
    closed.draw(clean.data(), 64, 64, 48, devices);
    CHECK(clean == before);
}
