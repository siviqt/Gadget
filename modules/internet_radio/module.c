// SPDX-License-Identifier: MIT
#include "radio.h"
#include "view.h"

#include <string.h>

static radio_station_t presets[] = {
    {.name = "Groove Salad", .url = "https://ice5.somafm.com/groovesalad-128-mp3"},
    {.name = "Drone Zone", .url = "https://ice5.somafm.com/dronezone-128-mp3"},
    {.name = "Indie Pop Rocks!", .url = "https://ice5.somafm.com/indiepop-128-mp3"},
    {.name = "", .url = ""},
};
static unsigned preset_total = sizeof(presets) / sizeof(presets[0]) - 1;
static const radio_station_t *visible_presets[sizeof(presets) / sizeof(presets[0])];
static unsigned preset_count;
static const char *const volume_keys[] = {"speaker_volume", "headphone_volume"};

typedef enum { NO_PAGE, LOAD_QUERY, LOAD_PREVIOUS, LOAD_NEXT, PREFETCH_NEXT } page_request_t;
typedef enum { NO_WRITE, SAVE_STATION, ADD_STATION, REMOVE_STATION } catalog_write_t;

static radio_view_t view;
static radio_station_t station, remove_target;
static radio_station_form_t add_form;
static unsigned selection, volume[2];
static bool requested, muted, recent_unlisted, remove_preset;
static catalog_write_t pending_write;
static const char *notice;

static char query[RADIO_QUERY_BYTES];
static size_t query_length;
static radio_directory_mode_t search_mode;
static unsigned result_row;
static radio_directory_page_t results, next_page, response;
static page_request_t page_request;

#define UNLISTED UINT32_MAX

static unsigned catalog_count(void)
{
    return preset_count + radio_saved_count();
}

static void refresh_presets(void)
{
    preset_count = 0;
    for (unsigned i = 0; i < preset_total; ++i) {
        if (!radio_preset_hidden(presets[i].url)) {
            visible_presets[preset_count++] = &presets[i];
        }
    }
}

const radio_station_t *radio_catalog_station(unsigned index)
{
    const unsigned count = catalog_count();
    if (index < preset_count) {
        return visible_presets[index];
    }
    if (index < count) {
        return radio_saved_station(index - preset_count);
    }
    return index == count && (requested || recent_unlisted) ? &station : NULL;
}

static unsigned catalog_index(const radio_station_t *candidate)
{
    for (unsigned i = 0; i < catalog_count(); ++i) {
        if (radio_same_station(candidate, radio_catalog_station(i))) {
            return i;
        }
    }
    return UNLISTED;
}

bool radio_catalog_contains(const radio_station_t *candidate)
{
    return catalog_index(candidate) != UNLISTED;
}

static unsigned station_count(void)
{
    return catalog_count() + ((requested || recent_unlisted) && !radio_catalog_contains(&station));
}

static void set_levels(void)
{
    radio_set_levels(volume[0], volume[1], muted);
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
    set_levels();

    const char *url = module->host->setting_string(module, "station_url");
    if (radio_station_url_valid(url)) {
        const char *name = module->host->setting_string(module, "station_name");
        radio_station_t *custom = &presets[preset_total++];
        module->host->format(custom->name, sizeof(custom->name), "%s", name != NULL ? name : "Custom stream");
        module->host->format(custom->url, sizeof(custom->url), "%s", url);
    }
    radio_load_stations(module);
    refresh_presets();

    const radio_station_t *first = radio_catalog_station(0);
    if (first != NULL) {
        station = *first;
    }
    const char *previous = module->host->state_string(module, "station_url");
    if (radio_http_url(previous)) {
        radio_station_t recent = {0};
        const char *name = module->host->state_string(module, "station_name");
        module->host->format(recent.name, sizeof(recent.name), "%s", name != NULL ? name : "Recent station");
        module->host->format(recent.url, sizeof(recent.url), "%s", previous);
        const unsigned index = catalog_index(&recent);
        if (index != UNLISTED) {
            station = *radio_catalog_station(index);
            selection = index;
        } else if (!radio_preset_hidden(recent.url)) {
            recent_unlisted = true;
            station = recent;
            selection = catalog_count();
        }
    }
    return true;
}

static bool service_start(gadget_module_context_t *module)
{
    return module->host->add_worker(module, "gadget_radio_net", radio_network_worker, 10240) &&
           module->host->add_worker(module, "gadget_radio_dir", radio_directory_worker, 10240);
}

