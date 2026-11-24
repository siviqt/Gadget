// SPDX-License-Identifier: MIT
// Station catalog format and duplicate checks adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "radio.h"

#include <string.h>

#define STATION_LIMIT 128
#define STATION_FILE "stations.txt"
#define UUID_COMMENT "#RADIOBROWSERUUID:"
#define EMPTY_CATALOG "#GADGETEMPTY"
#define HIDDEN_PRESET "#GADGETHIDDENPRESET:"

static radio_station_t saved[STATION_LIMIT];
static unsigned count;
static char contents[GADGET_STATE_FILE_LIMIT + 1];
static char draft[sizeof(contents)];
static size_t length, draft_length;
static bool unreadable, writing, pending_uuid;

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

static const char *const codec_names[] = {
    [RADIO_CODEC_AUTO] = "",
    [RADIO_CODEC_MP3] = "MP3",
    [RADIO_CODEC_AAC] = "AAC",
    [RADIO_CODEC_AAC_PLUS] = "AAC+",
    [RADIO_CODEC_HE_AAC_V2] = "HE-AACv2",
};

const char *radio_codec_name(radio_codec_t codec)
{
    return (unsigned)codec < sizeof(codec_names) / sizeof(codec_names[0]) ? codec_names[codec] : "";
}

