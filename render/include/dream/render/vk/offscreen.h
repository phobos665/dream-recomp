// Rendering a frame at the guest's own resolution (WP2.3 step 6).
//
// The renderer draws into an image of its own rather than straight into the window. Two reasons,
// and both matter more than the extra copy costs:
//
//   * The guest expects the frame to end up in video memory, at the address FB_W_SOF1 names. A
//     title that renders geometry and then writes pixels over it, or samples what it just drew as
//     a texture, only works when the rendered frame is really there. That needs the pixels back on
//     the host, which needs somewhere to read them from.
//   * Render size and window size stop being the same number. Drawing at two or four times the
//     guest's resolution is then a change to one argument rather than a change to the design.
//
// The readback is synchronous: record, submit, wait. A frame is a megabyte or so and the guest is
// blocked on the render anyway, which is exactly what the hardware makes it do.
#pragma once

#include <cstdint>
#include <string>

#include "dream/render/vk/context.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Offscreen {
public:
    Offscreen() = default;
    ~Offscreen();
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;

    // Colour and depth attachments at `width` by `height`, a render pass that leaves the colour
    // image ready to copy, and the command buffer and fence used to submit one frame.
    bool create(Context& ctx, std::uint32_t width, std::uint32_t height);
    void destroy();

    // The render pass the renderer's pipelines are built against. Valid after create().
    VkRenderPass render_pass() const noexcept { return render_pass_; }
    VkExtent2D extent() const noexcept { return extent_; }
    std::uint32_t width() const noexcept { return extent_.width; }
    std::uint32_t height() const noexcept { return extent_.height; }

    // Draws one decoded frame and waits for it to finish. Afterwards pixels() holds the result.
    bool render(Renderer& renderer, const Frame& frame, const FrameGeometry& geometry);

    // The last rendered frame as RGBA8888, width() * height() pixels, row 0 at the top. Null
    // before the first successful render().
    const std::uint32_t* pixels() const noexcept;

    const std::string& error() const noexcept { return error_; }
    std::uint64_t frames = 0;

private:
    Context* ctx_ = nullptr;
    VkExtent2D extent_{};
    AttachmentImage colour_, depth_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    HostBuffer readback_;
    bool have_pixels_ = false;
    std::string error_;
};

}  // namespace dream::render::vk
