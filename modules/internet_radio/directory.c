// SPDX-License-Identifier: MIT
// Radio Browser request flow adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "radio.h"

#include <stdatomic.h>
#include <string.h>

#define RESPONSE_BYTES 8192
#define LOOKAHEAD_BATCHES 3u

typedef struct {
    unsigned serial;
    radio_directory_cursor_t cursor;
    radio_directory_mode_t mode;
    char query[RADIO_QUERY_BYTES];
} directory_request_t;

typedef struct {
    unsigned serial;
    radio_directory_page_t page;
} directory_response_t;

typedef struct {
    radio_station_t rows[LOOKAHEAD_BATCHES * RADIO_PAGE_ROWS];
    char query[RADIO_QUERY_BYTES];
    radio_directory_mode_t mode;
    unsigned offset, next_offset, count;
    bool valid, more;
} directory_window_t;

_Static_assert(sizeof(directory_response_t) <= GADGET_SHARED_COPY_LIMIT, "Radio directory page is too large");

static directory_request_t request;
static directory_response_t response, incoming;
static directory_window_t window;
static atomic_uint requested;
static char body[RESPONSE_BYTES + 1];

void radio_directory_request(gadget_module_context_t *module, const char *query, radio_directory_mode_t mode,
                             radio_directory_cursor_t cursor)
{
    directory_request_t next = {.serial = request.serial + 1, .cursor = cursor, .mode = mode};
    if (query != NULL) {
        memcpy(next.query, query, radio_text_length(query, sizeof(next.query) - 1));
    }
    module->host->copy_shared(module, &request, &next, sizeof(next));
    atomic_store(&requested, next.serial);
    module->host->wake_worker(module);
}

void radio_directory_page(gadget_module_context_t *module, radio_directory_page_t *page)
{
    directory_response_t snapshot;
    module->host->copy_shared(module, &snapshot, &response, sizeof(snapshot));
    if (request.serial == snapshot.serial) {
        *page = snapshot.page;
    } else {
        *page = (radio_directory_page_t){.cursor = request.cursor, .status = DIRECTORY_LOADING};
    }
}

static bool cancelled(gadget_module_context_t *module, unsigned serial)
{
    return atomic_load(&requested) != serial || module->host->wait_for_stop(module, 0);
}

static void encode_query(const char *query, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)query; *p != 0; ++p) {
        const unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            *out++ = (char)c;
        } else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 15];
        }
    }
    *out = '\0';
}