static bool same_codec_name(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const char left = *a >= 'a' && *a <= 'z' ? *a - ('a' - 'A') : *a;
        const char right = *b >= 'a' && *b <= 'z' ? *b - ('a' - 'A') : *b;
        if (left != right) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

radio_codec_t radio_codec_from_name(const char *name)
{
    if (name != NULL) {
        for (unsigned i = RADIO_CODEC_MP3; i < sizeof(codec_names) / sizeof(codec_names[0]); ++i) {
            if (same_codec_name(name, codec_names[i])) {
                return (radio_codec_t)i;
            }
        }
        if (same_codec_name(name, "AAC-LC")) {
            return RADIO_CODEC_AAC;
        }
        if (same_codec_name(name, "HE-AAC")) {
            return RADIO_CODEC_AAC_PLUS;
        }
    }
    return RADIO_CODEC_AUTO;
}

bool radio_same_url(const radio_station_t *a, const radio_station_t *b)
{
    const size_t size = radio_text_length(a->url, RADIO_URL_BYTES);
    return size != 0 && size < RADIO_URL_BYTES && size == radio_text_length(b->url, RADIO_URL_BYTES) &&
           memcmp(a->url, b->url, size) == 0;
}

bool radio_same_station(const radio_station_t *a, const radio_station_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (a->uuid[0] != '\0' && b->uuid[0] != '\0') {
        bool same = true;
        for (unsigned i = 0; i < RADIO_UUID_BYTES - 1; ++i) {
            same &= lower_hex(a->uuid[i]) == lower_hex(b->uuid[i]);
        }
        if (same) {
            return true;
        }
    }
    return radio_same_url(a, b);
}

static bool duplicate(const radio_station_t *station)
{
    for (unsigned i = 0; i < count; ++i) {
        if (radio_same_station(station, &saved[i])) {
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

bool radio_station_name_valid(const char *name)
{
    return name[0] != '#' && field_ok(name, RADIO_NAME_BYTES, false);
}

bool radio_station_url_valid(const char *url)
{
    if (!radio_http_url(url) || !field_ok(url, RADIO_URL_BYTES, true)) {
        return false;
    }
    const char first = url[url[4] == ':' ? 7 : 8];
    return first != '/' && first != '?' && first != '#';
}

bool radio_preset_hidden(const char *url)
{
    const size_t prefix = sizeof(HIDDEN_PRESET) - 1;
    const size_t size = radio_text_length(url, RADIO_URL_BYTES);
    for (size_t start = 0, end; start < length; start = end + 1) {
        end = start;
        while (end < length && contents[end] != '\n') {
            ++end;
        }
        size_t first = start, last = end;
        while (first < last && (contents[first] == ' ' || contents[first] == '\t')) {
            ++first;
        }
        while (last > first &&
               (contents[last - 1] == '\r' || contents[last - 1] == ' ' || contents[last - 1] == '\t')) {
            --last;
        }
        if (last - first == prefix + size && memcmp(contents + first, HIDDEN_PRESET, prefix) == 0 &&
            memcmp(contents + first + prefix, url, size) == 0) {
            return true;
        }
    }
    return false;
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

static bool parse_record(char *line, radio_station_t *station, bool *direct)
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
        if (i < 2) {
            // Older writers could put a field's trailing colon against the separator.
            const char *end = separator + 3;
            while (*end == ':') {
                ++end;
            }
            separator += (end - separator) % 3;
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
    station->codec = radio_codec_from_name(fields[3]);
    *direct = station->codec != RADIO_CODEC_AUTO || same_codec_name(fields[3], "AUTO");
    return true;
}

typedef struct {
    size_t position, uuid_start, uuid_end;
    unsigned invalid;
    bool empty;
    char uuid[RADIO_UUID_BYTES];
} catalog_reader_t;

typedef struct {
    radio_station_t station;
    size_t start, end, uuid_start, uuid_end;
    bool direct;
} catalog_record_t;

static bool next_record(catalog_reader_t *reader, catalog_record_t *record)
{
    while (reader->position < length) {
        const size_t start = reader->position;
        size_t end = start;
        while (end < length && contents[end] != '\n') {
            ++end;
        }
        reader->position = end < length ? end + 1 : end;
        char buffer[1024];
        const size_t size = end - start;
        if (size >= sizeof(buffer)) {
            ++reader->invalid;
            reader->uuid[0] = '\0';
            reader->empty = false;
            continue;
        }
        memcpy(buffer, contents + start, size);
        buffer[size] = '\0';
        char *line = trim(buffer);
        if (line[0] == '#' || line[0] == '\0') {
            const bool empty = radio_text_length(line, sizeof(EMPTY_CATALOG)) == sizeof(EMPTY_CATALOG) - 1 &&
                               memcmp(line, EMPTY_CATALOG, sizeof(EMPTY_CATALOG)) == 0;
            const size_t prefix = sizeof(HIDDEN_PRESET) - 1;
            const bool hidden = radio_text_length(line, prefix) == prefix &&
                                memcmp(line, HIDDEN_PRESET, prefix) == 0 &&
                                radio_station_url_valid(line + prefix);
            if (empty) {
                reader->empty = true;
            }
            if (empty || hidden) {
                reader->uuid[0] = '\0';
            } else if (radio_text_length(line, sizeof(UUID_COMMENT) - 1) == sizeof(UUID_COMMENT) - 1 &&
                memcmp(line, UUID_COMMENT, sizeof(UUID_COMMENT) - 1) == 0) {
                const char *value = line + sizeof(UUID_COMMENT) - 1;
                if (radio_uuid_valid(value)) {
                    memcpy(reader->uuid, value, sizeof(reader->uuid));
                    reader->uuid_start = start;
                    reader->uuid_end = reader->position;
                } else {
                    reader->uuid[0] = '\0';
                }
            }
            continue;
        }

        *record = (catalog_record_t){.start = start, .end = reader->position};
        reader->empty = false;
        if (parse_record(line, &record->station, &record->direct)) {
            if (reader->uuid[0] != '\0') {
                memcpy(record->station.uuid, reader->uuid, sizeof(reader->uuid));
                record->uuid_start = reader->uuid_start;
                record->uuid_end = reader->uuid_end;
            }
            reader->uuid[0] = '\0';
            return true;
        }
        ++reader->invalid;
        reader->uuid[0] = '\0';
    }
    return false;
}

static bool parse_catalog(void)
{
    count = 0;
    catalog_reader_t reader = {0};
    catalog_record_t record;
    bool any = false;
    while (next_record(&reader, &record)) {
        any = true;
        if (record.direct && count < STATION_LIMIT && !duplicate(&record.station)) {
            saved[count++] = record.station;
        }
    }
    pending_uuid = reader.uuid[0] != '\0';
    return any || reader.invalid == 0 || reader.empty;
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
    if (length != 0) {
        parse_catalog();
    } else {
        pending_uuid = false;
    }
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

static void commit_draft(void)
{
    memcpy(contents, draft, draft_length);
    contents[draft_length] = '\0';
    length = draft_length;
    parse_catalog();
}

static radio_save_t write_draft(gadget_module_context_t *module)
{
    if (GADGET_HOST_HAS(module->host, state_write_status) &&
        GADGET_HOST_HAS(module->host, state_write_file_checked)) {
        writing = module->host->state_write_file_checked(module, STATION_FILE, draft, draft_length);
        return writing ? RADIO_SAVE_PENDING : RADIO_SAVE_FAILED;
    }
    if (!module->host->state_write_file(module, STATION_FILE, draft, draft_length)) {
        return RADIO_SAVE_FAILED;
    }
    commit_draft();
    return RADIO_SAVE_QUEUED;
}

gadget_state_write_status_t radio_catalog_write_status(gadget_module_context_t *module)
{
    if (!writing) {
        return GADGET_STATE_WRITE_IDLE;
    }
    const gadget_state_write_status_t status = module->host->state_write_status(module);
    if (status == GADGET_STATE_WRITE_SAVED) {
        commit_draft();
    }
    writing = status == GADGET_STATE_WRITE_PENDING;
    return status;
}

static const char *separator_padding(const char *field, size_t capacity)
{
    const size_t size = radio_text_length(field, capacity);
    return size != 0 && field[size - 1] == ':' ? " " : "";
}

radio_save_t radio_save_station(gadget_module_context_t *module, const radio_station_t *station)
{
    if (duplicate(station)) {
        return RADIO_DUPLICATE;
    }
    if (writing || unreadable || count == STATION_LIMIT ||
        !radio_station_name_valid(station->name) || !radio_station_url_valid(station->url)) {
        return RADIO_SAVE_FAILED;
    }
    const bool uuid = radio_uuid_valid(station->uuid);
    const bool comment = uuid || pending_uuid;
    const char *codec = radio_codec_name(station->codec);
    if (codec[0] == '\0') {
        codec = GADGET_HOST_HAS(module->host, aac) ? "AUTO" : "MP3";
    }
    const size_t available = sizeof(draft) - length;
    const int written = module->host->format(draft + length, available, "%s%s%s%s%s%s:::%s%s:::%s:::%s:::%u\n",
                          length && contents[length - 1] != '\n' ? "\n" : "",
                          comment ? UUID_COMMENT : "", uuid ? station->uuid : "", comment ? "\n" : "",
                          station->name, separator_padding(station->name, sizeof(station->name)),
                          station->url, separator_padding(station->url, sizeof(station->url)),
                          station->country, codec, (unsigned)station->bitrate);
    if (written < 0 || (size_t)written >= available) {
        return RADIO_SAVE_FAILED;
    }
    memcpy(draft, contents, length);
    draft_length = length + (size_t)written;
    return write_draft(module);
}

static void append_range(size_t start, size_t end)
{
    memcpy(draft + draft_length, contents + start, end - start);
    draft_length += end - start;
}

radio_save_t radio_remove_station(gadget_module_context_t *module, const radio_station_t *station, bool preset)
{
    if (writing || unreadable || (preset && !radio_station_url_valid(station->url))) {
        return RADIO_SAVE_FAILED;
    }
    catalog_reader_t reader = {0};
    catalog_record_t record;
    size_t preserved = 0;
    draft_length = 0;
    while (next_record(&reader, &record)) {
        if (!record.direct || !radio_same_station(station, &record.station)) {
            continue;
        }
        if (record.uuid_end != 0) {
            append_range(preserved, record.uuid_start);
            preserved = record.uuid_end;
        }
        append_range(preserved, record.start);
        preserved = record.end;
    }
    if (preserved == 0 && !preset) {
        return RADIO_SAVE_FAILED;
    }
    append_range(preserved, length);
    if (preset) {
        const size_t available = sizeof(draft) - draft_length;
        const int written = module->host->format(draft + draft_length, available, "%s%s%s\n",
            draft_length && draft[draft_length - 1] != '\n' ? "\n" : "", HIDDEN_PRESET, station->url);
        if (written < 0 || (size_t)written >= available) {
            return RADIO_SAVE_FAILED;
        }
        draft_length += (size_t)written;
    }
    if ((count == 0 || (count == 1 && duplicate(station))) && draft_length != 0) {
        // Keep unrelated lines and distinguish an intentional removal from a damaged primary file.
        if (draft[draft_length - 1] != '\n') {
            draft[draft_length++] = '\n';
        }
        const char empty[] = EMPTY_CATALOG "\n";
        if (sizeof(empty) - 1 > GADGET_STATE_FILE_LIMIT - draft_length) {
            return RADIO_SAVE_FAILED;
        }
        memcpy(draft + draft_length, empty, sizeof(empty) - 1);
        draft_length += sizeof(empty) - 1;
    }
    return write_draft(module);
}
