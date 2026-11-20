// SPDX-License-Identifier: MIT
// Open-Meteo requests and geocoding adapted from Weather, copyright (c) 2026 Weather project.
#include "forecast.h"

#include <string.h>

#define CITY_BYTES 48
#define BODY_BYTES 8192

typedef enum {
    NO_LOCATION,
    REFRESHING,
    READY,
    LOCATION_UNKNOWN,
    OFFLINE,
    PROVIDER_ERROR,
} weather_status_t;

typedef struct {
    unsigned location_revision;
    unsigned refresh_id;
    char city[CITY_BYTES];
} weather_request_t;

typedef struct {
    unsigned location_revision;
    weather_status_t status;
    bool cached;
    char label[CITY_BYTES + 5];
    char updated_at[9];
    forecast_t forecast;
} weather_view_t;

_Static_assert(sizeof(weather_request_t) <= GADGET_SHARED_COPY_LIMIT, "Weather request exceeds shared copy limit");
_Static_assert(sizeof(weather_view_t) <= GADGET_SHARED_COPY_LIMIT, "Weather view exceeds shared copy limit");

static char editing_city[CITY_BYTES];
static size_t editing_length;
static bool editing;
static weather_request_t request;
static weather_view_t shared_view;
static uint32_t refresh_ms;
static char body[BODY_BYTES + 1];  // Worker-owned response buffer.

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool init(gadget_module_context_t *module)
{
    if (!GADGET_HOST_HAS(module->host, state_set_string)) {
        return false;
    }
    const char *saved = module->host->state_string(module, "location");
    const char *configured = saved != NULL ? saved : module->host->setting_string(module, "location");
    if (configured != NULL) {
        const size_t length = bounded_length(configured, sizeof(request.city));
        if (length < sizeof(request.city)) {
            memcpy(request.city, configured, length + 1);
        }
    }
    if (saved == NULL && request.city[0] != '\0') {
        module->host->state_set_string(module, "location", request.city);
    }
    const int minutes = module->host->state_int(module, "refresh_minutes",
                                               module->host->setting_int(module, "refresh_minutes", 15));
    refresh_ms = (minutes >= 1 && minutes <= 60 ? minutes : 15) * 60000u;
    return true;
}

static void show(gadget_module_context_t *module, const weather_view_t *view)
{
    module->host->copy_shared(module, &shared_view, view, sizeof(*view));
    module->host->invalidate(module);
}

static gadget_json_t *fetch_json(gadget_module_context_t *module, const char *url)
{
    int status = 0;
    gadget_http_stream_t *stream = module->host->http_open(module, url, &status);
    if (stream == NULL) {
        return NULL;
    }
    size_t used = 0;
    int count = -1;
    if (status == 200) {
        const uint64_t deadline = module->host->uptime_ms() + 8000;
        while (used < sizeof(body) && module->host->uptime_ms() < deadline &&
               !module->host->wait_for_stop(module, 0)) {
            const size_t free = sizeof(body) - used;
            count = module->host->http_read(module, stream, body + used, free < 1024 ? free : 1024);
            if (count <= 0) {
                break;
            }
            used += (size_t)count;
        }
    }
    module->host->http_close(module, stream);
    if (count != 0 || used > BODY_BYTES) {
        return NULL;
    }
    body[used] = '\0';
    return module->host->json->parse(body, used);
}

static void encode_city(const char *city, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)city; *p != '\0'; ++p) {
        const unsigned char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            *out++ = (char)ch;
        } else {
            *out++ = '%';
            *out++ = hex[ch >> 4];
            *out++ = hex[ch & 15];
        }
    }
    *out = '\0';
}

static weather_status_t request_error(gadget_module_context_t *module)
{
    return module->host->network_connected(module) ? PROVIDER_ERROR : OFFLINE;
}

static weather_status_t resolve_city(gadget_module_context_t *module, const char *city, char *label,
                                    size_t label_size, float *latitude, float *longitude)
{
    char query[3 * CITY_BYTES];
    char url[256];
    encode_city(city, query);
    module->host->format(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en",
                         query);
    gadget_json_t *document = fetch_json(module, url);
    if (document == NULL) {
        return request_error(module);
    }

    const gadget_json_api_t *json = module->host->json;
    const gadget_json_t *place = json->at(json->member(document, "results"), 0);
    const char *name = json->string(json->member(place, "name"));
    const char *country = json->string(json->member(place, "country_code"));
    weather_status_t status = LOCATION_UNKNOWN;
    if (place != NULL && json->number(json->member(place, "latitude"), latitude) &&
        json->number(json->member(place, "longitude"), longitude) &&
        *latitude >= -90 && *latitude <= 90 && *longitude >= -180 && *longitude <= 180) {
        if (name == NULL || name[0] == '\0' || bounded_length(name, CITY_BYTES) >= CITY_BYTES) {
            name = city;
        }
        if (country != NULL && country[0] >= 'A' && country[0] <= 'Z' &&
            country[1] >= 'A' && country[1] <= 'Z' && country[2] == '\0') {
            module->host->format(label, label_size, "%s, %s", name, country);
        } else {
            module->host->format(label, label_size, "%s", name);
        }
        status = READY;
    }
    json->destroy(document);
    return status;
}

