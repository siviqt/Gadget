#include "gadget_modules.h"

#include <dirent.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gadget_config.h"
#include "gadget_dice.h"
#include "gadget_services.h"
#include "gadget_settings.h"
#include "manifest.h"
#include "native.h"

typedef enum { DISCOVERED, REJECTED, RUNNING } module_status_t;

#define WORKER_LIMIT 3

typedef struct module_instance module_instance_t;

typedef struct {
    module_instance_t *module;
    gadget_worker_fn entry;
    const char *name;
    uint32_t stack;
    TaskHandle_t task;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_storage;
    gadget_http_stream_t *stream;
    atomic_bool executing;
} module_worker_t;

struct module_instance {
    gadget_module_context_t context;
    gadget_manifest_t manifest;
    char folder[GADGET_MODULE_PATH_SIZE];
    module_status_t status;
    kbelf_dyn image;
    const gadget_module_t *hooks;
    cJSON *saved_settings;
    atomic_uint write_status;
    bool reading_state;
    module_worker_t workers[WORKER_LIMIT];
    unsigned worker_count;
    bool registering_workers;
    atomic_bool stopping;
    bool redraw;
    bool reads_system_details;
    gadget_damage_t damage;
    portMUX_TYPE shared_lock;
    uint64_t next_update;
    bool init_attempted;
    bool service_attempted;
};

static module_instance_t catalog[GADGET_MODULE_LIMIT];
static module_instance_t *start_order[GADGET_MODULE_LIMIT];
static module_instance_t *workspaces[GADGET_FACE_COUNT];
static module_instance_t *focused;
static unsigned catalog_size, running_count;
static const char *const TAG = "gadget_modules";

static void host_log(gadget_module_context_t *context, const char *message)
{
    const module_instance_t *module = (const module_instance_t *)context;
    if (message != NULL) {
        ESP_LOGI(TAG, "%s: %.200s", module->manifest.id, message);
    }
}

static int64_t host_time(gadget_module_context_t *context)
{
    const module_instance_t *module = (const module_instance_t *)context;
    return module->manifest.capabilities & MODULE_TIME ? gadget_services_unix_seconds() : -1;
}

static int32_t host_setting_int(gadget_module_context_t *context, const char *key, int32_t fallback)
{
    const module_instance_t *module = (const module_instance_t *)context;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(module->manifest.settings, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= INT32_MIN && item->valuedouble <= INT32_MAX &&
        item->valuedouble == (int32_t)item->valuedouble) {
        return (int32_t)item->valuedouble;
    }
    return fallback;
}

static const char *host_setting_string(gadget_module_context_t *context, const char *key)
{
    const module_instance_t *module = (const module_instance_t *)context;
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(module->manifest.settings, key));
}

static bool host_civil_time(gadget_module_context_t *context, bool utc, gadget_civil_time_t *result)
{
    const module_instance_t *module = (const module_instance_t *)context;
    return (module->manifest.capabilities & MODULE_TIME) && gadget_services_civil_time(utc, result);
}

static void host_copy_shared(gadget_module_context_t *context, void *out, const void *in, size_t size)
{
    module_instance_t *module = (module_instance_t *)context;
    if (size <= GADGET_SHARED_COPY_LIMIT) {
        taskENTER_CRITICAL(&module->shared_lock);
        memcpy(out, in, size);
        taskEXIT_CRITICAL(&module->shared_lock);
    }
}

static void host_invalidate_region(gadget_module_context_t *context, gadget_damage_t rect)
{
    module_instance_t *module = (module_instance_t *)context;
    if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
        rect.x + rect.width > INT16_MAX || rect.y + rect.height > INT16_MAX) {
        rect = (gadget_damage_t){0};
    }
    taskENTER_CRITICAL(&module->shared_lock);
    const bool pending = module->redraw;
    if (!pending || rect.width == 0) {
        module->damage = rect;
    } else if (module->damage.width != 0) {
        const gadget_damage_t old = module->damage;
        const int left = old.x < rect.x ? old.x : rect.x;
        const int top = old.y < rect.y ? old.y : rect.y;
        const int right = old.x + old.width > rect.x + rect.width ? old.x + old.width : rect.x + rect.width;
        const int bottom = old.y + old.height > rect.y + rect.height ? old.y + old.height : rect.y + rect.height;
        module->damage = (gadget_damage_t){left, top, right - left, bottom - top};
    }
    module->redraw = true;
    taskEXIT_CRITICAL(&module->shared_lock);
    if (!pending) {
        gadget_services_wake();
    }
}

static void host_invalidate(gadget_module_context_t *context)
{
    host_invalidate_region(context, (gadget_damage_t){0});
}

