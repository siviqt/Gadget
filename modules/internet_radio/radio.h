#pragma once

#include "gadget_module.h"

#define RADIO_NAME_BYTES 97
#define RADIO_URL_BYTES 513
#define RADIO_UUID_BYTES 37
#define RADIO_PAGE_ROWS 4
#define RADIO_QUERY_BYTES 56
#define RADIO_TITLE_BYTES 160
#define DIRECTORY_MIRRORS 3
#define DIRECTORY_HOST_BYTES 64

typedef struct {
    char name[RADIO_NAME_BYTES];
    char url[RADIO_URL_BYTES];
    char uuid[RADIO_UUID_BYTES];
    char country[3];
    uint16_t bitrate;
} radio_station_t;

typedef enum {
    RADIO_SAVE_PENDING, RADIO_SAVE_QUEUED, RADIO_DUPLICATE, RADIO_SAVE_FAILED,
} radio_save_t;

typedef enum {
    RADIO_STOPPED, RADIO_CONNECTING, RADIO_BUFFERING, RADIO_PLAYING,
    RADIO_OFFLINE, RADIO_RETRYING, RADIO_FORMAT_ERROR, RADIO_OUTPUT_ERROR,
} radio_playback_t;

typedef enum {
    DIRECTORY_IDLE, DIRECTORY_LOADING, DIRECTORY_RESULTS, DIRECTORY_OFFLINE, DIRECTORY_ERROR,
} directory_status_t;

typedef enum { DIRECTORY_NAME, DIRECTORY_COUNTRY } radio_directory_mode_t;

typedef struct {
    unsigned offset;
    unsigned count;
    bool more;
    directory_status_t status;
    radio_station_t rows[RADIO_PAGE_ROWS];
} radio_directory_page_t;

void radio_network_worker(gadget_module_context_t *module);
void radio_decode_worker(gadget_module_context_t *module);
void radio_directory_worker(gadget_module_context_t *module);

void radio_tune(gadget_module_context_t *module, const char *url);
void radio_set_levels(unsigned speaker, unsigned headphones, bool muted);
radio_playback_t radio_playback(void);
void radio_stream_title(gadget_module_context_t *module, char out[RADIO_TITLE_BYTES]);
void radio_directory_request(gadget_module_context_t *module, const char *query, radio_directory_mode_t mode,
                             unsigned offset);
void radio_directory_page(gadget_module_context_t *module, radio_directory_page_t *page);
unsigned radio_find_directory_hosts(gadget_module_context_t *module,
                                    char hosts[DIRECTORY_MIRRORS][DIRECTORY_HOST_BYTES]);
void radio_load_stations(gadget_module_context_t *module);
unsigned radio_saved_count(void);
const radio_station_t *radio_saved_station(unsigned index);
bool radio_same_url(const radio_station_t *a, const radio_station_t *b);
bool radio_same_station(const radio_station_t *a, const radio_station_t *b);
bool radio_station_name_valid(const char *name);
bool radio_station_url_valid(const char *url);
bool radio_preset_hidden(const char *url);
radio_save_t radio_save_station(gadget_module_context_t *module, const radio_station_t *station);
radio_save_t radio_remove_station(gadget_module_context_t *module, const radio_station_t *station, bool preset);
gadget_state_write_status_t radio_catalog_write_status(gadget_module_context_t *module);
bool radio_uuid_valid(const char *uuid);

static inline size_t radio_text_length(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

static inline bool radio_http_url(const char *url)
{
    if (url == NULL) {
        return false;
    }
    const size_t length = radio_text_length(url, RADIO_URL_BYTES);
    return length >= 8 && length < RADIO_URL_BYTES && url[0] == 'h' && url[1] == 't' &&
           url[2] == 't' && url[3] == 'p' &&
           ((url[4] == ':' && url[5] == '/' && url[6] == '/') ||
            (length >= 9 && url[4] == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/'));
}
