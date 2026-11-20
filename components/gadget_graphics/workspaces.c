#include "drawing.h"

#include <stdio.h>

static const pax_col_t accents[GADGET_FACE_COUNT] = {
    0xff6ed9ed, 0xffedb664, 0xffe7a8dc, 0xff7ee0ad, 0xffa8bdfd, 0xfff1a096,
};

#define WORKSPACE_COLOR 0xff1c2938u

static const char *titles[GADGET_FACE_COUNT];
static unsigned function_keys[GADGET_FACE_COUNT];

typedef struct {
    int left, top, right, bottom;
} bounds_t;

typedef struct {
    pax_buf_t *surface;
    bounds_t clip;  // Physical scanout coordinates.
} workspace_canvas_t;

const char *gadget_workspace_name(unsigned face)
{
    return titles[face] != NULL ? titles[face] : "Empty";
}

unsigned gadget_workspace_function_key(unsigned face)
{
    return function_keys[face] != 0 ? function_keys[face] : face + 1;
}

void gadget_draw_set_workspace(unsigned face, unsigned function_key, const char *title)
{
    titles[face] = title;
    function_keys[face] = function_key;
}

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }

static bounds_t physical_bounds(const pax_buf_t *surface, int x, int y, int width, int height)
{
    const pax_vec2i start = pax_orient_det_vec2i(surface, (pax_vec2i){x, y});
    const pax_vec2i end = pax_orient_det_vec2i(surface, (pax_vec2i){x + width - 1, y + height - 1});
    return (bounds_t){
        minimum(start.x, end.x), minimum(start.y, end.y), maximum(start.x, end.x) + 1, maximum(start.y, end.y) + 1,
    };
}

static uint16_t panel_color(const pax_buf_t *surface, uint32_t argb)
{
    const uint16_t rgb = gadget_rgb565(argb);
    return surface->reverse_endianness ? __builtin_bswap16(rgb) : rgb;
}

static void fill(pax_buf_t *surface, bounds_t area, uint32_t argb)
{
    const uint16_t color = panel_color(surface, argb);
    uint16_t *pixels = pax_buf_get_pixels_rw(surface);
    for (int y = area.top; y < area.bottom; ++y) {
        uint16_t *row = pixels + y * surface->width;
        for (int x = area.left; x < area.right; ++x) {
            row[x] = color;
        }
    }
}

static void canvas_rect(void *handle, int x, int y, int w, int h, uint32_t argb)
{
    const workspace_canvas_t *canvas = handle;
    if (w <= 0 || h <= 0) {
        return;
    }
    if ((argb >> 24) != 255) {
        pax_draw_rect(canvas->surface, argb, x, y, w, h);
        return;
    }
    const bounds_t raw = physical_bounds(canvas->surface, x, y, w, h);
    const bounds_t clipped = {
        maximum(raw.left, canvas->clip.left), maximum(raw.top, canvas->clip.top),
        minimum(raw.right, canvas->clip.right), minimum(raw.bottom, canvas->clip.bottom),
    };
    pax_join();
    fill(canvas->surface, clipped, argb);
}

static void canvas_ellipse(void *handle, int cx, int cy, int rx, int ry, uint32_t argb)
{
    const workspace_canvas_t *canvas = handle;
    if (rx < 1 || ry < 1 || rx > 128 || ry > 128) {
        return;
    }
    pax_buf_t *surface = canvas->surface;
    const bounds_t raw = physical_bounds(surface, cx - rx, cy - ry, 2 * rx + 1, 2 * ry + 1);
    rx = (raw.right - raw.left) / 2;
    ry = (raw.bottom - raw.top) / 2;
    cx = raw.left + rx;
    cy = raw.top + ry;
    const int xx = rx * rx, yy = ry * ry;
    const int limit = xx * yy;
    const uint16_t color = panel_color(surface, argb);
    const bounds_t clip = canvas->clip;
    pax_join();
    uint16_t *pixels = pax_buf_get_pixels_rw(surface);
    int dx = rx;
    for (int dy = 0; dy <= ry; ++dy) {
        while (dx * dx * yy + dy * dy * xx > limit) {
            --dx;
        }
        const int left = maximum(cx - dx, clip.left);
        const int right = minimum(cx + dx + 1, clip.right);
        const int rows[] = {cy - dy, cy + dy};
        for (unsigned row = 0; row < (dy == 0 ? 1u : 2u); ++row) {
            if (rows[row] >= clip.top && rows[row] < clip.bottom) {
                uint16_t *line = pixels + rows[row] * surface->width;
                for (int x = left; x < right; ++x) {
                    line[x] = color;
                }
            }
        }
    }
}

