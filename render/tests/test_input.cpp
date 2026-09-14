// The binding model and its file format. A binding file outlives the SDL version that wrote it and
// is the one thing here a user edits by hand, so the round trip and the failure behaviour are
// pinned rather than assumed.
#include "dream/render/input.h"

#include "doctest.h"

using namespace dream::render;

TEST_CASE("every control has a distinct file key and label") {
    for (unsigned i = 0; i < kPadControlCount; ++i) {
        const auto a = static_cast<PadControl>(i);
        REQUIRE(pad_control_key(a) != nullptr);
        REQUIRE(pad_control_label(a) != nullptr);
        PadControl back{};
        CHECK(pad_control_from_key(pad_control_key(a), back));
        CHECK(back == a);  // the key must round-trip, or a saved binding reverts on load
        for (unsigned j = i + 1; j < kPadControlCount; ++j) {
            const auto b = static_cast<PadControl>(j);
            CHECK(std::string(pad_control_key(a)) != pad_control_key(b));
        }
    }
}

TEST_CASE("a binding round-trips through its text form") {
    for (const char* t :
         {"key:Return", "key:Left Shift", "button:a", "axis:righttrigger+", "axis:lefty-"}) {
        Binding b;
        REQUIRE_MESSAGE(Binding::parse(t, b), t);
        CHECK(b.text() == t);
    }
}

TEST_CASE("an axis without a half is rejected rather than guessed") {
    // Guessing "+" would bind the wrong direction half the time, and the symptom would be a stick
    // that only works one way, which reads as a broken pad.
    Binding b;
    CHECK_FALSE(Binding::parse("axis:lefty", b));
    CHECK_FALSE(Binding::parse("axis:+", b));
    CHECK_FALSE(Binding::parse("key:", b));
    CHECK_FALSE(Binding::parse("wheel:x+", b));
    CHECK_FALSE(Binding::parse("noseparator", b));
}

TEST_CASE("defaults are the keyboard table the launcher always had") {
    const Bindings d = Bindings::defaults();
    CHECK(d.keyboard[PadControl::A].text() == "key:Z");
    CHECK(d.keyboard[PadControl::Start].text() == "key:Return");
    CHECK(d.keyboard[PadControl::RightTrigger].text() == "key:W");
    // The d-pad drives the stick too, as the launcher used to do silently.
    CHECK(d.keyboard[PadControl::StickLeft].text() == "key:Left");
    // A pad works on first plug-in without visiting any UI.
    CHECK(d.gamepad[PadControl::A].text() == "button:a");
    CHECK(d.gamepad[PadControl::RightTrigger].text() == "axis:righttrigger+");
    CHECK(d.gamepad[PadControl::StickUp].text() == "axis:lefty-");
}

TEST_CASE("bindings round-trip through the file format") {
    Bindings a = Bindings::defaults();
    a.keyboard[PadControl::A] = Binding{BindSource::Key, "Space", 1};
    a.gamepad[PadControl::StickLeft] = Binding{BindSource::Axis, "rightx", -1};
    a.deadzone_percent = 35;
    a.trigger_ramp = false;
    const Bindings b = Bindings::from_text(a.to_text());
    CHECK(b.keyboard[PadControl::A].text() == "key:Space");
    CHECK(b.gamepad[PadControl::StickLeft].text() == "axis:rightx-");
    CHECK(b.deadzone_percent == 35);
    CHECK(b.trigger_ramp == false);
    for (unsigned i = 0; i < kPadControlCount; ++i) {
        const auto c = static_cast<PadControl>(i);
        CHECK(b.keyboard[c] == a.keyboard[c]);
        CHECK(b.gamepad[c] == a.gamepad[c]);
    }
}

TEST_CASE("a damaged file leaves the game playable") {
    // The risk the design study names: a rebind that makes the game unplayable with no way back.
    // Anything unreadable must fall back to the default rather than to nothing.
    std::string w;
    const Bindings b = Bindings::from_text(
        "version = 1\n"
        "deadzone = banana\n"
        "[keyboard]\n"
        "a = key:Space\n"
        "b = nonsense\n"
        "notacontrol = key:P\n"
        "[martians]\n"
        "a = key:Q\n",
        &w);
    CHECK(b.keyboard[PadControl::A].text() == "key:Space");       // the good line applied
    CHECK(b.keyboard[PadControl::B].text() == "key:X");           // the bad one kept its default
    CHECK(b.keyboard[PadControl::Start].text() == "key:Return");  // absent lines keep theirs
    CHECK(b.deadzone_percent == 20);
    CHECK(w.find("banana") != std::string::npos);
    CHECK(w.find("nonsense") != std::string::npos);
    CHECK(w.find("notacontrol") != std::string::npos);
    CHECK(w.find("martians") != std::string::npos);
}

TEST_CASE("an empty value unbinds deliberately, and survives a round trip") {
    Bindings a = Bindings::defaults();
    a.keyboard[PadControl::Y] = Binding{};
    CHECK(a.keyboard[PadControl::Y].label() == "--");
    const Bindings b = Bindings::from_text(a.to_text());
    CHECK_FALSE(b.keyboard[PadControl::Y].bound());
}

TEST_CASE("a silly dead zone is clamped, not obeyed") {
    CHECK(Bindings::from_text("deadzone = 5000\n").deadzone_percent == 90);
    CHECK(Bindings::from_text("deadzone = -3\n").deadzone_percent == 0);
}
