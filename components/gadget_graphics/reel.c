#include "drawing.h"

#include <math.h>
#include <string.h>

#define TEXEL_UNIT 65536

static inline uint16_t blend565(uint16_t from, uint16_t to, unsigned amount)
{
    const uint32_t mask = 0x07e0f81fu;
    const uint32_t a = (from | (uint32_t)from << 16) & mask;
    const uint32_t b = (to | (uint32_t)to << 16) & mask;
    const uint32_t mixed = ((a * (16 - amount) + b * amount + 0x01004008u) >> 4) & mask;
    return (uint16_t)(mixed | (mixed >> 16));
}

static int clamp_texel(int sample, int last)
{
    return sample < 0 ? 0 : sample > last ? last : sample;
}

static void draw_card(pax_buf_t *surface, const pax_buf_t *preview, float scale, float top)
{
    const int width = surface->width, height = surface->height;
    const float left = width * (1.0f - scale) * 0.5f;
    const int first_x = (int)ceilf(left - 0.5f);
    const int end_x = (int)ceilf(width - left - 0.5f);
    int first_y = (int)ceilf(top - 0.5f);
    int end_y = (int)ceilf(top + height * scale - 0.5f);
    if (first_y < 0) {
        first_y = 0;
    }
    if (end_y > height) {
        end_y = height;
    }
    if (first_y >= end_y) {
        return;
    }

    // The cached texture and output share the same aspect ratio. Sample both axes at the same rate.
    const int step = (int)(TEXEL_UNIT / scale + 0.5f);
    const int first_u = (int)((first_x + 0.5f - left) * step - TEXEL_UNIT * 0.5f);
    int tex_v = (int)((first_y + 0.5f - top) * step - TEXEL_UNIT * 0.5f);
    const int last_u = (width - 1) * TEXEL_UNIT;
    const int last_v = (height - 1) * TEXEL_UNIT;
    const uint16_t *pixels = preview->buf_16bpp;
    for (int y = first_y; y < end_y; ++y, tex_v += step) {
        uint16_t *out = surface->buf_16bpp + y * width;
        const int v = clamp_texel(tex_v, last_v);
        const uint16_t *upper = pixels + (v >> 16) * width;
        const uint16_t *lower = v < last_v ? upper + width : upper;
        const unsigned fy = ((v & 0xffff) + 2048) >> 12;
        int tex_u = first_u;
        for (int x = first_x; x < end_x; ++x, tex_u += step) {
            const int u = clamp_texel(tex_u, last_u);
            const int tx = u >> 16;
            const int right = tx + (u < last_u);
            const unsigned fx = ((u & 0xffff) + 2048) >> 12;
            const uint16_t a = blend565(upper[tx], upper[right], fx);
            const uint16_t b = blend565(lower[tx], lower[right], fx);
            out[x] = blend565(a, b, fy);
        }
    }
}

void gadget_draw_reel(pax_buf_t *surface, const pax_buf_t previews[GADGET_FACE_COUNT], gadget_dice_frame_t frame)
{
    const int height = surface->height;
    const size_t count = (size_t)surface->width * height;
    if (frame.scale == 1.0f) {
        memcpy(surface->buf_16bpp, previews[(unsigned)frame.position].buf_16bpp, count * sizeof(uint16_t));
        return;
    }

    const uint16_t background = gadget_rgb565(GADGET_BACKGROUND);
    for (size_t i = 0; i < count; ++i) {
        surface->buf_16bpp[i] = background;
    }
    const float pitch = height * (frame.scale + 0.035f);
    const float center_top = height * (1.0f - frame.scale) * 0.5f;
    for (unsigned face = 0; face < GADGET_FACE_COUNT; ++face) {
        float offset = frame.position - (float)face;
        const float half_turn = GADGET_FACE_COUNT / 2.0f;
        if (offset > half_turn) {
            offset -= GADGET_FACE_COUNT;
        } else if (offset < -half_turn) {
            offset += GADGET_FACE_COUNT;
        }
        draw_card(surface, &previews[face], frame.scale, center_top + offset * pitch);
    }
}
