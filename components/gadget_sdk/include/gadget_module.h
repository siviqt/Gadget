#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GADGET_MODULE_ABI 1u
#define GADGET_MODULE_ID_SIZE 32u
#define GADGET_SHARED_COPY_LIMIT 3072u
#define GADGET_STATE_FILE_LIMIT (64u * 1024u)
#define GADGET_STATE_FILE_MISSING (-1)
#define GADGET_STATE_FILE_UNREADABLE (-2)
#define GADGET_HTTP_AGAIN (-2)

typedef struct gadget_module_context gadget_module_context_t;
typedef struct gadget_http_stream gadget_http_stream_t;
typedef struct gadget_json gadget_json_t;

typedef enum {
    GADGET_KEY_ENTER = 1,
    GADGET_KEY_LEFT,
    GADGET_KEY_RIGHT,
    GADGET_KEY_UP,
    GADGET_KEY_DOWN,
    GADGET_KEY_BACKSPACE,
    GADGET_KEY_TEXT,
    GADGET_KEY_TAB,
    GADGET_KEY_F1,
    GADGET_KEY_F2,
    GADGET_KEY_F3,
    GADGET_KEY_F4,
    GADGET_KEY_F5,
    GADGET_KEY_F6,
} gadget_key_kind_t;

enum {
    GADGET_MOD_SHIFT = 1u << 0,
    GADGET_MOD_CTRL = 1u << 1,
    GADGET_MOD_ALT = 1u << 2,
    GADGET_MOD_HYPER = 1u << 3,
};

typedef struct {
    uint32_t kind;
    uint32_t modifiers;
    char text[8];
} gadget_key_t;

typedef struct {
    char sender[GADGET_MODULE_ID_SIZE];
    uint32_t topic;
    int32_t value;
} gadget_event_t;

// The runtime sends this event with an empty sender.
enum {
    GADGET_EVENT_WEATHER_REFRESH = 1,
    GADGET_EVENT_VOLUME_STEP,
    GADGET_EVENT_AUDIO_JACK,
};

typedef struct {
    uint16_t year;
    uint8_t month, day, weekday;  // Sunday is 0.
    uint8_t hour, minute, second;
    const char *zone;
    bool daylight_saving;
} gadget_civil_time_t;

typedef struct {
    // The document is limited to 8 KiB and must have a trailing NUL at text[length].
    gadget_json_t *(*parse)(const char *text, size_t length);
    void (*destroy)(gadget_json_t *document);
    const gadget_json_t *(*member)(const gadget_json_t *object, const char *key);
    const gadget_json_t *(*at)(const gadget_json_t *array, unsigned index);
    bool (*number)(const gadget_json_t *value, float *number);
    const char *(*string)(const gadget_json_t *value);
} gadget_json_api_t;

// Workspace-relative damage. A zero width denotes a full redraw.
typedef struct {
    int16_t x, y, width, height;
} gadget_damage_t;

typedef struct {
    int x, y, width, height, advance;
} gadget_text_bounds_t;

typedef enum {
    GADGET_STATE_CURRENT,
    GADGET_STATE_PENDING,
    GADGET_STATE_BACKUP,
} gadget_state_copy_t;

typedef enum {
    GADGET_STATE_WRITE_IDLE,
    GADGET_STATE_WRITE_PENDING,
    GADGET_STATE_WRITE_SAVED,
    GADGET_STATE_WRITE_FAILED,
} gadget_state_write_status_t;

typedef struct {
    uint64_t sampled_at_ms;
    float gyro[3];   // Degrees per second.
    float accel[3];  // Meters per second squared.
    bool gyro_ready, accel_ready;
} gadget_motion_t;

typedef void (*gadget_worker_fn)(gadget_module_context_t *);

typedef struct {
    uint32_t free_bytes, low_bytes, largest_block_bytes;
} gadget_heap_status_t;

typedef enum {
    GADGET_WIFI_OFF,
    GADGET_WIFI_CONNECTING,
    GADGET_WIFI_OFFLINE,
    GADGET_WIFI_ONLINE,
} gadget_wifi_status_t;

