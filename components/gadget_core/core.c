#include "gadget_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bootloader_common.h"
#include "bsp/device.h"
#include "bsp/storage.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "gadget_config.h"
#include "gadget_dice.h"
#include "gadget_graphics.h"
#include "gadget_input.h"
#include "gadget_modules.h"
#include "gadget_services.h"
#include "nvs_flash.h"

#define SD_APPS_PATH "/sd/apps/"
// AppFS 2.1.0 leaves the launcher path after the magic, handle and boot flag.
#define APPFS_BOOT_MAGIC UINT64_C(0x2af7de4a994f7236)
#define APPFS_PATH_OFFSET 13
#define APPFS_PATH_SIZE 128
#define MAX_MESSAGES_PER_FRAME 32

static const char *const TAG = "gadget";
static gadget_layout_t layout, saved_layout;
static uint64_t next_metrics_ms;

_Static_assert(sizeof(((rtc_retain_mem_t *)0)->custom) >= APPFS_PATH_OFFSET + APPFS_PATH_SIZE,
               "The AppFS launcher argument must fit in retained RTC memory");

static const char *launcher_app_slug(void)
{
    const rtc_retain_mem_t *retained = bootloader_common_get_rtc_retain_mem();
    uint64_t magic;
    memcpy(&magic, retained->custom, sizeof(magic));
    const char *path = (const char *)retained->custom + APPFS_PATH_OFFSET;
    if (magic != APPFS_BOOT_MAGIC || memchr(path, '\0', APPFS_PATH_SIZE) == NULL ||
        strncmp(path, SD_APPS_PATH, sizeof(SD_APPS_PATH) - 1) != 0) {
        return NULL;
    }
    const char *slug = path + sizeof(SD_APPS_PATH) - 1;
    const size_t length = strlen(slug);
    if (length == 0 || length >= GADGET_APP_SLUG_SIZE || slug[0] == '.' ||
        slug[strspn(slug, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")] != '\0') {
        return NULL;
    }
    return slug;
}

static bool package_file_exists(const char *slug, const char *filename)
{
    char path[sizeof(SD_APPS_PATH) + GADGET_APP_SLUG_SIZE + sizeof("/metadata.json")];
    struct stat file;
    const int length = snprintf(path, sizeof(path), SD_APPS_PATH "%s/%s", slug, filename);
    return length > 0 && (size_t)length < sizeof(path) && stat(path, &file) == 0 && S_ISREG(file.st_mode);
}

static void log_focus(unsigned face)
{
    ESP_LOGI(TAG, "Focus F%u %s (slot %u)", gadget_workspace_function_key(face), gadget_workspace_name(face), face + 1);
}

static TickType_t wait_until(uint64_t deadline_ms)
{
    const uint64_t now = gadget_services_uptime_ms();
    return deadline_ms <= now ? 0 : pdMS_TO_TICKS(deadline_ms - now + portTICK_PERIOD_MS - 1);
}

static TickType_t wait_for_update(const gadget_dice_t *dice)
{
    if (gadget_graphics_blanked()) {
        return wait_until(next_metrics_ms);
    }
    if (dice->moving) {
        return 0;
    }
    const uint64_t next = gadget_modules_update_at(dice->active);
    return wait_until(next < next_metrics_ms ? next : next_metrics_ms);
}

static void sample_metrics(void)
{
    const uint64_t now = gadget_services_uptime_ms();
    if (now >= next_metrics_ms) {
        next_metrics_ms = now + GADGET_METRICS_INTERVAL_MS;
        gadget_services_measure();
        gadget_services_measure_stack("gadget_keys", gadget_input_stack_free());
        gadget_services_measure_stack("gadget_sd_save", gadget_config_stack_free());
        gadget_modules_measure_stacks();
        gadget_services_log_metrics();
    }
}

static esp_err_t present_workspace(unsigned face, bool confirmed, gadget_damage_t damage)
{
    const gadget_workspace_draw_t draw = gadget_modules_title(face) == NULL ? NULL : gadget_modules_draw;
    return gadget_graphics_workspace(face, confirmed, draw, damage);
}

static const gadget_input_event_t *dispatch_message(const gadget_message_t *message)
{
    if (message->kind == GADGET_MESSAGE_MODULE_EVENT) {
        gadget_modules_event(&message->event);
    }
    if (message->kind != GADGET_MESSAGE_INPUT) {
        return NULL;
    }

    const gadget_input_event_t *input = &message->input;
    if (input->action == GADGET_INPUT_TOGGLE_DISPLAY) {
        const bool waking = gadget_graphics_blanked();
        const esp_err_t result = gadget_graphics_set_blanked(!waking);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Display toggle failed: %s", esp_err_to_name(result));
        }
        return waking && result == ESP_OK ? input : NULL;
    }
    if (input->action == GADGET_INPUT_REFRESH_WEATHER) {
        const gadget_event_t refresh = {.topic = GADGET_EVENT_WEATHER_REFRESH};
        gadget_modules_event(&refresh);
        return NULL;
    }
    if (input->action == GADGET_INPUT_VOLUME_STEP || input->action == GADGET_INPUT_AUDIO_JACK) {
        const gadget_event_t audio = {
            .topic = input->action == GADGET_INPUT_VOLUME_STEP ? GADGET_EVENT_VOLUME_STEP : GADGET_EVENT_AUDIO_JACK,
            .value = input->value,
        };
        gadget_modules_event(&audio);
        return NULL;
    }
    return !gadget_graphics_blanked() || input->action == GADGET_INPUT_EXIT ? input : NULL;
}

static esp_err_t show_sd_error(const char *description)
{
    ESP_RETURN_ON_ERROR(gadget_graphics_error(description), TAG, "Unable to display SD error");
    for (;;) {
        gadget_message_t message;
        if (!gadget_services_receive(&message, portMAX_DELAY)) {
            continue;
        }
        const gadget_input_event_t *input = dispatch_message(&message);
        if (input == NULL) {
            continue;
        }
        if (input->action == GADGET_INPUT_EXIT) {
            return ESP_OK;
        }
        if (input->action == GADGET_INPUT_TOGGLE_DISPLAY) {
            ESP_RETURN_ON_ERROR(gadget_graphics_error(description), TAG, "Unable to restore SD error");
        }
    }
}

static bool guide_selection(const gadget_input_event_t *input, unsigned *face)
{
    if (input->action == GADGET_INPUT_EXIT) {
        *face = GADGET_FACE_COUNT;
        return true;
    }
    if (input->action == GADGET_INPUT_SELECT_FACE && input->face < GADGET_FACE_COUNT) {
        *face = layout.shortcuts[input->face];
        return true;
    }
    return false;
}

static bool letter(const gadget_key_t *key, char lower)
{
    return key->kind == GADGET_KEY_TEXT && (key->modifiers & ~GADGET_MOD_SHIFT) == 0 &&
           (key->text[0] == lower || key->text[0] == lower - 'a' + 'A') && key->text[1] == '\0';
}

static void swap_layout_rows(gadget_layout_t *draft, const gadget_face_assignment_t *faces, bool shortcuts,
                             unsigned first, unsigned second)
{
    if (shortcuts) {
        const unsigned first_key = gadget_layout_key(draft, first), second_key = gadget_layout_key(draft, second);
        draft->shortcuts[first_key] = second;
        draft->shortcuts[second_key] = first;
    } else {
        draft->faces[first] = (gadget_face_setting_t){.assigned = true};
        draft->faces[second] = (gadget_face_setting_t){.assigned = true};
        if (faces[second].id != NULL) {
            strcpy(draft->faces[first].id, faces[second].id);
        }
        if (faces[first].id != NULL) {
            strcpy(draft->faces[second].id, faces[first].id);
        }
    }
}

// A workspace shortcut or exit closes both the editor and guide.
static esp_err_t edit_layout(unsigned *face, bool *selected)
{
    gadget_layout_t draft = saved_layout;
    gadget_layout_view_t view = {0};
    for (;;) {
        gadget_face_assignment_t faces[GADGET_FACE_COUNT];
        gadget_modules_preview_layout(&draft, faces);
        for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
            view.names[i] = faces[i].title;
            view.function_keys[i] = gadget_layout_key(&draft, i) + 1;
        }
        ESP_RETURN_ON_ERROR(gadget_graphics_layout(&view), TAG, "Unable to present layout editor");
        for (;;) {
            sample_metrics();
            gadget_message_t message;
            if (!gadget_services_receive(&message, wait_until(next_metrics_ms))) {
                continue;
            }
            const gadget_input_event_t *input = dispatch_message(&message);
            if (input == NULL) {
                continue;
            }
            if (guide_selection(input, face)) {
                *selected = true;
                return ESP_OK;
            }
            if (input->action == GADGET_INPUT_TOGGLE_DISPLAY) {
                break;
            }
            if (input->action != GADGET_INPUT_KEY) {
                continue;
            }
            const gadget_key_t *key = &input->key;
            if (key->kind == GADGET_LAYOUT_BACK_KEY || key->kind == GADGET_KEY_BACKSPACE) {
                return ESP_OK;
            }
            if (key->kind == GADGET_LAYOUT_SAVE_KEY || key->kind == GADGET_KEY_ENTER) {
                if (gadget_layout_save(&draft)) {
                    saved_layout = draft;
                    view.notice = "Layout queued. Relaunch Gadget to apply.";
                } else {
                    view.notice = "Could not queue layout save.";
                }
            } else if (key->kind == GADGET_LAYOUT_MODE_KEY || letter(key, 's')) {
                view.edit_shortcuts = !view.edit_shortcuts;
            } else if (key->kind == GADGET_LAYOUT_DEFAULTS_KEY || letter(key, 'd')) {
                gadget_layout_defaults(&draft);
                view.notice = "Save to apply the defaults.";
            } else if (key->kind == GADGET_KEY_UP || key->kind == GADGET_KEY_DOWN) {
                view.selected = (view.selected + (key->kind == GADGET_KEY_DOWN ? 1 : GADGET_FACE_COUNT - 1)) %
                                GADGET_FACE_COUNT;
            } else if (key->kind == GADGET_KEY_LEFT || key->kind == GADGET_KEY_RIGHT) {
                const unsigned other = (view.selected + (key->kind == GADGET_KEY_RIGHT ? 1 : GADGET_FACE_COUNT - 1)) %
                                       GADGET_FACE_COUNT;
                swap_layout_rows(&draft, faces, view.edit_shortcuts, view.selected, other);
                view.selected = other;
                view.notice = "Save to apply the layout.";
            } else {
                continue;
            }
            break;
        }
    }
}

