// SPDX-License-Identifier: MIT
#include "gadget_module.h"
#include "gadget_keycap.h"

#define SERVICES_PER_PAGE 4
#define SYSTEM_PREV_PAGE_KEY GADGET_KEY_F1
#define SYSTEM_NEXT_PAGE_KEY GADGET_KEY_F2

static const uint32_t background = 0xff111923;
static const uint32_t foreground = 0xfff0f5f8;
static const uint32_t muted = 0xffa8b9c7;
static const uint32_t accent = 0xff6ed9ed;
static const uint32_t rule = 0xff324657;
static gadget_system_status_t status;
static gadget_system_details_t details;
static gadget_battery_status_t battery;
static bool battery_valid, details_valid, network_page;
static unsigned service_page;

static bool init(gadget_module_context_t *module)
{
    return GADGET_HOST_HAS(module->host, system_status) && GADGET_HOST_HAS(module->host, system_service);
}

static void activate(gadget_module_context_t *module)
{
    module->host->system_status(module, &status);
    battery_valid = GADGET_HOST_HAS(module->host, battery_status) && module->host->battery_status(module, &battery);
    details_valid = GADGET_HOST_HAS(module->host, system_details) && module->host->system_details(module, &details);
    const unsigned pages = (status.services + SERVICES_PER_PAGE - 1) / SERVICES_PER_PAGE;
    if (service_page >= pages) {
        service_page = 0;
    }
}

static void update(gadget_module_context_t *module, uint64_t now_ms)
{
    (void)now_ms;
    if (!GADGET_HOST_HAS(module->host, system_details)) {
        activate(module);
        module->host->invalidate(module);
    }
}

static void event(gadget_module_context_t *module, const gadget_event_t *event)
{
    if (event->sender[0] == '\0' && event->topic == GADGET_EVENT_SYSTEM_SAMPLE) {
        activate(module);
        module->host->invalidate(module);
    }
}

static const char *wifi_text(unsigned state)
{
    if (details_valid && details.network.associated && state != GADGET_WIFI_ONLINE) {
        for (unsigned i = 0; i < details.network.ipv6_count; ++i) {
            if (!details.network.ipv6[i].link_local) {
                return "Connected";
            }
        }
        return details.network.ipv4[0] ? "Connected" : "Obtaining IP";
    }
    switch (state) {
    case GADGET_WIFI_OFF: return "Off";
    case GADGET_WIFI_CONNECTING: return "Connecting";
    case GADGET_WIFI_OFFLINE: return "Offline";
    case GADGET_WIFI_ONLINE: return "Connected";
    default: return "Unavailable";
    }
}

static const char *pcm_text(unsigned state)
{
    switch (state) {
    case GADGET_PCM_IDLE: return "Idle";
    case GADGET_PCM_BUFFERING: return "Buffering";
    case GADGET_PCM_PLAYING: return "Playing";
    case GADGET_PCM_BUSY: return "Busy";
    default: return "Unavailable";
    }
}

static void metric(const gadget_canvas_t *canvas, int x, int y, const char *label, const char *value)
{
    canvas->text(canvas->handle, x, y, 16, muted, label);
    canvas->text(canvas->handle, x, y + 21, 18, foreground, value);
}

static void heap_metric(const gadget_host_t *host, const gadget_canvas_t *canvas, int x, int y,
                        const char *label, gadget_heap_status_t heap, uint32_t total)
{
    char line[80];
    if (total != 0) {
        const uint32_t used = total - (heap.free_bytes < total ? heap.free_bytes : total);
        const unsigned used_kib = used / 1024, total_kib = total / 1024;
        const unsigned percent = total_kib != 0 ? used_kib * 100 / total_kib : used * 100 / total;
        host->format(line, sizeof(line), "%u / %u KiB used (%u%%)",
                     used_kib, total_kib, percent);
        metric(canvas, x, y, label, line);
    }
    host->format(line, sizeof(line), "Free %u   Low %u   Largest %u KiB",
                 (unsigned)(heap.free_bytes / 1024), (unsigned)(heap.low_bytes / 1024),
                 (unsigned)(heap.largest_block_bytes / 1024));
    if (total != 0) {
        canvas->text(canvas->handle, x, y + 39, 14, muted, line);
    } else {
        metric(canvas, x, y, label, line);
    }
}

