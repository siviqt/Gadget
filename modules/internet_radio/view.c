// SPDX-License-Identifier: MIT
#include "view.h"
#include "gadget_keycap.h"

#include <string.h>

// Midnight and Vivid palettes adapted from TanRadio, copyright (c) 2026 TanRadio project.
typedef struct {
    uint32_t background, panel, field, line_color, highlight;
    uint32_t accent, text_color, muted_text, secondary, live_color, warning;
} radio_palette_t;

static const radio_palette_t themes[] = {
    {.background = 0xff101820, .panel = 0xff1c2a36, .field = 0xff263d4c, .line_color = 0xff405666,
     .highlight = 0xff173f50, .accent = 0xff62dfff, .text_color = 0xfff1f5f9, .muted_text = 0xffb8c8d6,
     .secondary = 0xff93a8b8, .live_color = 0xff83dbac, .warning = 0xffffd28a},
    {.background = 0xff0c1426, .panel = 0xff192747, .field = 0xff233963, .line_color = 0xff627cad,
     .highlight = 0xff153e85, .accent = 0xff00e5ff, .text_color = 0xfff1f5f9, .muted_text = 0xffc5d6f2,
     .secondary = 0xff96aac7, .live_color = 0xff43ec9c, .warning = 0xffffcc46},
};
static const radio_palette_t *palette = &themes[0];

enum { MARGIN = 24, GAP = 16, TOP = 84, FOOTER = 76 };
enum { TITLE_SIZE = 24, BODY_SIZE = 18, SMALL_SIZE = 13 };

typedef struct { int x, y, width, height; } area_t;

static const char *const playback_text[] = {
    [RADIO_STOPPED] = "Stopped",
    [RADIO_CONNECTING] = "Connecting to station…",
    [RADIO_BUFFERING] = "Buffering…",
    [RADIO_PLAYING] = "Playing live",
    [RADIO_OFFLINE] = "Offline · Reconnecting to Wi-Fi",
    [RADIO_RETRYING] = "Stream interrupted · Retrying",
    [RADIO_FORMAT_ERROR] = "Stream format unavailable · Retrying",
    [RADIO_OUTPUT_ERROR] = "Audio output unavailable · Retrying",
};

static int minimum(int a, int b)
{
    return a < b ? a : b;
}

static int measured_width(const gadget_canvas_t *canvas, unsigned size, const char *value)
{
    if (*value == '\0') {
        return 0;
    }
    gadget_text_bounds_t bounds;
    if (canvas->text_bounds == NULL) {
        return (int)radio_text_length(value, 128) * (int)size;
    }
    if (!canvas->text_bounds(canvas->handle, size, value, &bounds)) {
        return -1;
    }
    const int right = bounds.x + bounds.width;
    return right > bounds.advance ? right : bounds.advance;
}

static void label(const gadget_canvas_t *canvas, int x, int y, unsigned size, uint32_t color, const char *value)
{
    canvas->text(canvas->handle, x, y, size, color, value);
}

static size_t codepoint_bytes(const char *text, size_t available)
{
    size_t size = 1;
    while (size < available && size < 4 && ((unsigned char)text[size] & 0xc0) == 0x80) {
        ++size;
    }
    return size;
}

static void fit_label(const gadget_canvas_t *canvas, int x, int y, int width, unsigned size, uint32_t color,
                      const char *value)
{
    if (width <= 0 || value == NULL || value[0] == '\0') {
        return;
    }
    const int measured = measured_width(canvas, size, value);
    if (measured >= 0 && measured <= width) {
        label(canvas, x, y, size, color, value);
        return;
    }

    char fitted[128];
    size_t source_size = radio_text_length(value, sizeof(fitted) - 4);
    while (source_size != 0 && ((unsigned char)value[source_size] & 0xc0) == 0x80) {
        --source_size;
    }
    size_t used = 0;
    for (size_t at = 0; at < source_size;) {
        const size_t step = codepoint_bytes(value + at, source_size - at);
        char glyph[5] = {0};
        memcpy(glyph, value + at, step);
        if (measured < 0 && glyph[0] != ' ' && measured_width(canvas, size, glyph) < 0) {
            fitted[used++] = '?';
        } else {
            memcpy(fitted + used, glyph, step);
            used += step;
        }
        at += step;
    }
    fitted[used] = '\0';
    const int sanitized_width = measured_width(canvas, size, fitted);
    if (value[source_size] == '\0' && sanitized_width >= 0 && sanitized_width <= width) {
        label(canvas, x, y, size, color, fitted);
        return;
    }

    do {
        memcpy(fitted + used, "...", sizeof("..."));
        const int fitted_width = measured_width(canvas, size, fitted);
        if (fitted_width >= 0 && fitted_width <= width) {
            label(canvas, x, y, size, color, fitted);
            return;
        }
        if (used == 0) {
            return;
        }
        do {
            --used;
        } while (used != 0 && ((unsigned char)fitted[used] & 0xc0) == 0x80);
    } while (true);
}

