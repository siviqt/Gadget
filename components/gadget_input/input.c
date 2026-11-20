#include "gadget_input.h"

#include <limits.h>
#include <string.h>

#include "bsp/input.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "gadget_dice.h"

static QueueHandle_t keyboard;
static TaskHandle_t input_task;
static void (*send_input)(const gadget_input_event_t *);

#define EXIT_WINDOW_MS 850
#define HYPER_WINDOW_US INT64_C(2000000)
#define VOLUME_FIRST_REPEAT_MS 500
#define VOLUME_REPEAT_MS 100

_Static_assert(BSP_INPUT_NAVIGATION_KEY_F6 - BSP_INPUT_NAVIGATION_KEY_F1 + 1 == GADGET_FACE_COUNT,
               "Gadget faces must match the contiguous F1 through F6 BSP keys");

static uint32_t modifiers(uint32_t bsp)
{
    return ((bsp & BSP_INPUT_MODIFIER_SHIFT) ? GADGET_MOD_SHIFT : 0) |
           ((bsp & BSP_INPUT_MODIFIER_CTRL) ? GADGET_MOD_CTRL : 0) |
           ((bsp & BSP_INPUT_MODIFIER_ALT) ? GADGET_MOD_ALT : 0) |
           ((bsp & BSP_INPUT_MODIFIER_SUPER) ? GADGET_MOD_HYPER : 0);
}

static bool decode(const bsp_input_event_t *event, bool hyper_latched, gadget_input_event_t *out)
{
    if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        const char ascii = event->args_keyboard.ascii;
        if (event->args_keyboard.modifiers & BSP_INPUT_MODIFIER_SUPER) {
            if (ascii == 'h' || ascii == 'H') {
                out->action = GADGET_INPUT_GUIDE;
                return true;
            }
            if (ascii == 'r' || ascii == 'R') {
                out->action = GADGET_INPUT_REFRESH_WEATHER;
                return true;
            }
        }
        const unsigned char first = event->args_keyboard.utf8[0];
        if (first < 32 || first == 127) {
            return false;
        }
        out->action = GADGET_INPUT_KEY;
        out->key.kind = GADGET_KEY_TEXT;
        out->key.modifiers = modifiers(event->args_keyboard.modifiers);
        memcpy(out->key.text, event->args_keyboard.utf8, sizeof(event->args_keyboard.utf8));
        return true;
    }
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) {
        return false;
    }
    const bsp_input_navigation_key_t key = event->args_navigation.key;
    if ((hyper_latched || (event->args_navigation.modifiers & BSP_INPUT_MODIFIER_SUPER)) &&
        key >= BSP_INPUT_NAVIGATION_KEY_F1 && key <= BSP_INPUT_NAVIGATION_KEY_F6) {
        out->action = GADGET_INPUT_SELECT_FACE;
        out->face = key - BSP_INPUT_NAVIGATION_KEY_F1;
        return true;
    }
    out->action = GADGET_INPUT_KEY;
    out->key.modifiers = modifiers(event->args_navigation.modifiers);
    switch (key) {
    case BSP_INPUT_NAVIGATION_KEY_RETURN: out->key.kind = GADGET_KEY_ENTER; break;
    case BSP_INPUT_NAVIGATION_KEY_LEFT: out->key.kind = GADGET_KEY_LEFT; break;
    case BSP_INPUT_NAVIGATION_KEY_RIGHT: out->key.kind = GADGET_KEY_RIGHT; break;
    case BSP_INPUT_NAVIGATION_KEY_UP: out->key.kind = GADGET_KEY_UP; break;
    case BSP_INPUT_NAVIGATION_KEY_DOWN: out->key.kind = GADGET_KEY_DOWN; break;
    case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: out->key.kind = GADGET_KEY_BACKSPACE; break;
    default: return false;
    }
    return true;
}

static void send_value(gadget_input_action_t action, int32_t value)
{
    const gadget_input_event_t input = {.action = action, .value = value};
    send_input(&input);
}