static bool host_publish(gadget_module_context_t *context, uint32_t topic, int32_t value)
{
    const module_instance_t *module = (const module_instance_t *)context;
    if (atomic_load(&module->stopping)) {
        return false;
    }
    gadget_event_t event = {.topic = topic, .value = value};
    memcpy(event.sender, module->manifest.id, sizeof(event.sender));
    return gadget_services_publish(&event);
}

static module_worker_t *current_worker(module_instance_t *module)
{
    const TaskHandle_t running = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < module->worker_count; ++i) {
        if (module->workers[i].task == running) {
            return &module->workers[i];
        }
    }
    return NULL;
}

static bool host_wait_for_stop(gadget_module_context_t *context, uint32_t timeout_ms)
{
    module_instance_t *module = (module_instance_t *)context;
    if (atomic_load(&module->stopping) || current_worker(module) == NULL) {
        return true;
    }
    if (timeout_ms > 60000) {
        timeout_ms = 60000;
    }
    const TickType_t ticks = (timeout_ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
    ulTaskNotifyTake(pdTRUE, ticks);
    return atomic_load(&module->stopping);
}

static void host_wake_worker(gadget_module_context_t *context)
{
    const module_instance_t *module = (const module_instance_t *)context;
    for (unsigned i = 0; i < module->worker_count; ++i) {
        if (module->workers[i].task != NULL) {
            xTaskNotifyGive(module->workers[i].task);
        }
    }
}

static bool host_add_worker(gadget_module_context_t *context, const char *name, gadget_worker_fn entry,
                            uint32_t stack)
{
    module_instance_t *module = (module_instance_t *)context;
    if (!module->registering_workers || module->worker_count == WORKER_LIMIT || entry == NULL ||
        name == NULL || stack < 8192 || stack > 32768 || stack % 16 != 0) {
        return false;
    }
    module->workers[module->worker_count++] = (module_worker_t){
        .module = module, .entry = entry, .name = name, .stack = stack,
    };
    return true;
}

static bool host_network_connected(gadget_module_context_t *context)
{
    module_instance_t *module = (module_instance_t *)context;
    return (module->manifest.capabilities & MODULE_NETWORK) && current_worker(module) != NULL &&
           !atomic_load(&module->stopping) && gadget_services_network_connected();
}

static gadget_http_stream_t *open_module_stream(gadget_module_context_t *context, const char *url, int *status,
                                                uint32_t *interval)
{
    module_instance_t *module = (module_instance_t *)context;
    module_worker_t *worker = current_worker(module);
    if (!(module->manifest.capabilities & MODULE_NETWORK) || worker == NULL || worker->stream != NULL) {
        return NULL;
    }
    worker->stream = interval != NULL ? gadget_services_http_open_icy(url, status, interval, &module->stopping)
                                      : gadget_services_http_open(url, status, &module->stopping);
    return worker->stream;
}

static gadget_http_stream_t *host_http_open(gadget_module_context_t *context, const char *url, int *status)
{
    return open_module_stream(context, url, status, NULL);
}

static gadget_http_stream_t *host_http_open_icy(gadget_module_context_t *context, const char *url, int *status,
                                                uint32_t *interval)
{
    return interval != NULL ? open_module_stream(context, url, status, interval) : NULL;
}

static int host_http_read(gadget_module_context_t *context, gadget_http_stream_t *stream, void *data, size_t size)
{
    module_instance_t *module = (module_instance_t *)context;
    module_worker_t *worker = current_worker(module);
    return worker != NULL && stream != NULL && stream == worker->stream && !atomic_load(&module->stopping)
           ? gadget_services_http_read(stream, data, size) : -1;
}

static int host_http_read_timed(gadget_module_context_t *context, gadget_http_stream_t *stream, void *data,
                                size_t size, uint32_t timeout_ms)
{
    module_instance_t *module = (module_instance_t *)context;
    module_worker_t *worker = current_worker(module);
    return worker != NULL && stream != NULL && stream == worker->stream && !atomic_load(&module->stopping)
           ? gadget_services_http_read_timed(stream, data, size, timeout_ms) : -1;
}

static void host_http_close(gadget_module_context_t *context, gadget_http_stream_t *stream)
{
    module_instance_t *module = (module_instance_t *)context;
    module_worker_t *worker = current_worker(module);
    if (worker != NULL && stream != NULL && stream == worker->stream) {
        gadget_services_http_close(stream);
        worker->stream = NULL;
    }
}

static bool audio_worker(module_instance_t *module)
{
    return (module->manifest.capabilities & MODULE_AUDIO) && current_worker(module) != NULL;
}

static bool host_audio_open(gadget_module_context_t *context, uint32_t rate)
{
    module_instance_t *module = (module_instance_t *)context;
    return audio_worker(module) && !atomic_load(&module->stopping) && gadget_services_audio_open(module, rate);
}

static bool host_audio_open_buffered(gadget_module_context_t *context, uint32_t rate, unsigned volume)
{
    module_instance_t *module = (module_instance_t *)context;
    return audio_worker(module) && !atomic_load(&module->stopping) &&
           gadget_services_audio_open_buffered(module, rate, volume);
}

static void host_audio_volume(gadget_module_context_t *context, unsigned volume)
{
    module_instance_t *module = (module_instance_t *)context;
    if (audio_worker(module)) {
        gadget_services_audio_volume(module, volume);
    }
}

static void host_audio_levels(gadget_module_context_t *context, unsigned speaker, unsigned headphones)
{
    module_instance_t *module = (module_instance_t *)context;
    if (audio_worker(module)) {
        gadget_services_audio_levels(module, speaker, headphones);
    }
}

static bool host_audio_playing(gadget_module_context_t *context)
{
    module_instance_t *module = (module_instance_t *)context;
    return audio_worker(module) && gadget_services_audio_playing(module);
}

static int host_audio_write(gadget_module_context_t *context, const int16_t *data, size_t frames)
{
    module_instance_t *module = (module_instance_t *)context;
    return audio_worker(module) && !atomic_load(&module->stopping)
           ? gadget_services_audio_write(module, data, frames) : -1;
}

static void host_audio_close(gadget_module_context_t *context)
{
    module_instance_t *module = (module_instance_t *)context;
    if (audio_worker(module)) {
        gadget_services_audio_close(module);
    }
}

static bool host_motion_read(gadget_module_context_t *context, gadget_motion_t *out)
{
    const module_instance_t *module = (const module_instance_t *)context;
    return module == focused && (module->manifest.capabilities & MODULE_MOTION) && gadget_services_motion_read(out);
}

static int host_dns_query(gadget_module_context_t *context, const void *query, size_t length, void *reply, size_t capacity)
{
    module_instance_t *module = (module_instance_t *)context;
    return (module->manifest.capabilities & MODULE_NETWORK) && current_worker(module) != NULL
           ? gadget_services_dns_query(query, length, reply, capacity, &module->stopping) : -1;
}

static gadget_json_t *json_parse(const char *text, size_t length)
{
    return (gadget_json_t *)gadget_json_parse(text, length);
}

static void json_destroy(gadget_json_t *document)
{
    cJSON_Delete((cJSON *)document);
}

static const gadget_json_t *json_member(const gadget_json_t *object, const char *key)
{
    return (const gadget_json_t *)cJSON_GetObjectItemCaseSensitive((const cJSON *)object, key);
}

static const gadget_json_t *json_at(const gadget_json_t *array, unsigned index)
{
    return index <= INT_MAX && cJSON_IsArray((const cJSON *)array)
           ? (const gadget_json_t *)cJSON_GetArrayItem((const cJSON *)array, (int)index) : NULL;
}

static bool json_number(const gadget_json_t *value, float *number)
{
    const cJSON *item = (const cJSON *)value;
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= -FLT_MAX && item->valuedouble <= FLT_MAX)) {
        return false;
    }
    *number = (float)item->valuedouble;
    return true;
}

