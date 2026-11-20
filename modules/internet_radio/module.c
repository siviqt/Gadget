// SPDX-License-Identifier: MIT
#include "radio.h"

#include <string.h>

static radio_station_t presets[] = {
    {.name = "Groove Salad", .url = "https://ice5.somafm.com/groovesalad-128-mp3"},
    {.name = "Drone Zone", .url = "https://ice5.somafm.com/dronezone-128-mp3"},
    {.name = "Indie Pop Rocks!", .url = "https://ice5.somafm.com/indiepop-128-mp3"},
    {.name = "", .url = ""},
};
static const char *const status_text[] = {
    [RADIO_STOPPED] = "Stopped",
    [RADIO_CONNECTING] = "Connecting...",
    [RADIO_BUFFERING] = "Buffering...",
    [RADIO_PLAYING] = "Playing live",
    [RADIO_OFFLINE] = "Offline. Retrying Wi-Fi...",
    [RADIO_RETRYING] = "Stream interrupted. Retrying...",
    [RADIO_FORMAT_ERROR] = "MP3 stream unavailable. Retrying...",
    [RADIO_OUTPUT_ERROR] = "Audio output unavailable. Retrying...",
};

typedef enum { PLAYER, SEARCH_EDIT, SEARCH_RESULTS } view_t;

static unsigned preset_count = sizeof(presets) / sizeof(presets[0]) - 1;
static unsigned station_index, volume[2], row;
static radio_station_t station;
static view_t view;
static bool requested, searched;
static const char *notice;
static char query[RADIO_QUERY_BYTES];
static size_t query_length;

static const uint32_t accent = 0xffedb664, text = 0xfff4f7fb, muted = 0xffaebfd3;
static const int margin = 28;
static const char *const volume_keys[] = {"speaker_volume", "headphone_volume"};

#define OTHER_STATION UINT32_MAX

static unsigned catalog_size(void)
{
    return preset_count + radio_saved_count();
}

static const radio_station_t *catalog_station(unsigned index)
{
    return index < preset_count ? &presets[index] : radio_saved_station(index - preset_count);
}

static unsigned catalog_index(const char *url)
{
    const size_t length = radio_text_length(url, RADIO_URL_BYTES);
    for (unsigned index = 0; index < catalog_size(); ++index) {
        const char *stored = catalog_station(index)->url;
        if (length == radio_text_length(stored, RADIO_URL_BYTES) && memcmp(stored, url, length) == 0) {
            return index;
        }
    }
    return OTHER_STATION;
}

static bool init(gadget_module_context_t *module)
{
    if (!GADGET_HOST_HAS(module->host, audio_headphones)) {
        return false;
    }
    const int configured = module->host->setting_int(module, "volume_percent", 60);
    const int speaker = configured >= 0 && configured <= 100 ? configured : 60;
    for (unsigned i = 0; i < 2; ++i) {
        const int fallback = i == 0 ? speaker : (speaker < 50 ? speaker : 50);
        const int level = module->host->state_int(module, volume_keys[i], fallback);
        volume[i] = level >= 0 && level <= 100 ? (unsigned)level : (unsigned)fallback;
    }
    radio_set_levels(volume[0], volume[1]);
    const char *url = module->host->setting_string(module, "station_url");
    if (radio_http_url(url)) {
        const char *name = module->host->setting_string(module, "station_name");
        radio_station_t *custom = &presets[preset_count++];
        module->host->format(custom->name, sizeof(custom->name), "%s", name != NULL ? name : "Custom stream");
        module->host->format(custom->url, sizeof(custom->url), "%s", url);
    }
    radio_load_stations(module);
    const char *previous = module->host->state_string(module, "station_url");
    if (radio_http_url(previous)) {
        station_index = catalog_index(previous);
        if (station_index == OTHER_STATION) {
            const char *name = module->host->state_string(module, "station_name");
            module->host->format(station.name, sizeof(station.name), "%s", name != NULL ? name : "Recent station");
            module->host->format(station.url, sizeof(station.url), "%s", previous);
        } else {
            station = *catalog_station(station_index);
        }
    } else {
        station = presets[0];
    }
    return true;
}

static bool service_start(gadget_module_context_t *module)
{
    return module->host->add_worker(module, "gadget_radio_net", radio_network_worker, 10240) &&
           module->host->add_worker(module, "gadget_radio_dir", radio_directory_worker, 10240);
}

static void search(gadget_module_context_t *module, unsigned offset)
{
    notice = NULL;
    row = 0;
    searched = true;
    view = SEARCH_RESULTS;
    radio_directory_request(module, query, offset);
}

