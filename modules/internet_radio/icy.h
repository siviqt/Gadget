// SPDX-License-Identifier: MIT
#pragma once

#include "radio.h"

typedef struct {
    uint32_t interval, audio_left;
    size_t block_size, block_used;
    uint8_t block[255 * 16];
} radio_icy_t;

typedef void (*radio_icy_audio_fn)(const uint8_t *data, unsigned size);
typedef void (*radio_icy_title_fn)(void *context, const char *title);

void radio_icy_reset(radio_icy_t *filter, uint32_t interval);
void radio_icy_feed(radio_icy_t *filter, const uint8_t *data, unsigned size, radio_icy_audio_fn audio,
                    radio_icy_title_fn title, void *context);
