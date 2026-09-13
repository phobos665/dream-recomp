// See renderer.h.
#include "dream/render/vk/renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "dream/render/tsp.h"

#include "geometry_frag.h"
#include "geometry_vert.h"

namespace dream::render::vk {

namespace {

// Vertex layout handed to the shaders: position (x, y, 1/w), texture coordinates, base colour and
// offset colour as floats. Nine floats; unpacking the colours here keeps the shader simple and the
// cost is a frame's worth of arithmetic on the CPU, which is nothing beside the draw calls.
constexpr std::size_t kFloatsPerVertex = 3 + 2 + 4 + 4;

// ISP/TSP instruction word (Flycast's ISP_TSP union): the fields that decide pipeline state.
constexpr std::uint32_t isp_depth_mode(std::uint32_t isp) {
    return (isp >> 29) & 7u;
}
constexpr std::uint32_t isp_cull_mode(std::uint32_t isp) {
    return (isp >> 27) & 3u;
}
constexpr std::uint32_t isp_z_write_disable(std::uint32_t isp) {
    return (isp >> 26) & 1u;
}

// The hardware's eight depth comparisons, in its own order. The depth buffer holds a logarithm of
// 1/w, so "nearer" is a larger value and the senses are the reverse of the usual convention.
VkCompareOp depth_compare(std::uint32_t mode) {
    switch (mode) {
        case 0:
            return VK_COMPARE_OP_NEVER;
        case 1:
            return VK_COMPARE_OP_LESS;
        case 2:
            return VK_COMPARE_OP_EQUAL;
        case 3:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4:
            return VK_COMPARE_OP_GREATER;
        case 5:
            return VK_COMPARE_OP_NOT_EQUAL;
        case 6:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:
            return VK_COMPARE_OP_ALWAYS;
    }
}

// Culling is by signed area on the hardware, with two thresholds we cannot express directly. Modes
// 2 and 3 are the ordinary back-face and front-face cases; 0 and 1 disable it.
VkCullModeFlags cull_flags(std::uint32_t mode) {
    switch (mode) {
        case 2:
            return VK_CULL_MODE_FRONT_BIT;
        case 3:
            return VK_CULL_MODE_BACK_BIT;
        default:
            return VK_CULL_MODE_NONE;
    }
}

void unpack_colour(std::uint32_t argb, float* out) {
    out[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;  // r
    out[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;   // g
    out[2] = static_cast<float>(argb & 0xFFu) / 255.0f;          // b
    out[3] = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;  // a
}

VkShaderModule make_module(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

struct PushConstants {
    float scale[2];
    float offset[2];
    std::int32_t mode;
    float alpha_ref;
};

// What identifies a texture within one frame: the control word, plus the TSP bits that carry the
// size. Everything the sampler reads is decided per texture in prepare(), so it does not belong
// here; two polygons that share these bits share the decoded image.
constexpr std::uint64_t texture_key(const Polygon& p) {
    return (static_cast<std::uint64_t>(p.tcw) << 32) | (p.tsp & 0x3Fu);
}

// Parameter control word and TSP fields the fragment shader needs.
constexpr std::uint32_t pcw_texture(std::uint32_t p) {
    return (p >> 3) & 1u;
}
constexpr std::uint32_t pcw_offset(std::uint32_t p) {
    return (p >> 2) & 1u;
}
// The hardware's eight blend factors. Index 2 and 3 mean "the other colour", which is the
// destination colour for a source factor and the source colour for a destination factor; the rest
// are the same on both sides.
VkBlendFactor src_factor(std::uint32_t instr) {
    switch (instr) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 2:
            return VK_BLEND_FACTOR_DST_COLOR;
        case 3:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 4:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:
            return VK_BLEND_FACTOR_DST_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}
VkBlendFactor dst_factor(std::uint32_t instr) {
    switch (instr) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 2:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case 3:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:
            return VK_BLEND_FACTOR_DST_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

}  // namespace

Renderer::~Renderer() {
    destroy();
}

bool Renderer::create(Context& ctx, VkRenderPass render_pass) {
    ctx_ = &ctx;
    render_pass_ = render_pass;
    vs_ = make_module(ctx.device(), geometry_vert, sizeof geometry_vert);
    fs_ = make_module(ctx.device(), geometry_frag, sizeof geometry_frag);
    if (!vs_ || !fs_) {
        error_ = "failed to create the geometry shader modules";
        return false;
    }

    // One combined image sampler: the polygon's texture.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(ctx.device(), &dslci, nullptr, &set_layout_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    if (!textures_.create(ctx, set_layout_)) {
        error_ = textures_.error();
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.device(), &plci, nullptr, &layout_) != VK_SUCCESS) {
        error_ = "vkCreatePipelineLayout failed";
        return false;
    }
    return true;
}

VkPipeline Renderer::pipeline_for(const PipelineKey& key) {
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = static_cast<std::uint32_t>(kFloatsPerVertex * sizeof(float));
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::array<VkVertexInputAttributeDescription, 4> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 5 * sizeof(float)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 9 * sizeof(float)},
    }};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull_flags(key.cull_mode);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = depth_compare(key.depth_mode);

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (key.blend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = src_factor(key.src_blend);
        blend.dstColorBlendFactor = dst_factor(key.dst_blend);
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
        blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout_;
    gpci.renderPass = render_pass_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_->device(), VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        error_ = "vkCreateGraphicsPipelines failed";
        return VK_NULL_HANDLE;
    }
    pipelines_.emplace(key, pipeline);
    pipelines = static_cast<std::uint32_t>(pipelines_.size());
    return pipeline;
}

