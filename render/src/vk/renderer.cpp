// See renderer.h.
#include "dream/render/vk/renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "dream/render/tsp.h"

#include "geometry_frag.h"
#include "geometry_oit_frag.h"
#include "geometry_vert.h"
#include "resolve_frag.h"
#include "resolve_vert.h"

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
    float padding[2];  // the shaders' vec4 starts on a 16-byte boundary
    float clip[4];     // the user clip rectangle in guest pixels: x0, y0, x1, y1
    std::int32_t clip_mode;
    // Per-pixel translucency: depth compare in bits 0-2, source and destination factors in 3-5
    // and 6-8, the polygon's place in the translucent list from bit 9.
    std::int32_t oit;
};

// Per-pixel translucency: room for this many translucent fragments per pixel on average, and no
// more than kMaxFragmentBytes in all. Each fragment is 16 bytes.
constexpr std::uint64_t kFragmentsPerPixel = 8;
constexpr std::uint64_t kMaxFragmentBytes = 64ull << 20;
constexpr std::uint32_t kFragmentHeaderBytes = 16;  // count, capacity, two words of padding

// The fog uniform block, std140. Each table entry is the factor where its stretch of depth starts
// and where it ends; the end equals the next entry's start, and the shader interpolates between
// the two. Two entries share a vec4.
struct FogUniform {
    float table_colour[4];
    float vertex_colour[4];
    float clamp_min[4];
    float clamp_max[4];
    float params[4];  // x: density
    float table[64][4];
};

void unpack_rgba(std::uint32_t rgba, float* out) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<float>((rgba >> (8 * i)) & 0xFFu) / 255.0f;
}

FogUniform fog_uniform(const FogSettings& fog) {
    FogUniform u{};
    unpack_rgba(fog.table_colour, u.table_colour);
    unpack_rgba(fog.vertex_colour, u.vertex_colour);
    // FOG_CLAMP_MIN and MAX are ARGB, the order of the vertex colours.
    unpack_colour(fog.clamp_min, u.clamp_min);
    unpack_colour(fog.clamp_max, u.clamp_max);
    u.params[0] = fog.density;
    for (std::size_t i = 0; i < 128; ++i) {
        u.table[i / 2][(i & 1) * 2] = static_cast<float>(fog.factor[i]) / 255.0f;
        u.table[i / 2][(i & 1) * 2 + 1] = static_cast<float>(fog.delta[i]) / 255.0f;
    }
    return u;
}

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

