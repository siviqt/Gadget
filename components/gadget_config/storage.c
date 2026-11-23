#include "gadget_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DATA_DIRECTORY "/sd/data"
#define FILE_NAME_BYTES 32
#define PATH_BYTES (sizeof(store_directory) + GADGET_MODULE_ID_SIZE + FILE_NAME_BYTES + 8)
#define JOB_LIMIT 16
#define SAVE_DELAY_US 750000

typedef struct {
    char id[GADGET_MODULE_ID_SIZE];
    char filename[FILE_NAME_BYTES];
    void *contents;
    size_t size;
    int64_t due;
    atomic_uint *result;
} save_job_t;

static const char *const TAG = "gadget_config";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t done_storage;
static SemaphoreHandle_t done;
static TaskHandle_t worker;
static bool stopping;
static save_job_t jobs[JOB_LIMIT];
static char store_directory[sizeof(DATA_DIRECTORY "/") + GADGET_APP_SLUG_SIZE];

static bool safe_name(const char *name, size_t limit)
{
    const size_t count = name == NULL ? 0 : strnlen(name, limit);
    return count > 0 && count < limit && name[0] != '.' &&
           strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == count;
}

static bool safe_filename(const char *name)
{
    if (!safe_name(name, FILE_NAME_BYTES)) {
        return false;
    }
    const size_t size = strlen(name);
    return size < 4 || (strcasecmp(name + size - 4, ".tmp") != 0 && strcasecmp(name + size - 4, ".bak") != 0);
}

static bool make_directory(const char *path)
{
    if (mkdir(path, 0775) == 0) {
        return true;
    }
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void file_path(char out[PATH_BYTES], const char *id, const char *filename, const char *suffix)
{
    snprintf(out, PATH_BYTES, "%s/%s/%s%s", store_directory, id, filename, suffix);
}

static bool write_file(const save_job_t *job)
{
    char directory[PATH_BYTES], target[PATH_BYTES], temporary[PATH_BYTES], backup[PATH_BYTES];
    snprintf(directory, sizeof(directory), "%s/%s", store_directory, job->id);
    if (!make_directory(DATA_DIRECTORY) || !make_directory(store_directory) || !make_directory(directory)) {
        return false;
    }
    file_path(target, job->id, job->filename, "");
    file_path(temporary, job->id, job->filename, ".tmp");
    file_path(backup, job->id, job->filename, ".bak");

    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        return false;
    }
    const bool synced = fwrite(job->contents, 1, job->size, file) == job->size &&
                        fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0 || !synced) {
        unlink(temporary);
        return false;
    }

    struct stat info;
    if (stat(target, &info) != 0) {
        return errno == ENOENT && rename(temporary, target) == 0;
    }
    if (!S_ISREG(info.st_mode) || (unlink(backup) != 0 && errno != ENOENT) || rename(target, backup) != 0) {
        return false;
    }
    // FAT rename cannot replace an existing name. The next startup can recover
    // either copy if power is lost between moving the old and new file.
    if (rename(temporary, target) != 0) {
        rename(backup, target);
        return false;
    }
    return true;
}

static void save_worker(void *unused)
{
    (void)unused;
    for (;;) {
        save_job_t job = {0};
        const int64_t now = esp_timer_get_time();
        int64_t next_due = INT64_MAX;
        taskENTER_CRITICAL(&lock);
        for (unsigned i = 0; i < JOB_LIMIT; ++i) {
            if (jobs[i].contents != NULL && jobs[i].due < next_due) {
                next_due = jobs[i].due;
            }
        }
        if (next_due != INT64_MAX && (stopping || next_due <= now)) {
            for (unsigned i = 0; i < JOB_LIMIT; ++i) {
                if (jobs[i].contents != NULL && jobs[i].due == next_due) {
                    job = jobs[i];
                    jobs[i].contents = NULL;
                    break;
                }
            }
        }
        const bool finished = stopping && next_due == INT64_MAX;
        taskEXIT_CRITICAL(&lock);
        if (job.contents != NULL) {
            const bool saved = write_file(&job);
            if (!saved) {
                ESP_LOGW(TAG, "Could not save %s/%s", job.id, job.filename);
            }
            free(job.contents);
            if (job.result != NULL) {
                atomic_store(job.result, saved ? GADGET_STATE_WRITE_SAVED : GADGET_STATE_WRITE_FAILED);
            }
        } else if (finished) {
            break;
        } else {
            const TickType_t wait = next_due == INT64_MAX ? portMAX_DELAY :
                                    1 + pdMS_TO_TICKS((next_due - now) / 1000);
            ulTaskNotifyTake(pdTRUE, wait);
        }
    }
    xSemaphoreGive(done);
    vTaskSuspend(NULL);
}