static void rect(const gadget_canvas_t *canvas, area_t area, uint32_t color)
{
    canvas->rect(canvas->handle, area.x, area.y, area.width, area.height, color);
}

static void selected_box(const gadget_canvas_t *canvas, area_t area)
{
    rect(canvas, area, palette->highlight);
    rect(canvas, (area_t){area.x, area.y, 2, area.height}, palette->accent);
    rect(canvas, (area_t){area.x + area.width - 2, area.y, 2, area.height}, palette->accent);
    rect(canvas, (area_t){area.x, area.y, area.width, 2}, palette->accent);
    rect(canvas, (area_t){area.x, area.y + area.height - 2, area.width, 2}, palette->accent);
}

static const char *playback_label(const radio_screen_t *screen)
{
    return screen->muted && screen->playback == RADIO_PLAYING ? "Muted" : playback_text[screen->playback];
}

static uint32_t playback_color(const radio_screen_t *screen)
{
    switch (screen->playback) {
    case RADIO_STOPPED: return palette->muted_text;
    case RADIO_PLAYING: return screen->muted ? palette->accent : palette->live_color;
    case RADIO_FORMAT_ERROR:
    case RADIO_OUTPUT_ERROR:
    case RADIO_OFFLINE: return palette->warning;
    default: return palette->accent;
    }
}

static const char *play_action(const radio_screen_t *screen, const radio_station_t *selection)
{
    if (!screen->requested || selection == NULL || !radio_same_url(selection, screen->station)) {
        return screen->view == RADIO_SEARCH_VIEW ? "Preview" : "Play";
    }
    return screen->muted ? "Unmute" : "Mute";
}

static void header(gadget_module_context_t *module, const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    static const char *const titles[] = {
        [RADIO_PLAYER_VIEW] = "Internet Radio",
        [RADIO_SEARCH_VIEW] = "Find stations",
        [RADIO_ADD_VIEW] = "Add station",
        [RADIO_REMOVE_VIEW] = "Internet Radio",
        [RADIO_OPTIONS_VIEW] = "Radio Options",
        [RADIO_APPEARANCE_VIEW] = "Radio Appearance",
        [RADIO_ABOUT_VIEW] = "About Radio",
        [RADIO_DETAILS_VIEW] = "Stream details",
    };
    label(canvas, MARGIN, 20, TITLE_SIZE, palette->text_color, titles[screen->view]);

    char route[40];
    module->host->format(route, sizeof(route), "%s  %u%%", screen->headphones ? "Headphones" : "Speaker", screen->volume);
    const int volume_width = 148;
    const int right = canvas->width - MARGIN - volume_width;
    fit_label(canvas, right, 22, volume_width, SMALL_SIZE, palette->muted_text, route);
    rect(canvas, (area_t){right, 45, volume_width, 4}, palette->line_color);
    rect(canvas, (area_t){right, 45, volume_width * (int)screen->volume / 100, 4}, palette->accent);
    rect(canvas, (area_t){MARGIN, 66, canvas->width - 2 * MARGIN, 1}, palette->line_color);
}

static int shortcut(const gadget_canvas_t *canvas, int x, int y, gadget_key_kind_t key, const char *action, bool enabled)
{
    gadget_draw_function_key(canvas, x, y, key, enabled ? palette->field : palette->panel,
                             enabled ? GADGET_KEYCAP_OPAQUE : GADGET_KEYCAP_DIMMED);
    const int action_x = x + GADGET_KEYCAP_WIDTH + 7;
    label(canvas, action_x, y + 3, SMALL_SIZE, enabled ? palette->text_color : palette->secondary, action);
    return action_x + measured_width(canvas, SMALL_SIZE, action) + (canvas->width >= 640 ? 17 : 8);
}

