#include "dream/render/input_menu.h"

#include <algorithm>

#include "dream/render/overlay.h"

namespace dream::render {
namespace {
using u32 = std::uint32_t;
}

void InputMenu::open(const Bindings& b) {
    bindings_ = b;
    open_ = true;
    awaiting_ = false;
    dirty_ = false;
    row_ = kFirstBinding;
}

DeviceBindings& InputMenu::editing() {
    return device_ == 0 ? bindings_.keyboard : bindings_.gamepad;
}

const DeviceBindings& InputMenu::editing() const {
    return device_ == 0 ? bindings_.keyboard : bindings_.gamepad;
}

bool InputMenu::on_binding(int row, PadControl& out) const {
    if (row < kFirstBinding || row >= reset_row())
        return false;
    out = static_cast<PadControl>(row - kFirstBinding);
    return true;
}

void InputMenu::move(int delta) {
    if (awaiting_)
        return;  // the screen is modal while it waits: moving would bind the wrong row
    const int n = row_count();
    row_ = (row_ + delta % n + n) % n;
}

void InputMenu::adjust(int delta, unsigned device_count) {
    if (awaiting_ || delta == 0)
        return;
    if (row_ == kDeviceRow) {
        const int n = static_cast<int>(std::max(1u, device_count));
        device_ = static_cast<unsigned>((static_cast<int>(device_) + delta % n + n) % n);
    } else if (row_ == kDeadzoneRow) {
        bindings_.deadzone_percent = std::clamp(bindings_.deadzone_percent + delta * 5, 0, 90);
        dirty_ = true;
    } else if (row_ == kRampRow) {
        bindings_.trigger_ramp = !bindings_.trigger_ramp;
        dirty_ = true;
    }
}

InputMenu::Action InputMenu::activate() {
    if (awaiting_)
        return Action::None;
    if (row_ == close_row())
        return Action::Close;
    if (row_ == reset_row()) {
        // Only this device's set, so resetting the keyboard cannot silently discard a pad layout
        // somebody spent longer on.
        const Bindings d = Bindings::defaults();
        editing() = device_ == 0 ? d.keyboard : d.gamepad;
        dirty_ = true;
        return Action::Save;
    }
    if (row_ == kRampRow) {
        bindings_.trigger_ramp = !bindings_.trigger_ramp;
        dirty_ = true;
        return Action::Save;
    }
    PadControl c{};
    if (on_binding(row_, c)) {
        awaiting_ = true;
        return Action::None;
    }
    return Action::None;
}

void InputMenu::back() {
    if (awaiting_)
        awaiting_ = false;  // one escape leaves the capture, a second leaves the screen
    else
        open_ = false;
}

void InputMenu::apply_capture(const Binding& b, bool cancelled) {
    if (!awaiting_)
        return;
    awaiting_ = false;
    if (cancelled)
        return;
    PadControl c{};
    if (!on_binding(row_, c))
        return;
    // A physical input bound to two controls at once means pressing it does both, which reads as a
    // broken pad. The older binding gives way, and the row it came from is left unbound and
    // visibly so, rather than the new binding being refused with no explanation.
    DeviceBindings& d = editing();
    for (unsigned i = 0; i < kPadControlCount; ++i)
        if (d.b[i] == b)
            d.b[i] = Binding{};
    d[c] = b;
    dirty_ = true;
}

void InputMenu::draw(u32* rgba, unsigned pitch, unsigned width, unsigned height,
                     const std::vector<std::string>& devices) const {
    if (!open_ || !rgba || width == 0 || height == 0)
        return;
    const int w = static_cast<int>(width), h = static_cast<int>(height);
    // Everything is proportional to the frame, so the screen looks the same at --scale 1 and 4.
    const unsigned s = std::max(1u, width / 320u);
    const int line = static_cast<int>(text_height(s)) + static_cast<int>(s);
    const int pad = static_cast<int>(s) * 6;

    fill_rect(rgba, pitch, width, height, 0, 0, w, h, kUiShade);

    const int card_w = std::min(w - pad * 2, static_cast<int>(s) * 250);
    const int rows = row_count();
    const int card_h = std::min(h - pad * 2, line * (rows + 5) + pad * 2);
    const int cx = (w - card_w) / 2, cy = (h - card_h) / 2;
    fill_rect(rgba, pitch, width, height, cx, cy, card_w, card_h, kUiWhite);

    // The banner. A plain white block marks it and nothing more: an orange band with white type is
    // a palette, which nobody owns, while the console's spiral is a registered trademark and
    // reproducing it in this tool's own UI would be a bad idea however it was drawn.
    const int banner_h = line + pad;
    fill_rect(rgba, pitch, width, height, cx, cy, card_w, banner_h, kUiOrange);
    const int mark = banner_h / 3;
    fill_rect(rgba, pitch, width, height, cx + pad, cy + (banner_h - mark) / 2, mark, mark,
              kUiWhite);
    TextStyle banner{kUiWhite, 0, s};
    draw_text(rgba, pitch, width, height, cx + pad + mark + pad, cy + pad / 2, "CONTROLLER",
              banner);

    const int left = cx + pad;
    const int right = cx + card_w - pad;
    int y = cy + banner_h + pad;

    // One row. The selected row is a full-width orange bar with white type, which is the only
    // strong colour on the card and so reads as the cursor at a glance.
    auto row = [&](int index, const std::string& name, const std::string& value) {
        const bool sel = index == row_;
        if (sel)
            fill_rect(rgba, pitch, width, height, left - static_cast<int>(s) * 2,
                      y - static_cast<int>(s), right - left + static_cast<int>(s) * 4, line,
                      kUiOrange);
        const TextStyle t{sel ? kUiWhite : kUiInk, 0, s};
        draw_text(rgba, pitch, width, height, left, y, name, t);
        if (!value.empty()) {
            const int vx = right - static_cast<int>(text_width(value, s));
            draw_text(rgba, pitch, width, height, vx, y, value, t);
        }
        y += line;
    };

    const std::string device_name =
        device_ < devices.size() ? devices[device_] : std::string("KEYBOARD");
    row(kDeviceRow, "DEVICE", "< " + device_name + " >");
    row(kDeadzoneRow, "DEAD ZONE", "< " + std::to_string(bindings_.deadzone_percent) + "% >");
    row(kRampRow, "TRIGGER RAMP", bindings_.trigger_ramp ? "< ON >" : "< OFF >");

    y += static_cast<int>(s) * 2;
    fill_rect(rgba, pitch, width, height, left, y, right - left, static_cast<int>(s), kUiGrey);
    y += static_cast<int>(s) * 3;

    for (unsigned i = 0; i < kPadControlCount; ++i) {
        const auto c = static_cast<PadControl>(i);
        const int index = kFirstBinding + static_cast<int>(i);
        const bool waiting = awaiting_ && index == row_;
        row(index, pad_control_label(c), waiting ? "PRESS ANY INPUT" : editing()[c].label());
    }

    y += static_cast<int>(s) * 2;
    row(reset_row(), "RESET THIS DEVICE", "");
    row(close_row(), "CLOSE", "");

    // The footer says how to work the screen, because the screen is the only place that says it.
    const TextStyle hint{kUiGrey, 0, s};
    const char* help = awaiting_ ? "PRESS AN INPUT TO BIND IT, OR ESCAPE TO CANCEL"
                                 : "ARROWS MOVE, RETURN BINDS, ESCAPE CLOSES";
    draw_text(rgba, pitch, width, height, left, cy + card_h - line, help, hint);
}

}  // namespace dream::render