void Renderer::set_memory(const std::uint8_t* vram, std::size_t vram_size,
                          const std::uint32_t* palette_ram, PaletteFormat palette_format) {
    textures_.set_memory(vram, vram_size, palette_ram, palette_format);
    frame_sets_.clear();
}

// Textures are decoded and uploaded before the render pass begins, because a copy cannot be
// recorded inside one. The descriptor sets are remembered for the draw that follows.
void Renderer::prepare(VkCommandBuffer upload, const Frame& frame) {
    frame_sets_.clear();
    fallback_set_ = textures_.fallback(upload);
    // Two passes, because whether a texture is addressed with repeat or clamp is a property of
    // every polygon that samples it, not of the first one seen. One strip that genuinely wraps is
    // enough to need repeat for all of them; the union is the safe way round, since clamping
    // something that does tile would lose the repetition, while repeating something that does not
    // only costs the seam this is here to remove.
    struct Use {
        std::uint32_t tcw = 0, tsp = 0;
        bool tiled = false;
    };
    std::unordered_map<std::uint64_t, Use> used;
    for (const auto& list : frame.lists)
        for (const Polygon& p : list) {
            if (!pcw_texture(p.pcw))
                continue;
            Use& u = used[texture_key(p)];
            u.tcw = p.tcw;
            u.tsp = p.tsp;
            u.tiled = u.tiled || polygon_tiles(frame, p);
        }
    for (const auto& [key, u] : used)
        frame_sets_[key] = textures_.get(upload, u.tcw, u.tsp, u.tiled);
}