static void footer(const gadget_canvas_t *canvas, const radio_screen_t *screen, const radio_station_t *selected)
{
    const int y = canvas->height - FOOTER;
    rect(canvas, (area_t){MARGIN, y, canvas->width - 2 * MARGIN, 1}, palette->line_color);
    int x = MARGIN;
    int hint_width = canvas->width - 2 * MARGIN;
    const char *hint;
    if (screen->view == RADIO_DETAILS_VIEW) {
        shortcut(canvas, x, y + 11, GADGET_KEY_F2, "Back", true);
        return;
    }
    if (screen->view == RADIO_REMOVE_VIEW) {
        x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, "Cancel", !screen->saving);
        shortcut(canvas, x, y + 11, GADGET_KEY_F4, "Remove", !screen->saving);
        return;
    }
    if (screen->view == RADIO_ADD_VIEW) {
        x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, "Cancel", !screen->saving);
        shortcut(canvas, x, y + 11, GADGET_KEY_F4, "Save", !screen->saving);
        fit_label(canvas, MARGIN, y + 43, canvas->width - 2 * MARGIN, SMALL_SIZE, palette->muted_text,
                  "Tab Field  ·  Left/Right Cursor  ·  Backspace Delete");
        return;
    }
    if (screen->view == RADIO_OPTIONS_VIEW || screen->view == RADIO_APPEARANCE_VIEW ||
        screen->view == RADIO_ABOUT_VIEW) {
        x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, "Back", true);
        if (screen->view == RADIO_APPEARANCE_VIEW) {
            shortcut(canvas, x, y + 11, GADGET_KEY_F4, "Apply", true);
            hint = "Left/Right Change theme";
        } else if (screen->view == RADIO_OPTIONS_VIEW) {
            shortcut(canvas, x, y + 11, GADGET_KEY_F4, "Open", true);
            hint = "Up/Down Select   Enter Open";
        } else {
            return;
        }
        fit_label(canvas, MARGIN, y + 43, canvas->width - 2 * MARGIN, SMALL_SIZE, palette->muted_text, hint);
        return;
    }

    const bool search = screen->view == RADIO_SEARCH_VIEW;
    const bool can_play = selected != NULL && !selected->hls;
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F1, "Options", true);
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, search ? "Back" : "Find", true);
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F3, "Stop", screen->requested);
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F4, play_action(screen, selected), can_play);
    if (search) {
        const bool saved = can_play && radio_catalog_contains(selected);
        x = shortcut(canvas, x, y + 11, GADGET_KEY_F5, saved ? "Saved" : "Save",
                     can_play && !saved && !screen->saving);
        const bool retry = screen->page_failure != DIRECTORY_IDLE;
        if (canvas->width >= 640) {
            shortcut(canvas, x, y + 11, GADGET_KEY_F6, "Details", selected != NULL);
            hint = retry ? "Enter Retry  ·  Arrows Browse  ·  Backspace Delete" :
                           "Enter Search  ·  Arrows Browse  ·  Backspace Delete";
        } else {
            const int details_width = GADGET_KEYCAP_WIDTH + 7 + measured_width(canvas, SMALL_SIZE, "Details");
            shortcut(canvas, canvas->width - MARGIN - details_width, y + 41, GADGET_KEY_F6, "Details", selected != NULL);
            hint_width -= details_width + 12;
            hint = retry ? "Enter Retry  ·  Arrows Browse" : "Enter Search  ·  Arrows Browse";
        }
    } else {
        const bool save_preview = screen->preview && screen->selection + 1 == screen->station_count;
        shortcut(canvas, x, y + 11, GADGET_KEY_F5, save_preview ? (canvas->width >= 640 ? "Save preview" : "Save") : "Next",
                 save_preview ? !screen->saving : screen->station_count != 0);
        hint = selected != NULL ? "Arrows Browse  ·  Enter Play  ·  Tab Stream details" : "Arrows Browse  ·  Enter Play";
    }
    fit_label(canvas, MARGIN, y + 43, hint_width, SMALL_SIZE, palette->muted_text, hint);
}

