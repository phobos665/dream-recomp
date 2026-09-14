// The binding screen: what it shows, what the keys do, and how it draws.
//
// Device-independent, so the navigation and the rebinding state machine are unit-tested without a
// window. The caller feeds it presses and captured inputs; it owns the Bindings and says when they
// need saving.
//
// The look is a white card with an orange banner and black type, which is of the period without
// reproducing anyone's mark. Hand-drawn rather than built on a general widget toolkit because the
// screen is a fixed list of nineteen rows, and a toolkit would mean a vendored dependency and an
// ADR for something that fits in a few hundred lines.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dream/render/input.h"

namespace dream::render {

// An orange, a white and a near-black that reads as ink rather than as a hole in the card. A
// palette and a layout are not anybody's property; a logo is, so this screen carries none.
constexpr std::uint32_t kUiOrange = 0xFF2D8BEEu;  // RGBA, red in the low byte
constexpr std::uint32_t kUiWhite = 0xFFFFFFFFu;
constexpr std::uint32_t kUiInk = 0xFF241E1Cu;
constexpr std::uint32_t kUiGrey = 0xFF8A8A8Au;
constexpr std::uint32_t kUiShade = 0xB4000000u;  // dims the game behind the card

class InputMenu {
public:
    enum class Action { None, Close, Save };

    void open(const Bindings& b);
    void close() { open_ = false; }
    bool is_open() const noexcept { return open_; }

    // Which device's bindings are being edited. Both are always live in play; this is the list the
    // screen shows.
    unsigned device() const noexcept { return device_; }

    const Bindings& bindings() const noexcept { return bindings_; }
    bool dirty() const noexcept { return dirty_; }
    void mark_saved() noexcept { dirty_ = false; }

    // True while the screen is waiting for a physical input to bind.
    bool awaiting() const noexcept { return awaiting_; }

    // Navigation. `devices` is how many device rows exist, so the device selector can wrap.
    void move(int delta);
    void adjust(int delta, unsigned device_count);  // left/right on the current row
    Action activate();                              // return/A on the current row
    void back();                                    // escape/B

    // The result of a capture the caller ran on our behalf.
    void apply_capture(const Binding& b, bool cancelled);

    void draw(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height,
              const std::vector<std::string>& devices) const;

private:
    // Rows above the bindings, then one row per control, then the rows below.
    enum Row : int { kDeviceRow = 0, kDeadzoneRow, kRampRow, kFirstBinding };
    int row_count() const { return kFirstBinding + static_cast<int>(kPadControlCount) + 2; }
    int reset_row() const { return kFirstBinding + static_cast<int>(kPadControlCount); }
    int close_row() const { return reset_row() + 1; }
    bool on_binding(int row, PadControl& out) const;

    Bindings bindings_{};
    DeviceBindings& editing();
    const DeviceBindings& editing() const;

    bool open_ = false;
    bool awaiting_ = false;
    bool dirty_ = false;
    unsigned device_ = 0;  // 0 is the keyboard; anything else is the gamepad set
    int row_ = kFirstBinding;
};

}  // namespace dream::render