static void draw_resources(const gadget_host_t *host, const gadget_canvas_t *canvas, int x, int y)
{
    canvas->text(canvas->handle, x, y, 20, accent, "Resources");
    heap_metric(host, canvas, x, y + 38, "Internal heap", status.internal,
                details_valid ? details.internal_total_bytes : 0);
    heap_metric(host, canvas, x, y + 102, "PSRAM", status.psram,
                details_valid ? details.psram_total_bytes : 0);

    char line[96];
    if (status.cpu_count == 2) {
        host->format(line, sizeof(line), "Core 0  %u%%   Core 1  %u%%",
                     (unsigned)status.cpu_busy_percent[0], (unsigned)status.cpu_busy_percent[1]);
    } else if (status.cpu_count == 1) {
        host->format(line, sizeof(line), "Core 0  %u%%", (unsigned)status.cpu_busy_percent[0]);
    } else {
        host->format(line, sizeof(line), "Awaiting sample");
    }
    metric(canvas, x, y + 166, "CPU busy · 5 s", line);
    if (status.least_stack_task[0] != '\0') {
        host->format(line, sizeof(line), "%s  %u B", status.least_stack_task, (unsigned)status.least_stack_bytes);
    } else {
        host->format(line, sizeof(line), "Awaiting sample");
    }
    metric(canvas, x, y + 218, "Least stack headroom", line);
    if (status.transitions != 0) {
        host->format(line, sizeof(line), "Last %u ms   Longest %u ms",
                     (unsigned)(status.last_transition_us / 1000), (unsigned)(status.longest_transition_us / 1000));
    } else {
        host->format(line, sizeof(line), "Awaiting first switch");
    }
    metric(canvas, x, y + 270, "Reel settling time", line);
}

static void draw_signal(const gadget_canvas_t *canvas, int x, int y)
{
    const int dbm = details.network.rssi_dbm;
    const unsigned level = details_valid && details.network.signal_valid ?
                           1 + (dbm >= -80) + (dbm >= -70) + (dbm >= -60) : 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int height = 5 + (int)i * 4;
        canvas->rect(canvas->handle, x + (int)i * 8, y + 17 - height, 5, height, i < level ? accent : rule);
    }
}

static int text_width(const gadget_canvas_t *canvas, unsigned size, const char *text)
{
    gadget_text_bounds_t bounds;
    if (canvas->text_bounds != NULL && canvas->text_bounds(canvas->handle, size, text, &bounds)) {
        return bounds.advance > bounds.x + bounds.width ? bounds.advance : bounds.x + bounds.width;
    }
    int width = 0;
    for (; *text != '\0'; ++text) {
        width += (int)(size + 1) / 2;
    }
    return width;
}

static void draw_wifi_state(const gadget_canvas_t *canvas, int x, int y)
{
    const char *state = wifi_text(status.wifi);
    canvas->text(canvas->handle, x, y, 18, foreground, state);
    if (details_valid) {
        draw_signal(canvas, x + text_width(canvas, 18, state) + 10, y);
    }
}

static void draw_ssid(const gadget_canvas_t *canvas, int x, int y, int width, const char *ssid)
{
    if (width <= 0) {
        return;
    }
    char label[sizeof(details.network.ssid) + 3];
    unsigned length = 0;
    for (const unsigned char *p = (const unsigned char *)ssid; *p != '\0';) {
        char glyph[5] = {(char)*p};
        unsigned bytes = 1;
        if (*p >= 0xc2 && *p <= 0xf4) {
            const unsigned expected = *p < 0xe0 ? 2 : *p < 0xf0 ? 3 : 4;
            while (bytes < expected && (p[bytes] & 0xc0) == 0x80) {
                glyph[bytes] = (char)p[bytes];
                ++bytes;
            }
        }
        gadget_text_bounds_t bounds;
        if (*p < 32 || *p == 127 || (*p >= 128 && (canvas->text_bounds == NULL ||
            !canvas->text_bounds(canvas->handle, 16, glyph, &bounds)))) {
            label[length++] = '?';
        } else {
            for (unsigned i = 0; i < bytes; ++i) {
                label[length++] = glyph[i];
            }
        }
        p += bytes;
    }
    label[length] = '\0';
    if (text_width(canvas, 16, label) > width) {
        do {
            do {
                --length;
            } while (length > 0 && ((unsigned char)label[length] & 0xc0) == 0x80);
            label[length] = label[length + 1] = label[length + 2] = '.';
            label[length + 3] = '\0';
        } while (length > 0 && text_width(canvas, 16, label) > width);
    }
    canvas->text(canvas->handle, x, y, 16, foreground, label);
}

