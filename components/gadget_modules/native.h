#pragma once

#include "gadget_module.h"
#include "kbelf.h"

const char *gadget_native_load(const char *path, kbelf_dyn *image, const gadget_module_t **hooks);
void gadget_native_unload(kbelf_dyn image);