static void coordinate_text(gadget_module_context_t *module, float value, char out[12])
{
    const float absolute = value < 0 ? -value : value;
    const unsigned scaled = (unsigned)(absolute * 10000.0f + 0.5f);
    module->host->format(out, 12, "%s%u.%04u", value < 0 ? "-" : "", scaled / 10000, scaled % 10000);
}

static weather_status_t fetch_forecast(gadget_module_context_t *module, weather_view_t *view,
                                        float latitude, float longitude)
{
    char lat[12], lon[12], url[384];
    coordinate_text(module, latitude, lat);
    coordinate_text(module, longitude, lon);
    module->host->format(url, sizeof(url),
                         "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                         "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code"
                         "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                         "&forecast_days=3&timezone=auto&temperature_unit=celsius&wind_speed_unit=kmh", lat, lon);
    gadget_json_t *document = fetch_json(module, url);
    if (document == NULL) {
        return request_error(module);
    }
    const bool parsed = weather_forecast(module->host->json, document, &view->forecast);
    module->host->json->destroy(document);
    if (!parsed) {
        return PROVIDER_ERROR;
    }

    view->cached = true;
    gadget_civil_time_t now;
    view->updated_at[0] = '\0';
    if (module->host->civil_time(module, false, &now)) {
        module->host->format(view->updated_at, sizeof(view->updated_at), "%02u:%02u:%02u",
                             (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second);
    }
    return READY;
}

static void worker(gadget_module_context_t *module)
{
    weather_view_t view = {0};
    unsigned handled_refresh = 0;
    float latitude = 0, longitude = 0;
    bool resolved = false;
    uint64_t due = 0;
    while (!module->host->wait_for_stop(module, 0)) {
        weather_request_t current;
        module->host->copy_shared(module, &current, &request, sizeof(current));
        if (current.location_revision != view.location_revision) {
            view = (weather_view_t){.location_revision = current.location_revision};
            resolved = false;
            due = 0;
        }
        if (current.city[0] == '\0') {
            if (module->host->wait_for_stop(module, 60000)) {
                break;
            }
            continue;
        }

        const bool manual = current.refresh_id != handled_refresh;
        handled_refresh = current.refresh_id;
        const uint64_t now = module->host->uptime_ms();
        if (!manual && now < due &&
            (view.status != READY || module->host->network_connected(module))) {
            const uint64_t limit = view.status == READY ? 5000 : 60000;
            if (module->host->wait_for_stop(module, (uint32_t)(due - now < limit ? due - now : limit))) {
                break;
            }
            continue;
        }

        view.status = REFRESHING;
        show(module, &view);
        if (!resolved) {
            view.status = resolve_city(module, current.city, view.label, sizeof(view.label), &latitude, &longitude);
            resolved = view.status == READY;
        }
        if (resolved && !module->host->wait_for_stop(module, 0)) {
            view.status = fetch_forecast(module, &view, latitude, longitude);
        }
        if (module->host->wait_for_stop(module, 0)) {
            break;
        }

        due = module->host->uptime_ms();
        switch (view.status) {
        case READY:
            due += refresh_ms;
            module->host->log(module, "Forecast updated");
            break;
        case OFFLINE: due += 10000; break;
        case LOCATION_UNKNOWN: due = UINT64_MAX; break;
        default: due += 60000; break;
        }
        show(module, &view);
    }
}

static void refresh(gadget_module_context_t *module)
{
    weather_request_t next = request;
    ++next.refresh_id;
    module->host->copy_shared(module, &request, &next, sizeof(next));
    module->host->wake_worker(module);
}

static void event(gadget_module_context_t *module, const gadget_event_t *message)
{
    if (message->sender[0] == '\0' && message->topic == GADGET_EVENT_WEATHER_REFRESH) {
        refresh(module);
    }
}

static void save_city(gadget_module_context_t *module)
{
    size_t start = 0;
    while (start < editing_length && editing_city[start] == ' ') {
        ++start;
    }
    size_t end = editing_length;
    while (end > start && editing_city[end - 1] == ' ') {
        --end;
    }
    weather_request_t next = {.location_revision = request.location_revision + 1, .refresh_id = request.refresh_id};
    memcpy(next.city, editing_city + start, end - start);
    if (!module->host->state_set_string(module, "location", next.city)) {
        module->host->log(module, "Unable to queue weather location");
    }
    module->host->copy_shared(module, &request, &next, sizeof(next));
    editing = false;
    module->host->wake_worker(module);
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (editing) {
        if (key->kind == GADGET_KEY_ENTER) {
            save_city(module);
        } else if (key->kind == GADGET_KEY_BACKSPACE && editing_length != 0) {
            do {
                --editing_length;
            } while (editing_length != 0 && ((unsigned char)editing_city[editing_length] & 0xc0) == 0x80);
            editing_city[editing_length] = '\0';
        } else if (key->kind == GADGET_KEY_TEXT && !(key->modifiers & (GADGET_MOD_CTRL | GADGET_MOD_HYPER))) {
            const size_t length = bounded_length(key->text, sizeof(key->text));
            if (editing_length + length < sizeof(editing_city)) {
                memcpy(editing_city + editing_length, key->text, length);
                editing_length += length;
                editing_city[editing_length] = '\0';
            }
        }
    } else if (key->kind == GADGET_KEY_TEXT && (key->text[0] == 'l' || key->text[0] == 'L')) {
        editing = true;
        editing_length = 0;
        editing_city[0] = '\0';
    } else if (key->kind == GADGET_KEY_ENTER && request.city[0] != '\0') {
        refresh(module);
    } else {
        return;
    }
    module->host->invalidate(module);
}

static const uint32_t text = 0xfff4f7fb, muted = 0xffaebfd3, accent = 0xff7ee0ad;
static const int margin = 28;

static void render_forecast(gadget_module_context_t *module, const gadget_canvas_t *canvas, const forecast_t *forecast)
{
    const int left = canvas->width * 43 / 100;
    const int width = (canvas->width - left - margin) / FORECAST_DAYS;
    canvas->text(canvas->handle, left, 28, 18, muted, "3-day forecast");
    for (unsigned i = 0; i < FORECAST_DAYS; ++i) {
        const forecast_day_t *day = &forecast->days[i];
        const int x = left + (int)i * width;
        canvas->rect(canvas->handle, x, 58, width - 5, 101, 0xff253648);
        if (day->date[0] == '\0') {
            canvas->text(canvas->handle, x + 8, 85, 15, muted, "Unavailable");
            continue;
        }
        char line[24];
        canvas->text(canvas->handle, x + 8, 67, 15, muted, day->date + 5);
        canvas->text(canvas->handle, x + 8, 92, 14, text, weather_condition(day->code));
        module->host->format(line, sizeof(line), "%d / %d C", day->low, day->high);
        canvas->text(canvas->handle, x + 8, 121, 18, accent, line);
    }
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    if (editing) {
        canvas->text(canvas->handle, margin, 28, 26, text, "Set weather location");
        canvas->rect(canvas->handle, margin, 76, canvas->width - 2 * margin, 49, 0xff253648);
        canvas->text(canvas->handle, margin + 10, 83, 24, editing_length ? text : muted,
                     editing_length ? editing_city : "City or postal code");
        canvas->text(canvas->handle, margin, 144, 17, muted, "Enter  Save location     Backspace  Delete");
        return;
    }
    if (request.city[0] == '\0') {
        canvas->text(canvas->handle, margin, 28, 36, text, "No weather location");
        canvas->text(canvas->handle, margin, 88, 21, muted, "Press L to enter a city or postal code.");
        return;
    }

    weather_view_t view;
    module->host->copy_shared(module, &view, &shared_view, sizeof(view));
    if (view.location_revision != request.location_revision || view.status == NO_LOCATION) {
        view = (weather_view_t){.status = REFRESHING};
    }
    const char *city = view.label[0] ? view.label : request.city;
    canvas->text(canvas->handle, margin, 28, 24, text, city);
    char line[80];
    if (view.cached) {
        const forecast_t *data = &view.forecast;
        module->host->format(line, sizeof(line), "%d C", data->temperature);
        canvas->text(canvas->handle, margin, 59, 46, accent, line);
        canvas->text(canvas->handle, margin, 118, 20, text, weather_condition(data->code));
        if (data->humidity >= 0 && data->wind >= 0) {
            module->host->format(line, sizeof(line), "Humidity %d%%     Wind %d km/h", data->humidity, data->wind);
            canvas->text(canvas->handle, margin, 146, 16, muted, line);
        }
        render_forecast(module, canvas, data);
    }

    const char *status;
    switch (view.status) {
    case READY:
        module->host->format(line, sizeof(line), "Forecast updated%s%s", view.updated_at[0] ? " at " : "",
                             view.updated_at);
        status = line;
        break;
    case LOCATION_UNKNOWN: status = "Location not found. Press L to change it."; break;
    case OFFLINE: status = view.cached ? "Offline. Showing the last forecast." : "Offline. Waiting for Wi-Fi."; break;
    case PROVIDER_ERROR: status = view.cached ? "Update failed. Showing the last forecast." : "Weather provider unavailable."; break;
    default: status = "Refreshing forecast..."; break;
    }
    canvas->text(canvas->handle, margin, view.cached ? 173 : 91, 17, view.status == READY ? muted : accent, status);
    canvas->text(canvas->handle, margin, canvas->height - 44, 16, muted,
                 "L  Location    Enter  Refresh    Hyper + R  Refresh from any face");
    canvas->text(canvas->handle, canvas->width - 181, canvas->height - 44, 16, muted, "Data by Open-Meteo");
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .init = init,
    .input = input,
    .render = render,
    .event = event,
    .worker = worker,
};

const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
