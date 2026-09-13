// See resources.h.
#include "dream/render/vk/resources.h"

#include <cstring>

#include "dream/render/vk/context.h"

namespace dream::render::vk {

std::uint32_t find_memory_type(VkPhysicalDevice physical, std::uint32_t type_bits,
                               VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

HostBuffer::~HostBuffer() {
    destroy();
}

bool HostBuffer::ensure(const Context& ctx, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    if (bytes == 0)
        return true;
    if (buffer_ && bytes <= capacity_)
        return true;
    destroy();
    device_ = ctx.device();

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &buffer_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer_, &req);
    const std::uint32_t type = find_memory_type(
        ctx.physical_device(), req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        destroy();
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &mai, nullptr, &memory_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    vkBindBufferMemory(device_, buffer_, memory_, 0);
    if (vkMapMemory(device_, memory_, 0, req.size, 0, &mapped_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    capacity_ = bytes;
    return true;
}

void HostBuffer::write(const void* data, std::size_t bytes, VkDeviceSize offset) {
    if (mapped_ && bytes)
        std::memcpy(static_cast<char*>(mapped_) + offset, data, bytes);
}

void HostBuffer::destroy() {
    if (!device_)
        return;
    if (mapped_) {
        vkUnmapMemory(device_, memory_);
        mapped_ = nullptr;
    }
    if (buffer_) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (memory_) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
    capacity_ = 0;
}

AttachmentImage::~AttachmentImage() {
    destroy();
}

bool AttachmentImage::create(const Context& ctx, VkFormat format, VkExtent2D extent,
                             VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    destroy();
    device_ = ctx.device();

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &image_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image_, &req);
    const std::uint32_t type = find_memory_type(ctx.physical_device(), req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        destroy();
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &mai, nullptr, &memory_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    vkBindImageMemory(device_, image_, memory_, 0);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &view_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    return true;
}

void AttachmentImage::destroy() {
    if (!device_)
        return;
    if (view_) {
        vkDestroyImageView(device_, view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (memory_) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
}

VkFormat pick_depth_format(VkPhysicalDevice physical) {
    // 32-bit float depth first: the PowerVR's depth is 1/w over a wide range, and the fragment
    // shader writes a logarithm of it, so precision matters more than memory here.
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                       VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return f;
    }
    return VK_FORMAT_UNDEFINED;
}

}  // namespace dream::render::vk
