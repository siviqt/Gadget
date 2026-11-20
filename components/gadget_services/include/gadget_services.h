#pragma once

#include <stdatomic.h>

#include "gadget_input.h"

typedef enum {
    GADGET_MESSAGE_INPUT,
    GADGET_MESSAGE_MODULE_EVENT,
    GADGET_MESSAGE_WAKE,
} gadget_message_kind_t;

typedef struct {
    gadget_message_kind_t kind;
    union {
        gadget_input_event_t input;
        gadget_event_t event;
    };
} gadget_message_t;

esp_err_t gadget_services_init(void);
void gadget_services_shutdown(void);
bool gadget_services_receive(gadget_message_t *message, TickType_t wait);
void gadget_services_input(const gadget_input_event_t *event);
bool gadget_services_publish(const gadget_event_t *event);
void gadget_services_wake(void);
uint64_t gadget_services_uptime_ms(void);
int64_t gadget_services_unix_seconds(void);
bool gadget_services_civil_time(bool utc, gadget_civil_time_t *result);
void gadget_services_motion_active(bool active);
bool gadget_services_motion_read(gadget_motion_t *sample);

bool gadget_services_network_connected(void);
gadget_wifi_status_t gadget_services_wifi_status(void);
gadget_http_stream_t *gadget_services_http_open(const char *url, int *status, const atomic_bool *stopping);
int gadget_services_http_read(gadget_http_stream_t *stream, void *buffer, size_t size);
int gadget_services_http_read_timed(gadget_http_stream_t *stream, void *buffer, size_t size, uint32_t timeout_ms);
void gadget_services_http_close(gadget_http_stream_t *stream);
int gadget_services_dns_query(const void *query, size_t length, void *reply, size_t capacity,
                              const atomic_bool *stopping);
bool gadget_services_audio_open(const void *owner, uint32_t sample_rate);
bool gadget_services_audio_open_buffered(const void *owner, uint32_t sample_rate, unsigned volume);
void gadget_services_audio_volume(const void *owner, unsigned volume);
void gadget_services_audio_levels(const void *owner, unsigned speaker, unsigned headphones);
void gadget_services_audio_jack(bool plugged);
bool gadget_services_audio_headphones(void);
bool gadget_services_audio_playing(const void *owner);
int gadget_services_audio_write(const void *owner, const int16_t *stereo, size_t frames);
void gadget_services_audio_close(const void *owner);

#define GADGET_METRICS_INTERVAL_MS 5000

void gadget_services_measure(void);
void gadget_services_measure_stack(const char *name, unsigned free_bytes);
void gadget_services_log_metrics(void);
void gadget_services_record_transition(uint32_t elapsed_us);
void gadget_services_system_status(gadget_system_status_t *result);
