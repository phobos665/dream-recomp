#include "dream/render/settings_menu.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "dream/render/overlay.h"
#include "dream/runtime/maple/maple.h"

namespace dream::render {
namespace {
using u32 = std::uint32_t;

// The card's screen: dark pixels on whatever the glass behind them is.
constexpr u32 kLcdInk = 0xFF1E2A24u;
constexpr u32 kLcdGlass = 0xFF9AB4A4u;  // only drawn when there is no picture to show the glass
constexpr u32 kPressed = 0xC02D8BEEu;   // the UI orange, three quarters opaque
constexpr u32 kMist = 0xA0FFFFFFu;      // fades a picture whose slot is empty

constexpr unsigned kLcdW = 48, kLcdH = 32;

std::string_view strip_comment(std::string_view line) {
    const auto hash = line.find('#');
    return hash == std::string_view::npos ? line : line.substr(0, hash);
}

template <typename F>
void for_each_line(std::string_view text, F&& f) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto nl = text.find('\n', start);
        const auto end = nl == std::string_view::npos ? text.size() : nl;
        f(strip_comment(text.substr(start, end - start)));
        if (nl == std::string_view::npos)
            break;
        start = nl + 1;
    }
}

int wrap(int v, int n) {
    return n <= 0 ? 0 : ((v % n) + n) % n;
}

std::string choice_label(const SettingsMenu::Choice& c) {
    switch (c.kind) {
        case SettingsMenu::Choice::Kind::Empty:
            return "EMPTY";
        case SettingsMenu::Choice::Kind::Card:
            return card_label(c.file);
        case SettingsMenu::Choice::Kind::NewCard:
            return "NEW CARD";
        case SettingsMenu::Choice::Kind::Rumble:
            return "RUMBLE PACK";
    }
    return "";
}

}  // namespace

Hotspots parse_hotspots(std::string_view text, std::string* warnings) {
    Hotspots h{};
    unsigned line_no = 0;
    for_each_line(text, [&](std::string_view line) {
        ++line_no;
        std::istringstream in{std::string(line)};
        std::string name;
        if (!(in >> name))
            return;  // blank or comment
        float x = 0, y = 0, r = 0;
        PadControl c{};
        if (!(in >> x >> y >> r) || !pad_control_from_key(name, c) || x < 0 || x > 1 || y < 0 ||
            y > 1 || r <= 0 || r > 1) {
            if (warnings)
                *warnings +=
                    "line " + std::to_string(line_no) + ": ignored \"" + std::string(line) + "\"\n";
            return;
        }
        h[static_cast<unsigned>(c)] = Hotspot{x, y, r, true};
    });
    return h;
}

LcdPlacement parse_lcd_placement(std::string_view text) {
    LcdPlacement p;
    for_each_line(text, [&](std::string_view line) {
        std::istringstream in{std::string(line)};
        std::string key;
        if (!(in >> key))
            return;
        if (key == "lcd") {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (in >> x0 >> y0 >> x1 >> y1 && x0 >= 0 && y0 >= 0 && x1 <= 1 && y1 <= 1 && x1 > x0 &&
                y1 > y0) {
                p.x0 = x0;
                p.y0 = y0;
                p.x1 = x1;
                p.y1 = y1;
                p.set = true;
            }
        } else if (key == "flip_x") {
            p.flip_x = true;
        } else if (key == "flip_y") {
            p.flip_y = true;
        }
    });
    return p;
}

bool lcd_pixel(const std::array<std::uint8_t, 192>& lcd, unsigned x, unsigned y) {
    return maple::lcd_pixel(lcd, x, y);
}

std::string card_label(std::string_view file) {
    const auto slash = file.find_last_of("/\\");
    if (slash != std::string_view::npos)
        file.remove_prefix(slash + 1);
    const auto dot = file.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0)
        file = file.substr(0, dot);
    std::string s(file);
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

// --- state ----------------------------------------------------------------------------------

