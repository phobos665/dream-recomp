// See texture_cache.h.
#include "dream/render/vk/texture_cache.h"

#include <cstring>

#include "dream/render/tsp.h"

namespace dream::render::vk {

namespace {

constexpr std::uint32_t kMaxTextures = 2048;

// Repeat and mirror only describe what happens outside the unit square. A polygon that never goes
// there cannot tell them apart from clamping at the guest's own resolution, and above it clamping
// is the only one that does not fetch the far edge of the texture for the outermost half-texel.
VkSamplerAddressMode address_mode(bool clamp, bool flip, bool tiled) {
    if (clamp || !tiled)
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return flip ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

}  // namespace

TextureCache::~TextureCache() {
    destroy();
}

bool TextureCache::create(Context& ctx, VkDescriptorSetLayout layout) {
    ctx_ = &ctx;
    layout_ = layout;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = kMaxTextures;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kMaxTextures;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(ctx.device(), &dpci, nullptr, &pool_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorPool failed";
        return false;
    }
    return true;
}

void TextureCache::set_memory(const std::uint8_t* vram, std::size_t vram_size,
                              const std::uint32_t* palette_ram, PaletteFormat palette_format) {
    vram_ = vram;
    vram_size_ = vram_size;
    if (palette_ram)
        unpack_palette(palette_ram, palette_format, palette_);
    invalidate();
}

void TextureCache::invalidate() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    for (auto& [key, e] : entries_) free_entry(e);
    entries_.clear();
}

void TextureCache::free_entry(Entry& e) {
    if (e.set)
        vkFreeDescriptorSets(ctx_->device(), pool_, 1, &e.set);
    if (e.view)
        vkDestroyImageView(ctx_->device(), e.view, nullptr);
    if (e.image)
        vkDestroyImage(ctx_->device(), e.image, nullptr);
    if (e.memory)
        vkFreeMemory(ctx_->device(), e.memory, nullptr);
    e.staging.destroy();
}

bool TextureCache::set_palette(const std::uint32_t* palette_ram, PaletteFormat format) {
    if (!palette_ram)
        return false;
    std::uint32_t unpacked[1024]{};
    unpack_palette(palette_ram, format, unpacked);
    if (std::memcmp(unpacked, palette_, sizeof palette_) == 0)
        return false;
    std::memcpy(palette_, unpacked, sizeof palette_);
    invalidate();
    return true;
}

std::size_t TextureCache::invalidate_range(std::uint32_t begin, std::uint32_t end) {
    if (!ctx_ || !ctx_->device() || end <= begin)
        return 0;
    std::vector<std::uint64_t> doomed;
    for (const auto& [key, e] : entries_) {
        const std::uint32_t lo = e.info.address;
        const std::uint32_t bytes = e.info.size_bytes();
        // An entry the decoder rejected has no pixels and no size; it is kept so the failure is
        // not retried every frame, and a write cannot make it stale.
        if (bytes == 0)
            continue;
        if (lo < end && begin < lo + bytes)
            doomed.push_back(key);
    }
    if (doomed.empty())
        return 0;
    vkDeviceWaitIdle(ctx_->device());
    for (std::uint64_t key : doomed) {
        auto it = entries_.find(key);
        if (it == entries_.end())
            continue;
        free_entry(it->second);
        entries_.erase(it);
    }
    overwritten += static_cast<std::uint32_t>(doomed.size());
    return doomed.size();
}

VkSampler TextureCache::sampler_for(std::uint32_t tsp, bool tiled) {
    const std::uint32_t key = tsp_sampler_key(tsp) | (tiled ? 0x40u : 0u);
    if (auto it = samplers_.find(key); it != samplers_.end())
        return it->second;

    // Filter mode 0 is point sampling; anything else is bilinear (the hardware's trilinear modes
    // need mipmaps, which are decoded but not yet uploaded as a chain).
    const bool bilinear = tsp_filter_mode(tsp) != 0;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = address_mode(tsp_clamp_u(tsp) != 0, tsp_flip_u(tsp) != 0, tiled);
    sci.addressModeV = address_mode(tsp_clamp_v(tsp) != 0, tsp_flip_v(tsp) != 0, tiled);
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(ctx_->device(), &sci, nullptr, &sampler);
    samplers_.emplace(key, sampler);
    return sampler;
}

bool TextureCache::upload(VkCommandBuffer cmd, Entry& e, const std::vector<std::uint32_t>& pixels) {
    const VkDeviceSize bytes = pixels.size() * 4;
    if (!e.staging.ensure(*ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    e.staging.write(pixels.data(), static_cast<std::size_t>(bytes));

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {e.info.width, e.info.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->device(), &ici, nullptr, &e.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx_->device(), e.image, &req);
    const std::uint32_t type = find_memory_type(ctx_->physical_device(), req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx_->device(), &mai, nullptr, &e.memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(ctx_->device(), e.image, e.memory, 0);

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = e.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {e.info.width, e.info.height, 1};
    vkCmdCopyBufferToImage(cmd, e.staging.handle(), e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = e.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx_->device(), &vci, nullptr, &e.view) != VK_SUCCESS)
        return false;
    return true;
}

VkDescriptorSet TextureCache::get(VkCommandBuffer cmd, std::uint32_t tcw, std::uint32_t tsp,
                                  bool tiled) {
    if (!vram_ || !ctx_)
        return VK_NULL_HANDLE;
    // The size lives in the TSP word, the rest in the TCW; together they identify the texture, and
    // the addressing the polygons need decides the sampler bound with it.
    const std::uint64_t key =
        (static_cast<std::uint64_t>(tcw) << 32) | (tsp & 0x3Fu) | (tiled ? 0x40u : 0u);
    if (auto it = entries_.find(key); it != entries_.end()) {
        ++hits;
        return it->second.set;
    }

    Entry e;
    if (!describe_texture(tcw, tsp, 0, e.info)) {
        ++failed;
        entries_.emplace(key, std::move(e));  // remember the failure so it is not retried per frame
        return VK_NULL_HANDLE;
    }
    std::vector<std::uint32_t> pixels;
    if (!decode_texture(e.info, vram_, vram_size_, palette_, pixels)) {
        ++failed;
        entries_.emplace(key, std::move(e));
        return VK_NULL_HANDLE;
    }
    if (!upload(cmd, e, pixels)) {
        ++failed;
        entries_.emplace(key, std::move(e));
        return VK_NULL_HANDLE;
    }
    e.sampler = sampler_for(tsp, tiled);

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(ctx_->device(), &dsai, &e.set) != VK_SUCCESS) {
        ++failed;
        error_ = "the descriptor pool is exhausted";
        entries_.emplace(key, std::move(e));
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo image{};
    image.sampler = e.sampler;
    image.imageView = e.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = e.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);

    ++decoded;
    VkDescriptorSet set = e.set;
    entries_.emplace(key, std::move(e));
    return set;
}

VkDescriptorSet TextureCache::fallback(VkCommandBuffer cmd) {
    if (fallback_.set)
        return fallback_.set;
    fallback_.info = TextureInfo{};
    fallback_.info.width = 1;
    fallback_.info.height = 1;
    const std::vector<std::uint32_t> white{0xFFFFFFFFu};
    if (!upload(cmd, fallback_, white))
        return VK_NULL_HANDLE;
    fallback_.sampler = sampler_for(0, false);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(ctx_->device(), &dsai, &fallback_.set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorImageInfo image{};
    image.sampler = fallback_.sampler;
    image.imageView = fallback_.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = fallback_.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
    return fallback_.set;
}

void TextureCache::destroy() {
    if (!ctx_ || !ctx_->device())
        return;
    invalidate();
    if (fallback_.view)
        vkDestroyImageView(ctx_->device(), fallback_.view, nullptr);
    if (fallback_.image)
        vkDestroyImage(ctx_->device(), fallback_.image, nullptr);
    if (fallback_.memory)
        vkFreeMemory(ctx_->device(), fallback_.memory, nullptr);
    fallback_.staging.destroy();
    fallback_.set = VK_NULL_HANDLE;
    fallback_.sampler = VK_NULL_HANDLE;
    for (auto& [key, sampler] : samplers_) vkDestroySampler(ctx_->device(), sampler, nullptr);
    samplers_.clear();
    if (pool_) {
        vkDestroyDescriptorPool(ctx_->device(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

}  // namespace dream::render::vk
