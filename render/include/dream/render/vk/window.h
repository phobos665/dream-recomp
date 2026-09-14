// SDL3 window and Vulkan swapchain (WP2.3). Presentation only: what to draw comes from the
// renderer, and the probe draws a triangle to prove the stack works on this machine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dream/render/input.h"
#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

struct SDL_Window;
struct SDL_Gamepad;

namespace dream::render::vk {

// The controls a Dreamcast pad offers, as the window sees them. SDL's key codes stop here: the
// launcher maps these onto the Maple controller, so the keyboard layout lives in one place and the
// runtime never learns what a keyboard is.
//
//   arrow keys   d-pad          Z X A S   the A, B, X and Y buttons
//   return       start          Q W       the left and right analogue triggers
//   F12          screenshot     F11       capture everything about this frame
//   escape       quit
enum class Control : unsigned {
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    X,
    Y,
    Start,
    LeftTrigger,
    RightTrigger,
    Screenshot,
    Capture,
    ToggleFps,
    Count
};

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Opens the window and creates the context, surface and swapchain. False on failure with
    // error() set; a machine with no display is an ordinary failure, not an exception.
    bool create(const char* title, int width, int height, bool want_validation);
    void destroy();

    // Acquires the next image. Returns false when the swapchain needs rebuilding (a resize), in
    // which case the caller should call recreate_swapchain() and try again.
    bool begin_frame(std::uint32_t& image_index, VkCommandBuffer& cmd);
    bool end_frame(std::uint32_t image_index);
    bool recreate_swapchain();

    // True while the user has not closed the window; pumps the event queue and refreshes the
    // control state below.
    bool poll();

    // Held down as of the last poll().
    bool held(Control c) const noexcept;
    // Went down since the previous poll(): for controls that act once rather than hold.
    bool pressed(Control c) const noexcept;

    // --- the guest pad, through the player's bindings ------------------------------------------
    //
    // Control above is the launcher's own fixed keys (screenshot, capture, the menu). PadControl is
    // what the player binds, and the two are separate so that rebinding the game's buttons can
    // never take the menu key away and leave no way back.

    // Resolves the portable names in `b` to SDL codes once, here, rather than per frame.
    void set_bindings(const Bindings& b);
    const Bindings& bindings() const noexcept { return bindings_; }

    bool pad_held(PadControl c) const noexcept;
    bool pad_pressed(PadControl c) const noexcept;
    // 0 to 1. A digital source reads 1 while held, except that a trigger with the ramp enabled
    // rises over about 150 ms, because a keyboard accelerator that is only ever fully down or
    // fully up is most of why a driving game needs a pad.
    float pad_value(PadControl c) const noexcept;

    // "KEYBOARD" first, then one row per connected pad. Rebuilt on hot-plug.
    const std::vector<std::string>& devices() const noexcept { return devices_; }
    bool gamepad_connected() const noexcept { return !pads_.empty(); }

    // Rebinding: the next physical input becomes a Binding rather than reaching the game. Escape
    // cancels. While capturing, pad_held() reports nothing, so the key being bound cannot also be
    // played.
    void begin_capture() noexcept;
    void cancel_capture() noexcept;
    bool capturing() const noexcept { return capturing_; }
    // True once, on the poll that captured something. `cancelled` distinguishes escape from a
    // binding, because those mean different things to the caller.
    bool take_capture(Binding& out, bool& cancelled) noexcept;

    // Copies the last presented image into `out` as 8-bit RGB rows, top to bottom. For
    // screenshots and for tests that want to look at what was drawn.
    bool read_pixels(std::vector<std::uint8_t>& out, std::uint32_t& width, std::uint32_t& height);

    Context& context() noexcept { return ctx_; }
    VkRenderPass render_pass() const noexcept { return render_pass_; }
    VkFramebuffer framebuffer(std::uint32_t i) const noexcept { return framebuffers_[i]; }
    VkExtent2D extent() const noexcept { return extent_; }
    const std::string& error() const noexcept { return error_; }

private:
    bool create_swapchain();
    void destroy_swapchain();

    SDL_Window* window_ = nullptr;
    Context ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    AttachmentImage depth_;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds_;
    std::vector<VkFence> fences_;
    std::vector<VkSemaphore> acquired_, rendered_;
    std::uint32_t frame_ = 0;
    std::uint32_t last_presented_ = 0;
    bool held_[static_cast<unsigned>(Control::Count)]{};
    bool pressed_[static_cast<unsigned>(Control::Count)]{};
    bool running_ = true;
    std::string error_;

    // One binding resolved to the codes SDL actually compares against.
    struct Resolved {
        BindSource source = BindSource::None;
        int code = 0;
        int sign = 1;
    };
    Bindings bindings_;
    Resolved key_[kPadControlCount]{};
    Resolved gpad_[kPadControlCount]{};
    bool pad_held_[kPadControlCount]{};
    bool pad_pressed_[kPadControlCount]{};
    float pad_value_[kPadControlCount]{};
    // How long each trigger has been held, for the ramp. In poll ticks rather than seconds, scaled
    // by the measured frame interval, so a fast host does not ramp faster than a slow one.
    float ramp_[kPadControlCount]{};
    std::uint64_t last_poll_ns_ = 0;

    std::vector<SDL_Gamepad*> pads_;
    std::vector<std::string> devices_;
    void refresh_devices();

    bool capturing_ = false, captured_ = false, capture_cancelled_ = false;
    Binding capture_{};
};

}  // namespace dream::render::vk