void SettingsMenu::open(const Bindings& b, Tab tab) {
    input_.open(b);
    open_ = true;
    tab_ = tab;
    mem_row_ = kSlotA1;
    focus_slot_ = 0;
    pending_ = {-1, -1};
}

void SettingsMenu::switch_tab(int delta) {
    if (awaiting() || delta == 0)
        return;
    pending_ = {-1, -1};
    tab_ = static_cast<Tab>(wrap(static_cast<int>(tab_) + delta, static_cast<int>(Tab::Count)));
}

void SettingsMenu::set_slot(unsigned slot, SlotView view) {
    if (slot >= kSlots)
        return;
    slots_[slot] = std::move(view);
}

SettingsMenu::Choice SettingsMenu::current(unsigned slot) const {
    const SlotView& v = slots_[slot];
    if (v.rumble)
        return Choice{Choice::Kind::Rumble, {}};
    if (v.file.empty())
        return Choice{Choice::Kind::Empty, {}};
    return Choice{Choice::Kind::Card, v.file};
}

std::vector<SettingsMenu::Choice> SettingsMenu::choices(unsigned slot) const {
    std::vector<Choice> out;
    out.push_back({Choice::Kind::Empty, {}});
    // The second slot is where the pad's rumble pack goes on the console too.
    if (slot == 1 && rumble_available_)
        out.push_back({Choice::Kind::Rumble, {}});
    const Choice here = current(slot);
    const Choice there = current(1 - slot);
    if (here.kind == Choice::Kind::Card &&
        std::find(files_.begin(), files_.end(), here.file) == files_.end())
        out.push_back(here);  // a card from somewhere else stays choosable where it is
    // One file cannot be two cards: the other slot's card is not offered.
    for (const std::string& f : files_)
        if (!(there.kind == Choice::Kind::Card && there.file == f))
            out.push_back({Choice::Kind::Card, f});
    out.push_back({Choice::Kind::NewCard, {}});
    return out;
}

SettingsMenu::Choice SettingsMenu::shown_choice(unsigned slot) const {
    if (slot >= kSlots)
        return {};
    if (pending_[slot] >= 0) {
        const auto c = choices(slot);
        if (static_cast<std::size_t>(pending_[slot]) < c.size())
            return c[static_cast<std::size_t>(pending_[slot])];
    }
    return current(slot);
}

void SettingsMenu::move(int delta) {
    if (tab_ == Tab::Controller) {
        input_.move(delta);
        return;
    }
    pending_ = {-1, -1};  // moving off a slot row abandons a choice that was never confirmed
    mem_row_ = wrap(mem_row_ + delta, kMemRows);
    if (mem_row_ < kMemClose)
        focus_slot_ = static_cast<unsigned>(mem_row_);
}

void SettingsMenu::adjust(int delta, unsigned device_count) {
    if (tab_ == Tab::Controller) {
        input_.adjust(delta, device_count);
        return;
    }
    if (mem_row_ >= kMemClose || delta == 0)
        return;
    const unsigned slot = static_cast<unsigned>(mem_row_);
    const auto c = choices(slot);
    int i = pending_[slot];
    if (i < 0) {
        const auto it = std::find(c.begin(), c.end(), current(slot));
        i = it == c.end() ? 0 : static_cast<int>(it - c.begin());
    }
    i = wrap(i + delta, static_cast<int>(c.size()));
    pending_[slot] = c[static_cast<std::size_t>(i)] == current(slot) ? -1 : i;
}

SettingsMenu::Action SettingsMenu::activate(unsigned device_count) {
    if (tab_ == Tab::Controller)
        return input_.activate(device_count) == InputMenu::Action::Close ? Action::Close
                                                                         : Action::None;
    if (mem_row_ == kMemClose)
        return Action::Close;
    const unsigned slot = static_cast<unsigned>(mem_row_);
    if (pending_[slot] < 0)
        return Action::None;
    swap_slot_ = slot;
    swap_choice_ = shown_choice(slot);
    pending_[slot] = -1;
    return Action::Swap;
}

