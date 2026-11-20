// Freestanding memory operations linked into native modules.
#include <stdint.h>
#include <string.h>

void *memcpy(void *restrict destination, const void *restrict source, size_t size)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < size; ++i) {
        out[i] = in[i];
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    if ((uintptr_t)out <= (uintptr_t)in) {
        for (size_t i = 0; i < size; ++i) {
            out[i] = in[i];
        }
    } else {
        while (size != 0) {
            --size;
            out[size] = in[size];
        }
    }
    return destination;
}

void *memset(void *destination, int byte, size_t size)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < size; ++i) {
        out[i] = (unsigned char)byte;
    }
    return destination;
}

int memcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *left = a, *right = b;
    for (size_t i = 0; i < size; ++i) {
        if (left[i] != right[i]) {
            return (int)left[i] - right[i];
        }
    }
    return 0;
}