static bool ascii_letter(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static gadget_json_t *fetch_page(gadget_module_context_t *module, const char *host,
                                 const directory_request_t *job, unsigned offset)
{
    char encoded[3 * RADIO_QUERY_BYTES];
    char url[384];
    encode_query(job->query, encoded);
    const char *field = "name";
    if (job->mode == DIRECTORY_COUNTRY) {
        field = "country";
        if (ascii_letter(job->query[0]) && ascii_letter(job->query[1]) && job->query[2] == '\0') {
            field = "countrycode";
            for (unsigned i = 0; i < 2; ++i) {
                if (encoded[i] >= 'a' && encoded[i] <= 'z') {
                    encoded[i] -= 'a' - 'A';
                }
            }
        }
    }
    module->host->format(url, sizeof(url),
                         "https://%s/json/stations/search?%s=%s&hidebroken=true&order=name&limit=%u&offset=%u",
                         host, field, encoded, (unsigned)RADIO_PAGE_ROWS, offset);
    int status = 0;
    gadget_http_stream_t *stream = module->host->http_open(module, url, &status);
    if (stream == NULL) {
        return NULL;
    }
    size_t used = 0;
    int count = -1;
    const uint64_t deadline = module->host->uptime_ms() + 6000;
    while (status == 200 && used < sizeof(body) && !cancelled(module, job->serial) &&
           module->host->uptime_ms() < deadline) {
        const size_t free = sizeof(body) - used;
        count = module->host->http_read(module, stream, body + used, free < 1024 ? free : 1024);
        if (count == GADGET_HTTP_AGAIN) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
    }
    module->host->http_close(module, stream);
    if (count != 0 || used > RESPONSE_BYTES) {
        return NULL;
    }
    body[used] = '\0';
    size_t start = 0;
    while (start < used && (unsigned char)body[start] <= ' ') {
        ++start;
    }
    return body[start] == '[' ? module->host->json->parse(body, used) : NULL;
}

static void copy_label(char *out, size_t capacity, const char *name)
{
    while (*name != '\0' && (unsigned char)*name <= ' ') {
        ++name;
    }
    size_t size = radio_text_length(name, capacity - 1);
    while (size != 0 && ((unsigned char)name[size] & 0xc0) == 0x80) {
        --size;
    }
    for (size_t i = 0; i < size; ++i) {
        const unsigned char ch = (unsigned char)name[i];
        out[i] = ch < 32 || ch == 127 ? ' ' : (char)ch;
    }
    while (size != 0 && out[size - 1] == ' ') {
        --size;
    }
    out[size] = '\0';
}

static bool parse_station(const gadget_json_api_t *json, const gadget_json_t *item, bool aac_available,
                          radio_station_t *row)
{
    const char *name = json->string(json->member(item, "name"));
    const char *reported_codec = json->string(json->member(item, "codec"));
    const radio_codec_t codec = radio_codec_from_name(reported_codec);
    float hls_flag;
    const bool hls = json->number(json->member(item, "hls"), &hls_flag) && hls_flag != 0;
    if (name == NULL || (!hls && (codec == RADIO_CODEC_AUTO || (codec != RADIO_CODEC_MP3 && !aac_available)))) {
        return false;
    }
    const char *url = json->string(json->member(item, "url_resolved"));
    if (!radio_station_url_valid(url)) {
        url = json->string(json->member(item, "url"));
        if (!radio_station_url_valid(url)) {
            return false;
        }
    }
    *row = (radio_station_t){.codec = codec, .hls = hls};
    copy_label(row->name, sizeof(row->name), name);
    if (row->name[0] == '\0') {
        return false;
    }
    if (hls && codec == RADIO_CODEC_AUTO && reported_codec != NULL) {
        copy_label(row->reported_codec, sizeof(row->reported_codec), reported_codec);
        if (memcmp(row->reported_codec, "UNKNOWN", sizeof("UNKNOWN")) == 0) {
            row->reported_codec[0] = '\0';
        }
    }
    memcpy(row->url, url, radio_text_length(url, sizeof(row->url) - 1) + 1);
    const char *uuid = json->string(json->member(item, "stationuuid"));
    if (radio_uuid_valid(uuid)) {
        memcpy(row->uuid, uuid, sizeof(row->uuid));
    }
    const char *country = json->string(json->member(item, "countrycode"));
    if (country != NULL && radio_text_length(country, 3) == 2 &&
        country[0] >= 'A' && country[0] <= 'Z' && country[1] >= 'A' && country[1] <= 'Z') {
        memcpy(row->country, country, sizeof(row->country));
    }
    float rate;
    if (json->number(json->member(item, "bitrate"), &rate) && rate >= 0 && rate <= 10000) {
        row->bitrate = (uint16_t)rate;
    }
    return true;
}

static bool load_window(gadget_module_context_t *module, const char *host, const directory_request_t *job)
{
    const gadget_json_api_t *json = module->host->json;
    const bool aac_available = GADGET_HOST_HAS(module->host, aac);
    window.valid = false;
    window.count = 0;
    window.next_offset = job->cursor.offset;
    for (unsigned batch = 0; batch < LOOKAHEAD_BATCHES && !cancelled(module, job->serial); ++batch) {
        gadget_json_t *document = fetch_page(module, host, job, window.next_offset);
        if (document == NULL) {
            // A failed lookahead can leave an earlier batch available to browse.
            break;
        }
        bool playable = false;
        unsigned raw = 0;
        for (; raw < RADIO_PAGE_ROWS; ++raw) {
            const gadget_json_t *item = json->at(document, raw);
            if (item == NULL) {
                break;
            }
            radio_station_t *row = &window.rows[window.count];
            if (parse_station(json, item, aac_available, row)) {
                ++window.count;
                playable |= !row->hls;
            }
        }
        json->destroy(document);
        window.valid = true;
        window.next_offset += raw;
        window.more = raw == RADIO_PAGE_ROWS && window.next_offset <= RADIO_DIRECTORY_LAST_OFFSET;
        if (playable || !window.more) {
            break;
        }
    }
    window.offset = job->cursor.offset;
    window.mode = job->mode;
    memcpy(window.query, job->query, sizeof(window.query));
    return window.valid;
}

static void read_window_page(radio_directory_page_t *page)
{
    // Keep discovery order within each group and put playable rows first.
    unsigned position = 0;
    for (unsigned hls = 0; hls < 2 && page->count < RADIO_PAGE_ROWS; ++hls) {
        for (unsigned i = 0; i < window.count && page->count < RADIO_PAGE_ROWS; ++i) {
            const radio_station_t *row = &window.rows[i];
            if (row->hls == hls && position++ >= page->cursor.row) {
                page->rows[page->count++] = *row;
            }
        }
    }
    const unsigned next_row = page->cursor.row + page->count;
    if (next_row < window.count) {
        page->next = (radio_directory_cursor_t){.offset = page->cursor.offset, .row = next_row};
        page->more = true;
    } else {
        page->next = (radio_directory_cursor_t){.offset = window.next_offset};
        page->more = window.more;
    }
    page->status = DIRECTORY_RESULTS;
}

void radio_directory_worker(gadget_module_context_t *module)
{
    unsigned handled = 0, last_mirror = 0, mirror_count = 0;
    uint64_t expires = 0;
    char mirrors[DIRECTORY_MIRRORS][DIRECTORY_HOST_BYTES] = {{0}};
    while (!module->host->wait_for_stop(module, 0)) {
        const unsigned serial = atomic_load(&requested);
        if (serial == handled) {
            module->host->wait_for_stop(module, 60000);
            continue;
        }
        directory_request_t job;
        module->host->copy_shared(module, &job, &request, sizeof(job));
        if (job.serial != serial) {
            continue;
        }
        handled = serial;
        if (job.query[0] == '\0') {
            continue;
        }
        incoming = (directory_response_t){.serial = serial,
                                         .page = {.cursor = job.cursor, .status = DIRECTORY_ERROR}};
        const uint64_t now = module->host->uptime_ms();
        if (now >= expires) {
            mirror_count = radio_find_directory_hosts(module, mirrors);
            if (mirror_count == 0) {
                const char fallback[] = "de1.api.radio-browser.info";
                memcpy(mirrors[0], fallback, sizeof(fallback));
                mirror_count = 1;
                expires = now + 15000;
            } else {
                expires = now + 300000;
            }
            last_mirror = 0;
        }
        if (job.cursor.row != 0 && window.valid && job.cursor.offset == window.offset &&
            job.mode == window.mode && memcmp(job.query, window.query, sizeof(job.query)) == 0) {
            read_window_page(&incoming.page);
        } else {
            for (unsigned attempt = 0; attempt < mirror_count && !cancelled(module, serial); ++attempt) {
                const unsigned mirror = (last_mirror + attempt) % mirror_count;
                if (load_window(module, mirrors[mirror], &job)) {
                    read_window_page(&incoming.page);
                    last_mirror = mirror;
                    break;
                }
            }
        }
        if (!cancelled(module, serial)) {
            if (incoming.page.status == DIRECTORY_ERROR && !module->host->network_connected(module)) {
                incoming.page.status = DIRECTORY_OFFLINE;
            }
            if (incoming.page.status != DIRECTORY_RESULTS) {
                expires = 0;
            }
            module->host->copy_shared(module, &response, &incoming, sizeof(response));
            module->host->invalidate(module);
        }
    }
}