static void wrap_label(const gadget_canvas_t *canvas, int x, int y, int width, unsigned size, const char *value)
{
    const int measured = measured_width(canvas, size, value);
    if (measured < 0 || measured <= width) {
        fit_label(canvas, x, y, width, size, palette->text_color, value);
        return;
    }

    char first[RADIO_TITLE_BYTES];
    int split = -1;
    const size_t length = radio_text_length(value, sizeof(first) - 1);
    for (size_t i = 1; i < length; ++i) {
        if (value[i] == ' ') {
            memcpy(first, value, i);
            first[i] = '\0';
            const int partial = measured_width(canvas, size, first);
            if (partial > width) {
                break;
            }
            if (partial > 0) {
                split = (int)i;
            }
        }
    }
    if (split < 0) {
        fit_label(canvas, x, y, width, size, palette->text_color, value);
        return;
    }
    first[split] = '\0';
    label(canvas, x, y, size, palette->text_color, first);
    fit_label(canvas, x, y + (int)size + 4, width, size, palette->text_color, value + split + 1);
}

// Adapt the TanRadio Now Playing scroll to advance whole glyphs on the workspace canvas.
static bool scroll_label(const gadget_canvas_t *canvas, int x, int y, int width, unsigned size,
                         const char *value, unsigned step)
{
    const int measured = measured_width(canvas, size, value);
    if (measured < 0 || measured <= width) {
        fit_label(canvas, x, y, width, size, palette->text_color, value);
        return false;
    }
    uint8_t offsets[RADIO_TITLE_BYTES];
    const size_t size_bytes = radio_text_length(value, sizeof(offsets) - 1);
    unsigned count = 0;
    for (size_t at = 0; at < size_bytes; at += codepoint_bytes(value + at, size_bytes - at)) {
        offsets[count++] = (uint8_t)at;
    }
    offsets[count] = (uint8_t)size_bytes;
    unsigned first = 0, last = count;
    while (first < last) {
        const unsigned middle = first + (last - first) / 2;
        if (measured_width(canvas, size, value + offsets[middle]) <= width) {
            last = middle;
        } else {
            first = middle + 1;
        }
    }
    const unsigned phase = step % (first + 5);
    fit_label(canvas, x, y, width, size, palette->text_color, value + offsets[phase < first ? phase : first]);
    return true;
}

static void initials_badge(const gadget_canvas_t *canvas, int x, int y, const char *name)
{
    char initials[3] = {0};
    unsigned count = 0;
    bool word_start = true;
    for (; *name != '\0' && count < 2; ++name) {
        const char c = *name;
        if (word_start && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            initials[count++] = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
        }
        word_start = c == ' ' || c == '-';
    }
    if (count == 0) {
        initials[0] = 'R';
    }
    rect(canvas, (area_t){x, y, 48, 48}, palette->highlight);
    const int width = measured_width(canvas, BODY_SIZE, initials);
    label(canvas, x + (48 - width) / 2, y + 12, BODY_SIZE, palette->accent, initials);
}

static void station_details(const gadget_host_t *host, const radio_station_t *station, radio_stream_info_t stream,
                            bool saved, char *out, size_t capacity)
{
    const char *separator = "  ·  ";
    // The decoder reports the AAC family. Keep the directory's more specific profile.
    const bool aac_profile = stream.codec == RADIO_CODEC_AAC &&
                             (station->codec == RADIO_CODEC_AAC_PLUS || station->codec == RADIO_CODEC_HE_AAC_V2);
    const radio_codec_t codec = stream.codec == RADIO_CODEC_AUTO || aac_profile ? station->codec : stream.codec;
    const char *format = station->reported_codec[0] != '\0' ? station->reported_codec : radio_codec_name(codec);
    char rate[16] = "";
    const unsigned bitrate = station->bitrate != 0 ? station->bitrate : stream.bitrate;
    if (bitrate != 0) {
        host->format(rate, sizeof(rate), "%s%u kb/s", station->bitrate != 0 ? "" : "~", bitrate);
    }
    const char *status = station->hls ? "HLS  ·  Playback unavailable" : saved ? "Saved" : "";
    const bool media = format[0] != '\0' || rate[0] != '\0';
    const bool country = station->country[0] != '\0';
    host->format(out, capacity, "%s%s%s%s%s%s%s", format,
                 format[0] != '\0' && rate[0] != '\0' ? separator : "", rate,
                 media && country ? separator : "", station->country,
                 status[0] != '\0' && (media || country) ? separator : "", status);
}

