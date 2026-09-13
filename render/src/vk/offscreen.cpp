// See offscreen.h.
#include "dream/render/vk/offscreen.h"

namespace dream::render::vk {

Offscreen::~Offscreen() {
    destroy();
}

bool Offscreen::create(Context& ctx, std::uint32_t width, std::uint32_t height) {
    destroy();
    ctx_ = &ctx;
    extent_ = {width, height};
    if (width == 0 || height == 0) {
        error_ = "an offscreen target needs a non-empty size";
        return false;
    }

    // TRANSFER_SRC as well as COLOR_ATTACHMENT: the frame is copied back to the host after it is
    // drawn, and on some drivers an image without that usage cannot be a copy source at all.
    if (!colour_.create(ctx, VK_FORMAT_R8G8B8A8_UNORM, extent_,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        error_ = "could not create the offscreen colour attachment";
        return false;
    }
    const VkFormat depth_format = pick_depth_format(ctx.physical_device());
    if (!depth_.create(ctx, depth_format, extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT)) {
        error_ = "could not create the offscreen depth attachment";
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    // The copy that follows the render pass reads the colour attachment, so the transfer stage has
    // to wait for the writes the subpass made.
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &after;
    if (vkCreateRenderPass(ctx.device(), &rpci, nullptr, &render_pass_) != VK_SUCCESS) {
        error_ = "vkCreateRenderPass failed for the offscreen target";
        return false;
    }

    const VkImageView views[2] = {colour_.view(), depth_.view()};
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = render_pass_;
    fbci.attachmentCount = 2;
    fbci.pAttachments = views;
    fbci.width = width;
    fbci.height = height;
    fbci.layers = 1;
    if (vkCreateFramebuffer(ctx.device(), &fbci, nullptr, &framebuffer_) != VK_SUCCESS) {
        error_ = "vkCreateFramebuffer failed for the offscreen target";
        return false;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = ctx.queue_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(ctx.device(), &cpci, nullptr, &pool_) != VK_SUCCESS) {
        error_ = "vkCreateCommandPool failed for the offscreen target";
        return false;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx.device(), &cbai, &cmd_) != VK_SUCCESS) {
        error_ = "vkAllocateCommandBuffers failed for the offscreen target";
        return false;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(ctx.device(), &fci, nullptr, &fence_) != VK_SUCCESS) {
        error_ = "vkCreateFence failed for the offscreen target";
        return false;
    }
    if (!readback_.ensure(ctx, static_cast<VkDeviceSize>(width) * height * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        error_ = "could not allocate the readback buffer";
        return false;
    }
    return true;
}

void Offscreen::destroy() {
    if (ctx_ && ctx_->device()) {
        VkDevice dev = ctx_->device();
        if (fence_)
            vkDestroyFence(dev, fence_, nullptr);
        if (pool_)
            vkDestroyCommandPool(dev, pool_, nullptr);  // frees cmd_ with it
        if (framebuffer_)
            vkDestroyFramebuffer(dev, framebuffer_, nullptr);
        if (render_pass_)
            vkDestroyRenderPass(dev, render_pass_, nullptr);
    }
    fence_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    framebuffer_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    readback_.destroy();
    colour_.destroy();
    depth_.destroy();
    have_pixels_ = false;
    ctx_ = nullptr;
}

bool Offscreen::render(Renderer& renderer, const Frame& frame, const FrameGeometry& geometry) {
    if (!ctx_ || !cmd_)
        return false;
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) {
        error_ = "vkBeginCommandBuffer failed";
        return false;
    }

    renderer.prepare(cmd_, frame);  // texture uploads happen outside the render pass

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {0.0f, 0};  // the hardware's depth grows towards the viewer
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = render_pass_;
    rpbi.framebuffer = framebuffer_;
    rpbi.renderArea.extent = extent_;
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
        0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    renderer.draw(cmd_, frame, geometry, extent_);
    vkCmdEndRenderPass(cmd_);

    // The render pass left the colour image as a transfer source, so the copy needs no barrier of
    // its own; the subpass dependency above orders it.
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd_, colour_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_.handle(), 1, &copy);
    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
        error_ = "vkEndCommandBuffer failed";
        return false;
    }

    vkResetFences(ctx_->device(), 1, &fence_);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    if (vkQueueSubmit(ctx_->queue(), 1, &si, fence_) != VK_SUCCESS) {
        error_ = "vkQueueSubmit failed";
        return false;
    }
    // One second is far longer than any frame takes; waiting for ever would turn a lost device
    // into a hung launcher with nothing on screen to say so.
    if (vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) {
        error_ = "the GPU did not finish the frame within a second";
        return false;
    }
    have_pixels_ = true;
    ++frames;
    return true;
}

const std::uint32_t* Offscreen::pixels() const noexcept {
    if (!have_pixels_)
        return nullptr;
    return static_cast<const std::uint32_t*>(readback_.mapped());
}

}  // namespace dream::render::vk
