// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "player_menu_labels.hpp"

#include <cstdint>
#include <vector>

namespace PlayerMenu {
constexpr unsigned width = 800;
constexpr unsigned height = 480;

inline void draw(std::vector<uint32_t>& pixels, unsigned selected) {
    constexpr uint32_t bg = 0xff0c131d;
    constexpr uint32_t card = 0xff1c2837;
    constexpr uint32_t highlight = 0xff71e0d2;
    constexpr uint32_t ink = 0xffedf4fb;
    constexpr uint32_t hint = 0xffaabecd;

    pixels.assign(width * height, bg);
    const auto centered = [&](const MenuLabel& label, unsigned x, unsigned y, uint32_t color) {
        const unsigned left = x - label.width / 2;
        for (unsigned i = 0, offset = 0; i < label.size; i += 2) {
            const unsigned count = label.mask[i];
            const unsigned alpha = label.mask[i + 1];
            if (alpha != 0) {
                for (unsigned j = offset; j < offset + count; ++j) {
                    auto& pixel = pixels[(y + j / label.width) * width + left + j % label.width];
                    uint32_t blended = 0xff000000;
                    for (unsigned shift = 0; shift < 24; shift += 8) {
                        const unsigned from = (pixel >> shift) & 255;
                        const unsigned to = (color >> shift) & 255;
                        blended |= ((from * (255 - alpha) + to * alpha + 127) / 255) << shift;
                    }
                    pixel = blended;
                }
            }
            offset += count;
        }
    };

    centered(title_label, width / 2, 102, ink);
    for (unsigned i = 0; i < 3; ++i) {
        constexpr int card_width = 176;
        constexpr int card_height = 104;
        constexpr int radius = 14;
        const int left = 100 + static_cast<int>(i) * 212;
        const bool active = i + 2 == selected;
        for (int y = 0; y < card_height; ++y) {
            for (int x = 0; x < card_width; ++x) {
                const int dx = x < radius ? radius - 1 - x : x >= card_width - radius
                                                          ? x - (card_width - radius) : 0;
                const int dy = y < radius ? radius - 1 - y : y >= card_height - radius
                                                          ? y - (card_height - radius) : 0;
                if (dx * dx + dy * dy < radius * radius)
                    pixels[(192 + y) * width + left + x] = active ? highlight : card;
            }
        }
        centered(player_labels[i], left + card_width / 2, 224, active ? bg : ink);
    }
    centered(move_label, width / 2, 345, hint);
    centered(play_label, width / 2, 378, hint);
}
}