bool gadget_config_start(const char *slug)
{
    if (worker != NULL || !safe_name(slug, GADGET_APP_SLUG_SIZE)) {
        return false;
    }
    snprintf(store_directory, sizeof(store_directory), DATA_DIRECTORY "/%s", slug);
    stopping = false;
    done = xSemaphoreCreateBinaryStatic(&done_storage);
    return xTaskCreate(save_worker, "gadget_sd_save", 4096, NULL, 2, &worker) == pdPASS;
}

unsigned gadget_config_stack_free(void)
{
    return worker != NULL ? uxTaskGetStackHighWaterMark(worker) : UINT_MAX;
}

bool gadget_config_stop(void)
{
    if (worker == NULL) {
        return true;
    }
    taskENTER_CRITICAL(&lock);
    stopping = true;
    taskEXIT_CRITICAL(&lock);
    xTaskNotifyGive(worker);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "SD saves did not finish before exit");
        return false;
    }
    vTaskDelete(worker);
    worker = NULL;
    return true;
}

int gadget_config_read(const char *id, const char *filename, gadget_state_copy_t copy, char *out, size_t capacity)
{
    if (store_directory[0] == '\0' || !safe_name(id, GADGET_MODULE_ID_SIZE) || !safe_filename(filename) ||
        out == NULL || capacity == 0 || capacity > GADGET_STATE_FILE_LIMIT + 1 ||
        (unsigned)copy > GADGET_STATE_BACKUP) {
        return GADGET_STATE_FILE_UNREADABLE;
    }
    static const char *const suffix[] = {"", ".tmp", ".bak"};
    char path[PATH_BYTES];
    file_path(path, id, filename, suffix[copy]);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? GADGET_STATE_FILE_MISSING : GADGET_STATE_FILE_UNREADABLE;
    }
    const size_t count = fread(out, 1, capacity, file);
    const bool complete = count < capacity && feof(file) && !ferror(file);
    if (fclose(file) != 0 || !complete || memchr(out, 0, count) != NULL) {
        return GADGET_STATE_FILE_UNREADABLE;
    }
    out[count] = '\0';
    return (int)count;
}

static bool queue_write(const char *id, const char *filename, const void *contents, size_t size, atomic_uint *result)
{
    if (worker == NULL || !safe_name(id, GADGET_MODULE_ID_SIZE) || !safe_filename(filename) ||
        contents == NULL || size > GADGET_STATE_FILE_LIMIT) {
        return false;
    }
    save_job_t job = {
        .size = size, .due = esp_timer_get_time() + (result == NULL ? SAVE_DELAY_US : 0), .result = result,
    };
    strcpy(job.id, id);
    strcpy(job.filename, filename);
    job.contents = heap_caps_malloc(size != 0 ? size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job.contents == NULL) {
        return false;
    }
    memcpy(job.contents, contents, size);

    int slot = -1;
    void *replaced = NULL;
    taskENTER_CRITICAL(&lock);
    if (!stopping && (result == NULL || atomic_load(result) != GADGET_STATE_WRITE_PENDING)) {
        for (unsigned i = 0; i < JOB_LIMIT; ++i) {
            if (jobs[i].contents != NULL && strcmp(id, jobs[i].id) == 0 && strcmp(filename, jobs[i].filename) == 0) {
                // Preserve the ordering and the completion result of an explicit write.
                slot = result == NULL && jobs[i].result == NULL ? (int)i : -1;
                break;
            }
            if (slot < 0 && jobs[i].contents == NULL) {
                slot = (int)i;
            }
        }
        if (slot >= 0) {
            replaced = jobs[slot].contents;
            jobs[slot] = job;
            if (result != NULL) {
                atomic_store(result, GADGET_STATE_WRITE_PENDING);
            }
        }
    }
    taskEXIT_CRITICAL(&lock);
    free(replaced);
    if (slot < 0) {
        free(job.contents);
        return false;
    }
    xTaskNotifyGive(worker);
    return true;
}

bool gadget_config_write(const char *id, const char *filename, const void *contents, size_t size)
{
    return queue_write(id, filename, contents, size, NULL);
}

bool gadget_config_write_checked(const char *id, const char *filename, const void *contents, size_t size,
                                 atomic_uint *result)
{
    return result != NULL && queue_write(id, filename, contents, size, result);
}
