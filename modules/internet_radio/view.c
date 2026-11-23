// SPDX-License-Identifier: MIT
#include "view.h"
#include "gadget_keycap.h"

#include <string.h>

static const uint32_t background = 0xff101b29, panel = 0xff19283a, field = 0xff223449;
static const uint32_t line_color = 0xff354961, highlight = 0xff2b3b4d;
static const uint32_t accent = 0xffedb664, text_color = 0xffeef4fa, muted_text = 0xffb1c2d4;
static const uint32_t secondary = 0xff839ab2, live_color = 0xff7bd8ba, warning = 0xfff0a49a;

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
    [RADIO_FORMAT_ERROR] = "MP3 stream unavailable · Retrying",
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
    rect(canvas, area, highlight);
    rect(canvas, (area_t){area.x, area.y, 2, area.height}, accent);
}

static const char *playback_label(const radio_screen_t *screen)
{
    return screen->muted && screen->playback == RADIO_PLAYING ? "Muted" : playback_text[screen->playback];
}

static uint32_t playback_color(const radio_screen_t *screen)
{
    switch (screen->playback) {
    case RADIO_STOPPED: return muted_text;
    case RADIO_PLAYING: return screen->muted ? accent : live_color;
    case RADIO_FORMAT_ERROR:
    case RADIO_OUTPUT_ERROR:
    case RADIO_OFFLINE: return warning;
    default: return accent;
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
    const char *title = screen->view == RADIO_SEARCH_VIEW ? "Find stations" :
                        screen->view == RADIO_ADD_VIEW ? "Add station" : "Internet Radio";
    label(canvas, MARGIN, 20, TITLE_SIZE, text_color, title);

    char route[40];
    module->host->format(route, sizeof(route), "%s  %u%%", screen->headphones ? "Headphones" : "Speaker", screen->volume);
    const int volume_width = 148;
    const int right = canvas->width - MARGIN - volume_width;
    fit_label(canvas, right, 22, volume_width, SMALL_SIZE, muted_text, route);
    rect(canvas, (area_t){right, 45, volume_width, 4}, line_color);
    rect(canvas, (area_t){right, 45, volume_width * (int)screen->volume / 100, 4}, accent);
    rect(canvas, (area_t){MARGIN, 66, canvas->width - 2 * MARGIN, 1}, line_color);
}

static int shortcut(const gadget_canvas_t *canvas, int x, int y, gadget_key_kind_t key, const char *action, bool enabled)
{
    gadget_draw_function_key(canvas, x, y, key, enabled ? field : panel, enabled ? accent : secondary);
    const int action_x = x + GADGET_KEYCAP_WIDTH + 7;
    label(canvas, action_x, y + 3, SMALL_SIZE, enabled ? text_color : secondary, action);
    return action_x + measured_width(canvas, SMALL_SIZE, action) + 17;
}

static void footer(const gadget_canvas_t *canvas, const radio_screen_t *screen, const radio_station_t *selected)
{
    const int y = canvas->height - FOOTER;
    rect(canvas, (area_t){MARGIN, y, canvas->width - 2 * MARGIN, 1}, line_color);
    int x = MARGIN;
    const char *hint;
    if (screen->view == RADIO_REMOVE_VIEW) {
        x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, "Cancel", !screen->saving);
        shortcut(canvas, x, y + 11, GADGET_KEY_F4, "Remove", !screen->saving);
        return;
    }
    if (screen->view == RADIO_ADD_VIEW) {
        x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, "Cancel", !screen->saving);
        shortcut(canvas, x, y + 11, GADGET_KEY_F4, "Save", !screen->saving);
        fit_label(canvas, MARGIN, y + 43, canvas->width - 2 * MARGIN, SMALL_SIZE, muted_text,
                  "Tab Field  ·  Left/Right Cursor  ·  Backspace Delete");
        return;
    }

    const bool search = screen->view == RADIO_SEARCH_VIEW;
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F2, search ? "Back" : "Find", true);
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F3, "Stop", screen->requested);
    x = shortcut(canvas, x, y + 11, GADGET_KEY_F4, play_action(screen, selected), selected != NULL);
    if (search) {
        const bool saved = selected != NULL && radio_catalog_contains(selected);
        shortcut(canvas, x, y + 11, GADGET_KEY_F5, saved ? "Saved" : "Save",
                 selected != NULL && !saved && !screen->saving);
        hint = "Enter Search  ·  Arrows Browse  ·  Backspace Delete";
    } else {
        const bool save_preview = screen->preview && screen->selection + 1 == screen->station_count;
        shortcut(canvas, x, y + 11, GADGET_KEY_F5, save_preview ? "Save preview" : "Next",
                 save_preview ? !screen->saving : screen->station_count != 0);
        hint = "Arrows Browse  ·  Enter Play";
    }
    fit_label(canvas, MARGIN, y + 43, canvas->width - 2 * MARGIN, SMALL_SIZE, muted_text, hint);
}