static void search_edit(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (key->kind == GADGET_KEY_ENTER && query_length != 0) {
        search(module, 0);
    } else if (key->kind == GADGET_KEY_BACKSPACE) {
        if (query_length == 0) {
            view = PLAYER;
            return;
        }
        do {
            --query_length;
        } while (query_length != 0 && ((unsigned char)query[query_length] & 0xc0) == 0x80);
        query[query_length] = '\0';
    } else if (key->kind == GADGET_KEY_TEXT && !(key->modifiers & (GADGET_MOD_CTRL | GADGET_MOD_HYPER))) {
        const size_t length = radio_text_length(key->text, sizeof(key->text));
        if (query_length + length < sizeof(query)) {
            memcpy(query + query_length, key->text, length);
            query_length += length;
            query[query_length] = '\0';
        }
    }
}

static void remember_station(gadget_module_context_t *module)
{
    const bool url_saved = module->host->state_set_string(module, "station_url", station.url);
    const bool name_saved = module->host->state_set_string(module, "station_name", station.name);
    if (!url_saved || !name_saved) {
        module->host->log(module, "Unable to queue last station");
    }
}

static void save_station(gadget_module_context_t *module, const radio_station_t *candidate)
{
    switch (radio_save_station(module, candidate)) {
    case RADIO_SAVED: notice = "Added to saved stations"; break;
    case RADIO_DUPLICATE: notice = "Station already saved"; break;
    default: notice = "Unable to save station"; break;
    }
    station_index = catalog_index(station.url);
}

static void search_results(gadget_module_context_t *module, const gadget_key_t *key)
{
    radio_directory_page_t page;
    radio_directory_page(module, &page);
    switch (key->kind) {
    case GADGET_KEY_TEXT:
        if (key->text[0] == 's' || key->text[0] == 'S') {
            view = SEARCH_EDIT;
            notice = NULL;
        } else if ((key->text[0] == 'a' || key->text[0] == 'A') &&
                   page.status == DIRECTORY_RESULTS && row < page.count) {
            save_station(module, &page.rows[row]);
        }
        break;
    case GADGET_KEY_BACKSPACE: view = PLAYER; break;
    case GADGET_KEY_UP:
        if (row != 0) {
            --row;
        }
        break;
    case GADGET_KEY_DOWN:
        if (row + 1 < page.count) {
            ++row;
        }
        break;
    case GADGET_KEY_LEFT:
        if (page.status != DIRECTORY_LOADING && page.offset >= RADIO_PAGE_ROWS) {
            search(module, page.offset - RADIO_PAGE_ROWS);
        }
        break;
    case GADGET_KEY_RIGHT:
        if (page.more) {
            search(module, page.offset + RADIO_PAGE_ROWS);
        }
        break;
    case GADGET_KEY_ENTER:
        if (page.status == DIRECTORY_RESULTS && row < page.count) {
            station = page.rows[row];
            station_index = catalog_index(station.url);
            requested = true;
            remember_station(module);
            radio_tune(module, station.url);
            notice = NULL;
            view = PLAYER;
        } else if (page.status == DIRECTORY_OFFLINE || page.status == DIRECTORY_ERROR) {
            search(module, page.offset);
        }
        break;
    default: break;
    }
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (view == SEARCH_EDIT) {
        search_edit(module, key);
    } else if (view == SEARCH_RESULTS) {
        search_results(module, key);
    } else {
        switch (key->kind) {
        case GADGET_KEY_ENTER:
            requested = !requested;
            radio_tune(module, requested ? station.url : NULL);
            break;
        case GADGET_KEY_LEFT:
        case GADGET_KEY_RIGHT: {
            const unsigned count = catalog_size();
            if (station_index == OTHER_STATION) {
                station_index = key->kind == GADGET_KEY_RIGHT ? 0 : count - 1;
            } else {
                station_index = (station_index + (key->kind == GADGET_KEY_RIGHT ? 1 : count - 1)) % count;
            }
            station = *catalog_station(station_index);
            remember_station(module);
            notice = NULL;
            if (requested) {
                radio_tune(module, station.url);
            }
            break;
        }
        case GADGET_KEY_TEXT:
            if (key->text[0] == 's' || key->text[0] == 'S') {
                notice = NULL;
                view = searched ? SEARCH_RESULTS : SEARCH_EDIT;
            } else if (key->text[0] == 'a' || key->text[0] == 'A') {
                save_station(module, &station);
            } else {
                return;
            }
            break;
        default: return;
        }
    }
    module->host->invalidate(module);
}

