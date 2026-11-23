// SPDX-License-Identifier: MIT
// ICY framing and title normalization adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "icy.h"

#include <string.h>

static bool whitespace(uint8_t byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

static uint8_t lower_ascii(uint8_t byte)
{
    return byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte;
}

static bool key_byte(uint8_t byte)
{
    const uint8_t lower = lower_ascii(byte);
    return (lower >= 'a' && lower <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
}

static size_t utf8_size(const uint8_t *text, size_t length)
{
    const uint8_t first = text[0];
    if (first < 0x80) {
        return 1;
    }
    const size_t width = first >= 0xc2 && first <= 0xdf ? 2 :
                         first >= 0xe0 && first <= 0xef ? 3 :
                         first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (width == 0 || width > length) {
        return 0;
    }
    for (size_t i = 1; i < width; ++i) {
        if ((text[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    if ((first == 0xe0 && text[1] < 0xa0) || (first == 0xed && text[1] >= 0xa0) ||
        (first == 0xf0 && text[1] < 0x90) || (first == 0xf4 && text[1] >= 0x90)) {
        return 0;
    }
    return width;
}

static void copy_title(char out[RADIO_TITLE_BYTES], const uint8_t *text, size_t length)
{
    size_t used = 0;
    while (length != 0 && *text != 0) {
        size_t width = utf8_size(text, length);
        if (width == 0) {
            // Some ICY streams still send ISO-8859-1.
            if (*text >= 0xa0) {
                if (RADIO_TITLE_BYTES - used < 3) {
                    break;
                }
                out[used++] = (char)(0xc0 | (*text >> 6));
                out[used++] = (char)(0x80 | (*text & 0x3f));
            }
            width = 1;
        } else if (width == 1 && (*text <= ' ' || *text == 0x7f)) {
            if (used != 0 && out[used - 1] != ' ' && RADIO_TITLE_BYTES - used >= 2) {
                out[used++] = ' ';
            }
        } else {
            if (width >= RADIO_TITLE_BYTES - used) {
                break;
            }
            memcpy(out + used, text, width);
            used += width;
        }
        text += width;
        length -= width;
    }
    if (used != 0 && out[used - 1] == ' ') {
        --used;
    }
    out[used] = '\0';
}

static bool title_key(const uint8_t *key, size_t size)
{
    static const char wanted[] = "streamtitle";
    if (size != sizeof(wanted) - 1) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        if (lower_ascii(key[i]) != (uint8_t)wanted[i]) {
            return false;
        }
    }
    return true;
}

static void emit_title(radio_icy_t *filter, radio_icy_title_fn title, void *context)
{
    uint8_t *cursor = filter->block;
    const uint8_t *end = cursor + radio_text_length((const char *)cursor, filter->block_size);
    while (cursor < end) {
        while (cursor < end && (whitespace(*cursor) || *cursor == ';')) {
            ++cursor;
        }
        const uint8_t *key = cursor;
        while (cursor < end && key_byte(*cursor)) {
            ++cursor;
        }
        const size_t key_size = cursor - key;
        while (cursor < end && whitespace(*cursor)) {
            ++cursor;
        }
        if (key_size == 0 || cursor == end || *cursor++ != '=') {
            return;
        }
        while (cursor < end && whitespace(*cursor)) {
            ++cursor;
        }
        if (cursor == end || (*cursor != '\'' && *cursor != '"')) {
            return;
        }
        const uint8_t quote = *cursor++;
        uint8_t *value = cursor;
        while (cursor < end) {
            if (*cursor == '\\' && cursor + 1 < end && cursor[1] == quote) {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                const uint8_t *after = cursor + 1;
                while (after < end && whitespace(*after)) {
                    ++after;
                }
                if (after == end || *after == ';') {
                    break;
                }
            }
            ++cursor;
        }
        if (cursor == end) {
            return;
        }
        if (title_key(key, key_size)) {
            uint8_t *write = value;
            for (const uint8_t *read = value; read < cursor; ++read) {
                if (*read == '\\' && read + 1 < cursor && read[1] == quote) {
                    ++read;
                }
                *write++ = *read;
            }
            char clean[RADIO_TITLE_BYTES];
            copy_title(clean, value, (size_t)(write - value));
            title(context, clean);
            return;
        }
        ++cursor;
    }
}

void radio_icy_reset(radio_icy_t *filter, uint32_t interval)
{
    filter->interval = filter->audio_left = interval;
    filter->block_size = filter->block_used = 0;
}

void radio_icy_feed(radio_icy_t *filter, const uint8_t *data, unsigned size, radio_icy_audio_fn audio,
                    radio_icy_title_fn title, void *context)
{
    if (filter->interval == 0) {
        audio(data, size);
        return;
    }
    while (size != 0) {
        unsigned take = 1;
        if (filter->audio_left != 0) {
            take = size < filter->audio_left ? size : filter->audio_left;
            audio(data, take);
            filter->audio_left -= take;
        } else if (filter->block_size == 0) {
            filter->block_size = (size_t)*data * 16;
            if (filter->block_size == 0) {
                filter->audio_left = filter->interval;
            }
        } else {
            const unsigned left = (unsigned)(filter->block_size - filter->block_used);
            take = size < left ? size : left;
            memcpy(filter->block + filter->block_used, data, take);
            filter->block_used += take;
            if (filter->block_size == filter->block_used) {
                emit_title(filter, title, context);
                filter->block_size = filter->block_used = 0;
                filter->audio_left = filter->interval;
            }
        }
        data += take;
        size -= take;
    }
}
