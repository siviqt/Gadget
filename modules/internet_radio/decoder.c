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