static void event(gadget_module_context_t *module, const gadget_event_t *message)
{
    if (message->sender[0] != '\0') {
        return;
    }
    if (message->topic == GADGET_EVENT_AUDIO_JACK) {
        module->host->invalidate(module);
    } else if (message->topic == GADGET_EVENT_VOLUME_STEP && (message->value == 1 || message->value == -1)) {
        const unsigned route = module->host->audio_headphones();
        const int level = (int)volume[route] + message->value;
        if (level >= 0 && level <= 100) {
            volume[route] = (unsigned)level;
            radio_set_levels(volume[0], volume[1]);
            if (!module->host->state_set_int(module, volume_keys[route], level)) {
                module->host->log(module, "Unable to queue volume settings");
            }
            module->host->invalidate(module);
        }
    }
}

static void render_search(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    char header[96];
    const int content_width = canvas->width - 2 * margin;
    module->host->format(header, sizeof(header), "Radio Browser   %s", status_text[radio_playback()]);
    canvas->text(canvas->handle, margin, 23, 18, accent, header);
    if (view == SEARCH_EDIT) {
        canvas->text(canvas->handle, margin, 59, 24, text, "Search by station name");
        canvas->rect(canvas->handle, margin, 95, content_width, 46, 0xff253648);
        canvas->text(canvas->handle, margin + 10, 100, 22, query_length ? text : muted,
                     query_length ? query : "Type a name");
        canvas->text(canvas->handle, margin, canvas->height - 44, 16, muted,
                     "Enter  Search    Backspace  Delete / Player");
        return;
    }

    radio_directory_page_t page;
    radio_directory_page(module, &page);
    module->host->format(header, sizeof(header), "%s   /   Page %u", query, page.offset / RADIO_PAGE_ROWS + 1);
    canvas->text(canvas->handle, margin, 54, 19, text, header);
    if (page.status == DIRECTORY_RESULTS && page.count != 0) {
        const int row_pitch = (canvas->height - 176) / RADIO_PAGE_ROWS;
        for (unsigned i = 0; i < page.count; ++i) {
            const int y = 88 + (int)i * row_pitch;
            if (i == row) {
                canvas->rect(canvas->handle, margin, y, content_width, row_pitch - 8, 0xff344459);
            }
            canvas->text(canvas->handle, margin + 8, y + (row_pitch - 32) / 2, 16, i == row ? accent : text,
                         page.rows[i].name);
        }
    } else {
        const char *message = "No playable MP3 stations on this page.";
        if (page.status == DIRECTORY_LOADING) {
            message = "Searching...";
        } else if (page.status == DIRECTORY_OFFLINE) {
            message = "Offline. Press Enter to retry.";
        } else if (page.status == DIRECTORY_ERROR) {
            message = "Directory unavailable. Press Enter to retry.";
        }
        canvas->text(canvas->handle, margin, 93, 19, muted, message);
    }
    if (notice != NULL) {
        canvas->text(canvas->handle, margin, canvas->height - 75, 16, accent, notice);
    }
    canvas->text(canvas->handle, margin, canvas->height - 44, 16, muted,
                 "Enter  Play    A  Save    Arrows  Browse    S  Search    Backspace  Player");
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    if (view != PLAYER) {
        render_search(module, canvas);
        return;
    }

    const int right = canvas->width * 7 / 10;
    canvas->text(canvas->handle, margin, 28, 18, muted, "Internet Radio");
    canvas->text(canvas->handle, margin, 81, 32, text, station.name);
    const radio_playback_t state = radio_playback();
    canvas->text(canvas->handle, margin, 130, 21, state == RADIO_STOPPED ? muted : accent, status_text[state]);
    char line[48];
    const unsigned route = module->host->audio_headphones();
    const unsigned gain = volume[route];
    module->host->format(line, sizeof(line), "%s %u%%", route == 0 ? "Speaker" : "Headphones", gain);
    canvas->text(canvas->handle, right, 28, 18, text, line);
    const int bar_width = canvas->width - right - margin;
    canvas->rect(canvas->handle, right, 57, bar_width, 8, 0xff405164);
    canvas->rect(canvas->handle, right, 57, bar_width * (int)gain / 100, 8, accent);
    if (notice != NULL) {
        canvas->text(canvas->handle, margin, canvas->height - 75, 16, accent, notice);
    }
    canvas->text(canvas->handle, margin, canvas->height - 44, 16, muted,
                 "Enter  Play/Stop    Left/Right  Station    S  Search    A  Save    Vol+/−  Volume");
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .init = init,
    .render = render,
    .input = input,
    .service_start = service_start,
    .event = event,
    .worker = radio_decode_worker,
};

const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