bool Renderer::create(Context& ctx, VkRenderPass render_pass, const OitTargets& oit) {
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

    // Set 1: the fog uniform block, bound once per frame.
    VkDescriptorSetLayoutBinding fog_binding{};
    fog_binding.binding = 0;
    fog_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    fog_binding.descriptorCount = 1;
    fog_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo fog_dslci{};
    fog_dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    fog_dslci.bindingCount = 1;
    fog_dslci.pBindings = &fog_binding;
    if (vkCreateDescriptorSetLayout(ctx.device(), &fog_dslci, nullptr, &fog_layout_) !=
        VK_SUCCESS) {
        error_ = "vkCreateDescriptorSetLayout (fog) failed";
        return false;
    }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(ctx.device(), &dpci, nullptr, &fog_pool_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorPool (fog) failed";
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = fog_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &fog_layout_;
    // Sized once here: the descriptor points at this buffer, so it must never be reallocated.
    if (vkAllocateDescriptorSets(ctx.device(), &dsai, &fog_set_) != VK_SUCCESS ||
        !fog_buffer_.ensure(ctx, sizeof(FogUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        error_ = "cannot set up the fog uniform buffer";
        return false;
    }
    const FogUniform none = fog_uniform(FogSettings{});
    fog_buffer_.write(&none, sizeof none);
    VkDescriptorBufferInfo dbi{fog_buffer_.handle(), 0, sizeof(FogUniform)};
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = fog_set_;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wds.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(ctx.device(), 1, &wds, 0, nullptr);

    // Set 2, when the render pass sorts translucent fragments per pixel.
    if (oit.colour && oit.depth && !create_oit(oit))
        return false;

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(PushConstants);
    const VkDescriptorSetLayout set_layouts[] = {set_layout_, fog_layout_, oit_layout_};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = oit_ ? 3u : 2u;
    plci.pSetLayouts = set_layouts;
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

    const VkPipeline pipeline =
        build_pipeline(vs_, fs_, true, cull_flags(key.cull_mode), ds, blend, 0);
    if (!pipeline)
        return VK_NULL_HANDLE;
    pipelines_.emplace(key, pipeline);
    pipelines = static_cast<std::uint32_t>(pipelines_.size());
    return pipeline;
}

VkPipeline Renderer::build_pipeline(VkShaderModule vs, VkShaderModule fs, bool vertices,
                                    VkCullModeFlags cull,
                                    const VkPipelineDepthStencilStateCreateInfo& depth,
                                    const VkPipelineColorBlendAttachmentState& blend,
                                    std::uint32_t subpass) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
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
    if (vertices) {  // the resolve pass makes its triangle from the vertex index alone
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vi.pVertexAttributeDescriptions = attributes.data();
    }

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
    rs.cullMode = cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    gpci.pDepthStencilState = &depth;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout_;
    gpci.renderPass = render_pass_;
    gpci.subpass = subpass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_->device(), VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        error_ = "vkCreateGraphicsPipelines failed";
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool Renderer::create_oit(const OitTargets& oit) {
    VkDevice dev = ctx_->device();
    fs_append_ = make_module(dev, geometry_oit_frag, sizeof geometry_oit_frag);
    vs_resolve_ = make_module(dev, resolve_vert, sizeof resolve_vert);
    fs_resolve_ = make_module(dev, resolve_frag, sizeof resolve_frag);
    if (!fs_append_ || !vs_resolve_ || !fs_resolve_) {
        error_ = "failed to create the per-pixel translucency shader modules";
        return false;
    }

    // 0: the fragments and their count, 1: each pixel's first fragment, 2: the opaque depth (read
    // when listing), 3: the colour (read when blending).
    VkDescriptorSetLayoutBinding bindings[4]{};
    const VkDescriptorType types[4] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT};
    for (std::uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 4;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &oit_layout_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorSetLayout (per-pixel translucency) failed";
        return false;
    }
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                           {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2}};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &oit_pool_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorPool (per-pixel translucency) failed";
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = oit_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &oit_layout_;
    if (vkAllocateDescriptorSets(dev, &dsai, &oit_set_) != VK_SUCCESS) {
        error_ = "vkAllocateDescriptorSets (per-pixel translucency) failed";
        return false;
    }

    const std::uint64_t pixels = std::uint64_t{oit.extent.width} * oit.extent.height;
    fragment_capacity_ = static_cast<std::uint32_t>(
        std::min(pixels * kFragmentsPerPixel, (kMaxFragmentBytes - kFragmentHeaderBytes) / 16));
    if (!fragments_.create(*ctx_, kFragmentHeaderBytes + std::uint64_t{fragment_capacity_} * 16,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        !heads_.create(*ctx_, VK_FORMAT_R32_UINT, oit.extent,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT)) {
        error_ = "cannot allocate the per-pixel translucency buffers";
        return false;
    }

    const VkDescriptorBufferInfo buffer{fragments_.handle(), 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo images[3] = {
        {VK_NULL_HANDLE, heads_.view(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, oit.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, oit.colour, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = oit_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = types[i];
        if (i == 0)
            writes[i].pBufferInfo = &buffer;
        else
            writes[i].pImageInfo = &images[i - 1];
    }
    vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
    heads_ready_ = false;
    oit_ = true;
    return true;
}

// Lists a translucent polygon's fragments: no depth test or write in the pipeline (the shader tests
// against the opaque depth itself) and nothing written to the colour.
VkPipeline Renderer::append_pipeline(std::uint32_t cull_mode) {
    if (auto it = append_pipelines_.find(cull_mode); it != append_pipelines_.end())
        return it->second;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    const VkPipelineColorBlendAttachmentState masked{};
    const VkPipeline pipeline =
        build_pipeline(vs_, fs_append_, true, cull_flags(cull_mode), ds, masked, 1);
    if (pipeline)
        append_pipelines_.emplace(cull_mode, pipeline);
    return pipeline;
}

void Renderer::destroy_oit() {
    VkDevice dev = ctx_->device();
    for (auto& [cull, pipeline] : append_pipelines_) vkDestroyPipeline(dev, pipeline, nullptr);
    append_pipelines_.clear();
    if (resolve_pipeline_)
        vkDestroyPipeline(dev, resolve_pipeline_, nullptr);
    resolve_pipeline_ = VK_NULL_HANDLE;
    for (VkShaderModule* m : {&fs_append_, &vs_resolve_, &fs_resolve_}) {
        if (*m)
            vkDestroyShaderModule(dev, *m, nullptr);
        *m = VK_NULL_HANDLE;
    }
    if (oit_pool_)
        vkDestroyDescriptorPool(dev, oit_pool_, nullptr);  // frees oit_set_
    oit_pool_ = VK_NULL_HANDLE;
    oit_set_ = VK_NULL_HANDLE;
    if (oit_layout_)
        vkDestroyDescriptorSetLayout(dev, oit_layout_, nullptr);
    oit_layout_ = VK_NULL_HANDLE;
    fragments_.destroy();
    heads_.destroy();
    heads_ready_ = false;
    oit_ = false;
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

    if (oit_) {
        // Empty lists for the frame: every head says "none", and the count starts again. The
        // previous frame has finished with them, because each frame is waited for.
        const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!heads_ready_) {
            VkImageMemoryBarrier to_general{};
            to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.image = heads_.image();
            to_general.subresourceRange = whole;
            to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_general);
            heads_ready_ = true;
        }
        VkClearColorValue none{};
        none.uint32[0] = 0xFFFFFFFFu;
        vkCmdClearColorImage(upload, heads_.image(), VK_IMAGE_LAYOUT_GENERAL, &none, 1, &whole);
        const std::uint32_t header[2] = {0, fragment_capacity_};
        vkCmdUpdateBuffer(upload, fragments_.handle(), 0, sizeof header, header);
        VkMemoryBarrier ready{};
        ready.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &ready, 0, nullptr, 0,
                             nullptr);
    }
}

void Renderer::draw(VkCommandBuffer cmd, const Frame& frame, const FrameGeometry& geometry,
                    VkExtent2D target) {
    drawn_polygons = 0;
    drawn_vertices = 0;
    textured_polygons = 0;
    blended_polygons = 0;
    punch_through_polygons = 0;

    // With per-pixel sorting the render pass has three subpasses, and it has to reach the last one
    // whatever the frame holds: 0 draws the opaque and punch-through lists, 1 lists translucent
    // fragments, 2 sorts and blends them.
    std::uint32_t subpass = 0;
    const auto next_subpass = [&] {
        vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
        ++subpass;
    };
    const auto finish = [&](bool resolve) {
        if (!oit_)
            return;
        while (subpass < 2) next_subpass();
        if (!resolve)
            return;
        if (!resolve_pipeline_) {
            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            VkPipelineColorBlendAttachmentState all{};
            all.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            resolve_pipeline_ =
                build_pipeline(vs_resolve_, fs_resolve_, false, VK_CULL_MODE_NONE, ds, all, 2);
            if (!resolve_pipeline_)
                return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &oit_set_, 0,
                                nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };
    if (frame.vertices.empty()) {
        finish(false);
        return;
    }

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
    if (!vertices_.ensure(*ctx_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        finish(false);
        return;
    }
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
    const FogUniform fog = fog_uniform(geometry.fog);
    fog_buffer_.write(&fog, sizeof fog);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &fog_set_, 0,
                            nullptr);

    // Opaque geometry only in this step. Punch-through and translucent lists need the alpha test
    // and the sorted pass, which are steps 4 and 5.
    // The hardware draws the lists in a fixed order and the game relies on it: opaque geometry
    // first, then punch-through (opaque with holes, so it can also write depth), then translucent
    // blended over the result.
    //
    // The hardware sorts translucent polygons per pixel (in its auto-sort mode, which is what the
    // titles measured so far use). With a render pass that can (OitTargets), so do we: each
    // translucent fragment is listed in subpass 1 and every pixel's list is sorted and blended in
    // subpass 2, so surfaces that intersect, and a decal on the surface it was sent after, come
    // out right. Otherwise the list is sorted back to front per strip, which is what most
    // Dreamcast rendering used for years and is right for all but those cases.
    struct Item {
        const Polygon* poly;
        float depth;  // the strip's farthest 1/w (larger is nearer): its place in the sort
        bool blended;
        bool alpha_test;
    };
    std::vector<Item> items;
    // A strip is placed by its farthest vertex, the smallest 1/w, as Flycast's sortPolyParams
    // (core/hw/pvr/ta_util.cpp) does. The nearest vertex is a poor key because a vertex behind the
    // camera is clamped close to the eye: Daytona USA 2001 gives such vertices 1/w = 500, so a
    // fence crossing the screen sorted after the HUD's sprites at 400 and was blended over them.
    const auto add_list = [&](ListType list, bool blended, bool alpha_test) {
        for (const Polygon& p : frame.lists[static_cast<unsigned>(list)]) {
            float farthest = p.count ? std::numeric_limits<float>::max() : 0.0f;
            for (std::uint32_t i = 0; i < p.count; ++i)
                farthest = std::min(farthest, frame.vertices[p.first + i].z);
            items.push_back({&p, farthest, blended, alpha_test});
        }
    };
    add_list(ListType::Opaque, false, false);
    add_list(ListType::PunchThrough, false, true);
    const std::size_t translucent_start = items.size();
    add_list(ListType::Translucent, true, false);
    // Back to front: the farthest surface has the smallest 1/w and must be drawn first. Listed per
    // pixel instead, the polygons keep the order they were sent in, which settles ties.
    if (!oit_)
        std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(translucent_start),
                         items.end(),
                         [](const Item& a, const Item& b) { return a.depth < b.depth; });

    VkPipeline bound = VK_NULL_HANDLE;
    for (std::size_t n = 0; n < items.size(); ++n) {
        const Item& item = items[n];
        const Polygon& p = *item.poly;
        const bool listed = oit_ && n >= translucent_start;
        if (listed && subpass == 0) {
            next_subpass();
            bound = VK_NULL_HANDLE;  // a pipeline belongs to one subpass
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &oit_set_,
                                    0, nullptr);
        }
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
        VkPipeline pipeline = listed ? append_pipeline(key.cull_mode) : pipeline_for(key);
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
        push.mode |= static_cast<std::int32_t>(tsp_fog_control(p.tsp) << 7);
        if (tsp_colour_clamp(p.tsp))
            push.mode |= 512;
        if (item.blended)
            ++blended_polygons;
        push.alpha_ref = geometry.alpha_ref;
        push.clip_mode = static_cast<std::int32_t>(p.tile_clip);
        push.clip[0] = p.clip_x0;
        push.clip[1] = p.clip_y0;
        push.clip[2] = p.clip_x1;
        push.clip[3] = p.clip_y1;
        push.oit = listed ? static_cast<std::int32_t>(
                                key.depth_mode | (key.src_blend << 3) | (key.dst_blend << 6) |
                                (static_cast<std::uint32_t>(n - translucent_start) << 9))
                          : 0;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof push, &push);
        vkCmdDraw(cmd, p.count, 1, p.first, 0);
        ++drawn_polygons;
        drawn_vertices += p.count;
    }
    finish(true);
}

void Renderer::destroy() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    pipelines_.clear();
    destroy_oit();
    textures_.destroy();
    if (set_layout_) {
        vkDestroyDescriptorSetLayout(ctx_->device(), set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    vertices_.destroy();
    fog_buffer_.destroy();
    if (fog_pool_) {
        vkDestroyDescriptorPool(ctx_->device(), fog_pool_, nullptr);  // frees fog_set_
        fog_pool_ = VK_NULL_HANDLE;
        fog_set_ = VK_NULL_HANDLE;
    }
    if (fog_layout_) {
        vkDestroyDescriptorSetLayout(ctx_->device(), fog_layout_, nullptr);
        fog_layout_ = VK_NULL_HANDLE;
    }
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
