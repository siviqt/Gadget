#include "gadget_layout.h"

#include <string.h>

#include "gadget_settings.h"

// A leading underscore reserves this scope outside the module ID space.
#define LAYOUT_SCOPE "_runtime"

void gadget_layout_defaults(gadget_layout_t *layout)
{
    *layout = (gadget_layout_t){0};
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        layout->shortcuts[i] = i;
    }
}

static bool valid_id(const char *id)
{
    return id != NULL && (*id == '\0' ||
           ((*id >= 'a' && *id <= 'z') && strlen(id) < GADGET_MODULE_ID_SIZE &&
            id[strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789.-_")] == '\0'));
}

static bool decode_layout(const cJSON *settings, gadget_layout_t *layout)
{
    const cJSON *faces = cJSON_GetObjectItemCaseSensitive(settings, "faces");
    const cJSON *shortcuts = cJSON_GetObjectItemCaseSensitive(settings, "shortcuts");
    if (!cJSON_IsArray(faces) || cJSON_GetArraySize(faces) != GADGET_FACE_COUNT ||
        !cJSON_IsArray(shortcuts) || cJSON_GetArraySize(shortcuts) != GADGET_FACE_COUNT) {
        return false;
    }
    gadget_layout_defaults(layout);
    unsigned used_slots = 0;
    for (unsigned i = 0; i < GADGET_FACE_COUNT; ++i) {
        const cJSON *face = cJSON_GetArrayItem(faces, (int)i);
        if (!cJSON_IsNull(face)) {
            const char *id = cJSON_GetStringValue(face);
            if (!valid_id(id)) {
                return false;
            }
            for (unsigned j = 0; j < i && *id != '\0'; ++j) {
                if (strcmp(layout->faces[j].id, id) == 0) {
                    return false;
                }
            }
            layout->faces[i].assigned = true;
            strcpy(layout->faces[i].id, id);
        }
        const cJSON *key = cJSON_GetArrayItem(shortcuts, (int)i);
        if (!cJSON_IsNumber(key) || !(key->valuedouble >= 1 && key->valuedouble <= GADGET_FACE_COUNT) ||
            key->valuedouble != key->valueint) {
            return false;
        }
        const unsigned slot = (unsigned)key->valueint - 1;
        if (used_slots & (1u << slot)) {
            return false;
        }
        used_slots |= 1u << slot;
        layout->shortcuts[i] = slot;
    }
    return true;
}

static bool valid_layout(const cJSON *settings)
{
    gadget_layout_t layout;
    return decode_layout(settings, &layout);
}

void gadget_layout_load(gadget_layout_t *layout)
{
    cJSON *settings = gadget_settings_read(LAYOUT_SCOPE, valid_layout);
    if (settings == NULL || !decode_layout(settings, layout)) {
        gadget_layout_defaults(layout);
    }
    cJSON_Delete(settings);
}

bool gadget_layout_save(const gadget_layout_t *layout)
{
    cJSON *settings = cJSON_CreateObject();
    cJSON *faces = cJSON_AddArrayToObject(settings, "faces");
    cJSON *shortcuts = cJSON_AddArrayToObject(settings, "shortcuts");
    bool ready = faces != NULL && shortcuts != NULL;
    for (unsigned i = 0; ready && i < GADGET_FACE_COUNT; ++i) {
        const gadget_face_setting_t *face = &layout->faces[i];
        cJSON *id = face->assigned ? cJSON_CreateString(face->id) : cJSON_CreateNull();
        cJSON *slot = cJSON_CreateNumber(layout->shortcuts[i] + 1);
        const bool added_id = cJSON_AddItemToArray(faces, id);
        const bool added_slot = cJSON_AddItemToArray(shortcuts, slot);
        if (!added_id) {
            cJSON_Delete(id);
        }
        if (!added_slot) {
            cJSON_Delete(slot);
        }
        ready = added_id && added_slot;
    }
    const bool queued = ready && valid_layout(settings) && gadget_settings_write(LAYOUT_SCOPE, settings);
    cJSON_Delete(settings);
    return queued;
}

unsigned gadget_layout_key(const gadget_layout_t *layout, unsigned slot)
{
    for (unsigned key = 0; key < GADGET_FACE_COUNT; ++key) {
        if (layout->shortcuts[key] == slot) {
            return key;
        }
    }
    return slot;
}
