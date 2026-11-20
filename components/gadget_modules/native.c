#include "native.h"

#include <stdio.h>
#include <string.h>

#define MODULE_IMAGE_LIMIT (2u * 1024u * 1024u)
#define ELF_RISCV 243u

static bool range_ok(uint32_t offset, uint32_t length, uint32_t size)
{
    return offset <= size && length <= size - offset;
}

static bool inspect_dynamic(FILE *file, const kbelf_progheader *segment)
{
    if (segment->file_size % sizeof(kbelf_dynentry) != 0 || fseek(file, segment->offset, SEEK_SET) != 0) {
        return false;
    }
    for (size_t i = 0; i < segment->file_size / sizeof(kbelf_dynentry); ++i) {
        kbelf_dynentry entry;
        if (fread(&entry, sizeof(entry), 1, file) != 1 || entry.tag == DT_NEEDED || entry.tag == DT_INIT ||
            entry.tag == DT_FINI || entry.tag == DT_INIT_ARRAY || entry.tag == DT_FINI_ARRAY ||
            entry.tag == DT_PREINIT_ARRAY) {
            return false;
        }
        if (entry.tag == DT_NULL) {
            return true;
        }
    }
    return false;
}

static bool inspect_elf(FILE *file)
{
    const long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    kbelf_header header;
    if (size <= 0 || size > MODULE_IMAGE_LIMIT || fseek(file, 0, SEEK_SET) != 0 ||
        fread(&header, sizeof(header), 1, file) != 1 || memcmp(header.magic, "\177ELF", 4) != 0 ||
        header.word_size != 1 || header.endianness != 1 || header.type != ET_DYN ||
        header.machine != ELF_RISCV || (header.flags & 6u) != 2u ||
        header.ph_ent_size != sizeof(kbelf_progheader) || header.ph_ent_num > 24 ||
        !range_ok(header.ph_offset, header.ph_ent_num * sizeof(kbelf_progheader), size)) {
        return false;
    }

    bool entry_present = false;
    bool dynamic_present = false;
    for (unsigned i = 0; i < header.ph_ent_num; ++i) {
        kbelf_progheader segment;
        if (fseek(file, header.ph_offset + i * sizeof(segment), SEEK_SET) != 0 ||
            fread(&segment, sizeof(segment), 1, file) != 1 ||
            !range_ok(segment.offset, segment.file_size, size)) {
            return false;
        }
        if (segment.type == PT_LOAD) {
            if (!range_ok(segment.vaddr, segment.mem_size, MODULE_IMAGE_LIMIT) ||
                segment.file_size > segment.mem_size || segment.alignment > 65536 ||
                (segment.alignment & (segment.alignment - 1)) != 0) {
                return false;
            }
            entry_present |= (segment.flags & PF_X) && header.entry >= segment.vaddr &&
                             header.entry - segment.vaddr < segment.file_size;
        } else if (segment.type == PT_DYNAMIC) {
            if (dynamic_present || !inspect_dynamic(file, &segment)) {
                return false;
            }
            dynamic_present = true;
        } else if (segment.type == PT_INTERP || segment.type == PT_TLS) {
            return false;
        }
    }
    return entry_present && dynamic_present;
}

const char *gadget_native_load(const char *path, kbelf_dyn *image, const gadget_module_t **hooks)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return "module ELF missing";
    }
    const bool valid = inspect_elf(file);
    fclose(file);
    if (!valid) {
        return "expected a bounded, self-contained RV32 ILP32F module ELF";
    }
    kbelf_dyn loaded = kbelf_dyn_create(0);
    if (loaded == NULL || !kbelf_dyn_set_exec(loaded, path, NULL) || !kbelf_dyn_load(loaded)) {
        kbelf_dyn_destroy(loaded);
        return "BadgeELF could not load or relocate the module";
    }
    const gadget_module_entry_fn entry = (gadget_module_entry_fn)(uintptr_t)kbelf_dyn_entrypoint(loaded);
    const gadget_module_t *table = entry != NULL ? entry() : NULL;
    if (table == NULL || table->abi != GADGET_MODULE_ABI || table->size != sizeof(*table)) {
        gadget_native_unload(loaded);
        return "entry point returned an incompatible callbacks table";
    }
    *image = loaded;
    *hooks = table;
    return NULL;
}