static void draw_network(const gadget_host_t *host, const gadget_canvas_t *canvas, int x, int y)
{
    const gadget_network_details_t *network = &details.network;
    canvas->text(canvas->handle, x, y, 16, muted, "Wi-Fi network");
    draw_ssid(canvas, x, y + 21, canvas->width - 2 * x - 176,
              network->ssid[0] ? network->ssid : network->associated ? "Unnamed network" : wifi_text(status.wifi));
    const int signal_x = canvas->width - x - 148;
    draw_wifi_state(canvas, signal_x, y);
    if (network->signal_valid) {
        char signal[24];
        host->format(signal, sizeof(signal), "%d dBm", (int)network->rssi_dbm);
        canvas->text(canvas->handle, signal_x, y + 21, 16, foreground, signal);
    }

    char mac[18];
    if (network->mac_valid) {
        host->format(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     (unsigned)network->mac[0], (unsigned)network->mac[1], (unsigned)network->mac[2],
                     (unsigned)network->mac[3], (unsigned)network->mac[4], (unsigned)network->mac[5]);
    }
    metric(canvas, x, y + 54, "Wi-Fi MAC", network->mac_valid ? mac : "Unavailable");
    metric(canvas, x, y + 107, "IPv4", network->ipv4[0] ? network->ipv4 : "Unassigned");
    if (network->ipv6_count == 0) {
        metric(canvas, x, y + 160, "IPv6", "Unassigned");
    }
    for (unsigned i = 0; i < network->ipv6_count; ++i) {
        metric(canvas, x, y + 160 + (int)i * 49,
               network->ipv6[i].link_local ? "IPv6 · Link-local" : "IPv6", network->ipv6[i].address);
    }
}

static void draw_services(gadget_module_context_t *module, const gadget_canvas_t *canvas, int x, int y, int right)
{
    const gadget_host_t *host = module->host;
    char line[88];
    canvas->text(canvas->handle, x, y, 20, accent, "Services");
    const int pcm_x = right - 108;
    canvas->text(canvas->handle, x, y + 42, 16, muted, "Wi-Fi");
    draw_wifi_state(canvas, x, y + 63);
    metric(canvas, pcm_x, y + 42, "PCM audio", pcm_text(status.pcm));
    if (details_valid && details.network.associated) {
        draw_ssid(canvas, x, y + 79, pcm_x - x - 8,
                  details.network.ssid[0] ? details.network.ssid : "Unnamed network");
    }
    host->format(line, sizeof(line), "%u modules   %u services", (unsigned)status.modules, (unsigned)status.services);
    canvas->text(canvas->handle, x, y + 100, 16, muted, line);
    for (unsigned row = 0; row < SERVICES_PER_PAGE; ++row) {
        gadget_service_status_t service;
        if (!host->system_service(module, service_page * SERVICES_PER_PAGE + row, &service)) {
            break;
        }
        if (service.workers == 0) {
            host->format(line, sizeof(line), "Running");
        } else {
            host->format(line, sizeof(line), "%u/%u workers running", (unsigned)service.active_workers,
                         (unsigned)service.workers);
        }
        metric(canvas, x, y + 128 + (int)row * 48, service.name, line);
    }
    if (status.services > SERVICES_PER_PAGE) {
        host->format(line, sizeof(line), "Page %u/%u", service_page + 1,
                     ((unsigned)status.services + SERVICES_PER_PAGE - 1) / SERVICES_PER_PAGE);
        const int footer = y + 316;
        gadget_draw_function_key(canvas, x, footer, SYSTEM_PREV_PAGE_KEY, rule, GADGET_KEYCAP_OPAQUE);
        canvas->text(canvas->handle, x + GADGET_KEYCAP_WIDTH + 7, footer + 2, 16, foreground, "Prev");
        gadget_draw_function_key(canvas, x + 78, footer, SYSTEM_NEXT_PAGE_KEY, rule, GADGET_KEYCAP_OPAQUE);
        canvas->text(canvas->handle, x + 78 + GADGET_KEYCAP_WIDTH + 7, footer + 2, 16, foreground, "Next");
        canvas->text(canvas->handle, x + 160, footer + 2, 16, muted, line);
    }
}