static void remember_station(gadget_module_context_t *module, const radio_station_t *candidate)
{
    const bool url = module->host->state_set_string(module, "station_url", candidate != NULL ? candidate->url : "");
    const bool name = module->host->state_set_string(module, "station_name", candidate != NULL ? candidate->name : "");
    if (!url || !name) {
        module->host->log(module, "Unable to queue last station");
    }
}

static void play(gadget_module_context_t *module, const radio_station_t *candidate, bool toggle_mute)
{
    if (candidate == NULL) {
        return;
    }
    if (requested && radio_same_url(&station, candidate)) {
        if (toggle_mute) {
            muted = !muted;
            set_levels();
        }
        return;
    }
    station = *candidate;
    const unsigned index = catalog_index(&station);
    selection = index == UNLISTED ? catalog_count() : index;
    requested = true;
    recent_unlisted = index == UNLISTED;
    muted = false;
    set_levels();
    radio_tune(module, station.url);
    remember_station(module, &station);
}

static void clamp_selection(void)
{
    const unsigned count = station_count();
    if (selection >= count) {
        selection = count != 0 ? count - 1 : 0;
    }
}

static void stop(gadget_module_context_t *module)
{
    if (requested) {
        requested = false;
        muted = false;
        clamp_selection();
        if (!recent_unlisted && !radio_catalog_contains(&station)) {
            const radio_station_t *selected = radio_catalog_station(selection);
            station = selected != NULL ? *selected : (radio_station_t){0};
        }
        radio_tune(module, NULL);
    }
}

static void finish_add(void)
{
    const unsigned index = catalog_index(&add_form.station);
    if (index != UNLISTED) {
        selection = index;
    }
    view = RADIO_PLAYER_VIEW;
}

static void finish_removal(gadget_module_context_t *module)
{
    if (radio_same_station(&station, &remove_target)) {
        recent_unlisted = false;
        remember_station(module, NULL);
        if (requested) {
            selection = catalog_count();
        } else {
            const unsigned count = catalog_count();
            station = count != 0 ? *radio_catalog_station(selection < count ? selection : count - 1) :
                                   (radio_station_t){0};
        }
    }
    clamp_selection();
    view = RADIO_PLAYER_VIEW;
}

static void catalog_action(gadget_module_context_t *module, const radio_station_t *candidate, catalog_write_t action)
{
    if (pending_write != NO_WRITE || candidate == NULL) {
        return;
    }
    const bool remove = action == REMOVE_STATION;
    if (!remove && radio_catalog_contains(candidate)) {
        notice = "Already in your stations";
        return;
    }
    const radio_save_t result = remove ? radio_remove_station(module, candidate, remove_preset) :
                                        radio_save_station(module, candidate);
    switch (result) {
    case RADIO_SAVE_PENDING:
        pending_write = action;
        notice = remove ? "Removing from SD…" : "Saving to SD…";
        break;
    case RADIO_SAVE_QUEUED:
        refresh_presets();
        if (remove) {
            finish_removal(module);
        } else if (action == ADD_STATION) {
            finish_add();
        }
        notice = remove ? "SD removal queued" : "SD save queued";
        break;
    case RADIO_DUPLICATE:
        notice = "Already in your stations";
        break;
    default:
        notice = remove ? "Could not remove station. Check the SD card." : "Could not save station. Check the SD card.";
        break;
    }
}

static bool check_catalog(gadget_module_context_t *module)
{
    if (pending_write == NO_WRITE) {
        return false;
    }
    const gadget_state_write_status_t status = radio_catalog_write_status(module);
    if (status == GADGET_STATE_WRITE_PENDING) {
        return false;
    }
    const catalog_write_t action = pending_write;
    const bool remove = action == REMOVE_STATION;
    pending_write = NO_WRITE;
    if (status == GADGET_STATE_WRITE_SAVED) {
        refresh_presets();
        if (remove) {
            finish_removal(module);
        } else if (action == ADD_STATION) {
            finish_add();
        }
        notice = remove ? "Station removed" : "Station saved to SD";
    } else {
        notice = remove ? "Could not remove station. Check the SD card." : "Could not save station. Check the SD card.";
    }
    return true;
}

static void request_page(gadget_module_context_t *module, unsigned offset, page_request_t request)
{
    page_request = request;
    radio_directory_request(module, query, search_mode, offset);
}

static void clear_results(gadget_module_context_t *module)
{
    if (page_request != NO_PAGE) {
        radio_directory_request(module, NULL, search_mode, 0);
    }
    page_request = NO_PAGE;
    results.status = DIRECTORY_IDLE;
    next_page.status = DIRECTORY_IDLE;
    result_row = 0;
}