typedef enum {
    GADGET_PCM_IDLE,
    GADGET_PCM_BUFFERING,
    GADGET_PCM_PLAYING,
    GADGET_PCM_BUSY,
} gadget_pcm_status_t;

typedef struct {
    gadget_heap_status_t internal, psram;
    uint32_t least_stack_bytes;
    char least_stack_task[32];
    uint32_t last_transition_us, longest_transition_us, transitions;
    uint8_t cpu_busy_percent[2];
    uint8_t cpu_count;  // Zero until there are two counter samples.
    uint8_t wifi, pcm;
    uint8_t modules, services;
} gadget_system_status_t;

typedef struct {
    uint8_t percent;
    bool present;
    bool charging;
    bool external_power;
} gadget_battery_status_t;

typedef struct {
    char name[40];
    uint8_t workers, active_workers;
} gadget_service_status_t;

// Valid only inside render(). Coordinates are relative to the clipped workspace.
// Colors use 0xAARRGGBB. Text is NUL-terminated UTF-8.
typedef struct {
    uint16_t width, height;
    void *handle;
    void (*rect)(void *handle, int x, int y, int w, int h, uint32_t argb);
    void (*text)(void *handle, int x, int y, unsigned pixels, uint32_t argb, const char *text);
    // Available when the host provides invalidate_region.
    void (*ellipse)(void *handle, int cx, int cy, int rx, int ry, uint32_t argb);
    gadget_damage_t damage;
    // Available when the host provides state_set_string (0.3.2 or newer).
    bool (*text_bounds)(void *handle, unsigned pixels, const char *text, gadget_text_bounds_t *);
} gadget_canvas_t;

