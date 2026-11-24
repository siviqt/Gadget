// SPDX-License-Identifier: MIT
#pragma once

#include "gadget_module.h"

#define GADGET_KEYCAP_WIDTH 30
#define GADGET_KEYCAP_HEIGHT 22
#define GADGET_KEY_SYMBOL_SIZE 16
#define GADGET_KEYCAP_OPAQUE 0xff000000u
#define GADGET_KEYCAP_DIMMED 0x99000000u

// The key selects the legend's RGB color. The ink argument supplies its opacity.
static inline void gadget_draw_function_symbol(const gadget_canvas_t *canvas, int x, int y, gadget_key_kind_t key,
                                               uint32_t ink)
{
    if (key < GADGET_KEY_F1 || key > GADGET_KEY_F6 || (ink >> 24) == 0) {
        return;
    }
    // One bit per pixel, starting with the leftmost pixel in the low bit.
    static const struct {
        uint32_t rgb;
        uint16_t rows[GADGET_KEY_SYMBOL_SIZE];
    } symbols[] = {
        {0x00fb6969, {  // Cross
            0x0000, 0x6006, 0x700e, 0x381c, 0x1c38, 0x0e70, 0x07e0, 0x03c0,
            0x03c0, 0x07e0, 0x0e70, 0x1c38, 0x381c, 0x700e, 0x6006, 0x0000,
        }},
        {0x00ffa252, {  // Triangle
            0x0000, 0x0180, 0x0180, 0x03c0, 0x03c0, 0x0660, 0x0660, 0x0c30,
            0x0c30, 0x1818, 0x1818, 0x300c, 0x300c, 0x7ffe, 0xffff, 0x0000,
        }},
        {0x00f0d446, {  // Square
            0x0000, 0x7ffe, 0x7ffe, 0x6006, 0x6006, 0x6006, 0x6006, 0x6006,
            0x6006, 0x6006, 0x6006, 0x6006, 0x6006, 0x7ffe, 0x7ffe, 0x0000,
        }},
        {0x0096d851, {  // Circle
            0x03c0, 0x0ff0, 0x1ff8, 0x381c, 0x700e, 0x6006, 0xe007, 0xe007,
            0xe007, 0xe007, 0x6006, 0x700e, 0x381c, 0x1ff8, 0x0ff0, 0x03c0,
        }},
        {0x004fd6d5, {  // Clover
            0x0000, 0x03c0, 0x07e0, 0x0c30, 0x0c30, 0x0c30, 0x0c30, 0x781e,
            0xf00f, 0xc003, 0xc003, 0xc003, 0xc183, 0x7ffe, 0x3e7c, 0x0000,
        }},
        {0x00d977e6, {  // Diamond
            0x0180, 0x03c0, 0x07e0, 0x0e70, 0x1c38, 0x381c, 0x700e, 0xe007,
            0xe007, 0x700e, 0x381c, 0x1c38, 0x0e70, 0x07e0, 0x03c0, 0x0180,
        }},
    };

    const unsigned index = key - GADGET_KEY_F1;
    const uint32_t color = symbols[index].rgb | (ink & 0xff000000u);
    for (int row = 0; row < GADGET_KEY_SYMBOL_SIZE; ++row) {
        unsigned pixels = symbols[index].rows[row];
        int col = 0;
        while (pixels != 0) {
            if ((pixels & 1u) == 0) {
                ++col;
                pixels >>= 1;
                continue;
            }
            const int start = col;
            do {
                ++col;
                pixels >>= 1;
            } while ((pixels & 1u) != 0);
            canvas->rect(canvas->handle, x + start, y + row, col - start, 1, color);
        }
    }
}

static inline void gadget_draw_function_key(const gadget_canvas_t *canvas, int x, int y, gadget_key_kind_t key,
                                            uint32_t fill, uint32_t ink)
{
    if (key < GADGET_KEY_F1 || key > GADGET_KEY_F6) {
        return;
    }
    canvas->rect(canvas->handle, x + 2, y, GADGET_KEYCAP_WIDTH - 4, GADGET_KEYCAP_HEIGHT, fill);
    canvas->rect(canvas->handle, x, y + 2, GADGET_KEYCAP_WIDTH, GADGET_KEYCAP_HEIGHT - 4, fill);
    gadget_draw_function_symbol(canvas, x + (GADGET_KEYCAP_WIDTH - GADGET_KEY_SYMBOL_SIZE) / 2,
                                y + (GADGET_KEYCAP_HEIGHT - GADGET_KEY_SYMBOL_SIZE) / 2, key, ink);
}
