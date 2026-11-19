// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "libretro.h"
#include <array>
#include <memory>

class GpuCompositor {
public:
    GpuCompositor();
    ~GpuCompositor();

    void initialize(retro_hw_get_proc_address_t resolver, unsigned width, unsigned height,
                    unsigned players);
    void reset();
    void activate(unsigned player);
    void activate_frontend();
    void start_player_frame();
    void finish_player_frame(unsigned player);
    void compose(retro_hw_get_current_framebuffer_t frontend_framebuffer);
    void draw_menu(retro_hw_get_current_framebuffer_t frontend_framebuffer,
                   const uint32_t* pixels, unsigned selection);
    uintptr_t player_framebuffer(unsigned player) const;
    bool ready() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

void* create_shader_worker_context();
bool make_shader_worker_current(void* context);
void clear_shader_worker_current(void* context);
void destroy_shader_worker_context(void* context);