typedef struct {
    uint32_t abi;
    uint32_t size;
    void (*log)(gadget_module_context_t *, const char *message);
    uint64_t (*uptime_ms)(void);
    int64_t (*unix_seconds)(gadget_module_context_t *);
    int32_t (*setting_int)(gadget_module_context_t *, const char *key, int32_t fallback);
    void (*invalidate)(gadget_module_context_t *);
    bool (*publish)(gadget_module_context_t *, uint32_t topic, int32_t value);

    // Worker-only. Returns true when shutdown was requested.
    bool (*wait_for_stop)(gadget_module_context_t *, uint32_t timeout_ms);
    // Worker-only, with the network capability. HTTP operations may block.
    gadget_http_stream_t *(*http_open)(gadget_module_context_t *, const char *url, int *status);
    int (*http_read)(gadget_module_context_t *, gadget_http_stream_t *, void *data, size_t bytes);
    void (*http_close)(gadget_module_context_t *, gadget_http_stream_t *);
    // Worker-only, with the audio capability. PCM is signed 16-bit stereo.
    bool (*audio_open)(gadget_module_context_t *, uint32_t sample_rate);
    int (*audio_write)(gadget_module_context_t *, const int16_t *stereo, size_t frames);
    void (*audio_close)(gadget_module_context_t *);

    // Optional additions to the v1 table. Check size before using them.
    int (*format)(char *out, size_t size, const char *format, ...)
        __attribute__((format(printf, 3, 4)));
    const char *(*setting_string)(gadget_module_context_t *, const char *key);
    bool (*civil_time)(gadget_module_context_t *, bool utc, gadget_civil_time_t *result);
    void (*wake_worker)(gadget_module_context_t *);
    // Serializes short copies of module-owned data shared with the worker.
    void (*copy_shared)(gadget_module_context_t *, void *out, const void *in, size_t size);
    bool (*network_connected)(gadget_module_context_t *);
    // Worker-only. Returns GADGET_HTTP_AGAIN when the timeout expires with no data.
    int (*http_read_timed)(gadget_module_context_t *, gadget_http_stream_t *, void *data,
                          size_t bytes, uint32_t timeout_ms);
    const gadget_json_api_t *json;
    // Call during service_start. Each registered worker gets its own HTTP stream.
    bool (*add_worker)(gadget_module_context_t *, const char *name, gadget_worker_fn, uint32_t stack_bytes);
    // An opt-in, prebuffered PCM sink. Volume is applied by the board codec.
    bool (*audio_open_buffered)(gadget_module_context_t *, uint32_t sample_rate, unsigned volume);
    void (*audio_volume)(gadget_module_context_t *, unsigned volume);
    bool (*audio_playing)(gadget_module_context_t *);
    // Read the latest sample while a motion-capable face is focused.
    bool (*motion_read)(gadget_module_context_t *, gadget_motion_t *);
    void (*invalidate_region)(gadget_module_context_t *, gadget_damage_t);
    // Worker-only DNS exchange through the active network's configured resolver.
    int (*dns_query)(gadget_module_context_t *, const void *query, size_t length, void *reply, size_t capacity);
    // Main-task settings for modules declaring storage. Returned strings are borrowed until the next set.
    const char *(*state_string)(gadget_module_context_t *, const char *key);
    int32_t (*state_int)(gadget_module_context_t *, const char *key, int32_t fallback);
    bool (*state_set_string)(gadget_module_context_t *, const char *key, const char *value);
    bool (*state_set_int)(gadget_module_context_t *, const char *key, int32_t value);
    // Read during init. Returns the length, or a GADGET_STATE_FILE_* error. Allow space for the trailing NUL.
    int (*state_read_file)(gadget_module_context_t *, const char *filename, gadget_state_copy_t copy,
                           char *contents, size_t capacity);
    // The runtime takes a copy and saves it on a background task.
    bool (*state_write_file)(gadget_module_context_t *, const char *filename, const void *contents, size_t size);
    void (*audio_levels)(gadget_module_context_t *, unsigned speaker, unsigned headphones);
    bool (*audio_headphones)(void);
    // Read the periodic runtime snapshot and enumerate loaded services on the runtime task.
    bool (*system_status)(gadget_module_context_t *, gadget_system_status_t *);
    bool (*system_service)(gadget_module_context_t *, unsigned index, gadget_service_status_t *);
    // Runtime task only. A module may have one checked write pending at a time.
    bool (*state_write_file_checked)(gadget_module_context_t *, const char *filename, const void *contents, size_t size);
    gadget_state_write_status_t (*state_write_status)(gadget_module_context_t *);
    // Network worker only. Requests ICY metadata. Zero interval indicates unframed audio.
    gadget_http_stream_t *(*http_open_icy)(gadget_module_context_t *, const char *url, int *status, uint32_t *interval);
    // Runtime task only. Returns false until a valid battery sample is available.
    bool (*battery_status)(gadget_module_context_t *, gadget_battery_status_t *);
} gadget_host_t;

#define GADGET_HOST_HAS(host, member) \
    ((host)->size >= offsetof(gadget_host_t, member) + sizeof((host)->member) && (host)->member != NULL)

struct gadget_module_context {
    const gadget_host_t *host;
    const char *package_path;
    void *state;
};

// All callbacks except worker run on Gadget's runtime task. Only worker may block.
// shutdown and service_stop also clean up a failed init and service_start respectively.
typedef struct {
    uint32_t abi;
    uint32_t size;
    bool (*init)(gadget_module_context_t *);
    void (*shutdown)(gadget_module_context_t *);
    void (*activate)(gadget_module_context_t *);
    void (*deactivate)(gadget_module_context_t *);
    void (*update)(gadget_module_context_t *, uint64_t now_ms);
    void (*render)(gadget_module_context_t *, const gadget_canvas_t *);
    void (*input)(gadget_module_context_t *, const gadget_key_t *);
    bool (*service_start)(gadget_module_context_t *);
    void (*service_stop)(gadget_module_context_t *);
    void (*event)(gadget_module_context_t *, const gadget_event_t *);
    void (*worker)(gadget_module_context_t *);
} gadget_module_t;

// ELF entry point. This function returns a static table and performs no setup.
const gadget_module_t *gadget_module_entry(void);
typedef const gadget_module_t *(*gadget_module_entry_fn)(void);
