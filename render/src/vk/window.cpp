// See window.h.
#include "dream/render/vk/window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace dream::render::vk {

namespace {
constexpr std::uint32_t kFramesInFlight = 2;

// The keyboard layout, in Control order. Scancodes rather than key codes, so the physical key is
// the same wherever the layout puts the letter on it.
constexpr SDL_Scancode kScancodes[static_cast<unsigned>(Control::Count)] = {
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,  SDL_SCANCODE_Z,
    SDL_SCANCODE_X,  SDL_SCANCODE_A,    SDL_SCANCODE_S,    SDL_SCANCODE_RETURN, SDL_SCANCODE_Q,
    SDL_SCANCODE_W,  SDL_SCANCODE_F12,  SDL_SCANCODE_F11,  SDL_SCANCODE_F10,
};
}  // namespace

Window::~Window() {
    destroy();
}

bool Window::create(const char* title, int width, int height, bool want_validation) {
    // Gamepads are optional: a machine with none, or an SDL built without the subsystem, still
    // gets a window and a keyboard. Failing the whole launch over a missing joystick driver would
    // be a poor trade.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        SDL_ClearError();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error_ = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        error_ = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }

    std::uint32_t ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    std::vector<const char*> extensions(sdl_exts, sdl_exts + ext_count);
    if (!ctx_.create(extensions, want_validation)) {
        error_ = ctx_.error();
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window_, ctx_.instance(), nullptr, &surface_)) {
        error_ = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx_.physical_device(), ctx_.queue_family(), surface_,
                                         &supported);
    if (!supported) {
        error_ = "the graphics queue cannot present to this surface";
        return false;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx_.queue_family();
    vkCreateCommandPool(ctx_.device(), &pci, nullptr, &pool_);

    cmds_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(ctx_.device(), &cai, cmds_.data());

    fences_.resize(kFramesInFlight);
    acquired_.resize(kFramesInFlight);
    rendered_.resize(kFramesInFlight);
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device(), &fci, nullptr, &fences_[i]);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &acquired_[i]);
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &rendered_[i]);
    }
    // Whatever is already plugged in, plus the default bindings, so a pad works on a first run
    // with no configuration file and no visit to any UI.
    refresh_devices();
    set_bindings(Bindings::defaults());
    return create_swapchain();
}

bool Window::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical_device(), surface_, &caps);

    std::uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical_device(), surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical_device(), surface_, &format_count,
                                         formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = f;
    format_ = chosen.format;

    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent_.width = std::clamp(static_cast<std::uint32_t>(w), caps.minImageExtent.width,
                                   caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(h), caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0)
        return true;  // minimised: nothing to build yet

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0)
        image_count = std::min(image_count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = format_;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    // TRANSFER_SRC so read_pixels() can copy the presented image out for a screenshot.
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported; vsync
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(ctx_.device(), &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        error_ = "vkCreateSwapchainKHR failed";
        return false;
    }

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &count, images_.data());

    // Colour and depth. The renderer writes depth from the fragment shader (the hardware's depth
    // is 1/w), so a depth attachment is needed even for opaque geometry.
    depth_format_ = pick_depth_format(ctx_.physical_device());
    if (depth_format_ == VK_FORMAT_UNDEFINED) {
        error_ = "no supported depth format";
        return false;
    }
    if (!depth_.create(ctx_, depth_format_, extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT)) {
        error_ = "could not create the depth buffer";
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = format_;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1].format = depth_format_;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 2;
    rci.pAttachments = attachments;
    rci.subpassCount = 1;
    rci.pSubpasses = &subpass;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    vkCreateRenderPass(ctx_.device(), &rci, nullptr, &render_pass_);

    views_.resize(count);
    framebuffers_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx_.device(), &vci, nullptr, &views_[i]);
        const VkImageView fb_attachments[2] = {views_[i], depth_.view()};
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = render_pass_;
        fci.attachmentCount = 2;
        fci.pAttachments = fb_attachments;
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        vkCreateFramebuffer(ctx_.device(), &fci, nullptr, &framebuffers_[i]);
    }
    return true;
}