static void intake(void *argument)
{
    (void)argument;
    const bsp_input_navigation_key_t volume_keys[] = {
        BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN, BSP_INPUT_NAVIGATION_KEY_VOLUME_UP,
    };
    const int volume_delta[] = {-1, 1};
    uint64_t repeat_at[2] = {0};
    uint64_t exit_at = 0;
    int64_t hyper_until = 0;
    bool esc_held = false;
    bool headphones = false;
    bool have_route = bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &headphones) == ESP_OK;
    if (have_route) {
        send_value(GADGET_INPUT_AUDIO_JACK, headphones);
    }
    for (;;) {
        bsp_input_event_t event;
        const TickType_t wait = repeat_at[0] || repeat_at[1] || !have_route ? pdMS_TO_TICKS(50) : portMAX_DELAY;
        if (xQueueReceive(keyboard, &event, wait) == pdTRUE) {
            const int64_t now_us = esp_timer_get_time();
            const uint64_t now = now_us / 1000;
            const bool hyper_latched = hyper_until != 0 && now_us <= hyper_until;
            if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state) {
                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_SUPER) {
                    // Tanmatsu reports an unchorded Hyper tap when the physical key is released.
                    hyper_until = now_us + HYPER_WINDOW_US;
                    exit_at = 0;
                } else {
                    hyper_until = 0;
                }
            } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
                hyper_until = 0;
            } else if (event.type == INPUT_EVENT_TYPE_SCANCODE) {
                const bsp_input_scancode_t code = event.args_scancode.scancode;
                // F-key scancodes precede their navigation events. Other key presses cancel the latch.
                if (!(code & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) &&
                    (code < BSP_INPUT_SCANCODE_F1 || code > BSP_INPUT_SCANCODE_F6)) {
                    hyper_until = 0;
                }
            }
            if (event.type == INPUT_EVENT_TYPE_ACTION && event.args_action.type == BSP_INPUT_ACTION_TYPE_AUDIO_JACK) {
                have_route = true;
                send_value(GADGET_INPUT_AUDIO_JACK, event.args_action.state);
            } else if (event.type == INPUT_EVENT_TYPE_NAVIGATION &&
                       event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                if (event.args_navigation.state && !esc_held) {
                    if (exit_at != 0 && now <= exit_at) {
                        send_value(GADGET_INPUT_EXIT, 0);
                        exit_at = 0;
                    } else {
                        exit_at = now + EXIT_WINDOW_MS;
                    }
                }
                esc_held = event.args_navigation.state;
            } else if (event.type == INPUT_EVENT_TYPE_NAVIGATION &&
                       (event.args_navigation.key == volume_keys[0] || event.args_navigation.key == volume_keys[1])) {
                const unsigned index = event.args_navigation.key == volume_keys[1];
                if (!event.args_navigation.state) {
                    repeat_at[index] = 0;
                } else if (repeat_at[index] == 0) {
                    exit_at = 0;
                    repeat_at[index] = now + VOLUME_FIRST_REPEAT_MS;
                    send_value(GADGET_INPUT_VOLUME_STEP, volume_delta[index]);
                }
            } else {
                gadget_input_event_t input = {0};
                if (decode(&event, hyper_latched, &input)) {
                    exit_at = 0;
                    send_input(&input);
                }
            }
        } else if (!have_route &&
                   bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &headphones) == ESP_OK) {
            have_route = true;
            send_value(GADGET_INPUT_AUDIO_JACK, headphones);
        }
        const uint64_t now = esp_timer_get_time() / 1000;
        for (unsigned i = 0; i < 2; ++i) {
            if (repeat_at[i] != 0 && now >= repeat_at[i]) {
                bool held = false;
                // Re-read the key before repeating so a missed release cannot leave volume changing.
                if (bsp_input_read_navigation_key(volume_keys[i], &held) == ESP_OK && held) {
                    send_value(GADGET_INPUT_VOLUME_STEP, volume_delta[i]);
                    repeat_at[i] = now + VOLUME_REPEAT_MS;
                } else {
                    repeat_at[i] = 0;
                }
            }
        }
        if (esc_held && (exit_at == 0 || now > exit_at)) {
            bool held = true;
            if (bsp_input_read_navigation_key(BSP_INPUT_NAVIGATION_KEY_ESC, &held) == ESP_OK) {
                esc_held = held;
            }
        }
    }
}

esp_err_t gadget_input_init(void (*sink)(const gadget_input_event_t *))
{
    const esp_err_t result = bsp_input_get_queue(&keyboard);
    if (result != ESP_OK) {
        return result;
    }
    send_input = sink;
    return xTaskCreate(intake, "gadget_keys", 3072, NULL, 4, &input_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void gadget_input_shutdown(void)
{
    if (input_task != NULL) {
        vTaskDelete(input_task);
        input_task = NULL;
    }
}

unsigned gadget_input_stack_free(void)
{
    return input_task != NULL ? uxTaskGetStackHighWaterMark(input_task) : UINT_MAX;
}
