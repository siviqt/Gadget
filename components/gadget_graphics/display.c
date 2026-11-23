#include "gadget_graphics.h"
#include "drawing.h"

#include <inttypes.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/tanmatsu.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tanmatsu_coprocessor.h"

#define GAP_BUCKET_MS 2
#define GAP_BUCKETS 76

static const char *const TAG = "gadget_display";

typedef struct {
    int face;
    bool pending;
    gadget_damage_t damage;
} page_state_t;

typedef struct {
    unsigned frames, gaps, histogram[GAP_BUCKETS];
    int64_t last_frame, max_draw, max_scale, max_wait, max_interval;
} timing_t;

static struct {
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t refresh;
    ppa_client_handle_t scaler;
    pax_buf_t pages[GADGET_DISPLAY_BUFFERS];
    pax_buf_t previews[GADGET_FACE_COUNT];
    pax_buf_t animation;
    void *preview_pixels;
    void *animation_pixels;
    size_t width;
    size_t height;
    size_t page_bytes;
    size_t preview_stride;
    unsigned writable_page, shown_page, settled_face;
    unsigned ppa_frames;
    ppa_srm_rotation_angle_t rotation;
    page_state_t page_state[GADGET_DISPLAY_BUFFERS];
    timing_t transition_timing, workspace_timing;
    uint64_t workspace_report_at;
    bool settled, moving;
    bool big_endian;
    bool blanked, restore_lights;
    uint8_t lcd_pwm, keyboard_pwm;
    tanmatsu_coprocessor_handle_t backlight;
} display;

static StaticSemaphore_t refresh_storage;

