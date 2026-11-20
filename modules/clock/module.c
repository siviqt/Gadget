// SPDX-License-Identifier: MIT
// Civil date arithmetic adapted from Chronos, copyright (c) 2026 Chronos project.
#include "gadget_module.h"

static const char *const months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};
static const char *const weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char *const full_weekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const unsigned before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

static const uint32_t text = 0xfff4f7fb, muted = 0xffaebfd3, accent = 0xff6ed9ed;
static const int margin = 28;
static gadget_civil_time_t current;
static bool valid, show_utc;
static int viewed_month;  // Zero follows the current month.

static bool leap_year(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static unsigned first_column(unsigned year, unsigned month)
{
    const unsigned y = year - 1;
    const unsigned days = 365 * y + y / 4 - y / 100 + y / 400 + before_month[month - 1] +
                          (month > 2 && leap_year(year));
    return days % 7;  // January 1, year 1 was a Monday.
}

static int month_index(const gadget_civil_time_t *date)
{
    return date->year * 12 + date->month - 1;
}

static bool init(gadget_module_context_t *module)
{
    if (!GADGET_HOST_HAS(module->host, state_set_int)) {
        return false;
    }
    show_utc = module->host->state_int(module, "show_utc", 0) == 1;
    valid = module->host->civil_time(module, show_utc, &current);
    return true;
}

static void update(gadget_module_context_t *module, uint64_t now_ms)
{
    (void)now_ms;
    gadget_civil_time_t next;
    const bool available = module->host->civil_time(module, show_utc, &next);
    if (available != valid ||
        (available && (next.second != current.second || next.minute != current.minute ||
                       next.hour != current.hour || next.day != current.day ||
                       next.month != current.month || next.year != current.year))) {
        valid = available;
        if (valid) {
            current = next;
        }
        module->host->invalidate(module);
    }
}

static void activate(gadget_module_context_t *module)
{
    update(module, 0);
}

static void input(gadget_module_context_t *module, const gadget_key_t *key)
{
    switch (key->kind) {
    case GADGET_KEY_LEFT:
    case GADGET_KEY_RIGHT:
        if (!valid) {
            return;
        }
        if (viewed_month == 0) {
            viewed_month = month_index(&current);
        }
        viewed_month += key->kind == GADGET_KEY_LEFT ? -1 : 1;
        if (viewed_month < 1900 * 12) {
            viewed_month = 1900 * 12;
        } else if (viewed_month > 2199 * 12 + 11) {
            viewed_month = 2199 * 12 + 11;
        }
        break;
    case GADGET_KEY_ENTER:
        viewed_month = 0;
        break;
    case GADGET_KEY_TEXT:
        if (key->text[0] != 'u' && key->text[0] != 'U') {
            return;
        }
        show_utc = !show_utc;
        if (!module->host->state_set_int(module, "show_utc", show_utc)) {
            module->host->log(module, "Unable to queue clock settings");
        }
        valid = module->host->civil_time(module, show_utc, &current);
        break;
    default:
        return;
    }
    module->host->invalidate(module);
}

static void centered(const gadget_canvas_t *canvas, int cx, int cy, unsigned pixels, uint32_t color, const char *label)
{
    gadget_text_bounds_t bounds;
    if (canvas->text_bounds(canvas->handle, pixels, label, &bounds)) {
        canvas->text(canvas->handle, cx - bounds.width / 2 - bounds.x, cy - bounds.height / 2 - bounds.y,
                     pixels, color, label);
    }
}

static void render_calendar(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    const int today = month_index(&current);
    const int index = viewed_month != 0 ? viewed_month : today;
    const unsigned year = index / 12;
    const unsigned month = index % 12 + 1;
    const unsigned first = first_column(year, month);
    const unsigned count = before_month[month] - before_month[month - 1] + (month == 2 && leap_year(year));
    const int left = canvas->width / 2 + 12;
    const int cell = (canvas->width - left - margin) / 7;
    const int rows_top = 106;
    const int row_height = (canvas->height - rows_top - 60) / 6;
    const unsigned pixels = 16;
    gadget_text_bounds_t longest_day = {0};
    canvas->text_bounds(canvas->handle, pixels, "88", &longest_day);
    const int box_width = longest_day.width + 10 < cell - 4 ? longest_day.width + 10 : cell - 4;
    const int box_height = longest_day.height + 8 < row_height - 2 ? longest_day.height + 8 : row_height - 2;
    char label[32];
    module->host->format(label, sizeof(label), "%s %u", months[month - 1], year);
    centered(canvas, left + 7 * cell / 2, 49, 24, text, label);
    for (unsigned column = 0; column < 7; ++column) {
        centered(canvas, left + (int)column * cell + cell / 2, 87, 14, muted, weekdays[column]);
    }
    for (unsigned day = 1; day <= count; ++day) {
        const unsigned slot = first + day - 1;
        const int x = left + (int)(slot % 7) * cell + cell / 2;
        const int y = rows_top + (int)(slot / 7) * row_height + row_height / 2;
        const bool marked = index == today && day == current.day;
        if (marked) {
            canvas->rect(canvas->handle, x - box_width / 2, y - box_height / 2, box_width, box_height, accent);
        }
        module->host->format(label, sizeof(label), "%u", day);
        centered(canvas, x, y, pixels, marked ? 0xff13202c : text, label);
    }
}

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    if (!valid) {
        canvas->text(canvas->handle, margin, 26, 36, text, "RTC time unavailable");
        canvas->text(canvas->handle, margin, 92, 20, muted, "Set the date and time in the Tanmatsu launcher.");
        return;
    }

    char line[72];
    module->host->format(line, sizeof(line), "%s%s", current.zone, current.daylight_saving ? "  (DST)" : "");
    const int top = canvas->height / 5;
    canvas->text(canvas->handle, margin, top, 18, accent, line);
    module->host->format(line, sizeof(line), "%02u:%02u:%02u", (unsigned)current.hour,
                         (unsigned)current.minute, (unsigned)current.second);
    canvas->text(canvas->handle, margin, top + 35, 60, text, line);
    module->host->format(line, sizeof(line), "%s, %u %s %u", full_weekdays[current.weekday],
                         (unsigned)current.day, months[current.month - 1], (unsigned)current.year);
    canvas->text(canvas->handle, margin, top + 113, 20, muted, line);
    render_calendar(module, canvas);
    canvas->text(canvas->handle, margin, canvas->height - 44, 16, muted,
                 "Left/Right  Month     Enter  Today     U  Local/UTC");
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .init = init,
    .activate = activate,
    .update = update,
    .render = render,
    .input = input,
};

const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