static const char *json_string(const gadget_json_t *value)
{
    return cJSON_GetStringValue((const cJSON *)value);
}

static const gadget_json_api_t json_api = {
    .parse = json_parse,
    .destroy = json_destroy,
    .member = json_member,
    .at = json_at,
    .number = json_number,
    .string = json_string,
};

static const cJSON *state_item(gadget_module_context_t *context, const char *key)
{
    module_instance_t *module = (module_instance_t *)context;
    if (current_worker(module) != NULL || key == NULL) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(module->saved_settings, key);
}

static const char *host_state_string(gadget_module_context_t *context, const char *key)
{
    const char *value = cJSON_GetStringValue(state_item(context, key));
    return value != NULL && strnlen(value, 1024) < 1024 ? value : NULL;
}

static int32_t host_state_int(gadget_module_context_t *context, const char *key, int32_t fallback)
{
    const cJSON *item = state_item(context, key);
    return cJSON_IsNumber(item) && item->valuedouble >= INT32_MIN && item->valuedouble <= INT32_MAX &&
           item->valuedouble == (int32_t)item->valuedouble ? (int32_t)item->valuedouble : fallback;
}

static bool save_setting(gadget_module_context_t *context, const char *key, cJSON *value)
{
    module_instance_t *module = (module_instance_t *)context;
    if (value == NULL || module->saved_settings == NULL || current_worker(module) != NULL ||
        key == NULL || key[0] == '\0' || strnlen(key, 64) == 64) {
        cJSON_Delete(value);
        return false;
    }
    cJSON *next = cJSON_Duplicate(module->saved_settings, true);
    const bool inserted = next != NULL &&
                          (cJSON_GetObjectItemCaseSensitive(next, key) != NULL
                           ? cJSON_ReplaceItemInObjectCaseSensitive(next, key, value)
                           : cJSON_AddItemToObject(next, key, value));
    if (!inserted) {
        cJSON_Delete(value);
        cJSON_Delete(next);
        return false;
    }
    if (!gadget_settings_write(module->manifest.id, next)) {
        cJSON_Delete(next);
        return false;
    }
    cJSON_Delete(module->saved_settings);
    module->saved_settings = next;
    return true;
}

