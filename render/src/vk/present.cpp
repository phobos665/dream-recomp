// See present.h.
#include "dream/render/vk/present.h"

#include <cstring>

#include "present_frag.h"
#include "present_vert.h"

namespace dream::render::vk {

namespace {

struct PushConstants {
    float scale[2];
    float offset[2];
};

VkShaderModule make_module(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

}  // namespace

Presenter::~Presenter() {
    destroy();
}

bool Presenter::create(Context& ctx, VkRenderPass render_pass) {
    ctx_ = &ctx;
    render_pass_ = render_pass;
    VkDevice dev = ctx.device();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &set_layout_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorSetLayout failed for the presenter";
        return false;
    }
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &pool_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorPool failed for the presenter";
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(dev, &dsai, &set_) != VK_SUCCESS) {
        error_ = "vkAllocateDescriptorSets failed for the presenter";
        return false;
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = sci.addressModeU;
    sci.addressModeW = sci.addressModeU;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler_) != VK_SUCCESS) {
        error_ = "vkCreateSampler failed for the presenter";
        return false;
    }

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout_) != VK_SUCCESS) {
        error_ = "vkCreatePipelineLayout failed for the presenter";
        return false;
    }

    vs_ = make_module(dev, present_vert, sizeof present_vert);
    fs_ = make_module(dev, present_frag, sizeof present_frag);
    if (!vs_ || !fs_) {
        error_ = "the presentation shaders would not load";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;

    // No vertex buffer: the triangle's corners come from gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // The finished frame owns every pixel it covers: no depth test, no blending.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamic;

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
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_) !=
        VK_SUCCESS) {
        error_ = "vkCreateGraphicsPipelines failed for the presenter";
        return false;
    }
    return true;
}

void Presenter::destroy_image() {
    if (!ctx_ || !ctx_->device())
        return;
    VkDevice dev = ctx_->device();
    if (view_)
        vkDestroyImageView(dev, view_, nullptr);
    if (image_)
        vkDestroyImage(dev, image_, nullptr);
    if (memory_)
        vkFreeMemory(dev, memory_, nullptr);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    width_ = height_ = 0;
    uploaded_ = false;
}

void Presenter::destroy() {
    if (ctx_ && ctx_->device()) {
        VkDevice dev = ctx_->device();
        destroy_image();
        if (pipeline_)
            vkDestroyPipeline(dev, pipeline_, nullptr);
        if (layout_)
            vkDestroyPipelineLayout(dev, layout_, nullptr);
        if (vs_)
            vkDestroyShaderModule(dev, vs_, nullptr);
        if (fs_)
            vkDestroyShaderModule(dev, fs_, nullptr);
        if (sampler_)
            vkDestroySampler(dev, sampler_, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(dev, pool_, nullptr);  // frees set_ with it
        if (set_layout_)
            vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    staging_.destroy();
    ctx_ = nullptr;
}

bool Presenter::ensure_image(std::uint32_t width, std::uint32_t height) {
    if (image_ && width == width_ && height == height_)
        return true;
    destroy_image();
    VkDevice dev = ctx_->device();

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, &image_) != VK_SUCCESS) {
        error_ = "vkCreateImage failed for the presenter";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, image_, &req);
    const std::uint32_t type = find_memory_type(ctx_->physical_device(), req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        error_ = "no device-local memory for the presenter's image";
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory_) != VK_SUCCESS) {
        error_ = "vkAllocateMemory failed for the presenter's image";
        return false;
    }
    vkBindImageMemory(dev, image_, memory_, 0);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &view_) != VK_SUCCESS) {
        error_ = "vkCreateImageView failed for the presenter";
        return false;
    }

    VkDescriptorImageInfo dii{};
    dii.sampler = sampler_;
    dii.imageView = view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    width_ = width;
    height_ = height;
    return true;
}

bool Presenter::upload(
    VkCommandBuffer cmd, const std::uint32_t* rgba, std::uint32_t width, std::uint32_t height,
    const std::function<void(std::uint32_t*, std::uint32_t, std::uint32_t)>& decorate) {
    if (!ctx_ || !rgba || width == 0 || height == 0)
        return false;
    if (!ensure_image(width, height))
        return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (!staging_.ensure(*ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        error_ = "could not allocate the presenter's staging buffer";
        return false;
    }
    staging_.write(rgba, static_cast<std::size_t>(bytes));
    if (decorate)
        decorate(static_cast<std::uint32_t*>(staging_.mapped()), width, height);

    // The image was either just created (undefined) or read by the fragment shader last frame.
    // Either way it becomes a transfer destination, and the copy waits for that read to finish.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout =
        uploaded_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image_;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = uploaded_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd, uploaded_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging_.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);
    uploaded_ = true;
    return true;
}

bool Presenter::draw(VkCommandBuffer cmd, VkExtent2D target) {
    if (!uploaded_ || !pipeline_ || target.width == 0 || target.height == 0)
        return false;
    // Fit the guest's aspect ratio inside the window: shrink whichever axis has room to spare.
    const float image_aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float target_aspect =
        static_cast<float>(target.width) / static_cast<float>(target.height);
    PushConstants push{{1.0f, 1.0f}, {0.0f, 0.0f}};
    if (target_aspect > image_aspect)
        push.scale[0] = image_aspect / target_aspect;
    else
        push.scale[1] = target_aspect / image_aspect;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    return true;
}

}  // namespace dream::render::vk
