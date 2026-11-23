// SPDX-License-Identifier: MIT
// Download/decode separation and retry policy adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "radio.h"
#include "icy.h"
#include "vendor/minimp3/minimp3.h"

#include <stdatomic.h>
#include <string.h>

#define RING_BYTES (256u * 1024)
#define HTTP_BYTES 1024u
#define HTTP_READ_MS 200u
#define DECODE_BYTES 16384u
#define SYNC_BYTES 8192u
#define SYNC_TAIL 2048u
#define MAX_INVALID_BYTES (64u * 1024)
#define STALL_MS 4000u
#define STATUS_BITS 4u
#define STATUS_MASK ((1u << STATUS_BITS) - 1)

_Static_assert(RADIO_OUTPUT_ERROR <= STATUS_MASK, "Playback status must fit in its atomic field");

int radio_mp3_frame_bytes(const mp3dec_t *dec, const uint8_t *data);

typedef struct {
    unsigned serial;
    char url[RADIO_URL_BYTES];
} tune_request_t;

typedef struct {
    unsigned id, serial, position;
    char text[RADIO_TITLE_BYTES];
} stream_title_t;

static tune_request_t request;
// Publish the tune generation and the status together so an old worker cannot undo Stop.
static atomic_uint playback;
static atomic_uint levels = 60 | (45 << 8);

// The downloader alone advances head. The decoder alone advances tail.
// A new stream waits for the decoder to retire the previous stream's bytes.
static uint8_t ring[RING_BYTES];
static atomic_uint head, tail, stream, stream_request, acknowledged;
static atomic_uint failed_stream, failed_status, playing_stream;

static radio_icy_t icy;
static stream_title_t wire_title, decoded_title;
static atomic_uint title_revision;

// Decoder-owned state and scratch. minimp3 keeps its largest scratch array on this worker's stack.
static mp3dec_t decoder;
static uint8_t decode_input[DECODE_BYTES];
static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

void radio_tune(gadget_module_context_t *module, const char *url)
{
    tune_request_t next = {.serial = (request.serial + 1) & (UINT32_MAX >> STATUS_BITS)};
    if (url != NULL) {
        const size_t size = radio_text_length(url, sizeof(next.url) - 1);
        memcpy(next.url, url, size);
    }
    module->host->copy_shared(module, &request, &next, sizeof(next));
    atomic_store(&playback, (next.serial << STATUS_BITS) | (url != NULL ? RADIO_CONNECTING : RADIO_STOPPED));
    module->host->wake_worker(module);
}

void radio_set_levels(unsigned speaker, unsigned headphones, bool muted)
{
    atomic_store(&levels, muted ? 0 : (speaker | (headphones << 8)));
}

radio_playback_t radio_playback(void)
{
    return (radio_playback_t)(atomic_load(&playback) & STATUS_MASK);
}

static unsigned requested_serial(void)
{
    return atomic_load(&playback) >> STATUS_BITS;
}

void radio_stream_title(gadget_module_context_t *module, char out[RADIO_TITLE_BYTES])
{
    stream_title_t title;
    module->host->copy_shared(module, &title, &decoded_title, sizeof(title));
    out[0] = '\0';
    if (title.id != 0 && title.id == atomic_load(&stream) && title.serial == requested_serial()) {
        memcpy(out, title.text, sizeof(title.text));
    }
}

static void report(gadget_module_context_t *module, unsigned serial, radio_playback_t next)
{
    unsigned current = atomic_load(&playback);
    const unsigned desired = (serial << STATUS_BITS) | next;
    while (current >> STATUS_BITS == serial && current != desired) {
        if (atomic_compare_exchange_weak(&playback, &current, desired)) {
            module->host->invalidate(module);
            if (next == RADIO_OFFLINE || next == RADIO_RETRYING) {
                module->host->log(module, next == RADIO_OFFLINE
                                  ? "Stream offline. Retrying Wi-Fi." : "Stream ended. Retrying.");
            }
            return;
        }
    }
}

