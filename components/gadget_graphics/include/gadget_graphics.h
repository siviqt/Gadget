#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gadget_dice.h"
#include "gadget_module.h"

#define GADGET_DISPLAY_BUFFERS 2

typedef void (*gadget_workspace_draw_t)(unsigned face, const gadget_canvas_t *canvas);

typedef enum {
    GADGET_GUIDE_LOADING,
    GADGET_GUIDE_READY,
    GADGET_GUIDE_HELP,
} gadget_guide_t;

#define GADGET_GUIDE_LAYOUT_KEY GADGET_KEY_F1
#define GADGET_LAYOUT_MODE_KEY GADGET_KEY_F1
#define GADGET_LAYOUT_BACK_KEY GADGET_KEY_F2
#define GADGET_LAYOUT_DEFAULTS_KEY GADGET_KEY_F3
#define GADGET_LAYOUT_SAVE_KEY GADGET_KEY_F4

typedef struct {
    const char *names[GADGET_FACE_COUNT];
    unsigned function_keys[GADGET_FACE_COUNT];
    unsigned selected;
    bool edit_shortcuts;
    const char *notice;
} gadget_layout_view_t;

esp_err_t gadget_graphics_init(void);
void gadget_graphics_shutdown(void);
bool gadget_graphics_blanked(void);
esp_err_t gadget_graphics_set_blanked(bool blanked);
const char *gadget_workspace_name(unsigned face);
unsigned gadget_workspace_function_key(unsigned face);
void gadget_graphics_prepare_workspace(unsigned face, unsigned function_key, const char *title,
                                       gadget_workspace_draw_t preview);
esp_err_t gadget_graphics_workspace(unsigned face, bool received_input, gadget_workspace_draw_t draw,
                                    gadget_damage_t damage);
esp_err_t gadget_graphics_transition(gadget_dice_frame_t frame);
esp_err_t gadget_graphics_guide(gadget_guide_t guide);
esp_err_t gadget_graphics_layout(const gadget_layout_view_t *view);
esp_err_t gadget_graphics_error(const char *message);
void gadget_graphics_log_transition(void);
