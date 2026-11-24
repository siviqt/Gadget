#include "drawing.h"
#include "gadget_keycap.h"

#include <stdio.h>

static const uint32_t accent = 0xff6ed9ed;
static const uint32_t rule = 0xff324657;
static const uint32_t key_fill = 0xff263b4a;

static void keycap(pax_buf_t *surface, int x, int y, gadget_key_kind_t key)
{
    const gadget_canvas_t canvas = {.handle = surface, .rect = gadget_pax_rect};
    gadget_draw_function_key(&canvas, x, y, key, key_fill, GADGET_KEYCAP_OPAQUE);
}

static void action(pax_buf_t *surface, int x, int y, gadget_key_kind_t key, const char *text)
{
    keycap(surface, x, y, key);
    gadget_text(surface, GADGET_TEXT, 16, x + GADGET_KEYCAP_WIDTH + 8, y + 2, text);
}

void gadget_draw_guide(pax_buf_t *surface, gadget_guide_t guide)
{
    static const struct {
        const char *keys;
        const char *action;
    } controls[] = {
        {"Hyper + H", "Open this guide"},
        {"Esc twice within 850 ms", "Return to the Tanmatsu launcher"},
        {"Tap Power", "Turn display and keyboard backlights off or on"},
    };
    const int width = pax_buf_get_width(surface), height = pax_buf_get_height(surface);
    const int margin = 32;
    const int footer = height - 66;
    const bool wide = width >= 680;
    const int map_x = wide ? width * 5 / 8 : margin;

    pax_background(surface, GADGET_BACKGROUND);
    gadget_text(surface, GADGET_TEXT, 24, margin, 27, "Gadget workspace guide");
    pax_draw_rect(surface, rule, margin, 75, width - 2 * margin, 1);
    gadget_text(surface, accent, 18, margin, 98, "Shortcuts");

    const char *shortcut = "Hold or tap Hyper, then press";
    int shortcut_y = 132;
    gadget_text(surface, GADGET_TEXT, 20, margin, shortcut_y, shortcut);
    gadget_text_bounds_t bounds = {0};
    const int key_pitch = GADGET_KEYCAP_WIDTH + 4;
    const int strip_width = key_pitch * GADGET_FACE_COUNT - 4;
    const int shortcut_right = wide ? map_x - 24 : width - margin;
    int strip_x = margin;
    if (gadget_text_bounds(20, shortcut, &bounds) &&
        margin + bounds.advance + 9 + strip_width <= shortcut_right) {
        strip_x += bounds.advance + 9;
    } else {
        shortcut_y += 26;
    }
    for (gadget_key_kind_t key = GADGET_KEY_F1; key <= GADGET_KEY_F6; ++key) {
        keycap(surface, strip_x, shortcut_y, key);
        strip_x += key_pitch;
    }
    gadget_text(surface, GADGET_MUTED, 18, margin, shortcut_y + 26,
                "Switch workspaces. After a tap, press within 2 seconds.");

    int y = shortcut_y + 61;
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        gadget_text(surface, GADGET_TEXT, 20, margin, y, controls[i].keys);
        gadget_text(surface, GADGET_MUTED, 18, margin, y + 26, controls[i].action);
        y += 61;
    }

    if (guide != GADGET_GUIDE_LOADING) {
        const int map_y = wide ? 98 : y;
        gadget_text(surface, accent, 18, map_x, map_y, "Workspaces");
        const int label_x = map_x + 57;
        for (unsigned face = 0; face < GADGET_FACE_COUNT; ++face) {
            const int y = map_y + 35 + (int)face * 42;
            keycap(surface, map_x, y, GADGET_KEY_F1 + gadget_workspace_function_key(face) - 1);
            pax_clip(surface, label_x, y, width - margin - label_x, 29);
            gadget_text(surface, GADGET_TEXT, 18, label_x, y + 2, gadget_workspace_name(face));
            pax_noclip(surface);
        }
    }

    const char *status = guide == GADGET_GUIDE_LOADING ? "Loading modules from SD..."
                       : guide == GADGET_GUIDE_READY ? "Press Space to continue"
                       : "Enter to return to your workspace";
    pax_draw_rect(surface, rule, margin, footer, width - 2 * margin, 1);
    gadget_text(surface, accent, 18, margin, footer + 17, status);
    if (guide == GADGET_GUIDE_HELP) {
        action(surface, wide ? map_x : margin + 280, footer + 16, GADGET_GUIDE_LAYOUT_KEY, "Layout");
    }
    pax_join();
}

void gadget_draw_layout(pax_buf_t *surface, const gadget_layout_view_t *view)
{
    const int width = pax_buf_get_width(surface), height = pax_buf_get_height(surface);
    const int margin = 32, face_x = margin + 58, key_x = width - margin - 104;
    const int footer = height - 113;
    char label[16];

    pax_background(surface, GADGET_BACKGROUND);
    gadget_text(surface, GADGET_TEXT, 24, margin, 27, "Workspace layout");
    gadget_text(surface, GADGET_MUTED, 18, margin, 83, "Slot");
    gadget_text(surface, view->edit_shortcuts ? GADGET_MUTED : accent, 18, face_x, 83, "Face order");
    gadget_text(surface, view->edit_shortcuts ? accent : GADGET_MUTED, 18, key_x, 83, "Shortcut");
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        const int y = 115 + (int)i * 39;
        if (i == view->selected) {
            pax_draw_rect(surface, 0xff263b4a, margin - 8, y, width - 2 * margin + 16, 33);
            pax_draw_rect(surface, accent, view->edit_shortcuts ? key_x - 10 : face_x - 10, y + 5, 2, 23);
        }
        snprintf(label, sizeof(label), "%u", i + 1);
        gadget_text(surface, GADGET_MUTED, 18, margin, y + 5, label);
        pax_clip(surface, face_x, y, key_x - face_x - 12, 33);
        gadget_text(surface, GADGET_TEXT, 18, face_x, y + 5, view->names[i]);
        pax_noclip(surface);
        gadget_text(surface, GADGET_TEXT, 16, key_x, y + 6, "Hyper +");
        keycap(surface, key_x + 67, y + 5, GADGET_KEY_F1 + view->function_keys[i] - 1);
    }
    pax_draw_rect(surface, rule, margin, footer, width - 2 * margin, 1);
    gadget_text(surface, GADGET_MUTED, 16, margin, footer + 9, "Up/Down Select   Left/Right Swap");
    action(surface, margin, footer + 31, GADGET_LAYOUT_MODE_KEY, view->edit_shortcuts ? "Edit face order" : "Edit shortcuts");
    action(surface, width / 2, footer + 31, GADGET_LAYOUT_DEFAULTS_KEY, "Defaults");
    action(surface, margin, footer + 56, GADGET_LAYOUT_BACK_KEY, "Guide");
    action(surface, width / 2, footer + 56, GADGET_LAYOUT_SAVE_KEY, "Save");
    if (view->notice != NULL) {
        gadget_text(surface, accent, 16, margin, footer + 78, view->notice);
    }
    pax_join();
}