// On return, face is the current or chosen workspace. GADGET_FACE_COUNT means exit to the launcher.
static esp_err_t wait_in_guide(bool startup, unsigned *face)
{
    const gadget_guide_t mode = startup ? GADGET_GUIDE_READY : GADGET_GUIDE_HELP;
    ESP_RETURN_ON_ERROR(gadget_graphics_guide(mode), TAG, "Unable to present workspace guide");
    int64_t shown_at_us = esp_timer_get_time();
    for (;;) {
        sample_metrics();
        gadget_message_t message;
        if (!gadget_services_receive(&message, wait_until(next_metrics_ms))) {
            continue;
        }
        const gadget_input_event_t *input = dispatch_message(&message);
        if (input == NULL) {
            continue;
        }
        if (input->action == GADGET_INPUT_TOGGLE_DISPLAY) {
            ESP_RETURN_ON_ERROR(gadget_graphics_guide(mode), TAG, "Unable to restore workspace guide");
            shown_at_us = esp_timer_get_time();
            continue;
        }
        if ((!startup || input->action == GADGET_INPUT_EXIT) && guide_selection(input, face)) {
            return ESP_OK;
        }
        if (input->action != GADGET_INPUT_KEY) {
            continue;
        }
        const gadget_key_t *key = &input->key;
        if (startup) {
            if (input->received_at_us >= shown_at_us && key->kind == GADGET_KEY_TEXT && key->modifiers == 0 &&
                key->text[0] == ' ' && key->text[1] == '\0') {
                return ESP_OK;
            }
        } else if (key->kind == GADGET_KEY_ENTER) {
            return ESP_OK;
        } else if (key->kind == GADGET_GUIDE_LAYOUT_KEY || letter(key, 'c')) {
            bool selected = false;
            ESP_RETURN_ON_ERROR(edit_layout(face, &selected), TAG, "Layout editor failed");
            if (selected) {
                return ESP_OK;
            }
            ESP_RETURN_ON_ERROR(gadget_graphics_guide(mode), TAG, "Unable to restore workspace guide");
        }
    }
}

