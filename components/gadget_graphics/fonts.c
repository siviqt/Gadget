#include "drawing.h"

#include <string.h>

#include "pax_text.h"

extern const pax_font_t ibm_plex_sans_regular_14;
extern const pax_font_t ibm_plex_sans_regular_18;
extern const pax_font_t ibm_plex_sans_medium_20;
extern const pax_font_t ibm_plex_sans_semibold_24;
extern const pax_font_t ibm_plex_mono_semibold_56;

static const pax_font_t *font_for(unsigned pixels, const char *text)
{
    bool ascii = true, time_digits = pixels >= 50 && text[0] != '\0';
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        ascii &= *p < 128;
        time_digits &= (*p >= '0' && *p <= '9') || *p == ':' || *p == '.';
    }

    if (time_digits) {
        return &ibm_plex_mono_semibold_56;
    }
    if (pixels <= 13) {
        return &ibm_plex_sans_regular_14;
    }
    if (ascii && pixels >= 23) {
        return &ibm_plex_sans_semibold_24;
    }
    if (ascii && pixels >= 20) {
        return &ibm_plex_sans_medium_20;
    }
    return &ibm_plex_sans_regular_18;
}

void gadget_text(pax_buf_t *surface, uint32_t color, unsigned pixels, int x, int y, const char *text)
{
    const pax_font_t *font = font_for(pixels, text);
    pax_draw_text(surface, color, font, font->default_size, x, y, text);
}

bool gadget_text_bounds(unsigned pixels, const char *text, gadget_text_bounds_t *bounds)
{
    if (text == NULL || bounds == NULL || pixels == 0 || pixels > 128) {
        return false;
    }
    const pax_font_t *font = font_for(pixels, text);
    size_t remaining = strlen(text);
    int advance = 0, left = 0, top = 0, right = 0, bottom = 0;
    bool ink = false;
    while (remaining > 0) {
        uint32_t ch;
        const size_t step = pax_utf8_getch_l(text, remaining, &ch);
        if (step == 0 || ch == '\n' || ch == '\r' || ch == '\t') {
            return false;
        }
        const pax_bmpv_t *glyph = NULL;
        for (size_t i = 0; i < font->n_ranges; ++i) {
            const pax_font_range_t *range = &font->ranges[i];
            if (range->type == PAX_FONT_TYPE_BITMAP_VAR && ch >= range->start && ch <= range->end) {
                glyph = &range->bitmap_var.dims[ch - range->start];
                break;
            }
        }
        if (glyph == NULL) {
            return false;
        }
        if (ch != ' ' && glyph->draw_w != 0 && glyph->draw_h != 0) {
            const int x = advance + glyph->draw_x;
            const int y = glyph->draw_y;
            left = !ink || x < left ? x : left;
            top = !ink || y < top ? y : top;
            right = !ink || x + glyph->draw_w > right ? x + glyph->draw_w : right;
            bottom = !ink || y + glyph->draw_h > bottom ? y + glyph->draw_h : bottom;
            ink = true;
        }
        advance += glyph->measured_width;
        text += step;
        remaining -= step;
    }
    *bounds = (gadget_text_bounds_t){left, top, right - left, bottom - top, advance};
    return ink;
}