static bool host_state_set_string(gadget_module_context_t *context, const char *key, const char *value)
{
    if (value == NULL || strnlen(value, 1024) == 1024) {
        return false;
    }
    const char *current = host_state_string(context, key);
    return (current != NULL && strcmp(current, value) == 0) ||
           save_setting(context, key, cJSON_CreateString(value));
}

static bool host_state_set_int(gadget_module_context_t *context, const char *key, int32_t value)
{
    const cJSON *current = state_item(context, key);
    return (cJSON_IsNumber(current) && current->valuedouble == value) ||
           save_setting(context, key, cJSON_CreateNumber(value));
}

static int host_state_read_file(gadget_module_context_t *context, const char *filename, gadget_state_copy_t copy,
                                char *out, size_t capacity)
{
    const module_instance_t *module = (const module_instance_t *)context;
    return (module->manifest.capabilities & MODULE_STORAGE) && module->reading_state
           ? gadget_config_read(module->manifest.id, filename, copy, out, capacity) : GADGET_STATE_FILE_UNREADABLE;
}

static bool state_file_writable(module_instance_t *module, const char *filename)
{
    return (module->manifest.capabilities & MODULE_STORAGE) && current_worker(module) == NULL && filename != NULL &&
           strcasecmp(filename, GADGET_SETTINGS_FILE) != 0;
}

static bool host_state_write_file(gadget_module_context_t *context, const char *filename, const void *data, size_t size)
{
    module_instance_t *module = (module_instance_t *)context;
    return state_file_writable(module, filename) &&
           gadget_config_write(module->manifest.id, filename, data, size);
}

static bool host_state_write_file_checked(gadget_module_context_t *context, const char *filename,
                                          const void *data, size_t size)
{
    module_instance_t *module = (module_instance_t *)context;
    return state_file_writable(module, filename) &&
           gadget_config_write_checked(module->manifest.id, filename, data, size, &module->write_status);
}

static gadget_state_write_status_t host_state_write_status(gadget_module_context_t *context)
{
    module_instance_t *module = (module_instance_t *)context;
    return (module->manifest.capabilities & MODULE_STORAGE) && current_worker(module) == NULL
           ? (gadget_state_write_status_t)atomic_load(&module->write_status) : GADGET_STATE_WRITE_IDLE;
}

static bool host_system_status(gadget_module_context_t *context, gadget_system_status_t *status)
{
    if (status == NULL || current_worker((module_instance_t *)context) != NULL) {
        return false;
    }
    gadget_services_system_status(status);
    status->modules = running_count;
    status->services = 0;
    for (unsigned i = 0; i < running_count; ++i) {
        status->services += (start_order[i]->manifest.kind & MODULE_SERVICE) != 0;
    }
    return true;
}

static bool host_system_service(gadget_module_context_t *context, unsigned index, gadget_service_status_t *status)
{
    if (status == NULL || current_worker((module_instance_t *)context) != NULL) {
        return false;
    }
    for (unsigned i = 0; i < running_count; ++i) {
        const module_instance_t *module = start_order[i];
        if ((module->manifest.kind & MODULE_SERVICE) && index-- == 0) {
            *status = (gadget_service_status_t){.workers = module->worker_count};
            memcpy(status->name, module->manifest.name, sizeof(status->name));
            for (unsigned j = 0; j < module->worker_count; ++j) {
                status->active_workers += atomic_load(&module->workers[j].executing);
            }
            return true;
        }
    }
    return false;
}

static bool host_system_details(gadget_module_context_t *context, gadget_system_details_t *details)
{
    module_instance_t *module = (module_instance_t *)context;
    if (details == NULL || current_worker(module) != NULL) {
        return false;
    }
    module->reads_system_details = true;
    gadget_services_system_details(details);
    return true;
}