static void wrap_label(const gadget_canvas_t *canvas, int x, int y, int width, unsigned size, const char *value)
{
    const int measured = measured_width(canvas, size, value);
    if (measured < 0 || measured <= width) {
        fit_label(canvas, x, y, width, size, text_color, value);
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
        fit_label(canvas, x, y, width, size, text_color, value);
        return;
    }
    first[split] = '\0';
    label(canvas, x, y, size, text_color, first);
    fit_label(canvas, x, y + (int)size + 4, width, size, text_color, value + split + 1);
}

static void now_playing(gadget_module_context_t *module, const gadget_canvas_t *canvas, const radio_screen_t *screen,
                        area_t area)
{
    rect(canvas, area, panel);
    const int x = area.x + GAP, width = area.width - 2 * GAP;
    label(canvas, x, area.y + 15, SMALL_SIZE, muted_text, "Now playing");
    if (screen->station->url[0] == '\0') {
        label(canvas, x, area.y + 43, BODY_SIZE, text_color, "No station selected");
        return;
    }
    wrap_label(canvas, x, area.y + 43, width, TITLE_SIZE, screen->station->name);

    char details[48];
    if (screen->station->bitrate != 0) {
        module->host->format(details, sizeof(details), "MP3  ·  %u kb/s%s%s", (unsigned)screen->station->bitrate,
                             screen->station->country[0] != '\0' ? "  ·  " : "", screen->station->country);
    } else {
        module->host->format(details, sizeof(details), "MP3%s%s",
                             screen->station->country[0] != '\0' ? "  ·  " : "", screen->station->country);
    }
    fit_label(canvas, x, area.y + 106, width, SMALL_SIZE, muted_text, details);
    if (screen->track[0] != '\0') {
        label(canvas, x, area.y + 141, SMALL_SIZE, muted_text, "Track");
        wrap_label(canvas, x, area.y + 163, width, BODY_SIZE, screen->track);
    }
    rect(canvas, (area_t){x, area.y + area.height - 50, width, 1}, line_color);
    fit_label(canvas, x, area.y + area.height - 35, width, BODY_SIZE, playback_color(screen), playback_label(screen));
}

static void station_list(const gadget_canvas_t *canvas, const radio_screen_t *screen, area_t area)
{
    label(canvas, area.x, area.y + 3, BODY_SIZE, text_color, "Stations");
    const int remove_x = shortcut(canvas, area.x + area.width - 168, area.y, GADGET_KEY_F1, "Add", !screen->saving);
    shortcut(canvas, remove_x, area.y, GADGET_KEY_F6, "Remove", screen->removable);
    if (screen->station_count == 0) {
        label(canvas, area.x, area.y + 43, BODY_SIZE, muted_text, "Your station list is empty.");
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
        fit_label(canvas, area.x + 12, y + 7, area.width - 24 - tag_width, BODY_SIZE, text_color, station->name);
        if (tag != NULL) {
            label(canvas, area.x + area.width - tag_width, y + 11, SMALL_SIZE, on_air ? live_color : muted_text, tag);
        }
    }
}