static bool cancelled(gadget_module_context_t *module, unsigned serial)
{
    return requested_serial() != serial || module->host->wait_for_stop(module, 0);
}

static void wait_for_command(gadget_module_context_t *module, uint32_t delay)
{
    module->host->wait_for_stop(module, delay);
}

static void write_ring(const uint8_t *data, unsigned count)
{
    const unsigned position = atomic_load_explicit(&head, memory_order_relaxed);
    const unsigned start = position % RING_BYTES;
    const unsigned first = RING_BYTES - start < count ? RING_BYTES - start : count;
    memcpy(ring + start, data, first);
    memcpy(ring, data + first, count - first);
    atomic_store_explicit(&head, position + count, memory_order_release);
}

static unsigned read_ring(uint8_t *data, unsigned capacity)
{
    const unsigned position = atomic_load_explicit(&tail, memory_order_relaxed);
    const unsigned available = atomic_load_explicit(&head, memory_order_acquire) - position;
    const unsigned count = available < capacity ? available : capacity;
    const unsigned start = position % RING_BYTES;
    const unsigned first = RING_BYTES - start < count ? RING_BYTES - start : count;
    memcpy(data, ring + start, first);
    memcpy(data + first, ring, count - first);
    atomic_store_explicit(&tail, position + count, memory_order_release);
    return count;
}

typedef struct {
    gadget_module_context_t *module;
    stream_title_t last_title;
} download_context_t;

static void receive_title(void *argument, const char *text)
{
    download_context_t *context = argument;
    stream_title_t *title = &context->last_title;
    const size_t size = radio_text_length(text, RADIO_TITLE_BYTES - 1);
    if (size == radio_text_length(title->text, sizeof(title->text)) && memcmp(text, title->text, size) == 0) {
        return;
    }
    memcpy(title->text, text, size + 1);
    title->position = atomic_load_explicit(&head, memory_order_relaxed);
    context->module->host->copy_shared(context->module, &wire_title, title, sizeof(*title));
    atomic_fetch_add(&title_revision, 1);
}

static void update_title(gadget_module_context_t *module, unsigned id, unsigned position, unsigned *revision)
{
    const unsigned published = atomic_load(&title_revision);
    if (published != *revision) {
        stream_title_t title;
        module->host->copy_shared(module, &title, &wire_title, sizeof(title));
        if (title.id != id) {
            *revision = published;
        } else if (position - title.position < (1u << 31)) {
            *revision = published;
            module->host->copy_shared(module, &decoded_title, &title, sizeof(title));
            module->host->invalidate(module);
        }
    }
}

static void download(gadget_module_context_t *module, unsigned serial, unsigned id,
                     gadget_http_stream_t *source, uint32_t icy_interval)
{
    uint8_t block[HTTP_BYTES];
    download_context_t context = {.module = module, .last_title = {.id = id, .serial = serial}};
    radio_icy_reset(&icy, icy_interval);
    uint64_t last_byte = module->host->uptime_ms();
    uint64_t report_at = last_byte + 5000;
    unsigned low = RING_BYTES;
    while (!cancelled(module, serial) && atomic_load(&failed_stream) != id) {
        const unsigned used = atomic_load_explicit(&head, memory_order_relaxed) -
                              atomic_load_explicit(&tail, memory_order_acquire);
        const uint64_t now = module->host->uptime_ms();
        if (used < low) {
            low = used;
        }
        if (now >= report_at) {
            char line[80];
            module->host->format(line, sizeof(line), "MP3 buffer %u bytes, low %u bytes", used, low);
            module->host->log(module, line);
            report_at = now + 5000;
            low = used;
        }
        if (RING_BYTES - used < sizeof(block)) {
            // Backpressure on a full ring should never be counted as a stalled connection.
            last_byte = now;
            wait_for_command(module, 10);
            continue;
        }

        const int count = module->host->http_read_timed(module, source, block, sizeof(block), HTTP_READ_MS);
        if (count > 0) {
            radio_icy_feed(&icy, block, (unsigned)count, write_ring, receive_title, &context);
            last_byte = module->host->uptime_ms();
        } else if (count != GADGET_HTTP_AGAIN || module->host->uptime_ms() - last_byte >= STALL_MS) {
            break;
        }
    }
}