static bool IRAM_ATTR refreshed(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    (void)panel;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

static pax_orientation_t panel_orientation(void)
{
    switch (bsp_display_get_default_rotation()) {
    case BSP_DISPLAY_ROTATION_90:
        return PAX_O_ROT_CCW;
    case BSP_DISPLAY_ROTATION_180:
        return PAX_O_ROT_HALF;
    case BSP_DISPLAY_ROTATION_270:
        return PAX_O_ROT_CW;
    default:
        return PAX_O_UPRIGHT;
    }
}

static ppa_srm_rotation_angle_t ppa_rotation(pax_orientation_t orientation)
{
    switch (orientation) {
    case PAX_O_ROT_CCW: return PPA_SRM_ROTATION_ANGLE_90;
    case PAX_O_ROT_HALF: return PPA_SRM_ROTATION_ANGLE_180;
    case PAX_O_ROT_CW: return PPA_SRM_ROTATION_ANGLE_270;
    default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

esp_err_t gadget_graphics_init(void)
{
    bsp_display_color_format_t format;
    bsp_display_endianness_t endian;
    ESP_RETURN_ON_ERROR(bsp_display_get_parameters(&display.width, &display.height, &format, &endian), TAG,
                        "Unable to query display");
    ESP_RETURN_ON_FALSE(format == BSP_DISPLAY_COLOR_FORMAT_16_565RGB && display.width % 2 == 0 &&
                        display.height % 2 == 0, ESP_ERR_NOT_SUPPORTED, TAG, "RGB565 even-sized panel required");
    display.page_bytes = display.width * display.height * sizeof(uint16_t);
    display.big_endian = endian == BSP_DISPLAY_ENDIAN_BIG;

    void *pixels[GADGET_DISPLAY_BUFFERS];
    ESP_RETURN_ON_ERROR(bsp_display_get_panel(&display.panel), TAG, "Unable to get DSI panel");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(display.panel, GADGET_DISPLAY_BUFFERS,
                                                           &pixels[0], &pixels[1]), TAG,
                        "Unable to get the two panel pages");
    const pax_orientation_t orientation = panel_orientation();
    display.rotation = ppa_rotation(orientation);
    for (unsigned page = 0; page < GADGET_DISPLAY_BUFFERS; ++page) {
        pax_buf_init(&display.pages[page], pixels[page], display.width, display.height, PAX_BUF_16_565RGB);
        pax_buf_reversed(&display.pages[page], display.big_endian);
        pax_buf_set_orientation(&display.pages[page], orientation);
        display.page_state[page].face = -1;
    }
    display.writable_page = 1; // The DPI driver starts by scanning page 0.

    const int half_width = pax_buf_get_width(&display.pages[0]) / 2;
    const int half_height = pax_buf_get_height(&display.pages[0]) / 2;
    display.preview_stride = (display.page_bytes / 4 + 127) & ~(size_t)127;
    display.preview_pixels = heap_caps_aligned_alloc(128, GADGET_FACE_COUNT * display.preview_stride,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    display.animation_pixels = heap_caps_aligned_alloc(128, display.preview_stride,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(display.preview_pixels != NULL && display.animation_pixels != NULL, ESP_ERR_NO_MEM, TAG,
                        "Unable to allocate the preview cache and transition surface");
    for (unsigned face = 0; face < GADGET_FACE_COUNT; ++face) {
        void *preview = (uint8_t *)display.preview_pixels + face * display.preview_stride;
        pax_buf_init(&display.previews[face], preview, half_width, half_height, PAX_BUF_16_565RGB);
    }
    pax_buf_init(&display.animation, display.animation_pixels, half_width, half_height, PAX_BUF_16_565RGB);

    display.refresh = xSemaphoreCreateBinaryStatic(&refresh_storage);
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = refreshed};
    // Gadget owns native page flips. The BSP's copy-completion callback is unnecessary in this mode.
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(display.panel, &callbacks, display.refresh), TAG,
                        "Unable to subscribe to display refresh");
    const ppa_client_config_t config = {.oper_type = PPA_OPERATION_SRM};
    if (!display.big_endian && ppa_register_client(&config, &display.scaler) != ESP_OK) {
        ESP_LOGW(TAG, "PPA unavailable. Using CPU scaling");
    }
    ESP_LOGI(TAG, "RGB565 %zux%zu: %zu bytes in panel pages, %zu bytes in previews, %zu bytes in transition surface",
             display.width, display.height, GADGET_DISPLAY_BUFFERS * display.page_bytes,
             GADGET_FACE_COUNT * display.preview_stride, display.preview_stride);
    return ESP_OK;
}

static void restore_backlights(void)
{
    if (!display.restore_lights) {
        return;
    }
    const esp_err_t lcd = tanmatsu_coprocessor_set_display_backlight(display.backlight, display.lcd_pwm);
    const esp_err_t keyboard = tanmatsu_coprocessor_set_keyboard_backlight(display.backlight, display.keyboard_pwm);
    display.restore_lights = lcd != ESP_OK || keyboard != ESP_OK;
    if (display.restore_lights) {
        ESP_LOGW(TAG, "Backlight restore: display %s, keyboard %s", esp_err_to_name(lcd), esp_err_to_name(keyboard));
    }
}

bool gadget_graphics_blanked(void)
{
    return display.blanked;
}

esp_err_t gadget_graphics_set_blanked(bool blanked)
{
    if (blanked == display.blanked) {
        return ESP_OK;
    }
    if (!blanked) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(display.panel, true), TAG, "Unable to enable the LCD");
        display.blanked = false;
        return ESP_OK;
    }

    if (!display.restore_lights) {
        // Cache raw PWM so percentage conversion cannot dim the lights over repeated wake cycles.
        ESP_RETURN_ON_ERROR(bsp_tanmatsu_coprocessor_get_handle(&display.backlight), TAG, "Backlight control unavailable");
        ESP_RETURN_ON_ERROR(tanmatsu_coprocessor_get_display_backlight(display.backlight, &display.lcd_pwm),
                            TAG, "Unable to read display brightness");
        ESP_RETURN_ON_ERROR(tanmatsu_coprocessor_get_keyboard_backlight(display.backlight, &display.keyboard_pwm),
                            TAG, "Unable to read keyboard brightness");
    }
    display.restore_lights = true;
    esp_err_t result = tanmatsu_coprocessor_set_keyboard_backlight(display.backlight, 0);
    if (result == ESP_OK) {
        result = tanmatsu_coprocessor_set_display_backlight(display.backlight, 0);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_disp_on_off(display.panel, false);
    }
    if (result != ESP_OK) {
        esp_lcd_panel_disp_on_off(display.panel, true);
        restore_backlights();
        return result;
    }
    display.blanked = true;
    return ESP_OK;
}

