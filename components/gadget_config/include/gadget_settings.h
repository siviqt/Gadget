#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#define GADGET_SETTINGS_FILE "settings.json"

// The caller owns the returned settings object. NULL means to use defaults.
cJSON *gadget_settings_read(const char *scope, bool (*validate)(const cJSON *));
bool gadget_settings_write(const char *scope, const cJSON *settings);
cJSON *gadget_json_parse(const char *text, size_t length);
