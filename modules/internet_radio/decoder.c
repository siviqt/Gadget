// SPDX-License-Identifier: MIT
#include "radio.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "vendor/minimp3/minimp3.h"

int radio_mp3_frame_bytes(const mp3dec_t *dec, const uint8_t *data)
{
    if (dec->header[0] != 0xff || !hdr_compare(dec->header, data)) {
        return 0;
    }
    const int bytes = hdr_frame_bytes(data, dec->free_format_bytes);
    return bytes > 0 ? bytes + hdr_padding(data) : 0;
}
// ADTS sync adapted from TanRadio, copyright (c) 2026 TanRadio project.
unsigned radio_adts_frame_bytes(const uint8_t *data, unsigned size)
{
    if (size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0 || ((data[2] >> 2) & 15) > 12) {
        return 0;
    }
    const unsigned length = ((data[3] & 3) << 11) | (data[4] << 3) | (data[5] >> 5);
    return length > ((data[1] & 1) ? 7u : 9u) ? length : 0;
}

unsigned radio_adts_sync(const uint8_t *data, unsigned size)
{
    for (unsigned i = 0; i + 14 <= size; ++i) {
        const uint8_t *candidate = data + i;
        const unsigned length = radio_adts_frame_bytes(candidate, size - i);
        if (length != 0 && length + 7 <= size - i &&
            radio_adts_frame_bytes(candidate + length, size - i - length) != 0 &&
            (candidate[2] & 0x3c) == (candidate[length + 2] & 0x3c)) {
            return i;
        }
    }
    return size;
}
