// SPDX-License-Identifier: MIT
#pragma once

#include "gadget_module.h"

#define GADGET_KEYCAP_WIDTH 30
#define GADGET_KEYCAP_HEIGHT 22

static inline void gadget_draw_function_key(const gadget_canvas_t *canvas, int x, int y, gadget_key_kind_t key,
                                            uint32_t fill, uint32_t ink)
{
    if (key < GADGET_KEY_F1 || key > GADGET_KEY_F6) {
        return;
    }
    static const char labels[][3] = {"F1", "F2", "F3", "F4", "F5", "F6"};
    canvas->rect(canvas->handle, x + 2, y, GADGET_KEYCAP_WIDTH - 4, GADGET_KEYCAP_HEIGHT, fill);
    canvas->rect(canvas->handle, x, y + 2, GADGET_KEYCAP_WIDTH, GADGET_KEYCAP_HEIGHT - 4, fill);
    canvas->text(canvas->handle, x + 7, y + 3, 14, ink, labels[key - GADGET_KEY_F1]);
}