void Renderer::draw(VkCommandBuffer cmd, const Frame& frame, const FrameGeometry& geometry,
                    VkExtent2D target) {
    drawn_polygons = 0;
    drawn_vertices = 0;
    textured_polygons = 0;
    blended_polygons = 0;
    punch_through_polygons = 0;
    if (frame.vertices.empty())
        return;

    // Pack the vertices into the shader's layout.
    staging_.resize(frame.vertices.size() * kFloatsPerVertex);
    float* out = staging_.data();
    for (const Vertex& v : frame.vertices) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
        out[3] = v.u;
        out[4] = v.v;
        unpack_colour(v.base, out + 5);
        unpack_colour(v.offset, out + 9);
        out += kFloatsPerVertex;
    }
    const VkDeviceSize bytes = staging_.size() * sizeof(float);
    if (!vertices_.ensure(*ctx_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return;
    vertices_.write(staging_.data(), static_cast<std::size_t>(bytes));

    // The guest draws to its own framebuffer size; the window may be any size, and the viewport
    // the caller set already stretches one to the other.
    PushConstants push{};
    push.scale[0] = 2.0f / geometry.width;
    push.scale[1] = 2.0f / geometry.height;
    push.offset[0] = -1.0f - 2.0f * geometry.left / geometry.width;
    push.offset[1] = -1.0f - 2.0f * geometry.top / geometry.height;
    (void)target;

    const VkDeviceSize zero = 0;
    VkBuffer buffer = vertices_.handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &zero);

    // Opaque geometry only in this step. Punch-through and translucent lists need the alpha test
    // and the sorted pass, which are steps 4 and 5.
    // The hardware draws the lists in a fixed order and the game relies on it: opaque geometry
    // first, then punch-through (opaque with holes, so it can also write depth), then translucent
    // blended over the result.
    //
    // Translucent polygons are sorted back to front per strip. That is the fallback the hardware
    // does not need: a real PowerVR sorts per pixel, so two translucent surfaces that intersect
    // come out right and a per-strip sort cannot. Per-pixel sorting is a later refinement
    // (docs/runtime-render.md); a per-strip sort is what most Dreamcast rendering used for years
    // and is right for all but intersecting transparency.
    struct Item {
        const Polygon* poly;
        float depth;  // 1/w: larger is nearer
        bool blended;
        bool alpha_test;
    };
    std::vector<Item> items;
    const auto add_list = [&](ListType list, bool blended, bool alpha_test) {
        for (const Polygon& p : frame.lists[static_cast<unsigned>(list)]) {
            float nearest = 0.0f;
            for (std::uint32_t i = 0; i < p.count; ++i)
                nearest = std::max(nearest, frame.vertices[p.first + i].z);
            items.push_back({&p, nearest, blended, alpha_test});
        }
    };
    add_list(ListType::Opaque, false, false);
    add_list(ListType::PunchThrough, false, true);
    const std::size_t translucent_start = items.size();
    add_list(ListType::Translucent, true, false);
    // Back to front: the farthest surface has the smallest 1/w and must be drawn first.
    std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(translucent_start), items.end(),
                     [](const Item& a, const Item& b) { return a.depth < b.depth; });

    VkPipeline bound = VK_NULL_HANDLE;
    for (const Item& item : items) {
        const Polygon& p = *item.poly;
        PipelineKey key{};
        key.depth_mode = isp_depth_mode(p.isp) & 7u;
        // A translucent surface tests depth but does not write it: writing would hide surfaces
        // behind it that still have to be blended in.
        key.depth_write = (item.blended || isp_z_write_disable(p.isp)) ? 0u : 1u;
        key.cull_mode = isp_cull_mode(p.isp) & 3u;
        key.blend = item.blended ? 1u : 0u;
        key.src_blend = tsp_src_instr(p.tsp) & 7u;
        key.dst_blend = tsp_dst_instr(p.tsp) & 7u;
        key.padding = 0;
        VkPipeline pipeline = pipeline_for(key);
        if (!pipeline)
            continue;
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        // A polygon whose texture could not be decoded draws untextured rather than not at all:
        // a flat shape in the right place says more during bring-up than a hole.
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (pcw_texture(p.pcw)) {
            const auto it = frame_sets_.find(texture_key(p));
            if (it != frame_sets_.end())
                set = it->second;
        }
        push.mode = 0;
        if (set) {
            push.mode = static_cast<std::int32_t>(tsp_shading_instruction(p.tsp)) | 4;
            if (tsp_ignore_texture_alpha(p.tsp))
                push.mode |= 8;
            ++textured_polygons;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0,
                                    nullptr);
        } else if (fallback_set_) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                    &fallback_set_, 0, nullptr);
        }
        if (pcw_offset(p.pcw))
            push.mode |= 16;
        if (item.alpha_test) {
            push.mode |= 32;
            ++punch_through_polygons;
        }
        if (tsp_use_alpha(p.tsp))
            push.mode |= 64;
        if (item.blended)
            ++blended_polygons;
        push.alpha_ref = geometry.alpha_ref;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof push, &push);
        vkCmdDraw(cmd, p.count, 1, p.first, 0);
        ++drawn_polygons;
        drawn_vertices += p.count;
    }
}

void Renderer::destroy() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    pipelines_.clear();
    textures_.destroy();
    if (set_layout_) {
        vkDestroyDescriptorSetLayout(ctx_->device(), set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    vertices_.destroy();
    if (layout_) {
        vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (vs_) {
        vkDestroyShaderModule(ctx_->device(), vs_, nullptr);
        vs_ = VK_NULL_HANDLE;
    }
    if (fs_) {
        vkDestroyShaderModule(ctx_->device(), fs_, nullptr);
        fs_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

}  // namespace dream::render::vk
