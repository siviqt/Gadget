#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/cache_hal.h"
#include "native.h"
#include "kbelf/string.h"
#include "soc/soc_caps.h"

void gadget_native_unload(kbelf_dyn image)
{
    if (image != NULL) {
        kbelf_dyn_unload(image);
        kbelf_dyn_destroy(image);
    }
}

// SDK v1 modules use the Gadget host table. KBELF's weak built-in library
// table stays empty, so launcher applet symbols are outside the runtime.
kbelf_file kbelfx_find_lib(const char *name)
{
    (void)name;
    return NULL;
}

size_t kbelfq_strlen(const char *str) { return strlen(str); }
void kbelfq_strcpy(char *dst, const char *src) { strcpy(dst, src); }
const char *kbelfq_strrchr(const char *str, char ch) { return strrchr(str, ch); }
bool kbelfq_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
void kbelfq_memcpy(void *dst, const void *src, size_t count) { memcpy(dst, src, count); }
void kbelfq_memset(void *dst, uint8_t value, size_t count) { memset(dst, value, count); }
bool kbelfq_memeq(const void *a, const void *b, size_t count) { return memcmp(a, b, count) == 0; }

void *kbelfx_malloc(size_t size) { return malloc(size); }
void *kbelfx_realloc(void *memory, size_t size) { return realloc(memory, size); }
void kbelfx_free(void *memory) { free(memory); }

bool kbelfx_seg_alloc(kbelf_inst instance, size_t count, kbelf_segment *segments)
{
    if (count == 0 || !kbelf_inst_is_pie(instance)) {
        return false;
    }
    size_t alignment = 16;
    uintptr_t first = UINTPTR_MAX;
    uintptr_t end = 0;
    for (size_t i = 0; i < count; ++i) {
        const kbelf_segment *segment = &segments[i];
        if (segment->alignment > alignment) {
            alignment = segment->alignment;
        }
        if (segment->vaddr_req < first) {
            first = segment->vaddr_req;
        }
        if (segment->vaddr_req + segment->size > end) {
            end = segment->vaddr_req + segment->size;
        }
    }
    first &= ~(alignment - 1);
    const size_t rounded = (end - first + alignment - 1) & ~(alignment - 1);
    uint8_t *memory = aligned_alloc(alignment, rounded);
    if (memory == NULL) {
        return false;
    }
    segments[0].alloc_cookie = memory;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t address = (uintptr_t)(memory + (segments[i].vaddr_req - first));
        segments[i].laddr = address;
        segments[i].paddr = address;
        segments[i].vaddr_real = address;
    }
    return true;
}

void kbelfx_seg_free(kbelf_inst instance, size_t count, kbelf_segment *segments)
{
    (void)instance;
    if (count != 0) {
        free(segments[0].alloc_cookie);
    }
}

void *kbelfx_open(const char *path) { return fopen(path, "rb"); }
void kbelfx_close(void *file) { fclose(file); }
long kbelfx_read(void *file, void *buffer, long length)
{
    if (length < 0) {
        return -1;
    }
    return fread(buffer, 1, length, file);
}
int kbelfx_seek(void *file, long offset) { return fseek(file, offset, SEEK_SET); }

long kbelfx_load(kbelf_inst instance, void *file, kbelf_laddr address, kbelf_laddr length, kbelf_laddr capacity)
{
    (void)instance;
    if (length > capacity) {
        return -1;
    }
    memset((void *)(address + length), 0, capacity - length);
    return fread((void *)address, 1, length, file);
}

void kbelfx_cache_sync(kbelf_laddr address, size_t size)
{
#if SOC_CACHE_WRITEBACK_SUPPORTED
    cache_hal_writeback_addr(address, size);
    cache_hal_invalidate_addr(address, size);
#endif
    __asm__ volatile("fence.i" ::: "memory");
}

static size_t mapped_bytes(kbelf_inst instance, kbelf_laddr address)
{
    for (size_t i = 0; i < kbelf_inst_segment_len(instance); ++i) {
        const kbelf_segment segment = kbelf_inst_segment_get(instance, i);
        if (address >= segment.laddr && address - segment.laddr < segment.size) {
            return segment.size - (address - segment.laddr);
        }
    }
    return 0;
}

bool kbelfx_copy_from_user(kbelf_inst instance, void *buffer, kbelf_laddr address, size_t size)
{
    if (size > mapped_bytes(instance, address)) {
        return false;
    }
    memcpy(buffer, (const void *)address, size);
    return true;
}

bool kbelfx_copy_to_user(kbelf_inst instance, kbelf_laddr address, void *buffer, size_t size)
{
    if (size > mapped_bytes(instance, address)) {
        return false;
    }
    memcpy((void *)address, buffer, size);
    return true;
}

ptrdiff_t kbelfx_strlen_from_user(kbelf_inst instance, kbelf_laddr address)
{
    const char *start = (const char *)address;
    const size_t limit = mapped_bytes(instance, address);
    const char *end = limit != 0 ? memchr(start, '\0', limit) : NULL;
    return end != NULL ? end - start : -1;
}
