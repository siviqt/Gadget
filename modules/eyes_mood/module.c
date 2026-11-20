// SPDX-License-Identifier: MIT
// Motion response and expressions adapted from Eyes-plugin, copyright (c) 2026 Eyes-plugin project.
#include "gadget_module.h"

static const struct {
    uint32_t color;
    int lids[2], pupil;
} moods[] = {
    {0xffc5e7ff, {39, 39}, 12},
    {0xfff5d596, {48, 34}, 11},
    {0xffaaf6dd, {49, 49}, 9},
    {0xffaebcf6, {20, 20}, 10},
};

#define BLINK_DURATION_MS 180
static float neutral_x, neutral_y, tilt_x, tilt_y, gaze_x, gaze_y, lid[2];
static uint64_t last_update, last_motion, alert_until, curious_until, blink_at, next_blink;
static bool calibrated, blinking;
static unsigned mood;
static gadget_damage_t eye_region;
static struct {
    int x, y, lid[2];
} pose;

static float clamp(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static float absolute(float value)
{
    return value < 0 ? -value : value;
}

static float soften(float value, float zone)
{
    if (value > zone) {
        return value - zone;
    }
    return value < -zone ? value + zone : 0;
}

static int round_pixel(float value)
{
    return (int)(value + (value < 0 ? -0.5f : 0.5f));
}

static bool init(gadget_module_context_t *module)
{
    lid[0] = 39;
    lid[1] = 39;
    pose.lid[0] = 39;
    pose.lid[1] = 39;
    return GADGET_HOST_HAS(module->host, invalidate_region);
}

static void activate(gadget_module_context_t *module)
{
    const uint64_t now = module->host->uptime_ms();
    last_update = now;
    last_motion = now;
    alert_until = 0;
    curious_until = 0;
    next_blink = now + 3400;
    blinking = false;
    calibrated = false;
}

static void update(gadget_module_context_t *module, uint64_t now)
{
    const uint64_t elapsed = now - last_update;
    last_update = now;
    const float dt = (float)(elapsed > 60 ? 60 : elapsed) / 1000.0f;
    gadget_motion_t sample;
    const bool ready = module->host->motion_read(module, &sample);
    float drive_x = 0, drive_y = 0, motion = 0;

    if (ready && sample.accel_ready) {
        const float ax = sample.accel[0], ay = sample.accel[1], az = sample.accel[2];
        const float gravity = ax * ax + ay * ay + az * az;
        // Reject sustained accelerations while the badge is being shaken.
        if (gravity > 55 && gravity < 150) {
            if (!calibrated) {
                neutral_x = ax;
                neutral_y = ay;
                calibrated = true;
            }
            tilt_x += dt / (0.12f + dt) * (soften(ax - neutral_x, 0.25f) - tilt_x);
            tilt_y += dt / (0.12f + dt) * (soften(ay - neutral_y, 0.25f) - tilt_y);
        }
    } else {
        tilt_x *= 1 - dt / (0.2f + dt);
        tilt_y *= 1 - dt / (0.2f + dt);
    }
    if (ready && sample.gyro_ready) {
        drive_x = soften(-sample.gyro[1], 3);
        drive_y = soften(sample.gyro[0], 3);
        motion = absolute(drive_x) + absolute(drive_y) + absolute(sample.gyro[2]);
    }
    const float tilt_amount = absolute(tilt_x) + absolute(tilt_y);
    if (motion > 12 || tilt_amount > 0.65f) {
        last_motion = now;
    }
    if (motion > 75) {
        alert_until = now + 380;
    }
    if (tilt_amount > 1.1f || motion > 14) {
        curious_until = now + 550;
    }
    const unsigned next_mood = now < alert_until ? 2 : now < curious_until ? 1 : now - last_motion > 25000 ? 3 : 0;
    const bool color_changed = next_mood != mood;
    mood = next_mood;

    const float target_x = clamp(-tilt_x * 4 + drive_x * 0.18f, -30, 30);
    const float target_y = clamp(tilt_y * 3 + drive_y * 0.12f, -17, 17);
    const float response = dt / (0.05f + dt);
    gaze_x += response * (target_x - gaze_x);
    gaze_y += response * (target_y - gaze_y);

    if (!blinking && now >= next_blink) {
        blinking = true;
        blink_at = now;
    }
    float opening = 1;
    if (blinking) {
        const uint64_t age = now - blink_at;
        if (age >= BLINK_DURATION_MS) {
            blinking = false;
            next_blink = now + (mood == 3 ? 2200 : 3700);
        } else {
            opening = age < 65 ? 1 - (float)(unsigned)age / 65.0f :
                                 (float)((unsigned)age - 65) / (BLINK_DURATION_MS - 65);
        }
    }

    const int x = round_pixel(gaze_x), y = round_pixel(gaze_y);
    bool moved = x != pose.x || y != pose.y;
    pose.x = x;
    pose.y = y;
    for (unsigned eye = 0; eye < 2; ++eye) {
        lid[eye] += dt / (0.085f + dt) * (moods[mood].lids[eye] - lid[eye]);
        const int height = round_pixel(3 + (lid[eye] - 3) * opening);
        moved |= height != pose.lid[eye];
        pose.lid[eye] = height;
    }
    if (color_changed || moved) {
        module->host->invalidate_region(module, eye_region);
    }
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    (void)module;
    const int width = canvas->width, height = canvas->height;
    int rx = width * 3 / 16;
    if (rx > height / 2 - 10) {
        rx = height / 2 - 10;
    }
    if (rx > 128) {
        rx = 128;
    }
    if (rx < 8) {
        return;
    }
    const int first = width * 5 / 18;
    const int cy = height / 2;
    const int lid_max = 49 * rx / 55 + 2;
    eye_region = (gadget_damage_t){first - rx - 2, cy - lid_max, width - 2 * (first - rx - 2), 2 * lid_max + 1};
    const uint32_t color = moods[mood].color;
    canvas->rect(canvas->handle, 0, 0, width, height, 0xff121d2b);
    for (unsigned eye = 0; eye < 2; ++eye) {
        const int cx = eye == 0 ? first : width - first;
        const int ry = 1 + pose.lid[eye] * rx / 55;
        canvas->ellipse(canvas->handle, cx, cy, rx, ry, color);
        if (pose.lid[eye] <= 8) {
            continue;
        }
        const int pupil = 1 + moods[mood].pupil * rx / 55;
        const int radius = pupil < ry - 2 ? pupil : ry - 2;
        if (radius < 1) {
            continue;
        }
        const int px = cx + pose.x * (rx - radius - 3) / 40;
        const int py = cy + pose.y * (ry - radius - 2) / 30;
        canvas->ellipse(canvas->handle, px, py, radius, radius, 0xff172236);
        if (radius >= 8) {
            const int highlight = radius / 4;
            canvas->ellipse(canvas->handle, px - highlight, py - highlight, highlight, highlight, 0xfff4f7fb);
        }
    }
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .init = init,
    .activate = activate,
    .update = update,
    .render = render,
};

const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
