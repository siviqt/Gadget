// PCM buffering and output routing follow TanRadio's audio design.
// SPDX-License-Identifier: MIT
#include "gadget_services.h"
#include "peripherals.h"

#include <limits.h>
#include <string.h>

#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define PCM_BUFFER_BYTES (256 * 1024)
#define PCM_BLOCK_FRAMES 512
#define BYTES_PER_FRAME (2 * sizeof(int16_t))
#define PREFILL_MS 850
#define REPORT_INTERVAL_MS 5000

static const char *const TAG = "gadget_audio";
static StaticSemaphore_t lock_storage, joined_storage;
static SemaphoreHandle_t lock, joined;
static StaticStreamBuffer_t queue_storage;
static StreamBufferHandle_t queue;
static uint8_t *queue_memory;
static TaskHandle_t writer, drain_task;
static const void *owner;
static i2s_chan_handle_t channel;
static uint32_t rate;
static float previous_volume;
static atomic_bool closing, playing;
enum { ROUTE_UNKNOWN, ROUTE_SPEAKER, ROUTE_HEADPHONES };
static atomic_uint audio_route, levels;
static int16_t samples[2 * PCM_BLOCK_FRAMES];

void gadget_audio_init(void)
{
    lock = xSemaphoreCreateMutexStatic(&lock_storage);
    joined = xSemaphoreCreateBinaryStatic(&joined_storage);
}

static unsigned pack_levels(unsigned speaker, unsigned headphones)
{
    const unsigned left = speaker > 100 ? 100 : speaker;
    const unsigned right = headphones > 100 ? 100 : headphones;
    return left | (right << 8);
}

void gadget_services_audio_jack(bool plugged)
{
    atomic_store(&audio_route, plugged ? ROUTE_HEADPHONES : ROUTE_SPEAKER);
}

bool gadget_services_audio_headphones(void)
{
    return atomic_load(&audio_route) == ROUTE_HEADPHONES;
}

static void write_samples(unsigned *errors)
{
    const uint8_t *bytes = (const uint8_t *)samples;
    for (size_t sent = 0; sent < sizeof(samples) && !atomic_load(&closing);) {
        size_t done = 0;
        const esp_err_t result = i2s_channel_write(channel, bytes + sent, sizeof(samples) - sent, &done, 100);
        if (result != ESP_OK || done == 0) {
            ++*errors;
            vTaskDelay(1);
            break;
        }
        sent += done;
    }
}

static void drain_pcm(void *argument)
{
    (void)argument;
    unsigned underruns = 0, errors = 0, min_ms = UINT_MAX;
    unsigned applied_volume = UINT_MAX;
    bool started = false;
    unsigned applied_route = ROUTE_UNKNOWN;
    uint64_t report_at = gadget_services_uptime_ms() + REPORT_INTERVAL_MS;
    const unsigned bytes_per_second = rate * BYTES_PER_FRAME;
    const size_t threshold = bytes_per_second * PREFILL_MS / 1000;

    while (!atomic_load(&closing)) {
        const uint64_t now = gadget_services_uptime_ms();
        const unsigned route = atomic_load(&audio_route);
        if (route != applied_route) {
            bsp_audio_set_volume(0);
            bsp_audio_set_amplifier(false);
            bsp_audio_set_amplifier_force(false);
            applied_route = route;
            applied_volume = UINT_MAX;
            if (bsp_audio_set_amplifier(route == ROUTE_SPEAKER) != ESP_OK) {
                ++errors;
            }
            ESP_LOGI(TAG, "Output %s", route == ROUTE_HEADPHONES ? "headphones" : "speaker");
        }
        const unsigned pair = atomic_load(&levels);
        const unsigned requested = route == ROUTE_HEADPHONES ? pair >> 8 : pair & 0xff;
        if (route != ROUTE_UNKNOWN && requested != applied_volume) {
            if (bsp_audio_set_volume((float)requested) == ESP_OK) {
                applied_volume = requested;
            } else {
                ++errors;
            }
        }

        const size_t available = xStreamBufferBytesAvailable(queue);
        const unsigned buffered_ms = (unsigned)available * 1000 / bytes_per_second;
        if (started && buffered_ms < min_ms) {
            min_ms = buffered_ms;
        }
        if (started && available < sizeof(samples)) {
            ++underruns;
            started = false;
            atomic_store(&playing, false);
        } else if (!started && available >= threshold && route != ROUTE_UNKNOWN) {
            started = true;
            atomic_store(&playing, true);
            min_ms = buffered_ms;
        }
        if (started) {
            const size_t bytes = xStreamBufferReceive(queue, samples, sizeof(samples), 0);
            if (bytes < sizeof(samples)) {
                memset((uint8_t *)samples + bytes, 0, sizeof(samples) - bytes);
            }
        } else {
            memset(samples, 0, sizeof(samples));
        }
        write_samples(&errors);

        if (now >= report_at) {
            ESP_LOGI(TAG, "PCM %lu Hz, level %ums, low %ums, underruns %u, output errors %u",
                     (unsigned long)rate, buffered_ms, min_ms == UINT_MAX ? 0 : min_ms, underruns, errors);
            report_at = now + REPORT_INTERVAL_MS;
        }
    }
    ESP_LOGI(TAG, "PCM stopped. Underruns %u, output errors %u", underruns, errors);
    atomic_store(&playing, false);
    xSemaphoreGive(joined);
    vTaskSuspend(NULL);
}