void Window::destroy_swapchain() {
    if (!ctx_.device())
        return;
    vkDeviceWaitIdle(ctx_.device());
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(ctx_.device(), f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(ctx_.device(), v, nullptr);
    framebuffers_.clear();
    views_.clear();
    images_.clear();
    depth_.destroy();
    if (render_pass_) {
        vkDestroyRenderPass(ctx_.device(), render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (swapchain_) {
        vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool Window::recreate_swapchain() {
    destroy_swapchain();
    return create_swapchain();
}

void Window::refresh_devices() {
    for (SDL_Gamepad* g : pads_)
        if (g)
            SDL_CloseGamepad(g);
    pads_.clear();
    devices_.clear();
    devices_.push_back("KEYBOARD");
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) {
                pads_.push_back(g);
                const char* name = SDL_GetGamepadName(g);
                devices_.push_back(name && *name ? name : "GAMEPAD");
            }
        }
        SDL_free(ids);
    }
}

void Window::set_bindings(const Bindings& b) {
    bindings_ = b;
    // Names to codes once, here. Doing it per frame would mean a string lookup per control per
    // frame, and would also mean a typo in the file costing performance rather than being noticed.
    auto resolve = [](const DeviceBindings& d, Resolved* out, bool keyboard) {
        for (unsigned i = 0; i < kPadControlCount; ++i) {
            const Binding& bind = d.b[i];
            out[i] = Resolved{};
            if (!bind.bound())
                continue;
            if (keyboard && bind.source == BindSource::Key) {
                const SDL_Scancode sc = SDL_GetScancodeFromName(bind.code.c_str());
                if (sc != SDL_SCANCODE_UNKNOWN)
                    out[i] = Resolved{BindSource::Key, static_cast<int>(sc), 1};
            } else if (!keyboard && bind.source == BindSource::Button) {
                const SDL_GamepadButton bt = SDL_GetGamepadButtonFromString(bind.code.c_str());
                if (bt != SDL_GAMEPAD_BUTTON_INVALID)
                    out[i] = Resolved{BindSource::Button, static_cast<int>(bt), 1};
            } else if (!keyboard && bind.source == BindSource::Axis) {
                const SDL_GamepadAxis ax = SDL_GetGamepadAxisFromString(bind.code.c_str());
                if (ax != SDL_GAMEPAD_AXIS_INVALID)
                    out[i] = Resolved{BindSource::Axis, static_cast<int>(ax), bind.sign};
            }
            // Anything that did not resolve stays unbound: an unknown name must not throw, and
            // must not silently become scancode 0, which is a real key.
        }
    };
    resolve(bindings_.keyboard, key_, true);
    resolve(bindings_.gamepad, gpad_, false);
    SDL_ClearError();
}

void Window::begin_capture() noexcept {
    capturing_ = true;
    captured_ = false;
    capture_cancelled_ = false;
    capture_ = Binding{};
}

void Window::cancel_capture() noexcept {
    capturing_ = false;
    captured_ = false;
}

bool Window::take_capture(Binding& out, bool& cancelled) noexcept {
    if (!captured_)
        return false;
    captured_ = false;
    out = capture_;
    cancelled = capture_cancelled_;
    return true;
}

bool Window::poll() {
    // A capture consumes the physical input that ends it, so the key being bound does not also
    // reach the game on the same frame.
    bool captured_this_poll = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running_ = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                // Hot-plug: rebuild the list and drop every held state. A pad yanked mid-race must
                // release the accelerator rather than leave it held at its last value.
                refresh_devices();
                for (unsigned i = 0; i < kPadControlCount; ++i) {
                    pad_held_[i] = false;
                    pad_pressed_[i] = false;
                    pad_value_[i] = 0.0f;
                    ramp_[i] = 0.0f;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (capturing_ && !captured_this_poll) {
                    captured_this_poll = true;
                    captured_ = true;
                    capturing_ = false;
                    if (ev.key.key == SDLK_ESCAPE) {
                        capture_cancelled_ = true;
                    } else if (const char* n = SDL_GetScancodeName(ev.key.scancode); n && *n) {
                        capture_ = Binding{BindSource::Key, n, 1};
                    } else {
                        capture_cancelled_ = true;
                    }
                } else if (ev.key.key == SDLK_ESCAPE) {
                    running_ = false;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (capturing_ && !captured_this_poll) {
                    if (const char* n = SDL_GetGamepadStringForButton(
                            static_cast<SDL_GamepadButton>(ev.gbutton.button));
                        n && *n) {
                        captured_this_poll = true;
                        captured_ = true;
                        capturing_ = false;
                        capture_ = Binding{BindSource::Button, n, 1};
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                // Well past any plausible dead zone, so resting drift never binds an axis by
                // itself while the user is thinking about which button to press.
                if (capturing_ && !captured_this_poll && std::abs(ev.gaxis.value) > 20000) {
                    if (const char* n = SDL_GetGamepadStringForAxis(
                            static_cast<SDL_GamepadAxis>(ev.gaxis.axis));
                        n && *n) {
                        captured_this_poll = true;
                        captured_ = true;
                        capturing_ = false;
                        capture_ = Binding{BindSource::Axis, n, ev.gaxis.value < 0 ? -1 : 1};
                    }
                }
                break;
            default:
                break;
        }
    }

    // Read the whole keyboard rather than tracking key events: a frame wants the state as it is
    // now, and a key pressed and released between two polls should not be missed or repeated.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    for (unsigned i = 0; i < static_cast<unsigned>(Control::Count); ++i) {
        const bool was = held_[i];
        const bool now = keys && keys[kScancodes[i]];
        held_[i] = now;
        pressed_[i] = now && !was;
    }

    // Seconds since the last poll, for the trigger ramp. Measured rather than assumed, so the ramp
    // takes the same wall time on a fast host as on a slow one.
    const std::uint64_t now_ns = SDL_GetTicksNS();
    float dt = last_poll_ns_ ? static_cast<float>(now_ns - last_poll_ns_) * 1e-9f : 0.0f;
    last_poll_ns_ = now_ns;
    dt = std::clamp(dt, 0.0f, 0.25f);  // a long stall must not jump the ramp to full

    const float dead = static_cast<float>(bindings_.deadzone_percent) / 100.0f;
    for (unsigned i = 0; i < kPadControlCount; ++i) {
        // Digital and analogue sources are gathered separately, because only a digital one ramps.
        // A pad's real trigger position must pass straight through, or a pad would feel worse than
        // the keyboard it is replacing.
        bool digital_on = false;
        float analogue = 0.0f;
        // While capturing, the game gets nothing: a key held down to bind it must not also drive.
        if (!capturing_) {
            if (keys && key_[i].source == BindSource::Key && keys[key_[i].code])
                digital_on = true;
            for (SDL_Gamepad* g : pads_) {
                if (!g)
                    continue;
                if (gpad_[i].source == BindSource::Button) {
                    if (SDL_GetGamepadButton(g, static_cast<SDL_GamepadButton>(gpad_[i].code)))
                        digital_on = true;
                } else if (gpad_[i].source == BindSource::Axis) {
                    const float raw = static_cast<float>(SDL_GetGamepadAxis(
                                          g, static_cast<SDL_GamepadAxis>(gpad_[i].code))) /
                                      32767.0f;
                    // Only the bound half counts, so left and right are separate bindings on one
                    // stick.
                    const float half = gpad_[i].sign < 0 ? -raw : raw;
                    if (half > dead) {
                        // Rescaled from the dead zone's edge rather than from zero, or the stick
                        // would jump to `dead` the instant it left the centre.
                        analogue =
                            std::max(analogue, std::min(1.0f, (half - dead) / (1.0f - dead)));
                    }
                }
            }
        }

        const bool is_trigger = i == static_cast<unsigned>(PadControl::LeftTrigger) ||
                                i == static_cast<unsigned>(PadControl::RightTrigger);
        float value = analogue;
        if (is_trigger && bindings_.trigger_ramp) {
            constexpr float kRampSeconds = 0.15f;
            ramp_[i] = digital_on ? std::min(1.0f, ramp_[i] + dt / kRampSeconds) : 0.0f;
            value = std::max(value, ramp_[i]);
        } else {
            ramp_[i] = 0.0f;
            if (digital_on)
                value = 1.0f;
        }

        const bool was = pad_held_[i];
        pad_held_[i] = value > 0.0f;
        pad_pressed_[i] = pad_held_[i] && !was;
        pad_value_[i] = value;
    }
    return running_;
}

bool Window::pad_held(PadControl c) const noexcept {
    return pad_held_[static_cast<unsigned>(c)];
}

bool Window::pad_pressed(PadControl c) const noexcept {
    return pad_pressed_[static_cast<unsigned>(c)];
}

float Window::pad_value(PadControl c) const noexcept {
    return pad_value_[static_cast<unsigned>(c)];
}

bool Window::held(Control c) const noexcept {
    return held_[static_cast<unsigned>(c)];
}

bool Window::pressed(Control c) const noexcept {
    return pressed_[static_cast<unsigned>(c)];
}

bool Window::begin_frame(std::uint32_t& image_index, VkCommandBuffer& cmd) {
    if (!swapchain_)
        return false;
    vkWaitForFences(ctx_.device(), 1, &fences_[frame_], VK_TRUE, UINT64_MAX);
    const VkResult r = vkAcquireNextImageKHR(ctx_.device(), swapchain_, UINT64_MAX,
                                             acquired_[frame_], VK_NULL_HANDLE, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        return false;
    if (r != VK_SUCCESS) {
        error_ = "vkAcquireNextImageKHR failed";
        return false;
    }
    vkResetFences(ctx_.device(), 1, &fences_[frame_]);
    cmd = cmds_[frame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return true;
}

bool Window::end_frame(std::uint32_t image_index) {
    VkCommandBuffer cmd = cmds_[frame_];
    vkEndCommandBuffer(cmd);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_[frame_];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[frame_];
    vkQueueSubmit(ctx_.queue(), 1, &si, fences_[frame_]);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[frame_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    const VkResult r = vkQueuePresentKHR(ctx_.queue(), &pi);
    last_presented_ = image_index;
    frame_ = (frame_ + 1) % kFramesInFlight;
    return r == VK_SUCCESS;
}

bool Window::read_pixels(std::vector<std::uint8_t>& out, std::uint32_t& width,
                         std::uint32_t& height) {
    if (images_.empty())
        return false;
    vkDeviceWaitIdle(ctx_.device());
    width = extent_.width;
    height = extent_.height;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;

    HostBuffer staging;
    if (!staging.ensure(ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    // The image last presented is the one before the frame counter's current slot.
    const std::uint32_t last = (frame_ + kFramesInFlight - 1) % kFramesInFlight;
    VkCommandBuffer cmd = cmds_[last];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImage image = images_[last_presented_];
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_src);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle(), 1,
                           &copy);

    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &back);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx_.queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx_.queue());

    // The swapchain format is BGRA; hand back RGB.
    out.resize(static_cast<std::size_t>(width) * height * 3);
    const auto* src = static_cast<const std::uint8_t*>(staging.mapped());
    if (!src)
        return false;
    for (std::size_t i = 0, n = static_cast<std::size_t>(width) * height; i < n; ++i) {
        out[i * 3 + 0] = src[i * 4 + 2];
        out[i * 3 + 1] = src[i * 4 + 1];
        out[i * 3 + 2] = src[i * 4 + 0];
    }
    return true;
}

void Window::destroy() {
    if (ctx_.device()) {
        vkDeviceWaitIdle(ctx_.device());
        destroy_swapchain();
        for (VkFence f : fences_) vkDestroyFence(ctx_.device(), f, nullptr);
        for (VkSemaphore s : acquired_) vkDestroySemaphore(ctx_.device(), s, nullptr);
        for (VkSemaphore s : rendered_) vkDestroySemaphore(ctx_.device(), s, nullptr);
        fences_.clear();
        acquired_.clear();
        rendered_.clear();
        if (pool_) {
            vkDestroyCommandPool(ctx_.device(), pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
        }
    }
    if (surface_ && ctx_.instance()) {
        vkDestroySurfaceKHR(ctx_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    ctx_.destroy();
    for (SDL_Gamepad* g : pads_)
        if (g)
            SDL_CloseGamepad(g);
    pads_.clear();
    devices_.clear();
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        // Only the subsystem this class started. SDL_Quit() shuts down every subsystem in the
        // process, including the audio one the launcher's sink is still holding a stream from,
        // and the sink's destructor then runs against a torn-down subsystem and crashes the exit.
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

}  // namespace dream::render::vk