static void draw_battery(const gadget_host_t *host, const gadget_canvas_t *canvas, int x, int y)
{
    const bool present = battery_valid && battery.present;
    const uint32_t outline = present ? foreground : muted;
    canvas->rect(canvas->handle, x, y, 24, 14, outline);
    canvas->rect(canvas->handle, x + 1, y + 1, 22, 12, background);
    canvas->rect(canvas->handle, x + 24, y + 4, 3, 6, outline);
    const char *label = "Battery unavailable";
    char percentage[32];
    if (present) {
        const int fill = battery.percent * 20 / 100;
        if (fill > 0) {
            canvas->rect(canvas->handle, x + 2, y + 2, fill, 10, accent);
        }
        host->format(percentage, sizeof(percentage), "%u%%%s", (unsigned)battery.percent,
                     battery.charging ? "  Charging" : "");
        label = percentage;
    } else if (battery_valid) {
        label = battery.external_power ? "USB only" : "No battery";
    }
    canvas->text(canvas->handle, x + 34, y - 3, 16, outline, label);
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    const int margin = 32, top = 91;
    const bool wide = canvas->width >= 680;
    canvas->rect(canvas->handle, 0, 0, canvas->width, canvas->height, background);
    canvas->text(canvas->handle, margin, 27, 24, foreground, network_page ? "Network details" : "System status");
    draw_battery(module->host, canvas, canvas->width - margin - 168, 33);
    canvas->rect(canvas->handle, margin, 72, canvas->width - 2 * margin, 1, rule);
    if (network_page) {
        draw_network(module->host, canvas, margin, top);
    } else {
        if (wide) {
            canvas->rect(canvas->handle, canvas->width / 2, top, 1, 338, rule);
        }
        draw_resources(module->host, canvas, margin, top);
        draw_services(module, canvas, wide ? canvas->width / 2 + margin : margin, wide ? top : top + 325,
                      canvas->width - margin);
    }
    if (details_valid) {
        canvas->text(canvas->handle, margin, canvas->height - 35, 16, accent,
                     network_page ? "Tab   Overview" : "Tab   Network details");
    }
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (details_valid && key->kind == GADGET_KEY_TAB) {
        network_page = !network_page;
        module->host->invalidate(module);
        return;
    }
    const unsigned pages = ((unsigned)status.services + SERVICES_PER_PAGE - 1) / SERVICES_PER_PAGE;
    const bool previous = key->kind == SYSTEM_PREV_PAGE_KEY || key->kind == GADGET_KEY_LEFT;
    const bool next = key->kind == SYSTEM_NEXT_PAGE_KEY || key->kind == GADGET_KEY_RIGHT;
    if (!network_page && pages > 1 && (previous || next)) {
        service_page = (service_page + (next ? 1 : pages - 1)) % pages;
        module->host->invalidate(module);
    }
}

const gadget_module_t *gadget_module_entry(void)
{
    static const gadget_module_t module = {
        .abi = GADGET_MODULE_ABI,
        .size = sizeof(gadget_module_t),
        .init = init,
        .activate = activate,
        .update = update,
        .render = render,
        .input = input,
        .event = event,
    };
    return &module;
}
