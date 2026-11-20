#include "drawing.h"

#include <stdio.h>

static const uint32_t accent = 0xff6ed9ed;
static const uint32_t rule = 0xff324657;

void gadget_draw_guide(pax_buf_t *surface, gadget_guide_t guide)
{
    static const struct {
        const char *keys;
        const char *action;
    } controls[] = {
        {"Hold Hyper + F1-F6", "Switch to a workspace"},
        {"Tap Hyper, then F1-F6", "Use within 2 seconds of releasing Hyper"},
        {"Hyper + H", "Open this guide"},
        {"Esc twice within 850 ms", "Return to the Tanmatsu launcher"},
    };
    const int width = pax_buf_get_width(surface), height = pax_buf_get_height(surface);
    const int margin = 32;
    const int footer = height - 66;
    const bool wide = width >= 680;
    const int map_x = wide ? width * 5 / 8 : margin;
    const int map_y = wide ? 98 : 376;

    pax_background(surface, GADGET_BACKGROUND);
    gadget_text(surface, GADGET_TEXT, 24, margin, 27, "Gadget workspace guide");
    pax_draw_rect(surface, rule, margin, 75, width - 2 * margin, 1);
    gadget_text(surface, accent, 18, margin, 98, "Shortcuts");
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        const int y = 132 + (int)i * 61;
        gadget_text(surface, GADGET_TEXT, 20, margin, y, controls[i].keys);
        gadget_text(surface, GADGET_MUTED, 18, margin, y + 26, controls[i].action);
    }

    if (guide != GADGET_GUIDE_LOADING) {
        gadget_text(surface, accent, 18, map_x, map_y, "Workspaces");
        const int label_x = map_x + 57;
        for (unsigned face = 0; face < GADGET_FACE_COUNT; ++face) {
            const int y = map_y + 35 + (int)face * 42;
            char key[8];
            snprintf(key, sizeof(key), "F%u", gadget_workspace_function_key(face));
            pax_draw_rect(surface, 0xff263b4a, map_x, y, 43, 28);
            gadget_text(surface, accent, 18, map_x + 11, y + 2, key);
            pax_clip(surface, label_x, y, width - margin - label_x, 29);
            gadget_text(surface, GADGET_TEXT, 18, label_x, y + 2, gadget_workspace_name(face));
            pax_noclip(surface);
        }
    }

    const char *status = guide == GADGET_GUIDE_LOADING ? "Loading modules from SD..."
                       : guide == GADGET_GUIDE_READY ? "Enter to continue"
                       : "Enter to return to your workspace";
    pax_draw_rect(surface, rule, margin, footer, width - 2 * margin, 1);
    gadget_text(surface, accent, 18, margin, footer + 17, status);
    if (guide != GADGET_GUIDE_LOADING) {
        gadget_text(surface, accent, 18, wide ? map_x : margin + 280, footer + 17, "C  Layout");
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
        snprintf(label, sizeof(label), "Hyper + F%u", view->function_keys[i]);
        gadget_text(surface, GADGET_TEXT, 18, key_x, y + 5, label);
    }
    pax_draw_rect(surface, rule, margin, footer, width - 2 * margin, 1);
    gadget_text(surface, GADGET_MUTED, 16, margin, footer + 9, "Up/Down Select   Left/Right Swap");
    gadget_text(surface, GADGET_MUTED, 16, margin, footer + 32, view->edit_shortcuts ?
                "S  Edit face order   D  Defaults" : "S  Edit shortcuts   D  Defaults");
    gadget_text(surface, GADGET_TEXT, 16, margin, footer + 55, "Enter  Save   Backspace  Guide");
    if (view->notice != NULL) {
        gadget_text(surface, accent, 16, margin, footer + 78, view->notice);
    }
    pax_join();
}