void SettingsMenu::back() {
    if (tab_ == Tab::Controller && awaiting()) {
        input_.back();  // leaves the capture only
        return;
    }
    if (tab_ == Tab::Memory && mem_row_ < kMemClose &&
        pending_[static_cast<unsigned>(mem_row_)] >= 0) {
        pending_[static_cast<unsigned>(mem_row_)] = -1;
        return;
    }
    open_ = false;
}

// --- drawing --------------------------------------------------------------------------------

const Image& SettingsMenu::scaled(const Image& src, unsigned max_w, unsigned max_h) const {
    for (const Scaled& s : cache_)
        if (s.src == &src && s.max_w == max_w && s.max_h == max_h)
            return s.image;
    cache_.push_back(Scaled{&src, max_w, max_h, scale_to_fit(src, max_w, max_h)});
    return cache_.back().image;
}

void SettingsMenu::draw(u32* rgba, unsigned pitch, unsigned width, unsigned height,
                        const std::vector<std::string>& devices, const PadPreview& preview,
                        const SettingsArt& art) const {
    if (!open_ || !rgba || width == 0 || height == 0)
        return;
    // Pictures are scaled for one frame size; a different size starts the cache again. A deque,
    // so a picture scaled later in this frame never moves one scaled earlier.
    if (cache_width_ != width || cache_height_ != height) {
        cache_.clear();
        cache_width_ = width;
        cache_height_ = height;
    }
    const int w = static_cast<int>(width), h = static_cast<int>(height);
    const unsigned s = std::max(1u, width / 320u);
    const int si = static_cast<int>(s);
    const int line = static_cast<int>(text_height(s)) + si;
    const int pad = si * 6;
    const int margin = si * 3;

    fill_rect(rgba, pitch, width, height, 0, 0, w, h, kUiShade);
    const int cx = margin, cy = margin, cw = w - margin * 2, ch = h - margin * 2;
    if (cw <= pad * 4 || ch <= line * 4)
        return;
    fill_rect(rgba, pitch, width, height, cx, cy, cw, ch, kUiWhite);

    // The banner: the screen's name, then the tabs, the current one cut out of the orange.
    const int banner_h = line + pad;
    fill_rect(rgba, pitch, width, height, cx, cy, cw, banner_h, kUiOrange);
    const int mark = banner_h / 3;
    fill_rect(rgba, pitch, width, height, cx + pad, cy + (banner_h - mark) / 2, mark, mark,
              kUiWhite);
    draw_text(rgba, pitch, width, height, cx + pad + mark + pad, cy + pad / 2, "SETTINGS",
              TextStyle{kUiWhite, 0, s});
    static constexpr const char* kTabNames[] = {"CONTROLLER", "MEMORY CARD"};
    int tx = cx + cw - pad;
    for (int t = static_cast<int>(Tab::Count) - 1; t >= 0; --t) {
        const char* name = kTabNames[t];
        const int tw = static_cast<int>(text_width(name, s)) + pad;
        tx -= tw;
        const bool active = t == static_cast<int>(tab_);
        if (active)
            fill_rect(rgba, pitch, width, height, tx, cy + si * 2, tw, banner_h - si * 2, kUiWhite);
        draw_text(rgba, pitch, width, height, tx + pad / 2, cy + pad / 2, name,
                  TextStyle{active ? kUiOrange : kUiWhite, 0, s});
        tx -= si * 2;
    }

    const int left = cx + pad, right = cx + cw - pad;
    const int top = cy + banner_h + pad;
    const int footer_y = cy + ch - line;
    const int bottom = footer_y - si * 2;
    if (tab_ == Tab::Controller)
        draw_controller(rgba, pitch, width, height, left, top, right, bottom, s, devices, preview,
                        art);
    else
        draw_memory(rgba, pitch, width, height, left, top, right, bottom, s, art);

    // The footer says what the keys do on the row the cursor is on, because that is the question
    // a player has: a row with arrows round its value changes with left and right.
    const char* help = "< > CHOOSE  RETURN INSERTS  Q/W TABS  ESC CLOSES";
    if (tab_ == Tab::Controller) {
        if (awaiting())
            help = "PRESS A KEY OR BUTTON TO BIND IT, ESC CANCELS";
        else if (input_.on_device_row() && devices.size() <= 1)
            // Nothing to step to: the usual reason is a wireless pad that has gone to sleep.
            help = "NO PAD FOUND: SWITCH IT ON OR PLUG IT IN";
        else if (input_.on_setting_row())
            help = "LEFT/RIGHT CHANGES  Q/W TABS  ESC CLOSES";
        else
            help = "RETURN THEN PRESS THE NEW KEY  ESC CLOSES";
    }
    draw_text(rgba, pitch, width, height, left, footer_y, fit_text(help, right - left, s),
              TextStyle{kUiGrey, 0, s});
}

