#pragma once

#include "gadget_layout.h"

#define GADGET_MODULE_LIMIT 16

typedef struct {
    const char *id;
    const char *title;
} gadget_face_assignment_t;

void gadget_modules_start(const char *root, const gadget_layout_t *layout);
bool gadget_modules_shutdown(void);
void gadget_modules_preview_layout(const gadget_layout_t *layout, gadget_face_assignment_t faces[GADGET_FACE_COUNT]);
void gadget_modules_measure_stacks(void);
void gadget_modules_system_sample(void);
const char *gadget_modules_title(unsigned face);
bool gadget_modules_startup_preview(unsigned face);
void gadget_modules_focus(unsigned face);
bool gadget_modules_input(unsigned face, const gadget_key_t *key);
void gadget_modules_event(const gadget_event_t *event);
void gadget_modules_tick(unsigned face, uint64_t now_ms);
uint64_t gadget_modules_update_at(unsigned face);
bool gadget_modules_take_redraw(unsigned face, gadget_damage_t *damage);
void gadget_modules_draw(unsigned face, const gadget_canvas_t *canvas);
