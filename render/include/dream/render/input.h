// What a player has bound to each of the Dreamcast pad's inputs.
//
// Device-independent on purpose, like the rest of this layer: no SDL here, so the model, the
// defaults and the file format are unit-tested on every CI runner. Physical inputs are held as
// portable *names* ("Return", "righttrigger") rather than as SDL's numeric codes, because a
// binding file outlives the SDL version that wrote it and a renumbered enum would silently rebind
// somebody's controls. The SDL layer resolves names to codes once, when the bindings change.
//
// Two devices are bound independently and both are live at once, which is what players expect: the
// keyboard still works with a pad plugged in. The device list in the UI chooses which set you are
// *editing*, not which one is active.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace dream::render {

// The pad's inputs as things a player binds. The stick directions are separate entries because a
// keyboard binds four keys to them while a pad binds one axis; the binding layer is where that
// difference belongs, not the launcher.
enum class PadControl : unsigned {
    A,
    B,
    X,
    Y,
    Start,
    Up,
    Down,
    Left,
    Right,
    LeftTrigger,
    RightTrigger,
    StickUp,
    StickDown,
    StickLeft,
    StickRight,
    Count
};
constexpr unsigned kPadControlCount = static_cast<unsigned>(PadControl::Count);

// For the UI. Upper case because the overlay font is.
const char* pad_control_label(PadControl c);
// For the file. Stable forever once written: changing one silently drops that binding.
const char* pad_control_key(PadControl c);
bool pad_control_from_key(std::string_view key, PadControl& out);

enum class BindSource : unsigned { None, Key, Button, Axis };

struct Binding {
    BindSource source = BindSource::None;
    std::string code;  // SDL's own name for the scancode, button or axis
    int sign = 1;      // which half of an axis; ignored for the others

    bool bound() const noexcept { return source != BindSource::None; }
    bool operator==(const Binding& o) const noexcept {
        return source == o.source && code == o.code &&
               (source != BindSource::Axis || sign == o.sign);
    }
    bool operator!=(const Binding& o) const noexcept { return !(*this == o); }

    // "key:Return", "button:a", "axis:righttrigger+". Empty when unbound.
    std::string text() const;
    static bool parse(std::string_view text, Binding& out);
    // What the UI shows: the code alone, upper-cased, or "--" when unbound.
    std::string label() const;
};

struct DeviceBindings {
    std::array<Binding, kPadControlCount> b{};
    const Binding& operator[](PadControl c) const { return b[static_cast<unsigned>(c)]; }
    Binding& operator[](PadControl c) { return b[static_cast<unsigned>(c)]; }
};

struct Bindings {
    DeviceBindings keyboard;
    DeviceBindings gamepad;
    // Percent of an axis's travel ignored around the centre. A worn stick steers on its own
    // without one, which reads as a physics bug rather than as a hardware fault.
    int deadzone_percent = 20;
    // Ramp a digital trigger to full over about 150 ms rather than jumping. A keyboard accelerator
    // is otherwise fully down or fully up, which for a driving game is most of why a pad matters.
    bool trigger_ramp = true;

    static Bindings defaults();
    std::string to_text() const;
    // Never fails destructively: an unreadable or partial file leaves the defaults in place for
    // anything it did not set, so a bad file cannot make the game unplayable with no way back.
    // `warnings` collects what was ignored and why.
    static Bindings from_text(std::string_view text, std::string* warnings = nullptr);
};

}  // namespace dream::render