void SettingsMenu::draw_controller(u32* rgba, unsigned pitch, unsigned width, unsigned height,
                                   int left, int top, int right, int bottom, unsigned s,
                                   const std::vector<std::string>& devices, const PadPreview& pad,
                                   const SettingsArt& art) const {
    const int si = static_cast<int>(s);
    const int gap = si * 6;
    const int colw = (right - left - gap) * 44 / 100;
    const int rows_left = left + colw + gap;
    input_.draw_rows(rgba, pitch, width, height, rows_left, top, right, s, devices);

    PadControl selected{};
    const bool has_selected = input_.selected_control(selected);
    auto active = [&](PadControl c) {
        const unsigned i = static_cast<unsigned>(c);
        return pad.held[i] || pad.value[i] > 0.15f;
    };
    // Marks one control on a picture drawn at (px, py): lit while pressed, ringed while selected.
    auto mark = [&](const Image& img, int px, int py, const Hotspot& spot, PadControl c) {
        if (!spot.set)
            return;
        const int x = px + static_cast<int>(spot.x * static_cast<float>(img.width));
        const int y = py + static_cast<int>(spot.y * static_cast<float>(img.height));
        const int r = std::max(2, static_cast<int>(spot.r * static_cast<float>(img.width)));
        if (active(c))
            fill_disc(rgba, pitch, width, height, x, y, r, kPressed);
        if (has_selected && selected == c) {
            draw_ring(rgba, pitch, width, height, x, y, r + si * 2, si + 1, kUiOrange);
            draw_ring(rgba, pitch, width, height, x, y, r + si * 3, 1, kUiWhite);
        }
    };

    const int area_h = bottom - top;
    const Image& front = scaled(art.controller, static_cast<unsigned>(std::max(1, colw)),
                                static_cast<unsigned>(std::max(1, area_h * 62 / 100)));
    int below = top;
    if (!front.empty()) {
        const int fx = left + (colw - static_cast<int>(front.width)) / 2;
        blit(rgba, pitch, width, height, fx, top, front);
        for (unsigned i = 0; i < kPadControlCount; ++i)
            mark(front, fx, top, art.front[i], static_cast<PadControl>(i));
        // The stick's position, as a dot inside its four direction marks.
        const auto& f = art.front;
        const Hotspot& up = f[static_cast<unsigned>(PadControl::StickUp)];
        const Hotspot& dn = f[static_cast<unsigned>(PadControl::StickDown)];
        const Hotspot& lt = f[static_cast<unsigned>(PadControl::StickLeft)];
        const Hotspot& rt = f[static_cast<unsigned>(PadControl::StickRight)];
        if (up.set && dn.set && lt.set && rt.set) {
            const float fw = static_cast<float>(front.width), fh = static_cast<float>(front.height);
            const float sx = pad.value[static_cast<unsigned>(PadControl::StickRight)] -
                             pad.value[static_cast<unsigned>(PadControl::StickLeft)];
            const float sy = pad.value[static_cast<unsigned>(PadControl::StickDown)] -
                             pad.value[static_cast<unsigned>(PadControl::StickUp)];
            const float ccx = (lt.x + rt.x) / 2 * fw, ccy = (up.y + dn.y) / 2 * fh;
            const float rx = (rt.x - lt.x) / 2 * fw, ry = (dn.y - up.y) / 2 * fh;
            const int dx = fx + static_cast<int>(ccx + sx * rx);
            const int dy = top + static_cast<int>(ccy + sy * ry);
            fill_disc(rgba, pitch, width, height, dx, dy, si * 2 + 1, kUiWhite);
            draw_ring(rgba, pitch, width, height, dx, dy, si * 2 + 1, 1, kUiInk);
        }
        below = top + static_cast<int>(front.height) + gap;
    } else {
        // No pictures: say where they would come from, and keep the trigger gauges below.
        draw_text(rgba, pitch, width, height, left, top, "NO PICTURES", TextStyle{kUiGrey, 0, s});
        draw_text(rgba, pitch, width, height, left, top + static_cast<int>(text_height(s)) + si,
                  fit_text("SEE --UI-ASSETS", colw, s), TextStyle{kUiGrey, 0, s});
        below = top + (static_cast<int>(text_height(s)) + si) * 3;
    }

    // The underside, for the triggers, with a gauge for each beside it: a trigger is a travel,
    // and the gauge is what shows a pad's half-press reaching the game.
    const int label_h = static_cast<int>(text_height(s)) + si;
    const int gauge_w = si * 3;
    const int room_h = bottom - below - label_h;
    if (room_h < si * 8)
        return;
    const Image& back =
        scaled(art.controller_back, static_cast<unsigned>(std::max(1, colw * 55 / 100)),
               static_cast<unsigned>(room_h));
    const int bw = back.empty() ? colw / 3 : static_cast<int>(back.width);
    const int bh = back.empty() ? room_h : static_cast<int>(back.height);
    const int bx = left + (colw - bw) / 2;
    if (!back.empty()) {
        blit(rgba, pitch, width, height, bx, below, back);
        mark(back, bx, below, art.back[static_cast<unsigned>(PadControl::LeftTrigger)],
             PadControl::LeftTrigger);
        mark(back, bx, below, art.back[static_cast<unsigned>(PadControl::RightTrigger)],
             PadControl::RightTrigger);
    }
    auto gauge = [&](PadControl c, const char* label) {
        // On the side of the picture where the trigger itself is: the underside is mirrored.
        const Hotspot& spot = art.back[static_cast<unsigned>(c)];
        const bool on_right = spot.set ? spot.x > 0.5f : c == PadControl::RightTrigger;
        const int x = on_right ? bx + bw + gap : bx - gap - gauge_w;
        const int gh = bh * 3 / 4;
        const int y = below + (bh - gh) / 2;
        const float v = std::clamp(pad.value[static_cast<unsigned>(c)], 0.0f, 1.0f);
        const int fill = static_cast<int>(v * static_cast<float>(gh));
        const bool sel = has_selected && selected == c;
        fill_rect(rgba, pitch, width, height, x - 1, y - 1, gauge_w + 2, gh + 2,
                  sel ? kUiOrange : kUiGrey);
        fill_rect(rgba, pitch, width, height, x, y, gauge_w, gh, kUiWhite);
        fill_rect(rgba, pitch, width, height, x, y + gh - fill, gauge_w, fill, kUiOrange);
        const int lx = x + gauge_w / 2 - static_cast<int>(text_width(label, s)) / 2;
        draw_text(rgba, pitch, width, height, lx, y + gh + si * 2, label,
                  TextStyle{sel ? kUiOrange : kUiInk, 0, s});
    };
    gauge(PadControl::LeftTrigger, "L");
    gauge(PadControl::RightTrigger, "R");
}