static bool open_audio(const void *requester, uint32_t sample_rate, bool buffered, unsigned initial_volume)
{
    if (sample_rate < 8000 || sample_rate > 48000 ||
        xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool opened = false;
    i2s_chan_handle_t device = NULL;
    if (owner == NULL && bsp_audio_get_i2s_handle(&device) == ESP_OK && device != NULL) {
        if (buffered) {
            previous_volume = 50;
            bsp_audio_get_volume(&previous_volume);
            bsp_audio_set_amplifier(false);
            bsp_audio_set_volume(0);
        }
        if (i2s_channel_disable(device) == ESP_OK) {
            const esp_err_t configured = bsp_audio_set_rate(sample_rate);
            const esp_err_t enabled = i2s_channel_enable(device);
            if (configured == ESP_OK && enabled == ESP_OK) {
                if (!buffered) {
                    opened = bsp_audio_set_amplifier(true) == ESP_OK;
                } else {
                    queue_memory = heap_caps_malloc(PCM_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (queue_memory != NULL) {
                        queue = xStreamBufferCreateStatic(PCM_BUFFER_BYTES + 1, 1, queue_memory, &queue_storage);
                        channel = device;
                        rate = sample_rate;
                        atomic_store(&levels, pack_levels(initial_volume, initial_volume));
                        atomic_store(&closing, false);
                        opened = queue != NULL &&
                                 xTaskCreate(drain_pcm, "gadget_pcm", 4608, NULL, 6, &drain_task) == pdPASS;
                        if (!opened) {
                            queue = NULL;
                            heap_caps_free(queue_memory);
                            queue_memory = NULL;
                            channel = NULL;
                        }
                    }
                }
            }
        }
        if (opened) {
            owner = requester;
            channel = device;
            writer = xTaskGetCurrentTaskHandle();
        } else if (buffered) {
            bsp_audio_set_volume(previous_volume);
        }
    }
    xSemaphoreGive(lock);
    return opened;
}

bool gadget_services_audio_open(const void *requester, uint32_t sample_rate)
{
    return open_audio(requester, sample_rate, false, 0);
}

bool gadget_services_audio_open_buffered(const void *requester, uint32_t sample_rate, unsigned initial_volume)
{
    return open_audio(requester, sample_rate, true, initial_volume);
}

void gadget_services_audio_volume(const void *requester, unsigned requested)
{
    gadget_services_audio_levels(requester, requested, requested);
}

void gadget_services_audio_levels(const void *requester, unsigned speaker, unsigned headphones)
{
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (owner == requester && queue != NULL) {
            atomic_store(&levels, pack_levels(speaker, headphones));
        }
        xSemaphoreGive(lock);
    }
}

bool gadget_services_audio_playing(const void *requester)
{
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(30)) != pdTRUE) {
        return false;
    }
    const bool active = owner == requester && atomic_load(&playing);
    xSemaphoreGive(lock);
    return active;
}

gadget_pcm_status_t gadget_audio_status(void)
{
    if (xSemaphoreTake(lock, 0) != pdTRUE) {
        return GADGET_PCM_BUSY;
    }
    const gadget_pcm_status_t status = owner == NULL ? GADGET_PCM_IDLE :
                                       queue == NULL || atomic_load(&playing) ? GADGET_PCM_PLAYING : GADGET_PCM_BUFFERING;
    xSemaphoreGive(lock);
    return status;
}

void gadget_audio_measure_stack(void)
{
    unsigned bytes = UINT_MAX;
    if (xSemaphoreTake(lock, 0) == pdTRUE) {
        if (drain_task != NULL) {
            bytes = uxTaskGetStackHighWaterMark(drain_task);
        }
        xSemaphoreGive(lock);
    }
    gadget_services_measure_stack("gadget_pcm", bytes);
}

int gadget_services_audio_write(const void *requester, const int16_t *stereo, size_t frames)
{
    if (stereo == NULL || frames > INT_MAX / BYTES_PER_FRAME ||
        xSemaphoreTake(lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }
    int result = -1;
    if (owner == requester && writer == xTaskGetCurrentTaskHandle()) {
        if (queue != NULL) {
            // Keep queue operations aligned to whole stereo frames.
            const size_t space = xStreamBufferSpacesAvailable(queue) / BYTES_PER_FRAME;
            const size_t accepted = frames < space ? frames : space;
            result = accepted == 0 ? 0 :
                     (int)(xStreamBufferSend(queue, stereo, accepted * BYTES_PER_FRAME, 0) / BYTES_PER_FRAME);
        } else {
            size_t sent = 0;
            const esp_err_t wrote = i2s_channel_write(channel, stereo, frames * BYTES_PER_FRAME, &sent, 100);
            if (wrote == ESP_OK || sent != 0) {
                result = (int)(sent / BYTES_PER_FRAME);
            }
        }
    }
    xSemaphoreGive(lock);
    return result;
}

void gadget_services_audio_close(const void *requester)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (owner == requester) {
        if (drain_task != NULL) {
            atomic_store(&closing, true);
            xSemaphoreTake(joined, portMAX_DELAY);
            vTaskDelete(drain_task);
            drain_task = NULL;
            vStreamBufferDelete(queue);
            queue = NULL;
            heap_caps_free(queue_memory);
            queue_memory = NULL;
            bsp_audio_set_volume(0);
            bsp_audio_set_amplifier(false);
            bsp_audio_set_volume(previous_volume);
        } else {
            bsp_audio_set_amplifier(false);
        }
        owner = NULL;
        channel = NULL;
        writer = NULL;
    }
    xSemaphoreGive(lock);
}