static bool host_battery_status(gadget_module_context_t *context, gadget_battery_status_t *status)
{
    return status != NULL && current_worker((module_instance_t *)context) == NULL &&
           gadget_services_battery_status(status);
}

static const gadget_host_t host = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_host_t),
    .log = host_log,
    .uptime_ms = gadget_services_uptime_ms,
    .unix_seconds = host_time,
    .setting_int = host_setting_int,
    .invalidate = host_invalidate,
    .publish = host_publish,
    .wait_for_stop = host_wait_for_stop,
    .http_open = host_http_open,
    .http_read = host_http_read,
    .http_close = host_http_close,
    .audio_open = host_audio_open,
    .audio_write = host_audio_write,
    .audio_close = host_audio_close,
    .format = snprintf,
    .setting_string = host_setting_string,
    .civil_time = host_civil_time,
    .wake_worker = host_wake_worker,
    .copy_shared = host_copy_shared,
    .network_connected = host_network_connected,
    .http_read_timed = host_http_read_timed,
    .json = &json_api,
    .add_worker = host_add_worker,
    .audio_open_buffered = host_audio_open_buffered,
    .audio_volume = host_audio_volume,
    .audio_playing = host_audio_playing,
    .motion_read = host_motion_read,
    .invalidate_region = host_invalidate_region,
    .dns_query = host_dns_query,
    .state_string = host_state_string,
    .state_int = host_state_int,
    .state_set_string = host_state_set_string,
    .state_set_int = host_state_set_int,
    .state_read_file = host_state_read_file,
    .state_write_file = host_state_write_file,
    .audio_levels = host_audio_levels,
    .audio_headphones = gadget_services_audio_headphones,
    .system_status = host_system_status,
    .system_service = host_system_service,
    .state_write_file_checked = host_state_write_file_checked,
    .state_write_status = host_state_write_status,
    .http_open_icy = host_http_open_icy,
    .battery_status = host_battery_status,
    .system_details = host_system_details,
    .aac = &gadget_aac_api,
};

static void run_worker(void *argument)
{
    module_worker_t *worker = argument;
    module_instance_t *module = worker->module;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!atomic_load(&module->stopping)) {
        atomic_store(&worker->executing, true);
        worker->entry(&module->context);
        atomic_store(&worker->executing, false);
    }
    xSemaphoreGive(worker->done);
    // Stay valid as a task handle until the runtime joins and deletes the task.
    vTaskSuspend(NULL);
}

static void release(module_instance_t *module)
{
    atomic_store(&module->stopping, true);
    for (unsigned i = 0; i < module->worker_count; ++i) {
        module_worker_t *worker = &module->workers[i];
        if (worker->task != NULL) {
            vTaskDelete(worker->task);
            worker->task = NULL;
        }
        gadget_services_http_close(worker->stream);
        worker->stream = NULL;
    }
    if (module->service_attempted && module->hooks->service_stop != NULL) {
        module->hooks->service_stop(&module->context);
    }
    if (module->init_attempted && module->hooks->shutdown != NULL) {
        module->hooks->shutdown(&module->context);
    }
    cJSON_Delete(module->saved_settings);
    module->saved_settings = NULL;
    if (module->manifest.capabilities & MODULE_AUDIO) {
        gadget_services_audio_close(module);
    }
    gadget_native_unload(module->image);
    module->image = NULL;
    module->hooks = NULL;
}

