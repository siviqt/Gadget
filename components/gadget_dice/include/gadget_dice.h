#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GADGET_FACE_COUNT 6

typedef struct {
    float position;  // Face units around the reel. Increasing position moves cards down the panel.
    float scale;
} gadget_dice_frame_t;

typedef struct {
    unsigned active;
    unsigned target;
    bool moving;
    int64_t started_us;
    int64_t zoom_out_us;
    int64_t roll_us;
    float steps;
    gadget_dice_frame_t start_frame;
    gadget_dice_frame_t frame;
} gadget_dice_t;

void gadget_dice_init(gadget_dice_t *dice, unsigned face);
bool gadget_dice_select(gadget_dice_t *dice, unsigned face, int64_t now_us);
bool gadget_dice_update(gadget_dice_t *dice, int64_t now_us);
