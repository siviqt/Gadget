#include "gadget_module.h"
#include "../pulse_event.h"

static uint32_t interval;

static bool service_start(gadget_module_context_t *module)
{
    const int32_t configured = module->host->setting_int(module, "interval_ms", 1000);
    interval = configured >= 100 && configured <= 60000 ? (uint32_t)configured : 1000;
    module->host->log(module, module->host->unix_seconds(module) >= 0 ? "Service started with RTC time" :
                                                                      "Service started with monotonic time");
    return true;
}

static void worker(gadget_module_context_t *module)
{
    int32_t tick = 0;
    while (!module->host->wait_for_stop(module, interval)) {
        if (tick < INT32_MAX) {
            ++tick;
        }
        module->host->publish(module, PULSE_TOPIC, tick);
        if (tick == 1) {
            module->host->log(module, "First background tick");
        }
    }
    module->host->log(module, "Worker stopped cooperatively");
}

static void service_stop(gadget_module_context_t *module)
{
    module->host->log(module, "Service stopped");
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .service_start = service_start,
    .service_stop = service_stop,
    .worker = worker,
};

const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
