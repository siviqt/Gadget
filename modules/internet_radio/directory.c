// SPDX-License-Identifier: MIT
// Radio Browser request flow adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "radio.h"

#include <stdatomic.h>
#include <string.h>

#define RESPONSE_BYTES 8192
#define PAGE_LIMIT 5000

typedef struct {
    unsigned serial, offset;
    char query[RADIO_QUERY_BYTES];
} directory_request_t;

typedef struct {
    unsigned serial;
    radio_directory_page_t page;
} directory_response_t;

_Static_assert(sizeof(directory_response_t) <= GADGET_SHARED_COPY_LIMIT, "Radio directory page is too large");

static directory_request_t request;
static directory_response_t response, incoming;
static atomic_uint requested;
static char body[RESPONSE_BYTES + 1];

void radio_directory_request(gadget_module_context_t *module, const char *query, unsigned offset)
{
    directory_request_t next = {.serial = request.serial + 1, .offset = offset < PAGE_LIMIT ? offset : PAGE_LIMIT};
    memcpy(next.query, query, radio_text_length(query, sizeof(next.query) - 1));
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
        *page = (radio_directory_page_t){.offset = request.offset, .status = DIRECTORY_LOADING};
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

static gadget_json_t *fetch_page(gadget_module_context_t *module, const char *host,
                                 const directory_request_t *job)
{
    char encoded[3 * RADIO_QUERY_BYTES];
    char url[384];
    encode_query(job->query, encoded);
    module->host->format(url, sizeof(url),
                         "https://%s/json/stations/search?name=%s&codec=MP3&hidebroken=true&order=name&limit=%u&offset=%u",
                         host, encoded, RADIO_PAGE_ROWS, job->offset);
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

static void copy_station_name(char out[RADIO_NAME_BYTES], const char *name)
{
    size_t size = radio_text_length(name, RADIO_NAME_BYTES - 1);
    while (size != 0 && ((unsigned char)name[size] & 0xc0) == 0x80) {
        --size;
    }
    for (size_t i = 0; i < size; ++i) {
        const char ch = name[i];
        out[i] = (unsigned char)ch < 32 ? ' ' : ch;
    }
    out[size] = '\0';
}

static void parse_stations(const gadget_json_api_t *json, const gadget_json_t *document, radio_directory_page_t *page)
{
    unsigned raw = 0;
    for (; raw < RADIO_PAGE_ROWS; ++raw) {
        const gadget_json_t *item = json->at(document, raw);
        if (item == NULL) {
            break;
        }
        const char *name = json->string(json->member(item, "name"));
        const char *codec = json->string(json->member(item, "codec"));
        const char *url = json->string(json->member(item, "url_resolved"));
        if (!radio_http_url(url)) {
            url = json->string(json->member(item, "url"));
        }
        if (name == NULL || name[0] == '\0' || codec == NULL || radio_text_length(codec, 4) != 3 ||
            memcmp(codec, "MP3", 3) != 0 || !radio_http_url(url)) {
            continue;
        }
        radio_station_t *row = &page->rows[page->count++];
        copy_station_name(row->name, name);
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
    }
    page->more = raw == RADIO_PAGE_ROWS && page->offset < PAGE_LIMIT;
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
        incoming = (directory_response_t){.serial = serial,
                                         .page = {.offset = job.offset, .status = DIRECTORY_ERROR}};
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
        for (unsigned attempt = 0; attempt < mirror_count && !cancelled(module, serial); ++attempt) {
            const unsigned mirror = (last_mirror + attempt) % mirror_count;
            gadget_json_t *document = fetch_page(module, mirrors[mirror], &job);
            if (document != NULL) {
                parse_stations(module->host->json, document, &incoming.page);
                module->host->json->destroy(document);
                last_mirror = mirror;
                break;
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