static gadget_damage_t now_playing(gadget_module_context_t *module, const gadget_canvas_t *canvas,
                                    const radio_screen_t *screen, area_t area)
{
    rect(canvas, area, palette->panel);
    const int x = area.x + GAP, width = area.width - 2 * GAP;
    label(canvas, x, area.y + 15, SMALL_SIZE, palette->muted_text, "Now playing");
    if (screen->station->url[0] == '\0') {
        label(canvas, x, area.y + 43, BODY_SIZE, palette->text_color, "No station selected");
        return (gadget_damage_t){0};
    }
    initials_badge(canvas, x, area.y + 43, screen->station->name);
    bool scrolling = scroll_label(canvas, x + 60, area.y + 51, width - 60, TITLE_SIZE,
                                  screen->station->name, screen->scroll_step);

    char details[48];
    station_details(module->host, screen->station, screen->stream, false, details, sizeof(details));
    fit_label(canvas, x, area.y + 106, width, SMALL_SIZE, palette->muted_text,
              details[0] != '\0' ? details : "Direct audio");
    if (screen->track[0] != '\0') {
        label(canvas, x, area.y + 141, SMALL_SIZE, palette->muted_text, "Track");
        scrolling |= scroll_label(canvas, x, area.y + 163, width, BODY_SIZE, screen->track, screen->scroll_step);
    }
    rect(canvas, (area_t){x, area.y + area.height - 50, width, 1}, palette->line_color);
    fit_label(canvas, x, area.y + area.height - 35, width, BODY_SIZE, playback_color(screen), playback_label(screen));
    return scrolling ? (gadget_damage_t){area.x, area.y, area.width, area.height} : (gadget_damage_t){0};
}

static void station_list(const gadget_canvas_t *canvas, const radio_screen_t *screen, area_t area)
{
    label(canvas, area.x, area.y + 3, BODY_SIZE, palette->text_color, "Stations");
    const char *remove_hint = "Backspace Remove";
    label(canvas, area.x + area.width - measured_width(canvas, SMALL_SIZE, remove_hint), area.y + 5, SMALL_SIZE,
          screen->removable ? palette->muted_text : palette->secondary, remove_hint);
    if (screen->station_count == 0) {
        label(canvas, area.x, area.y + 43, BODY_SIZE, palette->muted_text, "Your station list is empty.");
        return;
    }
    const int pitch = 41;
    const unsigned capacity = area.height > 32 ? (unsigned)((area.height - 32) / pitch) : 0;
    const unsigned shown = screen->station_count < capacity ? screen->station_count : capacity;
    const unsigned middle = shown / 2;
    unsigned first = screen->selection > middle ? screen->selection - middle : 0;
    if (first + shown > screen->station_count) {
        first = screen->station_count - shown;
    }
    for (unsigned i = 0; i < shown; ++i) {
        const unsigned index = first + i;
        const radio_station_t *station = radio_catalog_station(index);
        const int y = area.y + 31 + (int)i * pitch;
        if (index == screen->selection) {
            selected_box(canvas, (area_t){area.x, y, area.width, pitch - 4});
        }

        const bool on_air = screen->requested && radio_same_station(station, screen->station);
        const char *tag = on_air ? (screen->playback == RADIO_PLAYING ? (screen->muted ? "Muted" : "On air") : "Tuned") :
                          index < screen->preset_count ? "Preset" :
                          screen->preview && index + 1 == screen->station_count ? "Preview" : NULL;
        const int tag_width = tag != NULL ? 72 : 0;
        fit_label(canvas, area.x + 12, y + 7, area.width - 24 - tag_width, BODY_SIZE, palette->text_color, station->name);
        if (tag != NULL) {
            label(canvas, area.x + area.width - tag_width, y + 11, SMALL_SIZE, on_air ? palette->live_color : palette->muted_text, tag);
        }
    }
}

static gadget_damage_t draw_player(gadget_module_context_t *module, const gadget_canvas_t *canvas,
                                   const radio_screen_t *screen)
{
    const int width = canvas->width - 2 * MARGIN;
    const int bottom = canvas->height - FOOTER;
    const int body_height = bottom - TOP - 35;
    gadget_damage_t scrolling;
    if (canvas->width >= 640) {
        const int current_width = (width - GAP) * 46 / 100;
        scrolling = now_playing(module, canvas, screen, (area_t){MARGIN, TOP, current_width, body_height});
        station_list(canvas, screen, (area_t){MARGIN + current_width + GAP, TOP,
                                             width - current_width - GAP, body_height});
    } else {
        const int summary_height = 256;
        scrolling = now_playing(module, canvas, screen, (area_t){MARGIN, TOP, width, summary_height});
        station_list(canvas, screen, (area_t){MARGIN, TOP + summary_height + GAP, width,
                                             body_height - summary_height - GAP});
    }
    fit_label(canvas, MARGIN, bottom - 25, width, SMALL_SIZE, palette->accent, screen->notice);
    footer(canvas, screen, radio_catalog_station(screen->selection));
    return scrolling;
}

