#pragma once

#include "cJSON.h"
#include "gadget_module.h"

#define GADGET_MODULE_PATH_SIZE 192
#define GADGET_DEPENDENCY_LIMIT 8

enum {
    MODULE_FACE = 1u << 0,
    MODULE_SERVICE = 1u << 1,
    MODULE_TIME = 1u << 0,
    MODULE_NETWORK = 1u << 1,
    MODULE_AUDIO = 1u << 2,
    MODULE_MOTION = 1u << 3,
    MODULE_STORAGE = 1u << 4,
};

typedef struct {
    char id[GADGET_MODULE_ID_SIZE];
    char name[40];
    char version[24];
    char executable[40];
    char dependencies[GADGET_DEPENDENCY_LIMIT][GADGET_MODULE_ID_SIZE];
    unsigned dependency_count;
    uint32_t sdk;
    unsigned kind;
    unsigned capabilities;
    unsigned face;
    bool startup_preview;
    uint32_t update_ms;
    uint32_t worker_stack;
    cJSON *document;
    const cJSON *settings;
} gadget_manifest_t;

const char *gadget_manifest_read(gadget_manifest_t *manifest, const char *folder);
bool gadget_manifest_int(const cJSON *item, uint32_t limit, uint32_t *value);
