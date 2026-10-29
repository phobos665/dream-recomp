// The settings screen: the controller and the memory cards, one tab each.
//
// The controller tab is the binding editor (InputMenu) beside a picture of the pad, where the
// control being bound is ringed and anything pressed lights up, so the player can see a layout
// work before going back to the game. The memory card tab shows what sits in the pad's two
// expansion slots, lets the player swap cards while the game is stopped, lists what is on the
// card, and shows the card's little screen as the title last drew it.
//
// Device-independent like InputMenu: the launcher feeds it presses, pad state and slot contents,
// and does whatever an Action asks for. Pictures are optional. They are the player's own files,
// passed in at run time and never part of this repository; without them every row still works
// and the picture column says where they would go.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "dream/render/image.h"
#include "dream/render/input.h"
#include "dream/render/input_menu.h"
#include "dream/runtime/maple/vmu_fs.h"

namespace dream::render {

// A control's place on a picture: x and y as fractions of its width and height, r of its width.
struct Hotspot {
    float x = 0, y = 0, r = 0;
    bool set = false;
};
using Hotspots = std::array<Hotspot, kPadControlCount>;

// "name x y r" per line, '#' starts a comment; names are the bindings file's keys ("a",
// "dpad_up", "ltrigger"...). Lines that do not parse are skipped and described in `warnings`.
Hotspots parse_hotspots(std::string_view text, std::string* warnings = nullptr);

// Where the memory card's screen sits on its picture, and which way round to draw it.
struct LcdPlacement {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // fractions of the picture
    bool flip_x = false, flip_y = false;
    bool set = false;
};
// "lcd x0 y0 x1 y1", and optionally "flip_x" / "flip_y" lines.
LcdPlacement parse_lcd_placement(std::string_view text);

struct SettingsArt {
    Image controller, controller_back, vmu;
    Hotspots front{}, back{};
    LcdPlacement lcd;
};

// What the pad is doing now, through the player's bindings, for the live test.
struct PadPreview {
    std::array<bool, kPadControlCount> held{};
    std::array<float, kPadControlCount> value{};
};

// One expansion slot, as the launcher reports it.
struct SlotView {
    std::string file;  // the card's file name, no directory; empty for an empty slot
    bool rumble = false;
    maple::VmuContents contents;
    std::array<std::uint8_t, 192> lcd{};
    bool lcd_drawn = false;  // the title has drawn on this card's screen at least once
};

// Which pixel of the card's 48x32 screen is dark, from the bytes a title sends.
bool lcd_pixel(const std::array<std::uint8_t, 192>& lcd, unsigned x, unsigned y);

class SettingsMenu {
public:
    enum class Tab : unsigned { Controller, Memory, Count };
    enum class Action { None, Close, Swap };

    // What can go in a slot.
    struct Choice {
        enum class Kind { Empty, Card, NewCard, Rumble };
        Kind kind = Kind::Empty;
        std::string file;  // for Card
        bool operator==(const Choice& o) const { return kind == o.kind && file == o.file; }
    };

    void open(const Bindings& b, Tab tab = Tab::Controller);
    void close() noexcept { open_ = false; }
    bool is_open() const noexcept { return open_; }
    Tab tab() const noexcept { return tab_; }
    // Q/W or the shoulder buttons. Ignored while a capture waits, which would otherwise bind the
    // key to a row the player can no longer see.
    void switch_tab(int delta);

    // The controller tab's editor.
    const Bindings& bindings() const noexcept { return input_.bindings(); }
    bool dirty() const noexcept { return input_.dirty(); }
    void mark_saved() noexcept { input_.mark_saved(); }
    bool awaiting() const noexcept { return input_.awaiting(); }
    // Whose layout the controller tab shows: 0 is the keyboard, anything else the pads.
    unsigned editing_device() const noexcept { return input_.device(); }
    void apply_capture(const Binding& b, bool cancelled) { input_.apply_capture(b, cancelled); }

    void move(int delta);
    void adjust(int delta, unsigned device_count);
    // Close from either tab's last row; Swap when a slot row is confirmed on a different choice.
    // On the controller tab a binding row starts a capture instead (see awaiting()), and the
    // device row steps to the next of `device_count` devices.
    Action activate(unsigned device_count = 1);
    // Leaves a capture, then a pending slot choice, then the screen.
    void back();

    // The memory card tab. Slot 0 is the pad's first expansion slot, where a card normally sits;
    // slot 1 is the second, where a rumble pack normally sits.
    static constexpr unsigned kSlots = 2;
    void set_card_files(std::vector<std::string> files) { files_ = std::move(files); }
    void set_rumble_available(bool yes) noexcept { rumble_available_ = yes; }
    void set_slot(unsigned slot, SlotView view);
    const SlotView& slot(unsigned i) const { return slots_[i]; }
    // The swap activate() asked for.
    unsigned swap_slot() const noexcept { return swap_slot_; }
    const Choice& swap_choice() const noexcept { return swap_choice_; }
    // What the slot row shows: the pending choice while one is being picked, the slot otherwise.
    Choice shown_choice(unsigned slot) const;

    void draw(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height,
              const std::vector<std::string>& devices, const PadPreview& pad,
              const SettingsArt& art) const;

private:
    enum MemRow : int { kSlotA1 = 0, kSlotA2, kMemClose, kMemRows };
    std::vector<Choice> choices(unsigned slot) const;
    Choice current(unsigned slot) const;
    void draw_controller(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height,
                         int left, int top, int right, int bottom, unsigned s,
                         const std::vector<std::string>& devices, const PadPreview& pad,
                         const SettingsArt& art) const;
    void draw_memory(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int left,
                     int top, int right, int bottom, unsigned s, const SettingsArt& art) const;
    // A picture scaled for the current frame size, kept until the size changes.
    const Image& scaled(const Image& src, unsigned max_w, unsigned max_h) const;

    InputMenu input_;
    bool open_ = false;
    Tab tab_ = Tab::Controller;
    int mem_row_ = kSlotA1;
    unsigned focus_slot_ = 0;  // the slot whose card the tab describes
    std::array<SlotView, kSlots> slots_{};
    std::array<int, kSlots> pending_{-1, -1};  // index into choices(); -1 while showing the slot
    std::vector<std::string> files_;
    bool rumble_available_ = false;
    unsigned swap_slot_ = 0;
    Choice swap_choice_{};

    struct Scaled {
        const Image* src = nullptr;
        unsigned max_w = 0, max_h = 0;
        Image image;
    };
    // A deque so that scaling one picture never moves another scaled earlier in the same frame.
    mutable std::deque<Scaled> cache_;
    mutable unsigned cache_width_ = 0, cache_height_ = 0;
};

// "rush2049-vmu.bin" -> "RUSH2049-VMU": what a slot row shows for a card file.
std::string card_label(std::string_view file);

}  // namespace dream::render
