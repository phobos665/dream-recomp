// dream_render_probe: opens a window and draws a triangle. Proves the graphics stack works on this
// machine (WP2.3 step 1) and prints the device capabilities the renderer will branch on. Not a
// unit test: it needs a display.
//
//   dream_render_probe [--frames N] [--validation]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dream/render/vk/window.h"

#include "triangle_frag.h"
#include "triangle_vert.h"

namespace {

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

int main(int argc, char** argv) {
    int frames = 0;  // 0: until the window is closed
    bool validation = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--validation"))
            validation = true;
    }

    dream::render::vk::Window window;
    if (!window.create("dream-recomp renderer probe", 960, 720, validation)) {
        std::fprintf(stderr, "probe: %s\n", window.error().c_str());
        return 1;
    }
    const auto& caps = window.context().caps();
    std::printf("device: %s\n", caps.device_name.c_str());
    std::printf("  fragment stores and atomics: %s (per-pixel transparency %s)\n",
                caps.fragment_stores_and_atomics ? "yes" : "no",
                caps.fragment_stores_and_atomics ? "available" : "unavailable, per-strip fallback");
    std::printf("  fragment shader interlock:   %s\n",
                caps.fragment_shader_interlock ? "yes" : "no");
    std::printf("  independent blend:           %s\n", caps.independent_blend ? "yes" : "no");
    std::printf("  anisotropic filtering:       %s\n", caps.sampler_anisotropy ? "yes" : "no");
    std::printf("  max texture size:            %u\n", caps.max_texture_size);

    VkDevice device = window.context().device();
    VkShaderModule vs = make_module(device, triangle_vert, sizeof triangle_vert);
    VkShaderModule fs = make_module(device, triangle_frag, sizeof triangle_frag);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &plci, nullptr, &layout);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

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
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout;
    gpci.renderPass = window.render_pass();
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "probe: vkCreateGraphicsPipelines failed\n");
        return 1;
    }

    int drawn = 0;
    while (window.poll() && (frames == 0 || drawn < frames)) {
        std::uint32_t image = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!window.begin_frame(image, cmd)) {
            if (!window.recreate_swapchain())
                break;
            continue;
        }
        VkClearValue clear{};
        clear.color = {{0.05f, 0.06f, 0.09f, 1.0f}};
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = window.render_pass();
        rpbi.framebuffer = window.framebuffer(image);
        rpbi.renderArea.extent = window.extent();
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(window.extent().width),
                                  static_cast<float>(window.extent().height),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, window.extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        if (!window.end_frame(image))
            window.recreate_swapchain();
        ++drawn;
    }
    std::printf("drew %d frames\n", drawn);

    vkDeviceWaitIdle(device);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    return 0;
}
