// SPDX-License-Identifier: MIT
// Station catalog format and duplicate checks adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "radio.h"

#include <string.h>

#define STATION_LIMIT 128
#define STATION_FILE "stations.txt"
#define UUID_COMMENT "#RADIOBROWSERUUID:"

static radio_station_t saved[STATION_LIMIT];
static unsigned count;
static char contents[GADGET_STATE_FILE_LIMIT + 1];
static size_t length;
static bool unreadable;

bool radio_uuid_valid(const char *uuid)
{
    if (uuid == NULL || radio_text_length(uuid, RADIO_UUID_BYTES) != RADIO_UUID_BYTES - 1) {
        return false;
    }
    for (unsigned i = 0; i < RADIO_UUID_BYTES - 1; ++i) {
        const unsigned char c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static char lower_hex(char ch)
{
    return ch >= 'A' && ch <= 'F' ? ch + ('a' - 'A') : ch;
}

static bool same_station(const radio_station_t *a, const radio_station_t *b)
{
    if (a->uuid[0] != '\0' && b->uuid[0] != '\0') {
        bool same = true;
        for (unsigned i = 0; i < RADIO_UUID_BYTES - 1; ++i) {
            same &= lower_hex(a->uuid[i]) == lower_hex(b->uuid[i]);
        }
        if (same) {
            return true;
        }
    }
    const size_t size = radio_text_length(a->url, RADIO_URL_BYTES);
    return size != 0 && size < RADIO_URL_BYTES && size == radio_text_length(b->url, RADIO_URL_BYTES) &&
           memcmp(a->url, b->url, size) == 0;
}

static bool duplicate(const radio_station_t *station)
{
    for (unsigned i = 0; i < count; ++i) {
        if (same_station(station, &saved[i])) {
            return true;
        }
    }
    return false;
}

static bool field_ok(const char *field, size_t capacity, bool url)
{
    const size_t size = radio_text_length(field, capacity);
    if (size == 0 || size == capacity || field[0] == ' ' || field[size - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = field[i];
        if (c < (url ? 33 : 32) || c == 127 || (c == ':' && size - i >= 3 && memcmp(field + i, ":::", 3) == 0)) {
            return false;
        }
    }
    return true;
}

static char *trim(char *value)
{
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    size_t size = radio_text_length(value, 1024);
    while (size != 0 && (value[size - 1] == ' ' || value[size - 1] == '\t' || value[size - 1] == '\r')) {
        value[--size] = '\0';
    }
    return value;
}

static bool parse_record(char *line, radio_station_t *station, bool *mp3)
{
    char *fields[5] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        char *separator = line;
        while (*separator != '\0' &&
               !(separator[0] == ':' && separator[1] == ':' && separator[2] == ':')) {
            ++separator;
        }
        if (*separator == '\0') {
            if (i != 3) {
                return false;
            }
            fields[3] = trim(line);
            line = separator;
            break;
        }
        *separator = '\0';
        fields[i] = trim(line);
        line = separator + 3;
    }
    fields[4] = trim(line);
    if (!field_ok(fields[0], RADIO_NAME_BYTES, false) || !radio_http_url(fields[1]) ||
        !field_ok(fields[1], RADIO_URL_BYTES, true)) {
        return false;
    }
    if (fields[2][0] != '\0') {
        if (radio_text_length(fields[2], 3) != 2 || fields[2][0] < 'A' || fields[2][0] > 'Z' ||
            fields[2][1] < 'A' || fields[2][1] > 'Z') {
            return false;
        }
        memcpy(station->country, fields[2], sizeof(station->country));
    }
    unsigned bitrate = 0;
    for (const char *p = fields[4]; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        bitrate = bitrate * 10 + (unsigned)(*p - '0');
        if (bitrate > 10000) {
            return false;
        }
    }
    if (!field_ok(fields[3], 33, false)) {
        return false;
    }
    station->bitrate = bitrate;
    memcpy(station->name, fields[0], radio_text_length(fields[0], RADIO_NAME_BYTES) + 1);
    memcpy(station->url, fields[1], radio_text_length(fields[1], RADIO_URL_BYTES) + 1);
    *mp3 = radio_text_length(fields[3], 4) == 3 && memcmp(fields[3], "MP3", 3) == 0;
    return true;
}

static bool parse_catalog(void)
{
    count = 0;
    char uuid[RADIO_UUID_BYTES] = {0};
    unsigned records = 0, invalid = 0;
    for (size_t start = 0, end; start < length; start = end + 1) {
        end = start;
        while (end < length && contents[end] != '\n') {
            ++end;
        }
        char buffer[1024];
        const size_t size = end - start;
        if (size >= sizeof(buffer)) {
            ++invalid;
            uuid[0] = '\0';
            continue;
        }
        memcpy(buffer, contents + start, size);
        buffer[size] = '\0';
        char *line = trim(buffer);
        if (line[0] == '#' || line[0] == '\0') {
            if (radio_text_length(line, sizeof(UUID_COMMENT) - 1) == sizeof(UUID_COMMENT) - 1 &&
                memcmp(line, UUID_COMMENT, sizeof(UUID_COMMENT) - 1) == 0) {
                const char *value = line + sizeof(UUID_COMMENT) - 1;
                if (radio_uuid_valid(value)) {
                    memcpy(uuid, value, sizeof(uuid));
                } else {
                    uuid[0] = '\0';
                }
            }
            continue;
        }

        radio_station_t station = {0};
        bool mp3 = false;
        if (parse_record(line, &station, &mp3)) {
            ++records;
            if (mp3 && count < STATION_LIMIT) {
                memcpy(station.uuid, uuid, sizeof(uuid));
                if (!duplicate(&station)) {
                    saved[count++] = station;
                }
            }
        } else {
            ++invalid;
        }
        uuid[0] = '\0';
    }
    return records != 0 || invalid == 0;
}

void radio_load_stations(gadget_module_context_t *module)
{
    unreadable = false;
    for (gadget_state_copy_t copy = GADGET_STATE_CURRENT; copy <= GADGET_STATE_BACKUP; ++copy) {
        const int size = module->host->state_read_file(module, STATION_FILE, copy, contents, sizeof(contents));
        if (copy == GADGET_STATE_CURRENT && size == GADGET_STATE_FILE_UNREADABLE) {
            unreadable = true;
        }
        if (size >= 0) {
            length = (size_t)size;
            if (parse_catalog()) {
                return;
            }
            module->host->log(module, "Unreadable station catalog copy");
        }
    }
    // Keep a manually edited catalog intact even if every record is unplayable.
    const int size = module->host->state_read_file(module, STATION_FILE, GADGET_STATE_CURRENT, contents, sizeof(contents));
    length = size >= 0 ? (size_t)size : 0;
    count = 0;
}

unsigned radio_saved_count(void)
{
    return count;
}

const radio_station_t *radio_saved_station(unsigned index)
{
    return index < count ? &saved[index] : NULL;
}

radio_save_t radio_save_station(gadget_module_context_t *module, const radio_station_t *station)
{
    if (duplicate(station)) {
        return RADIO_DUPLICATE;
    }
    if (unreadable || count == STATION_LIMIT || station->name[0] == '#' || !field_ok(station->name, RADIO_NAME_BYTES, false) ||
        !radio_http_url(station->url) || !field_ok(station->url, RADIO_URL_BYTES, true)) {
        return RADIO_SAVE_FAILED;
    }
    const bool uuid = radio_uuid_valid(station->uuid);
    const size_t available = sizeof(contents) - length;
    const int written = module->host->format(contents + length, available, "%s%s%s%s%s:::%s:::%s:::MP3:::%u\n",
                          length && contents[length - 1] != '\n' ? "\n" : "",
                          uuid ? UUID_COMMENT : "", uuid ? station->uuid : "", uuid ? "\n" : "",
                          station->name, station->url, station->country, (unsigned)station->bitrate);
    if (written < 0 || (size_t)written >= available ||
        !module->host->state_write_file(module, STATION_FILE, contents, length + (size_t)written)) {
        contents[length] = '\0';
        return RADIO_SAVE_FAILED;
    }
    length += (size_t)written;
    saved[count++] = *station;
    return RADIO_SAVED;
}
