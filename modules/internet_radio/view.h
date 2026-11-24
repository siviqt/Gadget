// SPDX-License-Identifier: MIT
#pragma once

#include "radio.h"

typedef enum {
    RADIO_PLAYER_VIEW, RADIO_SEARCH_VIEW, RADIO_ADD_VIEW, RADIO_REMOVE_VIEW,
    RADIO_OPTIONS_VIEW, RADIO_APPEARANCE_VIEW, RADIO_ABOUT_VIEW, RADIO_DETAILS_VIEW,
} radio_view_t;

typedef enum {
    RADIO_OPTION_ADD, RADIO_OPTION_APPEARANCE, RADIO_OPTION_ABOUT, RADIO_OPTION_COUNT,
} radio_option_t;

typedef struct {
    radio_station_t station;
    size_t name_cursor, url_cursor;
    bool url_focused;
} radio_station_form_t;

typedef struct {
    radio_view_t view;
    radio_playback_t playback;
    radio_stream_info_t stream;
    const radio_station_t *station, *remove_target, *detail_station;
    const radio_station_form_t *form;
    const radio_directory_page_t *results;
    const char *query, *notice, *track;
    radio_directory_mode_t search_mode;
    directory_status_t page_failure;
    radio_option_t option;
    unsigned selection, station_count, preset_count, result_row, volume, scroll_step;
    bool requested, muted, headphones, loading, saving, preview, removable;
    bool vivid, draft_vivid, aac_available, has_previous;
} radio_screen_t;

const radio_station_t *radio_catalog_station(unsigned index);
bool radio_catalog_contains(const radio_station_t *station);
gadget_damage_t radio_render(gadget_module_context_t *module, const gadget_canvas_t *canvas,
                             const radio_screen_t *screen);