void radio_network_worker(gadget_module_context_t *module)
{
    unsigned last_serial = 0, id = 0, retry_ms = 1000;
    while (!module->host->wait_for_stop(module, 0)) {
        const unsigned serial = requested_serial();
        tune_request_t next;
        module->host->copy_shared(module, &next, &request, sizeof(next));
        if (serial != next.serial) {
            continue;
        }
        if (last_serial != serial) {
            last_serial = serial;
            retry_ms = 1000;
        }
        if (next.url[0] == '\0') {
            wait_for_command(module, 60000);
            continue;
        }

        report(module, serial, RADIO_CONNECTING);
        int code = 0;
        uint32_t icy_interval = 0;
        gadget_http_stream_t *source = GADGET_HOST_HAS(module->host, http_open_icy)
                                      ? module->host->http_open_icy(module, next.url, &code, &icy_interval)
                                      : module->host->http_open(module, next.url, &code);
        radio_playback_t result = RADIO_RETRYING;
        if (source != NULL) {
            if (code == 200 && !cancelled(module, serial)) {
                if (++id == 0) {
                    ++id;
                }
                atomic_store(&stream_request, serial);
                atomic_store(&stream, id);
                report(module, serial, RADIO_BUFFERING);
                module->host->wake_worker(module);
                while (atomic_load(&acknowledged) != id && !cancelled(module, serial)) {
                    wait_for_command(module, 10);
                }
                download(module, serial, id, source, icy_interval);
                atomic_store(&stream, 0);
                module->host->wake_worker(module);
                if (atomic_load(&playing_stream) == id) {
                    retry_ms = 1000;
                }
                if (atomic_load(&failed_stream) == id) {
                    result = (radio_playback_t)atomic_load(&failed_status);
                }
            }
            module->host->http_close(module, source);
        }
        if (cancelled(module, serial)) {
            continue;
        }
        if (result == RADIO_RETRYING && !module->host->network_connected(module)) {
            result = RADIO_OFFLINE;
        }
        report(module, serial, result);
        wait_for_command(module, retry_ms);
        if (retry_ms < 16000) {
            retry_ms *= 2;
        }
    }
    atomic_store(&stream, 0);
}

static bool stream_current(gadget_module_context_t *module, unsigned id, unsigned serial)
{
    return atomic_load(&stream) == id && !cancelled(module, serial);
}

static void update_output(gadget_module_context_t *module, unsigned id, unsigned serial, unsigned *gain)
{
    const unsigned next = atomic_load(&levels);
    if (next != *gain) {
        module->host->audio_levels(module, next & 0xffu, next >> 8);
        *gain = next;
    }
    if (atomic_load(&stream) != id) {
        return;
    }
    if (module->host->audio_playing(module)) {
        if (atomic_exchange(&playing_stream, id) != id) {
            module->host->log(module, "Playing live");
        }
        report(module, serial, RADIO_PLAYING);
    } else {
        report(module, serial, RADIO_BUFFERING);
    }
}

static bool queue_pcm(gadget_module_context_t *module, unsigned id, unsigned serial,
                      int frames, int channels, unsigned *gain)
{
    if (channels == 1) {
        for (int i = frames; i-- > 0;) {
            const int16_t sample = pcm[i];
            pcm[2 * i] = sample;
            pcm[2 * i + 1] = sample;
        }
    }
    for (int sent = 0; sent < frames;) {
        if (!stream_current(module, id, serial)) {
            return false;
        }
        const int wrote = module->host->audio_write(module, pcm + 2 * sent, (size_t)(frames - sent));
        if (wrote < 0 || wrote > frames - sent) {
            return false;
        }
        update_output(module, id, serial, gain);
        sent += wrote;
        if (wrote == 0) {
            wait_for_command(module, 10);
        }
    }
    return true;
}

