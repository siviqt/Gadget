#include "gadget_settings.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "gadget_config.h"

#define SETTINGS_LIMIT 4096
#define SETTINGS_VERSION 1

static const char *const TAG = "gadget_config";

cJSON *gadget_json_parse(const char *text, size_t length)
{
    if (text == NULL || length == 0 || length > 8192 || text[length] != '\0' || memchr(text, 0, length) != NULL) {
        return NULL;
    }
    // Limit recursion before passing untrusted JSON to cJSON on a small task stack.
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        const char ch = text[i];
        if (quoted && ch == '\\') {
            ++i;
        } else if (ch == '"') {
            quoted = !quoted;
        } else if (!quoted && (ch == '[' || ch == '{')) {
            if (++depth > 24) {
                return NULL;
            }
        } else if (!quoted && (ch == ']' || ch == '}')) {
            if (depth == 0) {
                return NULL;
            }
            --depth;
        }
    }
    return cJSON_ParseWithLengthOpts(text, length + 1, NULL, true);
}

cJSON *gadget_settings_read(const char *scope, bool (*validate)(const cJSON *))
{
    char *text = malloc(SETTINGS_LIMIT + 1);
    if (text == NULL) {
        return NULL;
    }
    cJSON *settings = NULL;
    for (gadget_state_copy_t copy = GADGET_STATE_CURRENT; copy <= GADGET_STATE_BACKUP; ++copy) {
        const int size = gadget_config_read(scope, GADGET_SETTINGS_FILE, copy, text, SETTINGS_LIMIT + 1);
        if (size == GADGET_STATE_FILE_MISSING) {
            continue;
        }
        cJSON *root = size > 0 ? gadget_json_parse(text, (size_t)size) : NULL;
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        cJSON *candidate = cJSON_GetObjectItemCaseSensitive(root, "settings");
        if (cJSON_IsNumber(version) && version->valuedouble == SETTINGS_VERSION && cJSON_IsObject(candidate) &&
            (validate == NULL || validate(candidate))) {
            settings = cJSON_DetachItemViaPointer(root, candidate);
            cJSON_Delete(root);
            if (copy != GADGET_STATE_CURRENT) {
                ESP_LOGI(TAG, "Recovered %s settings from copy %d", scope, copy);
            }
            break;
        }
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Invalid %s settings copy %d", scope, copy);
    }
    free(text);
    return settings;
}

bool gadget_settings_write(const char *scope, const cJSON *settings)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddNumberToObject(root, "version", SETTINGS_VERSION) == NULL ||
        !cJSON_AddItemReferenceToObject(root, "settings", (cJSON *)settings)) {
        cJSON_Delete(root);
        return false;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const size_t size = text != NULL ? strlen(text) : 0;
    const bool queued = size > 0 && size <= SETTINGS_LIMIT &&
                        gadget_config_write(scope, GADGET_SETTINGS_FILE, text, size);
    cJSON_free(text);
    return queued;
}