static const char *search_status(const radio_screen_t *screen)
{
    if (screen->notice != NULL) {
        return screen->notice;
    }
    if (screen->loading) {
        return screen->results->status == DIRECTORY_RESULTS ? "Loading more stations…" : "Searching Radio Browser…";
    }
    if (screen->page_failure != DIRECTORY_IDLE) {
        return screen->page_failure == DIRECTORY_OFFLINE ? "Offline · Press Enter to retry" :
                                                          "Page unavailable · Press Enter to retry";
    }
    switch (screen->results->status) {
    case DIRECTORY_RESULTS:
        if (screen->results->count == 0) {
            return screen->results->more ? "No playable stations here · Down for more" : "No playable stations found";
        }
        if (!screen->results->more && screen->result_row + 1 == screen->results->count) {
            return !screen->has_previous && screen->results->count == 1 ? "Only result" : "End of results";
        }
        return !screen->has_previous && screen->result_row == 0 ? "Start of results" : "Arrows browse more results";
    case DIRECTORY_OFFLINE: return "Offline · Press Enter to retry";
    case DIRECTORY_ERROR: return "Directory unavailable · Press Enter to retry";
    default: return "Enter a search term to find stations";
    }
}

static void draw_search(gadget_module_context_t *module, const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const int width = canvas->width - 2 * MARGIN;
    const int bottom = canvas->height - FOOTER;
    label(canvas, MARGIN, 80, BODY_SIZE, palette->text_color, screen->search_mode == DIRECTORY_NAME ? "Station name" : "Country");
    label(canvas, canvas->width - MARGIN - 135, 80, SMALL_SIZE, palette->muted_text, "Tab  Name / country");
    rect(canvas, (area_t){MARGIN, 110, width, 40}, palette->field);
    const char *placeholder = screen->search_mode == DIRECTORY_NAME ? "Search by station name" : "Country or code, e.g. Bulgaria or BG";
    fit_label(canvas, MARGIN + 12, 119, width - 24, BODY_SIZE, screen->query[0] != '\0' ? palette->text_color : palette->muted_text,
              screen->query[0] != '\0' ? screen->query : placeholder);
    fit_label(canvas, MARGIN, 161, width, SMALL_SIZE, screen->notice != NULL ? palette->accent : palette->muted_text, search_status(screen));

    const radio_directory_page_t *results = screen->results;
    const int first_y = 188;
    const int pitch = minimum(50, (bottom - 38 - first_y) / RADIO_PAGE_ROWS);
    const radio_station_t *selected = NULL;
    if (results->status == DIRECTORY_RESULTS) {
        for (unsigned i = 0; i < results->count; ++i) {
            const radio_station_t *station = &results->rows[i];
            const int y = first_y + (int)i * pitch;
            if (i == screen->result_row) {
                selected_box(canvas, (area_t){MARGIN, y, width, pitch - 2});
                selected = station;
            }
            char details[80];
            station_details(module->host, station, (radio_stream_info_t){0}, radio_catalog_contains(station),
                            details, sizeof(details));
            fit_label(canvas, MARGIN + 12, y + 2, width - 24, BODY_SIZE, palette->text_color, station->name);
            fit_label(canvas, MARGIN + 12, y + 20, width - 24, SMALL_SIZE,
                      station->hls ? palette->warning : palette->muted_text, details);
        }
    }
    if (screen->requested) {
        char summary[128];
        module->host->format(summary, sizeof(summary), "%s  ·  %s", screen->station->name, playback_label(screen));
        rect(canvas, (area_t){MARGIN, bottom - 33, width, 25}, palette->panel);
        fit_label(canvas, MARGIN + 8, bottom - 29, width - 16, SMALL_SIZE, playback_color(screen), summary);
    }
    footer(canvas, screen, selected);
}

