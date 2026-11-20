#pragma once

#include "gadget_dice.h"
#include "gadget_graphics.h"
#include "pax_gfx.h"

#define GADGET_BACKGROUND 0xff111923u
#define GADGET_TEXT 0xfff4f7fbu
#define GADGET_MUTED 0xffaebfd3u

static inline uint16_t gadget_rgb565(uint32_t color)
{
    return (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f));
}

void gadget_text(pax_buf_t *surface, uint32_t color, unsigned pixels, int x, int y, const char *text);
bool gadget_text_bounds(unsigned pixels, const char *text, gadget_text_bounds_t *bounds);
void gadget_draw_set_workspace(unsigned face, unsigned function_key, const char *title);
void gadget_draw_layout(pax_buf_t *surface, const gadget_layout_view_t *view);
void gadget_draw_workspace(pax_buf_t *surface, unsigned face, bool received_input, gadget_workspace_draw_t draw);
void gadget_draw_workspace_damage(pax_buf_t *surface, unsigned face, gadget_workspace_draw_t draw,
                                  gadget_damage_t damage, int *first_row, int *end_row);
void gadget_draw_preview(pax_buf_t *surface, unsigned face);
void gadget_draw_reel(pax_buf_t *surface, const pax_buf_t previews[GADGET_FACE_COUNT], gadget_dice_frame_t frame);
void gadget_draw_guide(pax_buf_t *surface, gadget_guide_t guide);
