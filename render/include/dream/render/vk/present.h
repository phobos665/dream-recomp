// Putting the guest's framebuffer on screen (WP2.3 step 6).
//
// The window shows what the video hardware would be scanning out, which is the buffer FB_R_SOF1
// points at, decoded from video memory. Deliberately not "whatever the renderer drew last": a
// title that writes pixels into the framebuffer directly, for a video sequence or a 2D overlay, is
// then shown correctly without the renderer knowing anything about it, and double buffering works
// because the guest nominates the buffer rather than the renderer guessing.
//
// One texture, one fullscreen triangle. The image keeps the guest's aspect ratio inside whatever
// the window happens to be, so a resized window letterboxes instead of stretching.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Presenter {
public:
    Presenter() = default;
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // `render_pass` is the window's, since that is where the triangle is drawn.
    bool create(Context& ctx, VkRenderPass render_pass);
    void destroy();

    // Uploads one decoded framebuffer. `rgba` is width * height pixels, row 0 at the top. Call
    // outside the render pass: it records a copy. A changed size reallocates the image.
    //
    // `decorate`, when given, is called with the frame already copied into the staging buffer and
    // before it is handed to the GPU, so an overlay can be composited in place. Doing it here
    // rather than in the caller saves copying the whole frame a second time to change a corner of
    // it, and leaves the caller's own buffer untouched: what it hands in is still the guest's
    // picture, which is what a screenshot wants.
    bool upload(VkCommandBuffer cmd, const std::uint32_t* rgba, std::uint32_t width,
                std::uint32_t height,
                const std::function<void(std::uint32_t* pixels, std::uint32_t width,
                                         std::uint32_t height)>& decorate = {});
    // Draws the uploaded image, fitted to `target`. Call inside the window's render pass. False
    // when nothing has been uploaded yet, which leaves the cleared background showing.
    bool draw(VkCommandBuffer cmd, VkExtent2D target);

    // Nearest-neighbour rather than linear filtering: the guest's pixels shown as pixels. An
    // upscaling mode will want the choice, so it is a field rather than a constant.
    bool smooth = false;

    const std::string& error() const noexcept { return error_; }

private:
    bool ensure_image(std::uint32_t width, std::uint32_t height);
    void destroy_image();

    Context* ctx_ = nullptr;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    HostBuffer staging_;
    std::uint32_t width_ = 0, height_ = 0;
    bool uploaded_ = false;
    std::string error_;
};

}  // namespace dream::render::vk