static void draw_stream_url(const gadget_canvas_t *canvas, int x, int y, int width, const char *url)
{
    char line[RADIO_URL_BYTES];
    size_t used = 0;
    int advance = 0;
    const size_t length = radio_text_length(url, sizeof(line) - 1);
    for (size_t at = 0; at < length;) {
        const size_t step = codepoint_bytes(url + at, length - at);
        char glyph[5] = {0};
        memcpy(glyph, url + at, step);
        size_t bytes = step;
        int glyph_width = measured_width(canvas, SMALL_SIZE, glyph);
        if (glyph_width < 0) {
            glyph[0] = '?';
            glyph[1] = '\0';
            bytes = 1;
            glyph_width = measured_width(canvas, SMALL_SIZE, glyph);
        }
        if (used != 0 && advance + glyph_width > width) {
            line[used] = '\0';
            label(canvas, x, y, SMALL_SIZE, palette->text_color, line);
            y += SMALL_SIZE + 5;
            used = 0;
            advance = 0;
        }
        memcpy(line + used, glyph, bytes);
        used += bytes;
        advance += glyph_width;
        at += step;
    }
    line[used] = '\0';
    label(canvas, x, y, SMALL_SIZE, palette->text_color, line);
}

static void draw_stream_details(gadget_module_context_t *module, const gadget_canvas_t *canvas,
                                const radio_screen_t *screen)
{
    const radio_station_t *station = screen->detail_station;
    const int x = MARGIN + GAP, width = canvas->width - 2 * x;
    rect(canvas, (area_t){MARGIN, TOP, canvas->width - 2 * MARGIN, canvas->height - FOOTER - TOP - GAP}, palette->panel);
    fit_label(canvas, x, TOP + 16, width, TITLE_SIZE, palette->text_color, station->name);
    char details[80];
    const bool tuned = screen->requested && !station->hls && radio_same_url(station, screen->station);
    station_details(module->host, station, tuned ? screen->stream : (radio_stream_info_t){0}, false,
                    details, sizeof(details));
    fit_label(canvas, x, TOP + 54, width, BODY_SIZE, station->hls ? palette->warning : palette->muted_text,
              details[0] != '\0' ? details : "Codec and bitrate unavailable");
    label(canvas, x, TOP + 90, SMALL_SIZE, palette->muted_text, "Stream URL");
    draw_stream_url(canvas, x, TOP + 113, width, station->url);
    footer(canvas, screen, NULL);
}

static void draw_removal(const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const int width = minimum(576, canvas->width - 2 * MARGIN);
    const int x = (canvas->width - width) / 2;
    const int y = TOP + 24;
    rect(canvas, (area_t){x, y, width, 178}, palette->panel);
    label(canvas, x + GAP, y + 18, TITLE_SIZE, palette->text_color, "Remove station?");
    wrap_label(canvas, x + GAP, y + 60, width - 2 * GAP, TITLE_SIZE, screen->remove_target->name);
    fit_label(canvas, x + GAP, y + 132, width - 2 * GAP, SMALL_SIZE, palette->accent, screen->notice);
    footer(canvas, screen, NULL);
}

static void draw_input_text(const gadget_canvas_t *canvas, int x, int y, int width, const char *value, size_t cursor)
{
    char prefix[128];
    size_t start = cursor;
    int advance = 0;
    while (start != 0) {
        size_t previous = start;
        do {
            --previous;
        } while (previous != 0 && ((unsigned char)value[previous] & 0xc0) == 0x80);
        const size_t bytes = start - previous;
        if (bytes > 4 || cursor - previous >= sizeof(prefix)) {
            break;
        }
        char glyph[5] = {0};
        memcpy(glyph, value + previous, bytes);
        const int measured = measured_width(canvas, BODY_SIZE, glyph);
        const int glyph_width = measured > 0 ? measured : 8;
        if (advance + glyph_width > width - 22) {
            break;
        }
        advance += glyph_width;
        start = previous;
    }
    if (start != 0) {
        label(canvas, x, y, BODY_SIZE, palette->muted_text, "…");
        x += 18;
        width -= 18;
    }
    memcpy(prefix, value + start, cursor - start);
    prefix[cursor - start] = '\0';
    fit_label(canvas, x, y, width, BODY_SIZE, palette->text_color, prefix);
    fit_label(canvas, x + advance, y, width - advance, BODY_SIZE, palette->text_color, value + cursor);
    rect(canvas, (area_t){x + advance, y + 3, 2, 21}, palette->accent);
}