static esp_err_t run_workspaces(void)
{
    unsigned first_face = 0;
    ESP_RETURN_ON_ERROR(wait_in_guide(true, &first_face), TAG, "Startup guide failed");
    if (first_face == GADGET_FACE_COUNT) {
        return ESP_OK;
    }

    gadget_dice_t dice;
    gadget_dice_init(&dice, first_face);
    bool received_input[GADGET_FACE_COUNT] = {0};
    bool help_requested = false;
    gadget_modules_focus(dice.active);
    ESP_RETURN_ON_ERROR(present_workspace(dice.active, false, (gadget_damage_t){0}), TAG, "Unable to present initial workspace");
    log_focus(dice.active);

    for (;;) {
        bool dirty = false;
        gadget_message_t message;
        TickType_t wait = wait_for_update(&dice);
        for (unsigned read = 0; read < MAX_MESSAGES_PER_FRAME && gadget_services_receive(&message, wait); ++read) {
            wait = 0;
            const gadget_input_event_t *input = dispatch_message(&message);
            if (input == NULL) {
                continue;
            }
            if (input->action == GADGET_INPUT_EXIT) {
                return ESP_OK;
            }
            if (input->action == GADGET_INPUT_TOGGLE_DISPLAY) {
                dirty = true;
            } else if (input->action == GADGET_INPUT_SELECT_FACE && input->face < GADGET_FACE_COUNT) {
                if (gadget_dice_select(&dice, layout.shortcuts[input->face], esp_timer_get_time())) {
                    gadget_modules_focus(GADGET_FACE_COUNT);
                }
            } else if (input->action == GADGET_INPUT_GUIDE) {
                help_requested = true;
                break;
            } else if (input->action == GADGET_INPUT_KEY && !dice.moving &&
                       !gadget_modules_input(dice.active, &input->key) &&
                       input->key.kind == GADGET_KEY_ENTER && !received_input[dice.active]) {
                received_input[dice.active] = true;
                dirty = true;
                ESP_LOGI(TAG, "Enter received by F%u", gadget_workspace_function_key(dice.active));
            }
        }

        if (gadget_graphics_blanked()) {
            sample_metrics();
            continue;
        }
        const int64_t now = esp_timer_get_time();
        const bool settled = gadget_dice_update(&dice, now);
        if (settled) {
            dirty = true;
            gadget_modules_focus(dice.active);
        }
        if (dice.moving) {
            ESP_RETURN_ON_ERROR(gadget_graphics_transition(dice.frame), TAG, "Unable to present reel");
            continue;
        }
        gadget_modules_tick(dice.active, gadget_services_uptime_ms());
        gadget_damage_t damage = {0};
        const bool invalidated = gadget_modules_take_redraw(dice.active, &damage);
        if (dirty || invalidated) {
            ESP_RETURN_ON_ERROR(present_workspace(dice.active, received_input[dice.active],
                                                  dirty ? (gadget_damage_t){0} : damage), TAG,
                                "Unable to present workspace");
        }
        if (settled) {
            const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - dice.started_us);
            gadget_services_record_transition(elapsed_us);
            log_focus(dice.active);
            ESP_LOGI(TAG, "Last selection took %" PRIu32 " us through presentation", elapsed_us);
            gadget_graphics_log_transition();
        }
        sample_metrics();
        if (help_requested) {
            help_requested = false;
            unsigned face = dice.active;
            ESP_RETURN_ON_ERROR(wait_in_guide(false, &face), TAG, "Workspace guide failed");
            if (face == GADGET_FACE_COUNT) {
                return ESP_OK;
            }
            if (gadget_dice_select(&dice, face, esp_timer_get_time())) {
                gadget_modules_focus(GADGET_FACE_COUNT);
                continue;
            }
            ESP_RETURN_ON_ERROR(present_workspace(dice.active, received_input[dice.active], (gadget_damage_t){0}), TAG,
                                "Unable to restore workspace");
        }
    }
}