static void draw_player(gadget_module_context_t *module, const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const int width = canvas->width - 2 * MARGIN;
    const int bottom = canvas->height - FOOTER;
    const int body_height = bottom - TOP - 35;
    if (canvas->width >= 640) {
        const int current_width = (width - GAP) * 46 / 100;
        now_playing(module, canvas, screen, (area_t){MARGIN, TOP, current_width, body_height});
        station_list(canvas, screen, (area_t){MARGIN + current_width + GAP, TOP,
                                             width - current_width - GAP, body_height});
    } else {
        const int summary_height = 256;
        now_playing(module, canvas, screen, (area_t){MARGIN, TOP, width, summary_height});
        station_list(canvas, screen, (area_t){MARGIN, TOP + summary_height + GAP, width,
                                             body_height - summary_height - GAP});
    }
    fit_label(canvas, MARGIN, bottom - 25, width, SMALL_SIZE, accent, screen->notice);
    footer(canvas, screen, radio_catalog_station(screen->selection));
}

static const char *search_status(const radio_screen_t *screen)
{
    if (screen->notice != NULL) {
        return screen->notice;
    }
    if (screen->loading) {
        return screen->results->status == DIRECTORY_RESULTS ? "Loading more stations…" : "Searching Radio Browser…";
    }
    switch (screen->results->status) {
    case DIRECTORY_RESULTS:
        return screen->results->count == 0
               ? (screen->results->more ? "No direct MP3 here · Down for more" : "No direct MP3 stations found")
               : screen->results->more ? "Arrows browse more results" : "End of results";
    case DIRECTORY_OFFLINE: return "Offline · Press Enter to retry";
    case DIRECTORY_ERROR: return "Directory unavailable · Press Enter to retry";
    default: return "Enter a search term to find stations";
    }
}

static void draw_search(gadget_module_context_t *module, const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const int width = canvas->width - 2 * MARGIN;
    const int bottom = canvas->height - FOOTER;
    label(canvas, MARGIN, 80, BODY_SIZE, text_color, screen->search_mode == DIRECTORY_NAME ? "Station name" : "Country");
    label(canvas, canvas->width - MARGIN - 135, 80, SMALL_SIZE, muted_text, "Tab  Name / country");
    rect(canvas, (area_t){MARGIN, 110, width, 40}, field);
    const char *placeholder = screen->search_mode == DIRECTORY_NAME ? "Search by station name" : "Country or code, e.g. Bulgaria or BG";
    fit_label(canvas, MARGIN + 12, 119, width - 24, BODY_SIZE, screen->query[0] != '\0' ? text_color : muted_text,
              screen->query[0] != '\0' ? screen->query : placeholder);
    fit_label(canvas, MARGIN, 161, width, SMALL_SIZE, screen->notice != NULL ? accent : muted_text, search_status(screen));

    const radio_directory_page_t *results = screen->results;
    const int first_y = 188;
    const int pitch = minimum(50, (bottom - 42 - first_y) / RADIO_PAGE_ROWS);
    const radio_station_t *selected = NULL;
    if (results->status == DIRECTORY_RESULTS) {
        for (unsigned i = 0; i < results->count; ++i) {
            const radio_station_t *station = &results->rows[i];
            const int y = first_y + (int)i * pitch;
            if (i == screen->result_row) {
                selected_box(canvas, (area_t){MARGIN, y, width, pitch - 4});
                selected = station;
            }
            char details[48];
            const bool saved = radio_catalog_contains(station);
            if (station->bitrate != 0 && canvas->width >= 640) {
                module->host->format(details, sizeof(details), "%s%s%u kb/s%s", station->country,
                                     station->country[0] != '\0' ? "  ·  " : "", (unsigned)station->bitrate,
                                     saved ? "  ·  Saved" : "");
            } else {
                module->host->format(details, sizeof(details), "%s", saved ? "Saved" : station->country);
            }
            const int details_width = canvas->width >= 640 ? 165 : 62;
            fit_label(canvas, MARGIN + 12, y + 5, width - details_width - 24, BODY_SIZE, text_color, station->name);
            fit_label(canvas, MARGIN + width - details_width, y + 9, details_width - 8, SMALL_SIZE, muted_text, details);
        }
    }
    if (screen->requested) {
        char summary[128];
        module->host->format(summary, sizeof(summary), "%s  ·  %s", screen->station->name, playback_label(screen));
        rect(canvas, (area_t){MARGIN, bottom - 33, width, 25}, panel);
        fit_label(canvas, MARGIN + 8, bottom - 29, width - 16, SMALL_SIZE, playback_color(screen), summary);
    }
    footer(canvas, screen, selected);
}