static void start_search(gadget_module_context_t *module)
{
    size_t first = 0;
    while (first < query_length && query[first] == ' ') {
        ++first;
    }
    while (query_length > first && query[query_length - 1] == ' ') {
        --query_length;
    }
    if (first == query_length) {
        query[0] = '\0';
        query_length = 0;
        return;
    }
    query_length -= first;
    memmove(query, query + first, query_length);
    query[query_length] = '\0';
    const unsigned offset = results.status == DIRECTORY_ERROR || results.status == DIRECTORY_OFFLINE ? results.offset : 0;
    results.status = DIRECTORY_IDLE;
    next_page.status = DIRECTORY_IDLE;
    request_page(module, offset, LOAD_QUERY);
}

static void maybe_prefetch(gadget_module_context_t *module)
{
    if (page_request == NO_PAGE && results.status == DIRECTORY_RESULTS && results.more && results.count > 0 &&
        result_row + 2 >= results.count && next_page.status == DIRECTORY_IDLE) {
        request_page(module, results.offset + RADIO_PAGE_ROWS, PREFETCH_NEXT);
    }
}

static bool check_directory(gadget_module_context_t *module)
{
    if (page_request == NO_PAGE) {
        return false;
    }
    radio_directory_page(module, &response);
    if (response.status == DIRECTORY_LOADING) {
        return false;
    }
    const page_request_t completed = page_request;
    page_request = NO_PAGE;
    const bool end = response.status == DIRECTORY_RESULTS && response.count == 0 && !response.more;
    if ((completed == LOAD_NEXT || completed == PREFETCH_NEXT) && end) {
        results.more = false;
    } else if (completed == PREFETCH_NEXT) {
        next_page = response;
        return false;
    } else {
        results = response;
        result_row = completed == LOAD_PREVIOUS && results.count != 0 ? results.count - 1 : 0;
        next_page.status = DIRECTORY_IDLE;
        maybe_prefetch(module);
    }
    return true;
}

static void next_results(gadget_module_context_t *module)
{
    if (results.status != DIRECTORY_RESULTS || !results.more) {
        return;
    }
    if (page_request == PREFETCH_NEXT) {
        page_request = LOAD_NEXT;
    } else if (page_request == NO_PAGE && next_page.status == DIRECTORY_RESULTS) {
        results = next_page;
        result_row = 0;
        next_page.status = DIRECTORY_IDLE;
        maybe_prefetch(module);
    } else if (page_request == NO_PAGE) {
        request_page(module, results.offset + RADIO_PAGE_ROWS, LOAD_NEXT);
    }
}

static void previous_results(gadget_module_context_t *module)
{
    if (results.status == DIRECTORY_RESULTS && results.offset >= RADIO_PAGE_ROWS &&
        (page_request == NO_PAGE || page_request == PREFETCH_NEXT)) {
        request_page(module, results.offset - RADIO_PAGE_ROWS, LOAD_PREVIOUS);
    }
}

static const radio_station_t *search_selection(void)
{
    return results.status == DIRECTORY_RESULTS && result_row < results.count ? &results.rows[result_row] : NULL;
}

static size_t previous_character(const char *value, size_t position)
{
    while (position != 0 && ((unsigned char)value[--position] & 0xc0) == 0x80) {
    }
    return position;
}

static void edit_search(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (key->kind == GADGET_KEY_TAB) {
        search_mode = search_mode == DIRECTORY_NAME ? DIRECTORY_COUNTRY : DIRECTORY_NAME;
        query_length = 0;
    } else if (key->kind == GADGET_KEY_BACKSPACE) {
        if (query_length == 0) {
            return;
        }
        query_length = previous_character(query, query_length);
    } else {
        const size_t size = radio_text_length(key->text, sizeof(key->text));
        if (size == 0 || query_length + size >= sizeof(query)) {
            return;
        }
        memcpy(query + query_length, key->text, size);
        query_length += size;
    }
    query[query_length] = '\0';
    clear_results(module);
}