static esp_err_t start_from_sd(void)
{
    esp_err_t result = bsp_storage_mount_advanced(BSP_STORAGE_TYPE_SDCARD, "/sd", GADGET_MODULE_LIMIT + 4, false);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(result));
        return show_sd_error("Insert a readable SD card with Gadget installed.");
    }
    const char *slug = launcher_app_slug();
    if (slug != NULL && package_file_exists(slug, "metadata.json") && package_file_exists(slug, "gadget.bin")) {
        ESP_LOGI(TAG, "Running Gadget from SD");
        if (!gadget_config_start(slug)) {
            ESP_LOGW(TAG, "Unable to start SD settings writer");
        }
        gadget_layout_load(&layout);
        saved_layout = layout;
        char modules[sizeof(SD_APPS_PATH) + GADGET_APP_SLUG_SIZE + sizeof("/modules")];
        snprintf(modules, sizeof(modules), SD_APPS_PATH "%s/modules", slug);
        gadget_modules_start(modules, &layout);
        for (unsigned face = 0; face < GADGET_FACE_COUNT; ++face) {
            const char *title = gadget_modules_title(face);
            const gadget_workspace_draw_t preview = gadget_modules_startup_preview(face) ? gadget_modules_draw : NULL;
            gadget_graphics_prepare_workspace(face, gadget_layout_key(&layout, face) + 1, title, preview);
        }
        sample_metrics();
        result = run_workspaces();
        const bool modules_stopped = gadget_modules_shutdown();
        const bool settings_saved = gadget_config_stop();
        if (!modules_stopped || !settings_saved) {
            return ESP_ERR_TIMEOUT;
        }
        gadget_services_shutdown();
    } else {
        ESP_LOGW(TAG, "Launcher SD package is missing or invalid");
        result = show_sd_error("Install Gadget on SD and start it from the launcher.");
    }
    bsp_storage_unmount(BSP_STORAGE_TYPE_SDCARD);
    return result;
}

static esp_err_t start(void)
{
    const bsp_configuration_t board = {
        .display = {.requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
                    .num_fbs = GADGET_DISPLAY_BUFFERS},
    };
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "Unable to initialize launcher NVS");
    ESP_RETURN_ON_ERROR(bsp_device_initialize(&board), TAG, "Unable to initialize Tanmatsu BSP");
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Tanmatsu ESP32-P4 revision %u.%u, %u cores", chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_RETURN_ON_ERROR(gadget_services_init(), TAG, "Unable to initialize shared services");
    ESP_RETURN_ON_ERROR(gadget_input_init(gadget_services_input), TAG, "Unable to initialize keyboard input");
    ESP_RETURN_ON_ERROR(gadget_graphics_init(), TAG, "Unable to initialize graphics");
    ESP_RETURN_ON_ERROR(gadget_graphics_guide(GADGET_GUIDE_LOADING), TAG, "Unable to present loading guide");
    return start_from_sd();
}

void gadget_run(void)
{
    const esp_err_t result = start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Runtime stopped: %s", esp_err_to_name(result));
    }
    gadget_input_shutdown();
    gadget_graphics_shutdown();
    bsp_device_restart_to_launcher();
}