static void canvas_text(void *handle, int x, int y, unsigned pixels, uint32_t argb, const char *text)
{
    const workspace_canvas_t *canvas = handle;
    if (pixels > 0 && pixels <= 128 && text != NULL) {
        gadget_text(canvas->surface, argb, pixels, x, y, text);
    }
}

static bool canvas_text_bounds(void *handle, unsigned pixels, const char *text, gadget_text_bounds_t *bounds)
{
    (void)handle;
    return gadget_text_bounds(pixels, text, bounds);
}

static bounds_t draw_content(pax_buf_t *surface, unsigned face, gadget_workspace_draw_t draw, gadget_damage_t damage)
{
    const int width = pax_buf_get_width(surface), height = pax_buf_get_height(surface);
    const bool partial = damage.width != 0;
    if (!partial) {
        damage = (gadget_damage_t){0, 0, width, height};
    } else {
        if (damage.x >= width || damage.y >= height) {
            return (bounds_t){0, 0, 0, surface->height};
        }
        damage.width = minimum(damage.width, width - damage.x);
        damage.height = minimum(damage.height, height - damage.y);
    }
    workspace_canvas_t workspace = {
        .surface = surface,
        .clip = physical_bounds(surface, damage.x, damage.y, damage.width, damage.height),
    };
    const gadget_canvas_t canvas = {
        .width = width, .height = height,
        .handle = &workspace, .rect = canvas_rect, .text = canvas_text, .ellipse = canvas_ellipse,
        .damage = partial ? damage : (gadget_damage_t){0},
        .text_bounds = canvas_text_bounds,
    };
    if (partial) {
        fill(surface, workspace.clip, WORKSPACE_COLOR);
    }
    pax_clip(surface, damage.x, damage.y, damage.width, damage.height);
    draw(face, &canvas);
    pax_noclip(surface);
    return workspace.clip;
}

void gadget_draw_workspace_damage(pax_buf_t *surface, unsigned face, gadget_workspace_draw_t draw,
                                  gadget_damage_t damage, int *first_row, int *end_row)
{
    const bounds_t clip = draw_content(surface, face, draw, damage);
    pax_join();
    *first_row = clip.top;
    *end_row = clip.bottom;
}

void gadget_draw_workspace(pax_buf_t *surface, unsigned face, bool received_input, gadget_workspace_draw_t draw)
{
    pax_background(surface, WORKSPACE_COLOR);
    if (draw != NULL) {
        draw_content(surface, face, draw, (gadget_damage_t){0});
    } else {
        char label[8];
        snprintf(label, sizeof(label), "F%u", gadget_workspace_function_key(face));
        const int middle = pax_buf_get_height(surface) / 2;
        gadget_text(surface, accents[face], 20, 28, middle - 65, label);
        gadget_text(surface, GADGET_TEXT, 24, 28, middle - 28, "Empty workspace");
        if (received_input) {
            gadget_text(surface, GADGET_MUTED, 18, 28, middle + 16, "Workspace input received");
        }
    }
    pax_join();
}

void gadget_draw_preview(pax_buf_t *surface, unsigned face)
{
    pax_background(surface, WORKSPACE_COLOR);
    char label[8];
    snprintf(label, sizeof(label), "F%u", gadget_workspace_function_key(face));
    const int middle = surface->height / 2;
    gadget_text(surface, accents[face], 12, 14, middle - 33, label);
    gadget_text(surface, GADGET_TEXT, 14, 14, middle - 14, gadget_workspace_name(face));
    pax_join();
}
