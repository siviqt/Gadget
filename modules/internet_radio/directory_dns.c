// SPDX-License-Identifier: MIT
// Bounded DNS SRV parsing adapted from TanRadio, copyright (c) 2026 TanRadio project.
#include "radio.h"

#include <string.h>

static const char service[] = "_api._tcp.radio-browser.info";
static const char suffix[] = ".api.radio-browser.info";

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] << 8 | bytes[1];
}

static bool read_name(const uint8_t *packet, size_t length, size_t *position, char out[DIRECTORY_HOST_BYTES])
{
    size_t index = *position, used = 0;
    bool jumped = false;
    for (unsigned step = 0; step < 64 && index < length; ++step) {
        const uint8_t label = packet[index++];
        if (label == 0) {
            if (!jumped) {
                *position = index;
            }
            out[used] = '\0';
            return true;
        }
        if ((label & 0xc0) == 0xc0) {
            if (index >= length) {
                return false;
            }
            const size_t next = (size_t)(label & 0x3f) << 8 | packet[index++];
            if (next >= index - 2) {
                return false;
            }
            if (!jumped) {
                *position = index;
            }
            jumped = true;
            index = next;
            continue;
        }
        if (label > 63 || index + label > length || used + label + 1 >= DIRECTORY_HOST_BYTES) {
            return false;
        }
        if (used != 0) {
            out[used++] = '.';
        }
        for (unsigned i = 0; i < label; ++i) {
            unsigned char ch = packet[index++];
            if (ch >= 'A' && ch <= 'Z') {
                ch += 'a' - 'A';
            }
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) {
                return false;
            }
            out[used++] = (char)ch;
        }
    }
    return false;
}

static bool service_name(const char *name)
{
    return radio_text_length(name, sizeof(service)) == sizeof(service) - 1 &&
           memcmp(name, service, sizeof(service)) == 0;
}

static unsigned parse_response(const uint8_t *packet, size_t length, uint16_t id,
                               char hosts[DIRECTORY_MIRRORS][DIRECTORY_HOST_BYTES])
{
    if (length < 12 || read_u16(packet) != id || (read_u16(packet + 2) & 0xfa0f) != 0x8000 ||
        read_u16(packet + 4) != 1) {
        return 0;
    }

    char name[DIRECTORY_HOST_BYTES];
    size_t at = 12;
    if (!read_name(packet, length, &at, name) || !service_name(name) ||
        at + 4 > length || read_u16(packet + at) != 33 || read_u16(packet + at + 2) != 1) {
        return 0;
    }
    at += 4;
    unsigned count = 0;
    const unsigned answers = read_u16(packet + 6);
    for (unsigned i = 0; i < answers && count < DIRECTORY_MIRRORS; ++i) {
        if (!read_name(packet, length, &at, name) || at + 10 > length) {
            return 0;
        }
        const bool srv = service_name(name) && read_u16(packet + at) == 33 && read_u16(packet + at + 2) == 1;
        const size_t size = read_u16(packet + at + 8);
        at += 10;
        if (at + size > length) {
            return 0;
        }
        if (srv && size >= 7 && read_u16(packet + at + 4) == 443) {
            size_t target = at + 6;
            if (!read_name(packet, length, &target, name) || target != at + size) {
                return 0;
            }
            const size_t size_name = radio_text_length(name, sizeof(name));
            if (size_name > sizeof(suffix) - 1 &&
                memcmp(name + size_name - (sizeof(suffix) - 1), suffix, sizeof(suffix)) == 0) {
                memcpy(hosts[count++], name, size_name + 1);
            }
        }
        at += size;
    }
    return count;
}

unsigned radio_find_directory_hosts(gadget_module_context_t *module,
                                    char hosts[DIRECTORY_MIRRORS][DIRECTORY_HOST_BYTES])
{
    const uint16_t id = (uint16_t)module->host->uptime_ms();
    uint8_t query[64] = {id >> 8, id & 255, 1, 0, 0, 1};
    size_t size = 12;
    for (const char *part = service; *part != '\0';) {
        const char *end = part;
        while (*end != '\0' && *end != '.') {
            ++end;
        }
        const size_t label = (size_t)(end - part);
        query[size++] = (uint8_t)label;
        memcpy(query + size, part, label);
        size += label;
        part = *end == '.' ? end + 1 : end;
    }
    query[size++] = 0;
    query[size++] = 0;
    query[size++] = 33; // SRV
    query[size++] = 0;
    query[size++] = 1;  // IN
    uint8_t reply[1536];
    const int length = module->host->dns_query(module, query, size, reply, sizeof(reply));
    return length > 0 ? parse_response(reply, (size_t)length, id, hosts) : 0;
}