static void search_input(gadget_module_context_t *module, const gadget_key_t *key)
{
    const radio_station_t *candidate = search_selection();
    switch (key->kind) {
    case GADGET_KEY_TAB:
    case GADGET_KEY_BACKSPACE:
    case GADGET_KEY_TEXT:
        edit_search(module, key);
        break;
    case GADGET_KEY_UP:
        if (candidate != NULL && result_row != 0) {
            --result_row;
        } else {
            previous_results(module);
        }
        break;
    case GADGET_KEY_DOWN:
        if (candidate != NULL && result_row + 1 < results.count) {
            ++result_row;
            maybe_prefetch(module);
        } else {
            next_results(module);
        }
        break;
    case GADGET_KEY_LEFT:
        previous_results(module);
        break;
    case GADGET_KEY_RIGHT:
        next_results(module);
        break;
    case GADGET_KEY_ENTER:
        if (page_request == NO_PAGE || page_request == PREFETCH_NEXT) {
            start_search(module);
        }
        break;
    case GADGET_KEY_F4:
        if (candidate != NULL) {
            play(module, candidate, true);
        }
        break;
    case GADGET_KEY_F5:
        if (candidate != NULL) {
            catalog_action(module, candidate, SAVE_STATION);
        }
        break;
    default:
        break;
    }
}

static bool letter(const gadget_key_t *key, char lower)
{
    return key->kind == GADGET_KEY_TEXT && key->text[1] == '\0' &&
           (key->text[0] == lower || key->text[0] == lower - 'a' + 'A');
}

static void begin_add(void)
{
    if (pending_write == NO_WRITE) {
        add_form = (radio_station_form_t){0};
        view = RADIO_ADD_VIEW;
    }
}

static size_t trim_entry(char *value, size_t capacity)
{
    size_t length = radio_text_length(value, capacity);
    size_t first = 0;
    while (first < length && value[first] == ' ') {
        ++first;
    }
    while (length > first && value[length - 1] == ' ') {
        --length;
    }
    memmove(value, value + first, length - first);
    value[length - first] = '\0';
    return length - first;
}

static void save_entry(gadget_module_context_t *module)
{
    radio_station_t *candidate = &add_form.station;
    add_form.name_cursor = trim_entry(candidate->name, sizeof(candidate->name));
    add_form.url_cursor = trim_entry(candidate->url, sizeof(candidate->url));
    if (!radio_station_name_valid(candidate->name)) {
        add_form.url_focused = false;
        notice = candidate->name[0] == '\0' ? "Enter a station name." : "Use a name without a leading # or :::.";
    } else if (!radio_station_url_valid(candidate->url)) {
        add_form.url_focused = true;
        notice = "Use a direct http:// or https:// MP3 URL.";
    } else {
        catalog_action(module, candidate, ADD_STATION);
    }
}

static void add_input(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (pending_write != NO_WRITE) {
        return;
    }
    char *value = add_form.url_focused ? add_form.station.url : add_form.station.name;
    const size_t capacity = add_form.url_focused ? RADIO_URL_BYTES : RADIO_NAME_BYTES;
    size_t *cursor = add_form.url_focused ? &add_form.url_cursor : &add_form.name_cursor;
    const size_t length = radio_text_length(value, capacity);
    switch (key->kind) {
    case GADGET_KEY_TAB: add_form.url_focused = !add_form.url_focused; break;
    case GADGET_KEY_UP: add_form.url_focused = false; break;
    case GADGET_KEY_DOWN: add_form.url_focused = true; break;
    case GADGET_KEY_LEFT:
        *cursor = previous_character(value, *cursor);
        break;
    case GADGET_KEY_RIGHT:
        if (*cursor < length) {
            do {
                ++*cursor;
            } while (((unsigned char)value[*cursor] & 0xc0) == 0x80);
        }
        break;
    case GADGET_KEY_BACKSPACE:
        if (*cursor != 0) {
            const size_t start = previous_character(value, *cursor);
            memmove(value + start, value + *cursor, length - *cursor + 1);
            *cursor = start;
            notice = NULL;
        }
        break;
    case GADGET_KEY_TEXT: {
        const size_t size = radio_text_length(key->text, sizeof(key->text));
        if (size == 0) {
            break;
        }
        if (size >= capacity - length) {
            notice = add_form.url_focused ? "Stream address is limited to 512 bytes." : "Name is limited to 96 bytes.";
            break;
        }
        memmove(value + *cursor + size, value + *cursor, length - *cursor + 1);
        memcpy(value + *cursor, key->text, size);
        *cursor += size;
        notice = NULL;
        break;
    }
    case GADGET_KEY_ENTER:
    case GADGET_KEY_F4:
        save_entry(module);
        break;
    default:
        break;
    }
}

