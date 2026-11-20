#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gadget_dice.h"

#define MANIFEST_LIMIT 8192

static const cJSON *member(const cJSON *object, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

bool gadget_manifest_int(const cJSON *item, uint32_t limit, uint32_t *value)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0 && item->valuedouble <= limit)) {
        return false;
    }
    const uint32_t integer = (uint32_t)item->valuedouble;
    if (item->valuedouble != integer) {
        return false;
    }
    *value = integer;
    return true;
}

static bool copy_string(const cJSON *item, char *out, size_t capacity)
{
    const char *string = cJSON_GetStringValue(item);
    if (string == NULL || string[0] == '\0' || strlen(string) >= capacity) {
        return false;
    }
    strcpy(out, string);
    return true;
}

static bool module_id(const char *id)
{
    return id[0] >= 'a' && id[0] <= 'z' &&
           id[strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789.-_")] == '\0';
}

static cJSON *read_json(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    cJSON *document = NULL;
    if (length > 0 && length <= MANIFEST_LIMIT && fseek(file, 0, SEEK_SET) == 0) {
        char *text = malloc((size_t)length + 1);
        if (text != NULL) {
            if (fread(text, 1, length, file) == (size_t)length && memchr(text, 0, length) == NULL) {
                text[length] = '\0';
                document = cJSON_ParseWithLengthOpts(text, length + 1, NULL, true);
            }
            free(text);
        }
    }
    fclose(file);
    return document;
}

static bool read_capabilities(gadget_manifest_t *manifest, const cJSON *list)
{
    if (!cJSON_IsArray(list)) {
        return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const char *capability = cJSON_GetStringValue(item);
        if (capability == NULL) {
            return false;
        }
        if (strcmp(capability, "time") == 0) {
            manifest->capabilities |= MODULE_TIME;
        } else if (strcmp(capability, "network") == 0) {
            manifest->capabilities |= MODULE_NETWORK;
        } else if (strcmp(capability, "audio") == 0) {
            manifest->capabilities |= MODULE_AUDIO;
        } else if (strcmp(capability, "motion") == 0 && (manifest->kind & MODULE_FACE)) {
            manifest->capabilities |= MODULE_MOTION;
        } else if (strcmp(capability, "storage") == 0) {
            manifest->capabilities |= MODULE_STORAGE;
        } else {
            return false;
        }
    }
    return true;
}

static bool read_dependencies(gadget_manifest_t *manifest, const cJSON *list)
{
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > GADGET_DEPENDENCY_LIMIT) {
        return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        char *id = manifest->dependencies[manifest->dependency_count];
        if (!copy_string(item, id, GADGET_MODULE_ID_SIZE) || !module_id(id)) {
            return false;
        }
        ++manifest->dependency_count;
    }
    return true;
}

static const char *parse(gadget_manifest_t *manifest)
{
    const cJSON *doc = manifest->document;
    uint32_t format;
    if (!cJSON_IsObject(doc) || !gadget_manifest_int(member(doc, "format"), 1, &format) || format != 1) {
        return "expected module manifest format 1";
    }
    if (!copy_string(member(doc, "id"), manifest->id, sizeof(manifest->id)) || !module_id(manifest->id) ||
        !copy_string(member(doc, "name"), manifest->name, sizeof(manifest->name)) ||
        !copy_string(member(doc, "version"), manifest->version, sizeof(manifest->version)) ||
        !gadget_manifest_int(member(doc, "sdk"), UINT32_MAX, &manifest->sdk)) {
        return "invalid module id, name, version or SDK version";
    }
    if (!copy_string(member(doc, "executable"), manifest->executable, sizeof(manifest->executable)) ||
        manifest->executable[0] == '.' ||
        manifest->executable[strspn(manifest->executable, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")]) {
        return "executable must be a file name within the module folder";
    }
    const char *kind = cJSON_GetStringValue(member(doc, "kind"));
    if (kind != NULL && strcmp(kind, "face") == 0) {
        manifest->kind = MODULE_FACE;
    } else if (kind != NULL && strcmp(kind, "service") == 0) {
        manifest->kind = MODULE_SERVICE;
    } else if (kind != NULL && strcmp(kind, "combined") == 0) {
        manifest->kind = MODULE_FACE | MODULE_SERVICE;
    } else {
        return "unknown module kind";
    }
    uint32_t face;
    if (manifest->kind & MODULE_FACE) {
        if (!gadget_manifest_int(member(doc, "face"), GADGET_FACE_COUNT, &face) || face == 0) {
            return "face modules require a workspace number from 1 to 6";
        }
        manifest->face = face - 1;
        manifest->startup_preview = cJSON_IsTrue(member(doc, "startup_preview"));
    }
    const cJSON *interval = member(doc, "update_ms");
    if (interval != NULL && (!(manifest->kind & MODULE_FACE) ||
        !gadget_manifest_int(interval, 60000, &manifest->update_ms) || manifest->update_ms < 10)) {
        return "update_ms must be 10 to 60000 on a face module";
    }
    manifest->worker_stack = 8192;
    const cJSON *stack = member(doc, "worker_stack");
    if (stack != NULL && (!(manifest->kind & MODULE_SERVICE) ||
        !gadget_manifest_int(stack, 32768, &manifest->worker_stack) || manifest->worker_stack < 8192 ||
        manifest->worker_stack % 16 != 0)) {
        return "worker_stack must be 8192 to 32768 bytes in multiples of 16 on a service";
    }
    if (!read_capabilities(manifest, member(doc, "capabilities"))) {
        return "invalid or unavailable capabilities";
    }
    if (!read_dependencies(manifest, member(doc, "dependencies"))) {
        return "invalid dependency list";
    }
    manifest->settings = member(doc, "settings");
    return manifest->settings == NULL || cJSON_IsObject(manifest->settings) ? NULL : "settings must be an object";
}

const char *gadget_manifest_read(gadget_manifest_t *manifest, const char *folder)
{
    char path[GADGET_MODULE_PATH_SIZE + 16];
    int length = snprintf(path, sizeof(path), "%s/module.json", folder);
    if (length < 0 || length >= sizeof(path)) {
        return "module path too long";
    }
    manifest->document = read_json(path);
    if (manifest->document == NULL) {
        return "missing or invalid module.json";
    }
    return parse(manifest);
}