static void draw_removal(const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const int width = minimum(576, canvas->width - 2 * MARGIN);
    const int x = (canvas->width - width) / 2;
    const int y = TOP + 24;
    rect(canvas, (area_t){x, y, width, 178}, panel);
    label(canvas, x + GAP, y + 18, TITLE_SIZE, text_color, "Remove station?");
    wrap_label(canvas, x + GAP, y + 60, width - 2 * GAP, TITLE_SIZE, screen->remove_target->name);
    fit_label(canvas, x + GAP, y + 132, width - 2 * GAP, SMALL_SIZE, accent, screen->notice);
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
        label(canvas, x, y, BODY_SIZE, muted_text, "…");
        x += 18;
        width -= 18;
    }
    memcpy(prefix, value + start, cursor - start);
    prefix[cursor - start] = '\0';
    fit_label(canvas, x, y, width, BODY_SIZE, text_color, prefix);
    fit_label(canvas, x + advance, y, width - advance, BODY_SIZE, text_color, value + cursor);
    rect(canvas, (area_t){x + advance, y + 3, 2, 21}, accent);
}

static void draw_entry_field(const gadget_canvas_t *canvas, int x, int y, int width, const char *name,
                             const char *value, size_t cursor, bool focused)
{
    label(canvas, x, y, SMALL_SIZE, focused ? accent : muted_text, name);
    rect(canvas, (area_t){x, y + 25, width, 42}, field);
    if (focused) {
        rect(canvas, (area_t){x, y + 25, 2, 42}, accent);
        draw_input_text(canvas, x + 12, y + 33, width - 24, value, cursor);
    } else {
        fit_label(canvas, x + 12, y + 33, width - 24, BODY_SIZE, text_color, value);
    }
}

static void draw_add(const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    const radio_station_form_t *form = screen->form;
    const int width = minimum(650, canvas->width - 2 * MARGIN);
    const int x = (canvas->width - width) / 2;
    rect(canvas, (area_t){x, TOP + 16, width, 226}, panel);
    draw_entry_field(canvas, x + GAP, TOP + 34, width - 2 * GAP, "Station name",
                     form->station.name, form->name_cursor, !form->url_focused && !screen->saving);
    draw_entry_field(canvas, x + GAP, TOP + 119, width - 2 * GAP, "Direct MP3 URL",
                     form->station.url, form->url_cursor, form->url_focused && !screen->saving);
    fit_label(canvas, x + GAP, TOP + 202, width - 2 * GAP, SMALL_SIZE, accent, screen->notice);
    footer(canvas, screen, NULL);
}

void radio_render(gadget_module_context_t *module, const gadget_canvas_t *canvas, const radio_screen_t *screen)
{
    rect(canvas, (area_t){0, 0, canvas->width, canvas->height}, background);
    header(module, canvas, screen);
    switch (screen->view) {
    case RADIO_SEARCH_VIEW: draw_search(module, canvas, screen); break;
    case RADIO_ADD_VIEW: draw_add(canvas, screen); break;
    case RADIO_REMOVE_VIEW: draw_removal(canvas, screen); break;
    default: draw_player(module, canvas, screen); break;
    }
}
