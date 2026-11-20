// SPDX-License-Identifier: MIT
#include "gadget_module.h"

#define SERVICES_PER_PAGE 4

static const uint32_t background = 0xff111923;
static const uint32_t foreground = 0xfff0f5f8;
static const uint32_t muted = 0xffa8b9c7;
static const uint32_t accent = 0xff6ed9ed;
static const uint32_t rule = 0xff324657;
static gadget_system_status_t status;
static unsigned service_page;

static bool init(gadget_module_context_t *module)
{
    return GADGET_HOST_HAS(module->host, system_status) && GADGET_HOST_HAS(module->host, system_service);
}

static void activate(gadget_module_context_t *module)
{
    module->host->system_status(module, &status);
}

static void update(gadget_module_context_t *module, uint64_t now_ms)
{
    (void)now_ms;
    activate(module);
    module->host->invalidate(module);
}

static const char *wifi_text(unsigned state)
{
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
                        const char *label, gadget_heap_status_t heap)
{
    char line[80];
    host->format(line, sizeof(line), "Free %u   Low %u   Largest %u KiB",
                 (unsigned)(heap.free_bytes / 1024), (unsigned)(heap.low_bytes / 1024),
                 (unsigned)(heap.largest_block_bytes / 1024));
    metric(canvas, x, y, label, line);
}

static void draw_resources(const gadget_host_t *host, const gadget_canvas_t *canvas, int x, int y)
{
    canvas->text(canvas->handle, x, y, 20, accent, "Resources");
    heap_metric(host, canvas, x, y + 42, "Internal heap", status.internal);
    heap_metric(host, canvas, x, y + 100, "PSRAM", status.psram);

    char line[96];
    if (status.cpu_count == 2) {
        host->format(line, sizeof(line), "Core 0  %u%%   Core 1  %u%%",
                     (unsigned)status.cpu_busy_percent[0], (unsigned)status.cpu_busy_percent[1]);
    } else if (status.cpu_count == 1) {
        host->format(line, sizeof(line), "Core 0  %u%%", (unsigned)status.cpu_busy_percent[0]);
    } else {
        host->format(line, sizeof(line), "Awaiting sample");
    }
    metric(canvas, x, y + 158, "CPU busy", line);
    if (status.least_stack_task[0] != '\0') {
        host->format(line, sizeof(line), "%s  %u B", status.least_stack_task, (unsigned)status.least_stack_bytes);
    } else {
        host->format(line, sizeof(line), "Awaiting sample");
    }
    metric(canvas, x, y + 216, "Least stack headroom", line);
    if (status.transitions != 0) {
        host->format(line, sizeof(line), "Last %u ms   Longest %u ms",
                     (unsigned)(status.last_transition_us / 1000), (unsigned)(status.longest_transition_us / 1000));
    } else {
        host->format(line, sizeof(line), "Awaiting first switch");
    }
    metric(canvas, x, y + 274, "Reel settling time", line);
}

static void draw_services(gadget_module_context_t *module, const gadget_canvas_t *canvas, int x, int y)
{
    const gadget_host_t *host = module->host;
    char line[88];
    canvas->text(canvas->handle, x, y, 20, accent, "Services");
    metric(canvas, x, y + 42, "Wi-Fi", wifi_text(status.wifi));
    metric(canvas, x + 150, y + 42, "PCM audio", pcm_text(status.pcm));
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
        host->format(line, sizeof(line), "Left/Right  Services %u/%u", service_page + 1,
                     ((unsigned)status.services + SERVICES_PER_PAGE - 1) / SERVICES_PER_PAGE);
        canvas->text(canvas->handle, x, y + 326, 16, accent, line);
    }
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    const int margin = 32, top = 91;
    const bool wide = canvas->width >= 680;
    canvas->rect(canvas->handle, 0, 0, canvas->width, canvas->height, background);
    canvas->text(canvas->handle, margin, 27, 24, foreground, "System status");
    canvas->rect(canvas->handle, margin, 72, canvas->width - 2 * margin, 1, rule);
    if (wide) {
        canvas->rect(canvas->handle, canvas->width / 2, top, 1, 338, rule);
    }
    draw_resources(module->host, canvas, margin, top);
    draw_services(module, canvas, wide ? canvas->width / 2 + margin : margin, wide ? top : top + 345);
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    const unsigned pages = ((unsigned)status.services + SERVICES_PER_PAGE - 1) / SERVICES_PER_PAGE;
    if (pages > 1 && (key->kind == GADGET_KEY_LEFT || key->kind == GADGET_KEY_RIGHT)) {
        service_page = (service_page + (key->kind == GADGET_KEY_RIGHT ? 1 : pages - 1)) % pages;
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
    };
    return &module;
}
