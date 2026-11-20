#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gadget_module.h"

typedef enum {
    GADGET_INPUT_SELECT_FACE,
    GADGET_INPUT_GUIDE,
    GADGET_INPUT_REFRESH_WEATHER,
    GADGET_INPUT_VOLUME_STEP,
    GADGET_INPUT_AUDIO_JACK,
    GADGET_INPUT_KEY,
    GADGET_INPUT_EXIT,
} gadget_input_action_t;

typedef struct {
    gadget_input_action_t action;
    unsigned face;  // Zero-based F-key index for GADGET_INPUT_SELECT_FACE.
    int32_t value;
    gadget_key_t key;
} gadget_input_event_t;

esp_err_t gadget_input_init(void (*sink)(const gadget_input_event_t *));
void gadget_input_shutdown(void);
unsigned gadget_input_stack_free(void);