static radio_playback_t play_stream(gadget_module_context_t *module, unsigned id, unsigned serial)
{
    mp3dec_init(&decoder);
    unsigned start = 0, used = 0, skipped = 0;
    unsigned gain = atomic_load(&levels);
    unsigned title_seen = 0;
    int rate = 0;
    radio_playback_t error = RADIO_STOPPED;
    while (stream_current(module, id, serial)) {
        if (used < DECODE_BYTES / 2) {
            if (start != 0) {
                memmove(decode_input, decode_input + start, used);
                start = 0;
            }
            used += read_ring(decode_input + used, DECODE_BYTES - used);
        }
        if (used < 4) {
            if (rate != 0) {
                update_output(module, id, serial, &gain);
            }
            wait_for_command(module, 10);
            continue;
        }

        // Once synchronized, decode as soon as a complete frame is available.
        // Batch input only when scanning for a new MP3 header.
        const unsigned frame_bytes = (unsigned)radio_mp3_frame_bytes(&decoder, decode_input + start);
        const unsigned needed = frame_bytes == 0 ? SYNC_BYTES : frame_bytes;
        if (used < needed) {
            if (rate != 0) {
                update_output(module, id, serial, &gain);
            }
            wait_for_command(module, 10);
            continue;
        }

        mp3dec_frame_info_t info = {0};
        const int frames = mp3dec_decode_frame(&decoder, decode_input + start,
                                                frame_bytes == 0 ? used : frame_bytes, pcm, &info);
        unsigned consumed = info.frame_bytes > 0 ? (unsigned)info.frame_bytes : 0;
        if (info.layer == 0 && consumed == used && used > SYNC_TAIL) {
            consumed -= SYNC_TAIL;
        }
        if (consumed == 0 || consumed > used) {
            error = RADIO_FORMAT_ERROR;
            break;
        }
        used -= consumed;
        start += consumed;
        if (frames == 0) {
            skipped += consumed;
            if (skipped >= MAX_INVALID_BYTES) {
                error = RADIO_FORMAT_ERROR;
                break;
            }
            continue;
        }
        skipped = 0;
        if (info.layer != 3 || frames > MINIMP3_MAX_SAMPLES_PER_FRAME / 2 ||
            info.channels < 1 || info.channels > 2 || info.hz < 8000 || info.hz > 48000) {
            error = RADIO_FORMAT_ERROR;
            break;
        }
        if (info.hz != rate) {
            if (rate != 0) {
                module->host->audio_close(module);
            }
            gain = atomic_load(&levels);
            if (!module->host->audio_open_buffered(module, (uint32_t)info.hz, gain & 0xffu)) {
                error = RADIO_OUTPUT_ERROR;
                break;
            }
            module->host->audio_levels(module, gain & 0xffu, gain >> 8);
            rate = info.hz;
        }
        if (!queue_pcm(module, id, serial, frames, info.channels, &gain)) {
            error = RADIO_OUTPUT_ERROR;
            break;
        }
        // Display a title after its position reaches decoded audio, rather than on network arrival.
        update_title(module, id, atomic_load_explicit(&tail, memory_order_relaxed) - used, &title_seen);
    }
    module->host->audio_close(module);
    return error;
}

void radio_decode_worker(gadget_module_context_t *module)
{
    unsigned previous = 0;
    while (!module->host->wait_for_stop(module, 0)) {
        const unsigned id = atomic_load(&stream);
        const unsigned serial = atomic_load(&stream_request);
        if (id == 0 || id == previous || !stream_current(module, id, serial)) {
            wait_for_command(module, 100);
            continue;
        }
        previous = id;
        atomic_store_explicit(&tail, atomic_load_explicit(&head, memory_order_acquire), memory_order_release);
        atomic_store(&acknowledged, id);
        const radio_playback_t error = play_stream(module, id, serial);
        if (error != RADIO_STOPPED && stream_current(module, id, serial)) {
            atomic_store(&failed_status, error);
            atomic_store(&failed_stream, id);
            report(module, serial, error);
        }
    }
}
