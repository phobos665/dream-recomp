// Small Vulkan resource helpers (WP2.3). Deliberately not a memory allocator: the renderer's
// buffers are a few hundred kilobytes per frame and host-visible memory is fast enough for them on
// every platform we target. If that stops being true, this is the seam to replace.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Context;

// Finds a memory type with the requested properties; returns UINT32_MAX when there is none.
std::uint32_t find_memory_type(VkPhysicalDevice physical, std::uint32_t type_bits,
                               VkMemoryPropertyFlags want);

// A host-visible buffer that is written every frame and read by the GPU.
class HostBuffer {
public:
    HostBuffer() = default;
    ~HostBuffer();
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&& o) noexcept { *this = std::move(o); }
    HostBuffer& operator=(HostBuffer&& o) noexcept {
        if (this != &o) {
            destroy();
            device_ = o.device_;
            buffer_ = o.buffer_;
            memory_ = o.memory_;
            mapped_ = o.mapped_;
            capacity_ = o.capacity_;
            o.device_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.memory_ = VK_NULL_HANDLE;
            o.mapped_ = nullptr;
            o.capacity_ = 0;
        }
        return *this;
    }

    // Grows the buffer when `bytes` exceeds what is already allocated; keeps it otherwise, so a
    // steady frame size allocates once.
    bool ensure(const Context& ctx, VkDeviceSize bytes, VkBufferUsageFlags usage);
    void write(const void* data, std::size_t bytes, VkDeviceSize offset = 0);
    void destroy();

    VkBuffer handle() const noexcept { return buffer_; }
    const void* mapped() const noexcept { return mapped_; }
    // Writable, for a caller that composites into what it has already written rather than
    // assembling a second copy of the frame just to change a corner of it.
    void* mapped() noexcept { return mapped_; }
    VkDeviceSize capacity() const noexcept { return capacity_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize capacity_ = 0;
};

// A device-local image used as an attachment (the depth buffer).
class AttachmentImage {
public:
    AttachmentImage() = default;
    ~AttachmentImage();
    AttachmentImage(const AttachmentImage&) = delete;
    AttachmentImage& operator=(const AttachmentImage&) = delete;

    bool create(const Context& ctx, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                VkImageAspectFlags aspect);
    void destroy();

    VkImage image() const noexcept { return image_; }
    VkImageView view() const noexcept { return view_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
};

// The first of these formats the device supports as a depth attachment.
VkFormat pick_depth_format(VkPhysicalDevice physical);

}  // namespace dream::render::vk