static void player_input(gadget_module_context_t *module, const gadget_key_t *key)
{
    const unsigned count = station_count();
    switch (key->kind) {
    case GADGET_KEY_F1:
        begin_add();
        break;
    case GADGET_KEY_UP:
    case GADGET_KEY_LEFT:
        if (count != 0) {
            selection = (selection + count - 1) % count;
        }
        break;
    case GADGET_KEY_DOWN:
    case GADGET_KEY_RIGHT:
        if (count != 0) {
            selection = (selection + 1) % count;
        }
        break;
    case GADGET_KEY_ENTER:
    case GADGET_KEY_F4:
        play(module, radio_catalog_station(selection), key->kind == GADGET_KEY_F4);
        break;
    case GADGET_KEY_F5: {
        const unsigned listed = catalog_count();
        if (selection == listed && selection < count) {
            catalog_action(module, radio_catalog_station(selection), SAVE_STATION);
        } else if (listed != 0) {
            const unsigned index = catalog_index(&station);
            const unsigned next = index == UNLISTED ? 0 : (index + 1) % listed;
            play(module, radio_catalog_station(next), false);
        }
        break;
    }
    case GADGET_KEY_BACKSPACE:
    case GADGET_KEY_F6:
        if (pending_write == NO_WRITE && selection < catalog_count()) {
            remove_target = *radio_catalog_station(selection);
            remove_preset = selection < preset_count;
            view = RADIO_REMOVE_VIEW;
        }
        break;
    case GADGET_KEY_TEXT:
        if (letter(key, 's')) {
            view = RADIO_SEARCH_VIEW;
            maybe_prefetch(module);
        } else if (letter(key, 'a')) {
            if (selection == catalog_count() && selection < count) {
                catalog_action(module, radio_catalog_station(selection), SAVE_STATION);
            } else {
                begin_add();
            }
        }
        break;
    default:
        break;
    }
}

static void update(gadget_module_context_t *module, uint64_t now)
{
    (void)now;
    const bool catalog_changed = check_catalog(module);
    const bool directory_changed = check_directory(module);
    if (catalog_changed || directory_changed) {
        module->host->invalidate(module);
    }
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (key->modifiers & (GADGET_MOD_ALT | GADGET_MOD_CTRL | GADGET_MOD_HYPER)) {
        return;
    }
    const radio_view_t old_view = view;
    update(module, 0);
    if (view != old_view && key->kind != GADGET_KEY_F3) {
        return;
    }
    const bool editing = view == RADIO_ADD_VIEW || view == RADIO_REMOVE_VIEW;
    if (pending_write == NO_WRITE && !editing) {
        notice = NULL;
    }
    if (key->kind == GADGET_KEY_F3) {
        stop(module);
    } else if (key->kind == GADGET_KEY_F2 && (!editing || pending_write == NO_WRITE)) {
        if (view == RADIO_SEARCH_VIEW && page_request != NO_PAGE) {
            radio_directory_request(module, NULL, search_mode, 0);
            page_request = NO_PAGE;
        }
        view = view == RADIO_PLAYER_VIEW ? RADIO_SEARCH_VIEW : RADIO_PLAYER_VIEW;
        notice = NULL;
        if (view == RADIO_SEARCH_VIEW) {
            maybe_prefetch(module);
        }
    } else if (view == RADIO_ADD_VIEW) {
        add_input(module, key);
    } else if (view == RADIO_REMOVE_VIEW) {
        if (key->kind == GADGET_KEY_F4 || key->kind == GADGET_KEY_ENTER) {
            catalog_action(module, &remove_target, REMOVE_STATION);
        }
    } else if (view == RADIO_SEARCH_VIEW) {
        search_input(module, key);
    } else {
        player_input(module, key);
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
            set_levels();
            if (!module->host->state_set_int(module, volume_keys[route], level)) {
                module->host->log(module, "Unable to queue volume settings");
            }
            module->host->invalidate(module);
        }
    }
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    const bool headphones = module->host->audio_headphones();
    const unsigned count = station_count();
    char track[RADIO_TITLE_BYTES];
    radio_stream_title(module, track);
    const radio_screen_t screen = {
        .view = view, .playback = radio_playback(), .station = &station, .remove_target = &remove_target,
        .form = &add_form,
        .results = &results, .query = query, .notice = notice, .track = track, .search_mode = search_mode,
        .selection = selection, .station_count = count, .preset_count = preset_count,
        .result_row = result_row, .volume = volume[headphones], .headphones = headphones,
        .requested = requested, .muted = muted, .saving = pending_write != NO_WRITE, .preview = count > catalog_count(),
        .removable = selection < catalog_count() && pending_write == NO_WRITE,
        .loading = page_request != NO_PAGE && page_request != PREFETCH_NEXT,
    };
    radio_render(module, canvas, &screen);
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .init = init,
    .update = update,
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
