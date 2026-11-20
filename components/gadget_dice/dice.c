#include "gadget_dice.h"

#include <math.h>

#define REEL_SCALE 0.64f
#define ZOOM_US INT64_C(100000)
#define ROLL_BASE_US 110000.0f
#define ROLL_FACE_US 65000.0f

static float ease(int64_t elapsed, int64_t duration)
{
    const float t = (float)elapsed / duration;
    return t * t * (3.0f - 2.0f * t);
}

static float wrap(float position)
{
    if (position < 0.0f) {
        position += GADGET_FACE_COUNT;
    }
    if (position >= GADGET_FACE_COUNT) {
        position -= GADGET_FACE_COUNT;
    }
    return position;
}

void gadget_dice_init(gadget_dice_t *dice, unsigned face)
{
    *dice = (gadget_dice_t){.active = face, .target = face, .frame = {face, 1.0f}};
}

bool gadget_dice_select(gadget_dice_t *dice, unsigned face, int64_t now_us)
{
    if (face >= GADGET_FACE_COUNT || face == dice->target) {
        return false;
    }
    // A new selection starts on the last frame sent to the display.
    dice->start_frame = dice->frame;
    const float half_turn = GADGET_FACE_COUNT / 2.0f;
    float steps = (float)face - dice->frame.position;
    if (steps > half_turn) {
        steps -= GADGET_FACE_COUNT;
    } else if (steps <= -half_turn) {
        steps += GADGET_FACE_COUNT;
    }

    dice->steps = steps;
    const float zoom_remaining = (dice->frame.scale - REEL_SCALE) / (1.0f - REEL_SCALE);
    dice->zoom_out_us = steps == 0.0f ? 0 : (int64_t)(ZOOM_US * zoom_remaining);
    dice->roll_us = steps == 0.0f ? 0 : (int64_t)(ROLL_BASE_US + ROLL_FACE_US * fabsf(steps));
    dice->target = face;
    dice->started_us = now_us;
    dice->moving = true;
    return true;
}

bool gadget_dice_update(gadget_dice_t *dice, int64_t now_us)
{
    if (!dice->moving) {
        return false;
    }

    const int64_t elapsed = now_us - dice->started_us;
    const int64_t roll_end = dice->zoom_out_us + dice->roll_us;
    if (elapsed >= roll_end + ZOOM_US || (dice->roll_us == 0 && dice->start_frame.scale == 1.0f)) {
        dice->frame = (gadget_dice_frame_t){dice->target, 1.0f};
        dice->active = dice->target;
        dice->moving = false;
        return true;
    }
    if (elapsed <= 0) {
        return false;
    }

    if (elapsed < dice->zoom_out_us) {
        const float start_scale = dice->start_frame.scale;
        dice->frame.scale = start_scale + (REEL_SCALE - start_scale) * ease(elapsed, dice->zoom_out_us);
    } else if (elapsed < roll_end) {
        const float progress = ease(elapsed - dice->zoom_out_us, dice->roll_us);
        dice->frame.position = wrap(dice->start_frame.position + dice->steps * progress);
        dice->frame.scale = REEL_SCALE;
    } else {
        const float start_scale = dice->roll_us == 0 ? dice->start_frame.scale : REEL_SCALE;
        dice->frame.position = dice->target;
        dice->frame.scale = start_scale + (1.0f - start_scale) * ease(elapsed - roll_end, ZOOM_US);
    }
    return false;
}
