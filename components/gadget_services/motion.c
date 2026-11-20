#include "gadget_services.h"
#include "peripherals.h"

#include "bsp/orientation.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static TaskHandle_t sampler;
static StaticSemaphore_t joined_storage;
static SemaphoreHandle_t joined;
static atomic_bool wanted, stopping;
static portMUX_TYPE sample_lock = portMUX_INITIALIZER_UNLOCKED;
static gadget_motion_t latest;

static void publish(const gadget_motion_t *sample)
{
    taskENTER_CRITICAL(&sample_lock);
    latest = *sample;
    taskEXIT_CRITICAL(&sample_lock);
}

static void sample_motion(void *argument)
{
    (void)argument;
    bool gyro = false, accel = false;
    uint64_t retry_at = 0;
    while (!atomic_load(&stopping)) {
        if (!atomic_load(&wanted)) {
            if (gyro) {
                bsp_orientation_disable_gyroscope();
                gyro = false;
            }
            if (accel) {
                bsp_orientation_disable_accelerometer();
                accel = false;
            }
            publish(&(gadget_motion_t){0});
            retry_at = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const uint64_t now = gadget_services_uptime_ms();
        if ((!gyro || !accel) && now >= retry_at) {
            if (!gyro) {
                gyro = bsp_orientation_enable_gyroscope() == ESP_OK;
            }
            if (!accel) {
                accel = bsp_orientation_enable_accelerometer() == ESP_OK;
            }
            retry_at = now + 1000;
        }
        gadget_motion_t sample = {.sampled_at_ms = now};
        if ((gyro || accel) &&
            bsp_orientation_get(&sample.gyro_ready, &sample.accel_ready,
                                &sample.gyro[0], &sample.gyro[1], &sample.gyro[2],
                                &sample.accel[0], &sample.accel[1], &sample.accel[2]) == ESP_OK) {
            sample.gyro_ready &= gyro;
            sample.accel_ready &= accel;
            if (sample.gyro_ready || sample.accel_ready) {
                publish(&sample);
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(gyro || accel ? 20 : 1000));
    }
    if (gyro) {
        bsp_orientation_disable_gyroscope();
    }
    if (accel) {
        bsp_orientation_disable_accelerometer();
    }
    xSemaphoreGive(joined);
    vTaskSuspend(NULL);
}

void gadget_services_motion_active(bool active)
{
    const bool previous = atomic_exchange(&wanted, active);
    if (active && sampler == NULL) {
        joined = xSemaphoreCreateBinaryStatic(&joined_storage);
        if (xTaskCreate(sample_motion, "gadget_motion", 4096, NULL, 3, &sampler) != pdPASS) {
            sampler = NULL;
            ESP_LOGW("gadget_motion", "Unable to start IMU sampler");
        }
    } else if (sampler != NULL && active != previous) {
        xTaskNotifyGive(sampler);
    }
}

bool gadget_services_motion_read(gadget_motion_t *sample)
{
    taskENTER_CRITICAL(&sample_lock);
    *sample = latest;
    taskEXIT_CRITICAL(&sample_lock);
    return (sample->gyro_ready || sample->accel_ready) &&
           gadget_services_uptime_ms() - sample->sampled_at_ms <= 150;
}

void gadget_motion_measure_stack(void)
{
    if (sampler != NULL) {
        gadget_services_measure_stack("gadget_motion", uxTaskGetStackHighWaterMark(sampler));
    }
}

void gadget_motion_shutdown(void)
{
    if (sampler == NULL) {
        return;
    }
    atomic_store(&stopping, true);
    xTaskNotifyGive(sampler);
    if (xSemaphoreTake(joined, pdMS_TO_TICKS(1000)) == pdTRUE) {
        vTaskDelete(sampler);
        sampler = NULL;
    } else {
        ESP_LOGW("gadget_motion", "IMU sampler is still stopping");
    }
}