void gadget_graphics_shutdown(void)
{
    if (display.blanked) {
        const esp_err_t result = esp_lcd_panel_disp_on_off(display.panel, true);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Unable to enable the LCD before exit: %s", esp_err_to_name(result));
        }
    }
    restore_backlights();
    if (display.refresh != NULL) {
        const esp_lcd_dpi_panel_event_callbacks_t callbacks = {0};
        esp_lcd_dpi_panel_register_event_callbacks(display.panel, &callbacks, NULL);
    }
    if (display.scaler != NULL) {
        ppa_unregister_client(display.scaler);
    }
    heap_caps_free(display.animation_pixels);
    heap_caps_free(display.preview_pixels);
}

static esp_err_t present(int first_row, int end_row)
{
    if (display.blanked) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(display.panel, 0, first_row, display.width, end_row,
                                                  pax_buf_get_pixels(&display.pages[display.writable_page])),
                        TAG, "Page flip failed");

    // Retire the old DMA page only after two fresh frame edges, even if a flip lands near VSync.
    xSemaphoreTake(display.refresh, 0);
    for (unsigned edge = 0; edge < 2; ++edge) {
        ESP_RETURN_ON_FALSE(xSemaphoreTake(display.refresh, pdMS_TO_TICKS(100)) == pdTRUE, ESP_ERR_TIMEOUT, TAG,
                            "Timed out waiting for the panel to retire its old page");
    }
    display.shown_page = display.writable_page;
    display.writable_page ^= 1;
    restore_backlights();
    return ESP_OK;
}

typedef struct {
    int start, dx, dy;
} scanout_steps_t;

static scanout_steps_t scanout_steps(const pax_buf_t *page)
{
    const pax_vec2i origin = pax_orient_det_vec2i(page, (pax_vec2i){0, 0});
    const pax_vec2i right = pax_orient_det_vec2i(page, (pax_vec2i){1, 0});
    const pax_vec2i down = pax_orient_det_vec2i(page, (pax_vec2i){0, 1});
    return (scanout_steps_t){
        .start = origin.x + origin.y * page->width,
        .dx = right.x - origin.x + (right.y - origin.y) * page->width,
        .dy = down.x - origin.x + (down.y - origin.y) * page->width,
    };
}

