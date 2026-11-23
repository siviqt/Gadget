#pragma once

#include <stdatomic.h>

#include "gadget_module.h"

#define GADGET_APP_SLUG_SIZE 49

bool gadget_config_start(const char *slug);
bool gadget_config_stop(void);
unsigned gadget_config_stack_free(void);
int gadget_config_read(const char *module_id, const char *filename, gadget_state_copy_t copy, char *out, size_t capacity);
bool gadget_config_write(const char *module_id, const char *filename, const void *contents, size_t size);
// The result must outlive the storage worker and start in GADGET_STATE_WRITE_IDLE.
bool gadget_config_write_checked(const char *module_id, const char *filename, const void *contents, size_t size,
                                 atomic_uint *result);
