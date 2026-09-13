// dream_render_view: draws a captured display list in a window (WP2.3 step 3). The fastest way to
// see whether the decoder and the renderer agree with the hardware, without running the game.
//
//   dream_render_view FILE.bin [--width N] [--height N] [--frames N] [--screenshot OUT.ppm]
//
// Capture a FILE with `crazytaxi_boot --dump-ta FILE`.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "dream/render/background.h"
#include "dream/render/display_list.h"
#include "dream/render/framebuffer.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/window.h"

int main(int argc, char** argv) {
    const char* path = nullptr;
    int frames = 0;
    bool validation = false;
    const char* screenshot = nullptr;
    bool fit = false, all_lists = false, show_framebuffer = false;
    const char* vram_path = nullptr;
    dream::render::vk::FrameGeometry geometry;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--width") && i + 1 < argc)
            geometry.width = static_cast<float>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--height") && i + 1 < argc)
            geometry.height = static_cast<float>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--validation"))
            validation = true;
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc)
            screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--fit"))
            fit = true;
        else if (!std::strcmp(argv[i], "--framebuffer"))
            show_framebuffer = true;
        else if (!std::strcmp(argv[i], "--all"))
            all_lists = true;
        else if (!std::strcmp(argv[i], "--vram") && i + 1 < argc)
            vram_path = argv[++i];
        else if (argv[i][0] != '-')
            path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr, "usage: dream_render_view FILE.bin [--width N] [--height N]\n");
        return 2;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    in.seekg(0, std::ios::end);
    const std::size_t bytes = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint32_t> words(bytes / 4, 0);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));

    dream::render::DisplayList decoder;
    decoder.feed_stream(words.data(), words.size());
    const dream::render::Frame& frame = decoder.frame();
    // The background plane is added below, once video memory has been loaded: it does not come
    // from the parameter stream.
    std::printf("%s: %zu polygons (%u strips, %u sprites), %zu vertices, depth %.4f..%.4f\n", path,
                frame.polygon_count(), frame.strips, frame.sprites, frame.vertices.size(),
                static_cast<double>(frame.min_z), static_cast<double>(frame.max_z));
    std::printf("  opaque %zu, translucent %zu, punch-through %zu, modifier triangles %zu\n",
                frame.lists[0].size(), frame.lists[2].size(), frame.lists[4].size(),
                frame.modifiers.size());

    if (fit) {
        float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
        for (const dream::render::Vertex& v : frame.vertices) {
            xmin = v.x < xmin ? v.x : xmin;
            xmax = v.x > xmax ? v.x : xmax;
            ymin = v.y < ymin ? v.y : ymin;
            ymax = v.y > ymax ? v.y : ymax;
        }
        geometry.left = xmin;
        geometry.top = ymin;
        geometry.width = xmax - xmin;
        geometry.height = ymax - ymin;
        std::printf("  fitted to x %.1f..%.1f  y %.1f..%.1f\n", (double)xmin, (double)xmax,
                    (double)ymin, (double)ymax);
    }

    dream::render::vk::Window window;
    if (!window.create("dream-recomp display list", 960, 720, validation)) {
        std::fprintf(stderr, "view: %s\n", window.error().c_str());
        return 1;
    }
    dream::render::vk::Renderer renderer;
    renderer.draw_all_lists = all_lists;
    if (!renderer.create(window.context(), window.render_pass())) {
        std::fprintf(stderr, "view: %s\n", renderer.error().c_str());
        return 1;
    }

    // Video memory, so the textures the frame refers to can be decoded. Written by
    // `crazytaxi_boot --dump-vram FILE`, with the PVR registers beside it in FILE.regs.
    std::vector<std::uint8_t> vram;
    std::vector<std::uint32_t> pvr_regs;
    if (vram_path) {
        std::ifstream vf(vram_path, std::ios::binary);
        if (!vf) {
            std::fprintf(stderr, "cannot read %s\n", vram_path);
            return 1;
        }
        vram.assign(std::istreambuf_iterator<char>(vf), std::istreambuf_iterator<char>());
        std::ifstream rf(std::string(vram_path) + ".regs", std::ios::binary);
        dream::render::PaletteFormat palette_format = dream::render::PaletteFormat::Argb1555;
        const std::uint32_t* palette_ram = nullptr;
        if (rf) {
            pvr_regs.assign(0x2000 / 4, 0);
            rf.read(reinterpret_cast<char*>(pvr_regs.data()), 0x2000);
            // PAL_RAM_CTRL is at +0x108 and palette memory at +0x1000.
            palette_format = static_cast<dream::render::PaletteFormat>(pvr_regs[0x108 / 4] & 3u);
            palette_ram = pvr_regs.data() + 0x1000 / 4;
            // PT_ALPHA_REF at +0x11C: the punch-through threshold the guest chose.
            geometry.alpha_ref = static_cast<float>(pvr_regs[0x11C / 4] & 0xFFu) / 255.0f;
        }
        renderer.set_memory(vram.data(), vram.size(), palette_ram, palette_format);
        if (!pvr_regs.empty() && dream::render::add_background(decoder.frame(), pvr_regs.data(),
                                                               vram.data(), vram.size()))
            std::printf("  background plane added\n");
        std::printf("  video memory %zu bytes, palette format %u\n", vram.size(),
                    static_cast<unsigned>(palette_format));

        // What the video hardware was actually scanning out, and where the next frame was going.
        // Everything the title drew ends up in the first, whichever path drew it; a different
        // address in the second means it was rendering a texture rather than the screen.
        dream::render::FramebufferInfo fb;
        if (!pvr_regs.empty() && dream::render::describe_framebuffer(pvr_regs.data(), fb)) {
            std::printf("  display framebuffer: %s\n", fb.describe().c_str());
            const dream::render::RenderTarget rt =
                dream::render::describe_render_target(pvr_regs.data());
            std::printf("  render target: 0x%06x (%s)\n", rt.address,
                        rt.same_as_display ? "the displayed buffer"
                                           : "another buffer: double buffering or a texture");
            if (show_framebuffer) {
                std::vector<std::uint32_t> pixels;
                if (dream::render::decode_framebuffer(fb, vram.data(), vram.size(), pixels)) {
                    // Written as a picture rather than drawn: this is the guest's own output, not
                    // something the renderer produced, and mixing the two would confuse which is
                    // being looked at.
                    if (FILE* f = std::fopen("framebuffer.ppm", "wb")) {
                        std::fprintf(f, "P6\n%u %u\n255\n", fb.width, fb.height);
                        for (std::uint32_t p : pixels) {
                            const unsigned char rgb[3] = {static_cast<unsigned char>(p),
                                                          static_cast<unsigned char>(p >> 8),
                                                          static_cast<unsigned char>(p >> 16)};
                            std::fwrite(rgb, 1, 3, f);
                        }
                        std::fclose(f);
                        std::printf("  wrote framebuffer.ppm (%ux%u)\n", fb.width, fb.height);
                    }
                } else {
                    std::fprintf(stderr, "  the framebuffer lies outside video memory\n");
                }
            }
        }
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
        VkClearValue clears[2]{};
        clears[0].color = {{0.05f, 0.06f, 0.09f, 1.0f}};
        clears[1].depthStencil = {0.0f, 0};  // the hardware's depth grows towards the viewer
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = window.render_pass();
        rpbi.framebuffer = window.framebuffer(image);
        rpbi.renderArea.extent = window.extent();
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        renderer.prepare(cmd, frame);  // textures upload outside the render pass
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
        renderer.draw(cmd, frame, geometry, window.extent());
        vkCmdEndRenderPass(cmd);
        if (!window.end_frame(image))
            window.recreate_swapchain();
        if (drawn == 0)
            std::printf(
                "drew %u polygons (%u textured, %u blended, %u punch-through), %u vertices, "
                "%u pipelines; textures: %u decoded, %u failed\n",
                renderer.drawn_polygons, renderer.textured_polygons, renderer.blended_polygons,
                renderer.punch_through_polygons, renderer.drawn_vertices, renderer.pipelines,
                renderer.textures().decoded, renderer.textures().failed);
        ++drawn;
    }
    if (screenshot) {
        std::vector<std::uint8_t> pixels;
        std::uint32_t w = 0, h = 0;
        if (window.read_pixels(pixels, w, h)) {
            if (FILE* f = std::fopen(screenshot, "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                std::fwrite(pixels.data(), 1, pixels.size(), f);
                std::fclose(f);
                std::printf("wrote %s (%ux%u)\n", screenshot, w, h);
            }
        } else {
            std::fprintf(stderr, "could not read the framebuffer back\n");
        }
    }
    renderer.destroy();
    return 0;
}
