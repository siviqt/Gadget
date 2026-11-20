#pragma once

#include "gadget_dice.h"
#include "gadget_module.h"

typedef struct {
    bool assigned;  // false follows the manifest. An assigned empty ID leaves the slot vacant.
    char id[GADGET_MODULE_ID_SIZE];
} gadget_face_setting_t;

typedef struct {
    gadget_face_setting_t faces[GADGET_FACE_COUNT];
    uint8_t shortcuts[GADGET_FACE_COUNT];  // Zero-based reel slot for each F-key.
} gadget_layout_t;

void gadget_layout_defaults(gadget_layout_t *layout);
void gadget_layout_load(gadget_layout_t *layout);
bool gadget_layout_save(const gadget_layout_t *layout);
unsigned gadget_layout_key(const gadget_layout_t *layout, unsigned slot);
