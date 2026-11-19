// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "libretro_vulkan.h"

#include <array>
#include <memory>

class VulkanCompositor {
public:
    VulkanCompositor();
    ~VulkanCompositor();
    void initialize(const retro_hw_render_interface_vulkan& frontend, unsigned side, unsigned height,
                    unsigned players);
    void reset();
    void clear_sources();
    bool ready() const;
    void capture(unsigned player, const retro_vulkan_image* image, uint32_t semaphore_count,
                 uint32_t queue_family);
    void set_size(unsigned player, unsigned width, unsigned height);
    bool compose();
    bool draw_menu(const uint32_t* pixels, unsigned selection);

private:
    struct State;
    std::unique_ptr<State> state;
};