static const char *start(module_instance_t *module, unsigned face)
{
    const gadget_manifest_t *manifest = &module->manifest;
    if (face < GADGET_FACE_COUNT && workspaces[face] != NULL) {
        return "workspace already assigned";
    }
    char path[GADGET_MODULE_PATH_SIZE + sizeof(manifest->executable) + 1];
    snprintf(path, sizeof(path), "%s/%s", module->folder, manifest->executable);
    const char *error = gadget_native_load(path, &module->image, &module->hooks);
    if (error != NULL) {
        return error;
    }
    const gadget_module_t *hooks = module->hooks;
    if (((manifest->kind & MODULE_FACE) && hooks->render == NULL) ||
        (manifest->update_ms != 0 && hooks->update == NULL) ||
        (!(manifest->kind & MODULE_SERVICE) &&
         (hooks->worker != NULL || hooks->service_start != NULL || hooks->service_stop != NULL))) {
        return "callbacks do not match manifest kind or update interval";
    }
    module->context = (gadget_module_context_t){.host = &host, .package_path = module->folder};
    if (manifest->capabilities & MODULE_STORAGE) {
        module->saved_settings = gadget_settings_read(manifest->id, NULL);
        if (module->saved_settings == NULL) {
            module->saved_settings = cJSON_CreateObject();
        }
    }
    module->init_attempted = true;
    module->reading_state = true;
    const bool initialized = hooks->init == NULL || hooks->init(&module->context);
    module->reading_state = false;
    if (!initialized) {
        return "init failed";
    }
    if (manifest->kind & MODULE_SERVICE) {
        module->service_attempted = true;
        module->registering_workers = true;
        const bool registered = hooks->worker == NULL ||
                                host_add_worker(&module->context, manifest->id, hooks->worker, manifest->worker_stack);
        const bool ready = registered && (hooks->service_start == NULL || hooks->service_start(&module->context));
        module->registering_workers = false;
        if (!ready) {
            return "service_start failed";
        }
        for (unsigned i = 0; i < module->worker_count; ++i) {
            module_worker_t *worker = &module->workers[i];
            atomic_init(&worker->executing, false);
            worker->done = xSemaphoreCreateBinaryStatic(&worker->done_storage);
            if (xTaskCreate(run_worker, worker->name, worker->stack, worker, 4, &worker->task) != pdPASS) {
                worker->task = NULL;
                return "cannot allocate service worker";
            }
        }
    }
    if (face < GADGET_FACE_COUNT) {
        workspaces[face] = module;
    }
    module->status = RUNNING;
    start_order[running_count++] = module;
    host_wake_worker(&module->context);
    ESP_LOGI(TAG, "Loaded %s v%s from %s", manifest->id, manifest->version, module->folder);
    return NULL;
}

static void reject(module_instance_t *module, const char *reason)
{
    module->status = REJECTED;
    ESP_LOGW(TAG, "Rejected %s: %s", module->folder, reason);
}

static int compare_folders(const void *a, const void *b)
{
    return strcmp(((const module_instance_t *)a)->folder, ((const module_instance_t *)b)->folder);
}

static void discover(const char *root)
{
    DIR *directory = opendir(root);
    if (directory == NULL) {
        ESP_LOGI(TAG, "No SD modules directory at %s", root);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char folder[GADGET_MODULE_PATH_SIZE];
        int length = snprintf(folder, sizeof(folder), "%s/%s", root, entry->d_name);
        struct stat info;
        if (length < 0 || length >= sizeof(folder) || stat(folder, &info) != 0 || !S_ISDIR(info.st_mode)) {
            continue;
        }
        if (catalog_size == GADGET_MODULE_LIMIT) {
            ESP_LOGW(TAG, "Module catalog is full at %u entries", catalog_size);
            break;
        }
        module_instance_t *module = &catalog[catalog_size];
        memset(module, 0, sizeof(*module));
        strcpy(module->folder, folder);
        const char *error = gadget_manifest_read(&module->manifest, folder);
        if (error != NULL) {
            reject(module, error);
            cJSON_Delete(module->manifest.document);
            continue;
        }
        ++catalog_size;
    }
    closedir(directory);
    qsort(catalog, catalog_size, sizeof(catalog[0]), compare_folders);
}

static module_instance_t *find_module(const char *id)
{
    for (unsigned i = 0; i < catalog_size; ++i) {
        if (strcmp(catalog[i].manifest.id, id) == 0) {
            return &catalog[i];
        }
    }
    return NULL;
}

static bool vacant_requested(const gadget_face_setting_t *face)
{
    return face->assigned && face->id[0] == '\0';
}

static void requested_faces(const gadget_layout_t *layout, module_status_t status,
                            module_instance_t *faces[GADGET_FACE_COUNT])
{
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        const gadget_face_setting_t *setting = &layout->faces[i];
        module_instance_t *module = setting->assigned && setting->id[0] != '\0' ? find_module(setting->id) : NULL;
        faces[i] = module != NULL && module->status == status && (module->manifest.kind & MODULE_FACE) ? module : NULL;
    }
}

static unsigned assigned_slot(module_instance_t *const faces[GADGET_FACE_COUNT], const module_instance_t *module)
{
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        if (faces[i] == module) {
            return i;
        }
    }
    return GADGET_FACE_COUNT;
}

static unsigned startup_slot(const gadget_layout_t *layout, module_instance_t *const requested[GADGET_FACE_COUNT],
                             const module_instance_t *module)
{
    if (!(module->manifest.kind & MODULE_FACE)) {
        return GADGET_FACE_COUNT;
    }
    const unsigned explicit_slot = assigned_slot(requested, module);
    if (explicit_slot < GADGET_FACE_COUNT) {
        return explicit_slot;
    }
    const unsigned slot = module->manifest.face;
    // Start displaced modules too. A combined module's service does not depend on its slot.
    return requested[slot] != NULL || vacant_requested(&layout->faces[slot]) ? GADGET_FACE_COUNT : slot;
}