static void draw_entry_field(const gadget_canvas_t *canvas, int x, int y, int width, const char *name,
                             const char *value, size_t cursor, bool focused)
{
    label(canvas, x, y, SMALL_SIZE, focused ? palette->accent : palette->muted_text, name);
    rect(canvas, (area_t){x, y + 25, width, 42}, palette->field);
    if (focused) {
        rect(canvas, (area_t){x, y + 25, 2, 42}, palette->accent);
        draw_input_text(canvas, x + 12, y + 33, width - 24, value, cursor);
    } else {
        fit_label(canvas, x + 12, y + 33, width - 24, BODY_SIZE, palette->text_color, value);
    }
}

static void draw_add(const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const radio_station_form_t *form = screen->form;
    const int width = minimum(650, canvas->width - 2 * MARGIN);
    const int x = (canvas->width - width) / 2;
    rect(canvas, (area_t){x, TOP + 16, width, 226}, palette->panel);
    draw_entry_field(canvas, x + GAP, TOP + 34, width - 2 * GAP, "Station name",
                     form->station.name, form->name_cursor, !form->url_focused && !screen->saving);
    draw_entry_field(canvas, x + GAP, TOP + 119, width - 2 * GAP,
                     screen->aac_available ? "Direct MP3 or AAC URL" : "Direct MP3 URL",
                     form->station.url, form->url_cursor, form->url_focused && !screen->saving);
    fit_label(canvas, x + GAP, TOP + 202, width - 2 * GAP, SMALL_SIZE, palette->accent, screen->notice);
    footer(canvas, screen, NULL);
}

static void draw_options(const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    static const char *const labels[] = {
        [RADIO_OPTION_ADD] = "Add by URL",
        [RADIO_OPTION_APPEARANCE] = "Appearance",
        [RADIO_OPTION_ABOUT] = "About",
    };
    const int width = minimum(524, canvas->width - MARGIN * 2);
    const int x = (canvas->width - width) / 2;
    if (screen->view == RADIO_OPTIONS_VIEW) {
        for (unsigned i = 0; i < RADIO_OPTION_COUNT; ++i) {
            const int y = TOP + 18 + (int)i * 53;
            const area_t area = {x, y, width, 45};
            if (screen->option == i) {
                selected_box(canvas, area);
            } else {
                rect(canvas, area, palette->panel);
            }
            label(canvas, x + GAP, y + 11, BODY_SIZE, palette->text_color, labels[i]);
        }
    } else if (screen->view == RADIO_APPEARANCE_VIEW) {
        selected_box(canvas, (area_t){x, TOP + 18, width, 50});
        label(canvas, x + GAP, TOP + 32, BODY_SIZE, palette->text_color, "Theme");
        label(canvas, x + width - 124, TOP + 32, BODY_SIZE, palette->accent,
              screen->draft_vivid ? "Vivid" : "Midnight");
        fit_label(canvas, x, TOP + 94, width, SMALL_SIZE, palette->text_color, screen->notice);
    } else {
        rect(canvas, (area_t){x, TOP + 18, width, 140}, palette->panel);
        label(canvas, x + GAP, TOP + 34, TITLE_SIZE, palette->text_color, "Gadget Internet Radio");
        label(canvas, x + GAP, TOP + 74, BODY_SIZE, palette->muted_text, "Player adapted from TanRadio");
        label(canvas, x + GAP, TOP + 112, BODY_SIZE, palette->muted_text, "Station search by Radio Browser");
    }
    footer(canvas, screen, NULL);
}

gadget_damage_t radio_render(gadget_module_context_t *module, const gadget_canvas_t *canvas,
                             const radio_screen_t *screen)
{
    palette = &themes[screen->view == RADIO_APPEARANCE_VIEW ? screen->draft_vivid : screen->vivid];
    rect(canvas, (area_t){0, 0, canvas->width, canvas->height}, palette->background);
    header(module, canvas, screen);
    switch (screen->view) {
    case RADIO_SEARCH_VIEW: draw_search(module, canvas, screen); break;
    case RADIO_DETAILS_VIEW: draw_stream_details(module, canvas, screen); break;
    case RADIO_ADD_VIEW: draw_add(canvas, screen); break;
    case RADIO_REMOVE_VIEW: draw_removal(canvas, screen); break;
    case RADIO_OPTIONS_VIEW:
    case RADIO_APPEARANCE_VIEW:
    case RADIO_ABOUT_VIEW: draw_options(canvas, screen); break;
    case RADIO_PLAYER_VIEW: return draw_player(module, canvas, screen);
    }
    return (gadget_damage_t){0};
}
