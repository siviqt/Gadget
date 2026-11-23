#include "gadget_services.h"
#include "peripherals.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>

#include "bsp/power.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *const TAG = "gadget_metrics";
static gadget_system_status_t status;
static gadget_battery_status_t battery;
static uint64_t next_battery_ms;
static bool battery_valid;

static void measure_battery(void)
{
    const uint64_t now = gadget_services_uptime_ms();
    if (now < next_battery_ms) {
        return;
    }
    next_battery_ms = now + 15000;
    bsp_power_battery_information_t reading;
    battery_valid = bsp_power_get_battery_information(&reading) == ESP_OK;
    if (!battery_valid) {
        return;
    }

    const double percent = reading.remaining_percentage;
    if (reading.battery_available && !(percent >= 0.0 && percent <= 100.0)) {
        battery_valid = false;
        return;
    }
    battery = (gadget_battery_status_t){
        .percent = reading.battery_available ? (uint8_t)(percent + 0.5) : 0,
        .present = reading.battery_available,
        .charging = reading.battery_available && reading.battery_charging,
        .external_power = reading.power_supply_available,
    };
}

static gadget_heap_status_t measure_heap(uint32_t capabilities)
{
    multi_heap_info_t heap;
    heap_caps_get_info(&heap, capabilities | MALLOC_CAP_8BIT);
    return (gadget_heap_status_t){heap.total_free_bytes, heap.minimum_free_bytes, heap.largest_free_block};
}

static void measure_cpu(void)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP
    static configRUN_TIME_COUNTER_TYPE previous_idle[configNUMBER_OF_CORES];
    static uint64_t previous_time;
    _Static_assert(configNUMBER_OF_CORES <= sizeof(status.cpu_busy_percent), "System snapshot supports two cores");
    const uint64_t now = esp_timer_get_time();
    const uint64_t elapsed = now - previous_time;
    for (unsigned i = 0; i < configNUMBER_OF_CORES; ++i) {
        const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(i);
        if (previous_time != 0 && elapsed != 0) {
            const uint64_t idle_elapsed = idle - previous_idle[i];
            status.cpu_busy_percent[i] = idle_elapsed >= elapsed ? 0 : 100 - (idle_elapsed * 100 / elapsed);
        }
        previous_idle[i] = idle;
    }
    status.cpu_count = previous_time != 0 && elapsed != 0 ? configNUMBER_OF_CORES : 0;
    previous_time = now;
#endif
}

void gadget_services_measure_stack(const char *name, unsigned free_bytes)
{
    if (free_bytes == UINT_MAX) {
        return;
    }
    if (free_bytes < status.least_stack_bytes) {
        status.least_stack_bytes = free_bytes;
        snprintf(status.least_stack_task, sizeof(status.least_stack_task), "%s", name);
    }
    ESP_LOGI(TAG, "stack %s free_min=%u B", name, free_bytes);
}

void gadget_services_measure(void)
{
    measure_cpu();
    status.internal = measure_heap(MALLOC_CAP_INTERNAL);
    status.psram = measure_heap(MALLOC_CAP_SPIRAM);
    status.least_stack_bytes = UINT32_MAX;
    gadget_services_measure_stack("main", uxTaskGetStackHighWaterMark(NULL));
    gadget_audio_measure_stack();
    gadget_motion_measure_stack();
    measure_battery();
}

void gadget_services_system_status(gadget_system_status_t *result)
{
    *result = status;
    result->wifi = gadget_services_wifi_status();
    result->pcm = gadget_audio_status();
}

bool gadget_services_battery_status(gadget_battery_status_t *result)
{
    if (battery_valid) {
        *result = battery;
    }
    return battery_valid;
}

void gadget_services_log_metrics(void)
{
    gadget_system_status_t sample;
    gadget_services_system_status(&sample);
    ESP_LOGI(TAG, "internal free/low/largest=%" PRIu32 "/%" PRIu32 "/%" PRIu32
             " B, PSRAM free/low/largest=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " B",
             sample.internal.free_bytes, sample.internal.low_bytes, sample.internal.largest_block_bytes,
             sample.psram.free_bytes, sample.psram.low_bytes, sample.psram.largest_block_bytes);
    ESP_LOGI(TAG, "CPU busy=%u%%/%u%% (%u cores sampled), Wi-Fi=%u, PCM=%u, transitions=%" PRIu32
             " last/max=%" PRIu32 "/%" PRIu32 " us",
             sample.cpu_busy_percent[0], sample.cpu_busy_percent[1], sample.cpu_count, sample.wifi, sample.pcm,
             sample.transitions, sample.last_transition_us, sample.longest_transition_us);
}

void gadget_services_record_transition(uint32_t elapsed_us)
{
    status.last_transition_us = elapsed_us;
    if (elapsed_us > status.longest_transition_us) {
        status.longest_transition_us = elapsed_us;
    }
    ++status.transitions;
}