static void resolve_faces(const gadget_layout_t *layout, module_instance_t *faces[GADGET_FACE_COUNT])
{
    requested_faces(layout, RUNNING, faces);
    for (unsigned i = 0; i < running_count; ++i) {
        module_instance_t *module = start_order[i];
        const unsigned slot = module->manifest.face;
        if ((module->manifest.kind & MODULE_FACE) && faces[slot] == NULL && !vacant_requested(&layout->faces[slot]) &&
            assigned_slot(faces, module) == GADGET_FACE_COUNT) {
            faces[slot] = module;
        }
    }
}

void gadget_modules_preview_layout(const gadget_layout_t *layout, gadget_face_assignment_t faces[GADGET_FACE_COUNT])
{
    module_instance_t *preview[GADGET_FACE_COUNT];
    resolve_faces(layout, preview);
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        const module_instance_t *module = preview[i];
        faces[i] = module != NULL ? (gadget_face_assignment_t){module->manifest.id, module->manifest.name}
                                  : (gadget_face_assignment_t){NULL, "Empty"};
    }
}

void gadget_modules_measure_stacks(void)
{
    for (unsigned i = 0; i < running_count; ++i) {
        const module_instance_t *module = start_order[i];
        for (unsigned j = 0; j < module->worker_count; ++j) {
            const module_worker_t *worker = &module->workers[j];
            gadget_services_measure_stack(worker->name, uxTaskGetStackHighWaterMark(worker->task));
        }
    }
}

void gadget_modules_start(const char *root, const gadget_layout_t *layout)
{
    discover(root);
    for (unsigned i = 0; i < catalog_size; ++i) {
        module_instance_t *module = &catalog[i];
        atomic_init(&module->stopping, false);
        atomic_init(&module->write_status, GADGET_STATE_WRITE_IDLE);
        module->shared_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
        for (unsigned j = i + 1; j < catalog_size; ++j) {
            if (strcmp(module->manifest.id, catalog[j].manifest.id) == 0) {
                reject(module, "duplicate module ID");
                reject(&catalog[j], "duplicate module ID");
            }
        }
        if (module->status == DISCOVERED && module->manifest.sdk != GADGET_MODULE_ABI) {
            reject(module, "incompatible Gadget SDK version");
        }
    }

    module_instance_t *requested[GADGET_FACE_COUNT];
    requested_faces(layout, DISCOVERED, requested);
    bool changed;
    do {
        changed = false;
        for (unsigned i = 0; i < catalog_size; ++i) {
            module_instance_t *module = &catalog[i];
            if (module->status != DISCOVERED) {
                continue;
            }
            unsigned ready = 0;
            for (unsigned j = 0; j < module->manifest.dependency_count; ++j) {
                const char *id = module->manifest.dependencies[j];
                const module_instance_t *dependency = find_module(id);
                if (dependency == NULL || dependency->status == REJECTED) {
                    ESP_LOGW(TAG, "%s requires %s", module->manifest.id, id);
                    reject(module, "unavailable dependency");
                    changed = true;
                    break;
                }
                ready += dependency->status == RUNNING;
            }
            if (module->status == DISCOVERED && ready == module->manifest.dependency_count) {
                const char *error = start(module, startup_slot(layout, requested, module));
                if (error != NULL) {
                    release(module);
                    reject(module, error);
                }
                changed = true;
            }
        }
    } while (changed);
    for (unsigned i = 0; i < catalog_size; ++i) {
        if (catalog[i].status == DISCOVERED) {
            reject(&catalog[i], "dependency cycle or depends on a cycle");
        }
    }
    resolve_faces(layout, workspaces);
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        const gadget_face_setting_t *setting = &layout->faces[i];
        if (setting->assigned && setting->id[0] != '\0' &&
            (workspaces[i] == NULL || strcmp(workspaces[i]->manifest.id, setting->id) != 0)) {
            ESP_LOGW(TAG, "Slot %u: configured face %s unavailable, using %s", i + 1, setting->id,
                     workspaces[i] != NULL ? workspaces[i]->manifest.id : "an empty workspace");
        }
    }
    ESP_LOGI(TAG, "%u SD modules running", running_count);
}

