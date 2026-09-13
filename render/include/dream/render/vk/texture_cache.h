// Texture upload and caching (WP2.3 step 4). A guest texture is identified by the two hardware
// words that describe it plus the palette window it uses; the cache decodes it once, uploads it,
// and hands back a descriptor set the renderer can bind.
//
// Invalidation is deliberately coarse for now: the whole cache is dropped when the guest changes
// palette memory or when the caller says video memory has moved on. Tracking which texels a write
// touched is a later optimisation, and doing it wrong shows as textures that fail to update, which
// is a far worse failure than decoding one again.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "dream/render/texture.h"
#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class TextureCache {
public:
    TextureCache() = default;
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    bool create(Context& ctx, VkDescriptorSetLayout layout);
    void destroy();

    // Points the cache at the guest's video and palette memory. Both are borrowed, not copied.
    void set_memory(const std::uint8_t* vram, std::size_t vram_size,
                    const std::uint32_t* palette_ram, PaletteFormat palette_format);
    // Drops everything, for instance after the guest rewrites palette memory.
    void invalidate();
    // Re-reads palette memory and drops everything only if it actually changed. A title rewrites
    // the palette registers far more often than it changes what is in them, and dropping the cache
    // every frame would decode every texture every frame. Returns true when it invalidated.
    bool set_palette(const std::uint32_t* palette_ram, PaletteFormat format);
    // Drops the textures whose pixels lie in [begin, end) of video memory. This is what makes a
    // render to texture work: the frame the renderer just wrote back into video memory has to
    // replace whatever the cache decoded from those bytes before. It is also the test that tells a
    // render to texture apart from ordinary double buffering, because a double-buffered title
    // never samples the buffer it just wrote. Returns the number dropped.
    std::size_t invalidate_range(std::uint32_t begin, std::uint32_t end);

    // The descriptor set for a polygon's texture, or VK_NULL_HANDLE when it cannot be decoded.
    // `cmd` is a command buffer outside a render pass, used for the upload.
    //
    // `tiled` is whether any polygon in the frame actually samples this texture outside the unit
    // square (dream::render::polygon_tiles). A texture that is only ever mapped once across its
    // polygons is addressed clamp-to-edge whatever its TSP word says, because above the guest's
    // resolution repeat addressing wraps the outermost half-texel to the opposite edge and outlines
    // every quad. It is part of the key: the same texture can be tiled by one polygon and not by
    // another, and the two need different samplers.
    VkDescriptorSet get(VkCommandBuffer cmd, std::uint32_t tcw, std::uint32_t tsp, bool tiled);

    // A 1x1 opaque white texture, bound when a polygon's own texture could not be decoded. The
    // shader is told not to sample, but Vulkan still wants a valid set bound, and a white texel
    // means a stray sample changes nothing.
    VkDescriptorSet fallback(VkCommandBuffer cmd);

    std::size_t size() const noexcept { return entries_.size(); }
    std::uint32_t decoded = 0, failed = 0, hits = 0;
    // Textures dropped because the renderer wrote over them: a non-zero count means the title
    // really is rendering to a texture rather than merely double buffering.
    std::uint32_t overwritten = 0;
    const std::string& error() const noexcept { return error_; }

private:
    // HostBuffer owns a mapping, so an Entry moves but never copies.
    struct Entry {
        Entry() = default;
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        HostBuffer staging;
        TextureInfo info;
    };

    void free_entry(Entry& e);
    VkSampler sampler_for(std::uint32_t tsp, bool tiled);
    bool upload(VkCommandBuffer cmd, Entry& entry, const std::vector<std::uint32_t>& pixels);

    Context* ctx_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::unordered_map<std::uint32_t, VkSampler> samplers_;
    Entry fallback_{};
    const std::uint8_t* vram_ = nullptr;
    std::size_t vram_size_ = 0;
    std::uint32_t palette_[1024]{};
    std::string error_;
};

}  // namespace dream::render::vk
