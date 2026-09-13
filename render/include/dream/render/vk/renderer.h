// Drawing a decoded frame (WP2.3 step 3). Opaque geometry only for now: depth, culling and
// per-vertex colour, no textures and no transparency. The pipeline state that varies per polygon
// (depth compare, depth write, culling) comes from the hardware's own ISP word, so the pipelines
// are cached on exactly the bits that matter.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dream/render/display_list.h"
#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"
#include "dream/render/vk/texture_cache.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

// What the guest's registers say the frame is, so the projection matches the game's own idea of
// the screen rather than the window.
struct FrameGeometry {
    // The region of the guest's screen to show. Normally the whole 640x480 framebuffer; a
    // bring-up can widen it to see geometry the game places off-screen.
    float left = 0, top = 0;
    float width = 640, height = 480;
    // The punch-through threshold, from the hardware's PT_ALPHA_REF register (PVR + 0x11C).
    float alpha_ref = 1.0f / 255.0f;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool create(Context& ctx, VkRenderPass render_pass);
    // Points the renderer at the guest's video and palette memory so textures can be decoded.
    void set_memory(const std::uint8_t* vram, std::size_t vram_size,
                    const std::uint32_t* palette_ram, PaletteFormat palette_format);
    TextureCache& textures() noexcept { return textures_; }
    void destroy();

    // Records the draw commands for one decoded frame. The caller has already begun the render
    // pass and set the viewport and scissor.
    // `upload` is a command buffer outside a render pass, where textures are uploaded; it may be
    // the same buffer as `cmd` provided draw() is called before the render pass begins.
    void draw(VkCommandBuffer cmd, const Frame& frame, const FrameGeometry& geometry,
              VkExtent2D target);
    // Decodes and uploads every texture the frame needs. Call outside the render pass.
    void prepare(VkCommandBuffer upload, const Frame& frame);
    // Bring-up aid: draw every list with blending off, to see the geometry a frame contains
    // without transparency hiding any of it.
    bool draw_all_lists = false;

    const std::string& error() const noexcept { return error_; }
    // Diagnostics from the last draw.
    std::uint32_t drawn_polygons = 0, drawn_vertices = 0, pipelines = 0, textured_polygons = 0,
                  blended_polygons = 0, punch_through_polygons = 0;

private:
    // The pipeline state a polygon can vary. Packed so it can key the cache.
    struct PipelineKey {
        std::uint32_t depth_mode : 3;  // ISP DepthMode
        std::uint32_t depth_write : 1;
        std::uint32_t cull_mode : 2;  // ISP CullMode
        std::uint32_t src_blend : 3;  // TSP SrcInstr
        std::uint32_t dst_blend : 3;  // TSP DstInstr
        std::uint32_t blend : 1;      // blending on at all (the translucent list)
        std::uint32_t padding : 19;
        std::uint32_t bits() const noexcept {
            return depth_mode | (depth_write << 3) | (cull_mode << 4) | (src_blend << 6) |
                   (dst_blend << 9) | (blend << 12);
        }
        bool operator==(const PipelineKey& o) const noexcept { return bits() == o.bits(); }
    };
    struct KeyHash {
        std::size_t operator()(const PipelineKey& k) const noexcept { return k.bits(); }
    };

    VkPipeline pipeline_for(const PipelineKey& key);

    Context* ctx_ = nullptr;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, VkPipeline, KeyHash> pipelines_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    TextureCache textures_;
    std::unordered_map<std::uint64_t, VkDescriptorSet> frame_sets_;
    VkDescriptorSet fallback_set_ = VK_NULL_HANDLE;
    HostBuffer vertices_;
    std::vector<float> staging_;  // vertex data in the shader's layout
    std::string error_;
};

}  // namespace dream::render::vk