bool gadget_modules_shutdown(void)
{
    gadget_modules_focus(GADGET_FACE_COUNT);
    for (unsigned i = 0; i < running_count; ++i) {
        module_instance_t *module = start_order[i];
        atomic_store(&module->stopping, true);
        host_wake_worker(&module->context);
    }
    const uint64_t deadline = gadget_services_uptime_ms() + 3500;
    for (unsigned i = running_count; i-- > 0;) {
        module_instance_t *module = start_order[i];
        for (unsigned j = 0; j < module->worker_count; ++j) {
            module_worker_t *worker = &module->workers[j];
            const uint64_t now = gadget_services_uptime_ms();
            const TickType_t wait = deadline > now ? pdMS_TO_TICKS(deadline - now) : 0;
            if (xSemaphoreTake(worker->done, wait) != pdTRUE) {
                ESP_LOGE(TAG, "%s did not stop before reboot. Keeping its code and resources mapped", worker->name);
                return false;
            }
        }
    }
    while (running_count != 0) {
        module_instance_t *module = start_order[--running_count];
        release(module);
        ESP_LOGI(TAG, "Stopped and unloaded %s", module->manifest.id);
    }
    for (unsigned i = 0; i < catalog_size; ++i) {
        cJSON_Delete(catalog[i].manifest.document);
    }
    memset(workspaces, 0, sizeof(workspaces));
    catalog_size = 0;
    return true;
}

const char *gadget_modules_title(unsigned face)
{
    return workspaces[face] != NULL ? workspaces[face]->manifest.name : NULL;
}

bool gadget_modules_startup_preview(unsigned face)
{
    return workspaces[face] != NULL && workspaces[face]->manifest.startup_preview;
}

void gadget_modules_focus(unsigned face)
{
    module_instance_t *next = face < GADGET_FACE_COUNT ? workspaces[face] : NULL;
    if (focused == next) {
        return;
    }
    if (focused != NULL && focused->hooks->deactivate != NULL) {
        focused->hooks->deactivate(&focused->context);
    }
    focused = next;
    gadget_services_motion_active(focused != NULL && (focused->manifest.capabilities & MODULE_MOTION));
    if (focused != NULL) {
        focused->next_update = gadget_services_uptime_ms();
        if (focused->hooks->activate != NULL) {
            focused->hooks->activate(&focused->context);
        }
    }
}

bool gadget_modules_input(unsigned face, const gadget_key_t *key)
{
    module_instance_t *module = workspaces[face];
    if (module == NULL || module != focused) {
        return false;
    }
    if (module->hooks->input != NULL) {
        module->hooks->input(&module->context, key);
    }
    return true;
}

void gadget_modules_system_sample(void)
{
    if (focused != NULL && focused->reads_system_details && focused->hooks->event != NULL &&
        !gadget_services_request_network_sample()) {
        const gadget_event_t event = {.topic = GADGET_EVENT_SYSTEM_SAMPLE};
        focused->hooks->event(&focused->context, &event);
    }
}

void gadget_modules_event(const gadget_event_t *event)
{
    if (event->sender[0] == '\0' && event->topic == GADGET_EVENT_SYSTEM_SAMPLE) {
        if (focused != NULL && focused->reads_system_details && focused->hooks->event != NULL) {
            focused->hooks->event(&focused->context, event);
        }
        return;
    }
    if (event->sender[0] != '\0') {
        const module_instance_t *source = find_module(event->sender);
        if (source == NULL || source->status != RUNNING) {
            return;
        }
    }
    for (unsigned i = 0; i < running_count; ++i) {
        module_instance_t *module = start_order[i];
        if (module->hooks->event != NULL) {
            module->hooks->event(&module->context, event);
        }
    }
}

uint64_t gadget_modules_update_at(unsigned face)
{
    const module_instance_t *module = workspaces[face];
    return module != NULL && module == focused && module->manifest.update_ms != 0 ? module->next_update : UINT64_MAX;
}

void gadget_modules_tick(unsigned face, uint64_t now_ms)
{
    if (gadget_modules_update_at(face) <= now_ms) {
        module_instance_t *module = workspaces[face];
        const uint32_t interval = module->manifest.update_ms;
        module->next_update = now_ms + interval - (now_ms - module->next_update) % interval;
        module->hooks->update(&module->context, now_ms);
    }
}

bool gadget_modules_take_redraw(unsigned face, gadget_damage_t *damage)
{
    module_instance_t *module = workspaces[face];
    if (module == NULL) {
        return false;
    }
    taskENTER_CRITICAL(&module->shared_lock);
    const bool redraw = module->redraw;
    *damage = module->damage;
    module->redraw = false;
    taskEXIT_CRITICAL(&module->shared_lock);
    return redraw;
}

void gadget_modules_draw(unsigned face, const gadget_canvas_t *canvas)
{
    module_instance_t *module = workspaces[face];
    if (module != NULL) {
        module->hooks->render(&module->context, canvas);
    }
}