static void scale_animation(pax_buf_t *page)
{
    const int half_width = display.animation.width;
    const int half_height = display.animation.height;
    uint16_t *output = pax_buf_get_pixels_rw(page);
    if (display.scaler != NULL) {
        const ppa_srm_oper_config_t resize = {
            .in = {.buffer = display.animation_pixels, .pic_w = half_width, .pic_h = half_height,
                   .block_w = half_width, .block_h = half_height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out = {.buffer = output, .buffer_size = display.page_bytes, .pic_w = display.width,
                    .pic_h = display.height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .scale_x = 2.0f,
            .scale_y = 2.0f,
            .rotation_angle = display.rotation,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        const esp_err_t result = ppa_do_scale_rotate_mirror(display.scaler, &resize);
        if (result == ESP_OK) {
            ++display.ppa_frames;
            return;
        }
        ESP_LOGW(TAG, "PPA scaling failed (%s). Using CPU scaling", esp_err_to_name(result));
        ppa_unregister_client(display.scaler);
        display.scaler = NULL;
    }

    const uint16_t *input = display.animation_pixels;
    const scanout_steps_t steps = scanout_steps(page);
    for (int y = 0; y < half_height; ++y) {
        int at = steps.start + 2 * y * steps.dy;
        for (int x = 0; x < half_width; ++x, at += 2 * steps.dx) {
            uint16_t color = input[y * half_width + x];
            if (display.big_endian) {
                color = __builtin_bswap16(color);
            }
            output[at] = color;
            output[at + steps.dx] = color;
            output[at + steps.dy] = color;
            output[at + steps.dx + steps.dy] = color;
        }
    }
}

static void capture_preview(const pax_buf_t *source, unsigned face)
{
    pax_buf_t *preview = &display.previews[face];
    const uint16_t *in = pax_buf_get_pixels(source);
    uint16_t *out = pax_buf_get_pixels_rw(preview);
    if (display.scaler != NULL) {
        // Display pages are already in panel orientation. Capture an upright texture.
        const ppa_srm_oper_config_t resize = {
            .in = {.buffer = in, .pic_w = display.width, .pic_h = display.height,
                   .block_w = display.width, .block_h = display.height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out = {.buffer = out, .buffer_size = display.preview_stride, .pic_w = preview->width,
                    .pic_h = preview->height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .scale_x = 0.5f,
            .scale_y = 0.5f,
            .rotation_angle = (4 - display.rotation) % 4,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(display.scaler, &resize) == ESP_OK) {
            return;
        }
    }

    const scanout_steps_t steps = scanout_steps(source);
    for (int y = 0; y < preview->height; ++y) {
        int at = steps.start + 2 * y * steps.dy;
        for (int x = 0; x < preview->width; ++x, at += 2 * steps.dx) {
            uint16_t a = in[at], b = in[at + steps.dx];
            uint16_t c = in[at + steps.dy], d = in[at + steps.dy + steps.dx];
            if (display.big_endian) {
                a = __builtin_bswap16(a);
                b = __builtin_bswap16(b);
                c = __builtin_bswap16(c);
                d = __builtin_bswap16(d);
            }
            const uint32_t rb = ((a & 0xf81fu) + (b & 0xf81fu) + (c & 0xf81fu) + (d & 0xf81fu) + 0x1002u) >> 2;
            const uint32_t g = ((a & 0x07e0u) + (b & 0x07e0u) + (c & 0x07e0u) + 0x0040u) >> 2;
            *out++ = (uint16_t)((rb & 0xf81fu) | (g & 0x07e0u));
        }
    }
}

static gadget_damage_t union_damage(gadget_damage_t a, gadget_damage_t b)
{
    if (a.width == 0 || b.width == 0) {
        return (gadget_damage_t){0};
    }
    const int x = a.x < b.x ? a.x : b.x;
    const int y = a.y < b.y ? a.y : b.y;
    const int right = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    const int bottom = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return (gadget_damage_t){x, y, right - x, bottom - y};
}

static void record(timing_t *timing, int64_t start, int64_t drawn, int64_t scaled, int64_t finished,
                   bool continuous)
{
    ++timing->frames;
    const int64_t gap = finished - timing->last_frame;
    if (timing->last_frame != 0 && (continuous || gap < 150000)) {
        unsigned bucket = (unsigned)(gap / (GAP_BUCKET_MS * 1000));
        if (bucket >= GAP_BUCKETS) {
            bucket = GAP_BUCKETS - 1;
        }
        ++timing->histogram[bucket];
        ++timing->gaps;
        if (gap > timing->max_interval) {
            timing->max_interval = gap;
        }
    }
    timing->last_frame = finished;
    if (drawn - start > timing->max_draw) {
        timing->max_draw = drawn - start;
    }
    if (scaled - drawn > timing->max_scale) {
        timing->max_scale = scaled - drawn;
    }
    if (finished - scaled > timing->max_wait) {
        timing->max_wait = finished - scaled;
    }
}

static unsigned interval_p95(const timing_t *timing)
{
    if (timing->gaps == 0) {
        return 0;
    }
    const unsigned target = (timing->gaps * 95 + 99) / 100;
    unsigned seen = 0;
    for (unsigned i = 0; i < GAP_BUCKETS; ++i) {
        seen += timing->histogram[i];
        if (seen >= target) {
            return i + 1 == GAP_BUCKETS ? (unsigned)(timing->max_interval / 1000 + 1) : (i + 1) * GAP_BUCKET_MS;
        }
    }
    return 0;
}

void gadget_graphics_prepare_workspace(unsigned face, unsigned function_key, const char *title,
                                       gadget_workspace_draw_t preview)
{
    gadget_draw_set_workspace(face, function_key, title);
    if (title != NULL && preview == NULL) {
        gadget_draw_preview(&display.previews[face], face);
        return;
    }
    pax_buf_t *scratch = &display.pages[display.writable_page];
    gadget_draw_workspace(scratch, face, false, preview);
    display.page_state[display.writable_page].face = -1;
    capture_preview(scratch, face);
}

esp_err_t gadget_graphics_workspace(unsigned face, bool received_input, gadget_workspace_draw_t draw,
                                    gadget_damage_t damage)
{
    if (display.moving || !display.settled || display.settled_face != face) {
        damage = (gadget_damage_t){0};
        display.workspace_timing = (timing_t){0};
        display.workspace_report_at = (uint64_t)esp_timer_get_time() + 5000000;
    }
    display.moving = false;
    for (unsigned page = 0; page < GADGET_DISPLAY_BUFFERS; ++page) {
        page_state_t *state = &display.page_state[page];
        state->damage = state->pending ? union_damage(state->damage, damage) : damage;
        state->pending = true;
    }

    const int64_t start = esp_timer_get_time();
    const unsigned page = display.writable_page;
    page_state_t *state = &display.page_state[page];
    int first_row = 0, end_row = display.height;
    if (state->face == (int)face && state->damage.width != 0 && draw != NULL) {
        gadget_draw_workspace_damage(&display.pages[page], face, draw, state->damage, &first_row, &end_row);
    } else {
        gadget_draw_workspace(&display.pages[page], face, received_input, draw);
    }
    *state = (page_state_t){.face = (int)face};
    const int64_t drawn = esp_timer_get_time();
    const esp_err_t result = present(first_row, end_row);
    const int64_t finished = esp_timer_get_time();
    if (result == ESP_OK) {
        display.settled = true;
        display.settled_face = face;
        timing_t *timing = &display.workspace_timing;
        record(timing, start, drawn, drawn, finished, false);
        if ((uint64_t)finished >= display.workspace_report_at) {
            if (timing->gaps != 0) {
                ESP_LOGI(TAG, "F%u %u frames, p95 active interval %ums, max draw=%" PRIi64 "us, scanout=%" PRIi64 "us",
                         gadget_workspace_function_key(face), timing->frames, interval_p95(timing), timing->max_draw,
                         timing->max_wait);
            }
            *timing = (timing_t){.last_frame = finished};
            display.workspace_report_at = finished + 5000000;
        }
    }
    return result;
}

esp_err_t gadget_graphics_transition(gadget_dice_frame_t frame)
{
    const int64_t started = esp_timer_get_time();
    if (!display.moving && display.settled) {
        capture_preview(&display.pages[display.shown_page], display.settled_face);
    }
    display.moving = true;
    gadget_draw_reel(&display.animation, display.previews, frame);
    const int64_t rendered = esp_timer_get_time();
    scale_animation(&display.pages[display.writable_page]);
    const int64_t scaled = esp_timer_get_time();
    display.page_state[display.writable_page].face = -1;
    const esp_err_t result = present(0, display.height);
    if (result == ESP_OK) {
        record(&display.transition_timing, started, rendered, scaled, esp_timer_get_time(), true);
    }
    return result;
}

void gadget_graphics_log_transition(void)
{
    const timing_t *timing = &display.transition_timing;
    ESP_LOGI(TAG, "%u reel frames, %u with PPA, p95 interval %ums, max draw=%" PRIi64
             "us, scale=%" PRIi64 "us, scanout=%" PRIi64 "us",
             timing->frames, display.ppa_frames, interval_p95(timing), timing->max_draw, timing->max_scale, timing->max_wait);
    memset(&display.transition_timing, 0, sizeof(display.transition_timing));
    display.ppa_frames = 0;
}

static pax_buf_t *overlay_page(void)
{
    if (display.settled && !display.moving) {
        capture_preview(&display.pages[display.shown_page], display.settled_face);
    }
    display.settled = false;
    display.moving = false;
    display.page_state[display.writable_page].face = -1;
    return &display.pages[display.writable_page];
}

esp_err_t gadget_graphics_guide(gadget_guide_t guide)
{
    gadget_draw_guide(overlay_page(), guide);
    return present(0, display.height);
}

esp_err_t gadget_graphics_layout(const gadget_layout_view_t *view)
{
    gadget_draw_layout(overlay_page(), view);
    return present(0, display.height);
}

esp_err_t gadget_graphics_error(const char *message)
{
    pax_buf_t *surface = &display.pages[display.writable_page];
    pax_background(surface, GADGET_BACKGROUND);
    gadget_text(surface, GADGET_TEXT, 24, 28, 28, "Gadget");
    gadget_text(surface, GADGET_TEXT, 20, 28, 105, message);
    gadget_text(surface, GADGET_MUTED, 16, 28, 158, "Esc twice  Launcher");
    pax_join();
    return present(0, display.height);
}