void SettingsMenu::draw_memory(u32* rgba, unsigned pitch, unsigned width, unsigned height, int left,
                               int top, int right, int bottom, unsigned s,
                               const SettingsArt& art) const {
    const int si = static_cast<int>(s);
    const int gap = si * 6;
    const int line = static_cast<int>(text_height(s)) + si;
    const int colw = (right - left - gap) * 44 / 100;
    const SlotView& view = slots_[focus_slot_];
    const bool card_here = !view.rumble && !view.file.empty();

    // The card, with its screen as the title last drew it.
    auto draw_lcd = [&](int x0, int y0, int w, int h, bool glass) {
        const int px =
            std::max(1, std::min(w / static_cast<int>(kLcdW), h / static_cast<int>(kLcdH)));
        const int ox = x0 + (w - px * static_cast<int>(kLcdW)) / 2;
        const int oy = y0 + (h - px * static_cast<int>(kLcdH)) / 2;
        if (glass)
            fill_rect(rgba, pitch, width, height, ox - px * 2, oy - px * 2,
                      px * static_cast<int>(kLcdW + 4), px * static_cast<int>(kLcdH + 4),
                      kLcdGlass);
        if (!card_here || !view.lcd_drawn)
            return;
        for (unsigned y = 0; y < kLcdH; ++y)
            for (unsigned x = 0; x < kLcdW; ++x) {
                const unsigned sx = art.lcd.flip_x ? kLcdW - 1 - x : x;
                const unsigned sy = art.lcd.flip_y ? kLcdH - 1 - y : y;
                if (lcd_pixel(view.lcd, sx, sy))
                    fill_rect(rgba, pitch, width, height, ox + static_cast<int>(x) * px,
                              oy + static_cast<int>(y) * px, px, px, kLcdInk);
            }
    };
    const Image& pic = scaled(art.vmu, static_cast<unsigned>(std::max(1, colw)),
                              static_cast<unsigned>(std::max(1, bottom - top)));
    if (!pic.empty()) {
        const int px = left + (colw - static_cast<int>(pic.width)) / 2;
        const int py = top + (bottom - top - static_cast<int>(pic.height)) / 2;
        blit(rgba, pitch, width, height, px, py, pic);
        if (art.lcd.set) {
            const float pw = static_cast<float>(pic.width), ph = static_cast<float>(pic.height);
            const int x0 = px + static_cast<int>(art.lcd.x0 * pw);
            const int y0 = py + static_cast<int>(art.lcd.y0 * ph);
            draw_lcd(x0, y0, static_cast<int>((art.lcd.x1 - art.lcd.x0) * pw),
                     static_cast<int>((art.lcd.y1 - art.lcd.y0) * ph), false);
        }
        if (!card_here)
            blend_rect(rgba, pitch, width, height, px, py, static_cast<int>(pic.width),
                       static_cast<int>(pic.height), kMist);
    } else {
        // No picture: the screen on its own, as large as the column allows.
        draw_lcd(left, top, colw, std::min(bottom - top, colw * 2 / 3), true);
    }

    // The rows: the two slots, what the focused one holds, and the way back.
    const int rl = left + colw + gap;
    int y = top;
    auto bar = [&](bool sel) {
        if (sel)
            fill_rect(rgba, pitch, width, height, rl - si * 2, y - si, right - rl + si * 4, line,
                      kUiOrange);
    };
    for (unsigned slot = 0; slot < kSlots; ++slot) {
        const bool sel = mem_row_ == static_cast<int>(slot);
        bar(sel);
        const std::string name = slot == 0 ? "SLOT A1" : "SLOT A2";
        draw_text(rgba, pitch, width, height, rl, y, name,
                  TextStyle{sel ? kUiWhite : kUiInk, 0, s});
        const bool pending = pending_[slot] >= 0;
        const int room = right - rl - static_cast<int>(text_width(name + " ", s)) -
                         static_cast<int>(text_width("<  >", s));
        const std::string value = "< " + fit_text(choice_label(shown_choice(slot)), room, s) + " >";
        const int vw = static_cast<int>(text_width(value, s));
        const int vx = right - vw;
        // A choice not yet inserted is drawn cut out of the bar, so it does not read as done.
        if (pending)
            fill_rect(rgba, pitch, width, height, vx - si, y - si, vw + si * 2, line, kUiWhite);
        draw_text(rgba, pitch, width, height, vx, y, value,
                  TextStyle{pending ? kUiOrange : (sel ? kUiWhite : kUiInk), 0, s});
        y += line;
    }
    y += si * 2;
    fill_rect(rgba, pitch, width, height, rl, y, right - rl, si, kUiGrey);
    y += si * 4;

    const int close_y = bottom - line;
    const TextStyle ink{kUiInk, 0, s}, grey{kUiGrey, 0, s};
    if (view.rumble) {
        draw_text(rgba, pitch, width, height, rl, y, "RUMBLE PACK", ink);
        draw_text(rgba, pitch, width, height, rl, y + line,
                  fit_text("THE PAD SHAKES WHEN THE GAME ASKS", right - rl, s), grey);
    } else if (view.file.empty()) {
        draw_text(rgba, pitch, width, height, rl, y, "NO CARD IN THIS SLOT", grey);
    } else if (!view.contents.formatted) {
        draw_text(rgba, pitch, width, height, rl, y, "NOT FORMATTED", ink);
    } else {
        const std::string free = std::to_string(view.contents.free_blocks) + " / " +
                                 std::to_string(view.contents.user_blocks);
        draw_text(rgba, pitch, width, height, rl, y, "FREE BLOCKS", ink);
        draw_text(rgba, pitch, width, height, right - static_cast<int>(text_width(free, s)), y,
                  free, ink);
        y += line + si * 2;
        // One entry per file: its icon, its description, then its name and size in grey.
        const int is = std::max(1, si / 2);
        const int icon = static_cast<int>(maple::kVmuIconSize) * is;
        const int row_h = std::max(icon, line * 2) + si * 2;
        const auto& files = view.contents.files;
        if (files.empty())
            draw_text(rgba, pitch, width, height, rl, y, "NO SAVES ON THIS CARD", grey);
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            if (y + row_h > close_y - si * 2 && i + 1 < files.size()) {
                draw_text(rgba, pitch, width, height, rl, y,
                          "+" + std::to_string(files.size() - i) + " MORE", grey);
                break;
            }
            if (f.icon.size() == maple::kVmuIconSize * maple::kVmuIconSize) {
                Image ic{maple::kVmuIconSize, maple::kVmuIconSize, f.icon};
                const Image big = is == 1 ? ic
                                          : scale_to_fit(ic, static_cast<unsigned>(icon),
                                                         static_cast<unsigned>(icon));
                blit(rgba, pitch, width, height, rl, y, big);
            } else {
                fill_rect(rgba, pitch, width, height, rl, y, icon, icon, kUiGrey);
            }
            const int tx = rl + icon + si * 4;
            const std::string title = f.description.empty() ? f.name : f.description;
            draw_text(rgba, pitch, width, height, tx, y, fit_text(title, right - tx, s), ink);
            const std::string size =
                std::to_string(f.blocks) + (f.blocks == 1 ? " BLOCK" : " BLOCKS");
            draw_text(rgba, pitch, width, height, tx, y + line,
                      fit_text(f.name, right - tx - static_cast<int>(text_width(size + " ", s)), s),
                      grey);
            draw_text(rgba, pitch, width, height, right - static_cast<int>(text_width(size, s)),
                      y + line, size, grey);
            y += row_h;
        }
    }

    y = close_y;
    bar(mem_row_ == kMemClose);
    draw_text(rgba, pitch, width, height, rl, y, "BACK TO GAME",
              TextStyle{mem_row_ == kMemClose ? kUiWhite : kUiInk, 0, s});
}

}  // namespace dream::render
