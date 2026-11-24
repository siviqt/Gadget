#include "gadget_module.h"
#include "gadget_keycap.h"
#include "../pulse_event.h"

#define HELLO_INCREMENT_KEY GADGET_KEY_F1

static uint32_t presses;
static uint32_t ticks;

static bool init(gadget_module_context_t *module)
{
    presses = 0;
    ticks = 0;
    module->host->log(module, "Face initialized");
    return true;
}

static void activate(gadget_module_context_t *module)
{
    module->host->log(module, "Face activated");
}

static void deactivate(gadget_module_context_t *module)
{
    module->host->log(module, "Face deactivated");
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    if (key->kind == HELLO_INCREMENT_KEY || key->kind == GADGET_KEY_ENTER) {
        ++presses;
        module->host->log(module, "Counter incremented by the SD face");
        module->host->invalidate(module);
    }
}

static void event(gadget_module_context_t *module, const gadget_event_t *message)
{
    static const char source[] = PULSE_MODULE_ID;
    if (message->topic != PULSE_TOPIC || message->value < 0) {
        return;
    }
    for (size_t i = 0; i < sizeof(source); ++i) {
        if (message->sender[i] != source[i]) {
            return;
        }
    }
    ticks = (uint32_t)message->value;
    module->host->invalidate(module);
}

static const char *decimal(uint32_t value, char digits[11])
{
    unsigned index = 10;
    digits[index] = '\0';
    do {
        digits[--index] = '0' + value % 10;
        value /= 10;
    } while (value != 0);
    return &digits[index];
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    (void)module;
    const uint32_t white = 0xfff4f7fb;
    const uint32_t muted = 0xffaebfd3;
    const uint32_t accent = 0xff8ee3ce;
    const int margin = 28;
    const int right = canvas->width / 2;
    char digits[11];
    canvas->text(canvas->handle, margin, 28, 40, white, "Hello Gadget");
    canvas->rect(canvas->handle, margin, 84, canvas->width - 2 * margin, 1, 0xff3c5368);
    canvas->text(canvas->handle, margin, 100, 20, muted, "Counter");
    canvas->text(canvas->handle, right, 100, 20, muted, "Background service ticks");
    canvas->text(canvas->handle, margin, 128, 42, accent, decimal(presses, digits));
    canvas->text(canvas->handle, right, 128, 42, accent, decimal(ticks, digits));
    gadget_draw_function_key(canvas, margin, 183, HELLO_INCREMENT_KEY, 0xff263b4a, GADGET_KEYCAP_OPAQUE);
    canvas->text(canvas->handle, margin + GADGET_KEYCAP_WIDTH + 8, 185, 16, muted, "Increment");
    if (ticks == 0) {
        canvas->text(canvas->handle, right, 184, 16, muted, "Waiting for Pulse service");
    }
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .init = init,
    .activate = activate,
    .deactivate = deactivate,
    .render = render,
    .input = input,
    .event = event,
};

const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
