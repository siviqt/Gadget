#include "gadget_services.h"
#include "peripherals.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "bsp/power.h"
#include "bsp/rtc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "wifi_settings.h"

#define MESSAGE_COUNT 32
#define IO_TIMEOUT_MS 1500
#define WIFI_ATTEMPT_MS 8000
#define ICY_INTERVAL_LIMIT (16u * 1024 * 1024)

struct gadget_http_stream {
    esp_http_client_handle_t client;
    uint32_t icy_interval;
};

static QueueHandle_t inbox;
static StaticQueue_t inbox_storage;
static uint8_t messages[MESSAGE_COUNT * sizeof(gadget_message_t)];
static StaticSemaphore_t net_lock_storage;
static SemaphoreHandle_t net_lock;
static bool clock_valid, wifi_started, radio_used;
static bool wifi_events_registered;
static portMUX_TYPE network_details_lock = portMUX_INITIALIZER_UNLOCKED;
static gadget_network_details_t network_details;
static unsigned network_generation;
static TaskHandle_t network_sampler;
static StaticSemaphore_t network_sampler_done_storage;
static SemaphoreHandle_t network_sampler_done;
static atomic_bool network_sample_pending, network_sampler_stopping;
static char zone_name[64] = "UTC";
static unsigned next_wifi_slot;
static uint64_t next_wifi_attempt;
static const char *const TAG = "gadget_services";

static void load_timezone(void)
{
    nvs_handle_t nvs;
    if (nvs_open("system", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    char zone[96];
    size_t size = sizeof(zone);
    if (nvs_get_str(nvs, "tz", zone, &size) == ESP_OK && zone[0] != '\0' && setenv("TZ", zone, 1) == 0) {
        tzset();
        size = sizeof(zone_name);
        if (nvs_get_str(nvs, "timezone", zone_name, &size) != ESP_OK || zone_name[0] == '\0') {
            strcpy(zone_name, "Local time");
        }
    }
    nvs_close(nvs);
}

esp_err_t gadget_services_init(void)
{
    inbox = xQueueCreateStatic(MESSAGE_COUNT, sizeof(gadget_message_t), messages, &inbox_storage);
    net_lock = xSemaphoreCreateMutexStatic(&net_lock_storage);
    network_sampler_done = xSemaphoreCreateBinaryStatic(&network_sampler_done_storage);
    gadget_audio_init();
    clock_valid = bsp_rtc_update_time() == ESP_OK;
    load_timezone();
    return inbox && net_lock && network_sampler_done ? ESP_OK : ESP_ERR_NO_MEM;
}

void gadget_services_shutdown(void)
{
    gadget_motion_shutdown();
    if (network_sampler != NULL) {
        atomic_store(&network_sampler_stopping, true);
        xTaskNotifyGive(network_sampler);
        if (xSemaphoreTake(network_sampler_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Wi-Fi diagnostics still stopping");
            return;
        }
        network_sampler = NULL;
    }
    if (wifi_started) {
        wifi_connection_disconnect();
        esp_wifi_stop();
    }
    if (radio_used) {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    }
}

bool gadget_services_receive(gadget_message_t *message, TickType_t wait)
{
    return xQueueReceive(inbox, message, wait) == pdTRUE;
}

void gadget_services_input(const gadget_input_event_t *event)
{
    if (event->action == GADGET_INPUT_AUDIO_JACK) {
        gadget_services_audio_jack(event->value != 0);
    }
    const gadget_message_t message = {.kind = GADGET_MESSAGE_INPUT, .input = *event};
    xQueueSend(inbox, &message, portMAX_DELAY);
}

bool gadget_services_publish(const gadget_event_t *event)
{
    const gadget_message_t message = {.kind = GADGET_MESSAGE_MODULE_EVENT, .event = *event};
    return xQueueSend(inbox, &message, 0) == pdTRUE;
}

void gadget_services_wake(void)
{
    const gadget_message_t message = {.kind = GADGET_MESSAGE_WAKE};
    xQueueSend(inbox, &message, 0);
}

uint64_t gadget_services_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int64_t gadget_services_unix_seconds(void)
{
    const time_t epoch = clock_valid ? time(NULL) : -1;
    return epoch >= INT64_C(946684800) && epoch < INT64_C(4102444800) ? epoch : -1;
}

bool gadget_services_civil_time(bool utc, gadget_civil_time_t *result)
{
    const time_t epoch = gadget_services_unix_seconds();
    struct tm civil;
    if (epoch < 0 || result == NULL || !(utc ? gmtime_r(&epoch, &civil) : localtime_r(&epoch, &civil))) {
        return false;
    }
    *result = (gadget_civil_time_t){
        .year = civil.tm_year + 1900,
        .month = civil.tm_mon + 1,
        .day = civil.tm_mday,
        .weekday = civil.tm_wday,
        .hour = civil.tm_hour,
        .minute = civil.tm_min,
        .second = civil.tm_sec,
        .zone = utc ? "UTC" : zone_name,
        .daylight_saving = !utc && civil.tm_isdst > 0,
    };
    return true;
}

static bool wifi_connected(void)
{
    // The pinned manager can retain its connected bit after a disconnect.
    esp_netif_ip_info_t address;
    wifi_ap_record_t ap;
    return wifi_started && wifi_connection_is_connected() &&
           esp_netif_get_ip_info(wifi_get_netif(), &address) == ESP_OK && address.ip.addr != 0 &&
           esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool gadget_services_network_connected(void)
{
    if (xSemaphoreTake(net_lock, 0) != pdTRUE) {
        return false;
    }
    const bool connected = wifi_connected();
    xSemaphoreGive(net_lock);
    return connected;
}

gadget_wifi_status_t gadget_services_wifi_status(void)
{
    if (xSemaphoreTake(net_lock, 0) != pdTRUE) {
        return GADGET_WIFI_CONNECTING;
    }
    // Diagnostics run on the workspace task. Read the local interface without a request to the radio.
    esp_netif_t *netif = wifi_started ? wifi_get_netif() : NULL;
    esp_netif_ip_info_t address;
    const bool online = esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &address) == ESP_OK &&
                        address.ip.addr != 0;
    const gadget_wifi_status_t status = !radio_used ? GADGET_WIFI_OFF :
                                        online ? GADGET_WIFI_ONLINE : GADGET_WIFI_OFFLINE;
    xSemaphoreGive(net_lock);
    return status;
}

static void wifi_details_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    (void)arg;
    (void)base;
    if (event != WIFI_EVENT_STA_CONNECTED && event != WIFI_EVENT_STA_DISCONNECTED &&
        event != WIFI_EVENT_STA_STOP) {
        return;
    }
    taskENTER_CRITICAL(&network_details_lock);
    ++network_generation;
    network_details.associated = event == WIFI_EVENT_STA_CONNECTED;
    network_details.ssid[0] = '\0';
    network_details.ipv4[0] = '\0';
    network_details.ipv6_count = 0;
    network_details.signal_valid = false;
    if (network_details.associated && data != NULL) {
        const wifi_event_sta_connected_t *connected = data;
        const size_t length = connected->ssid_len < sizeof(network_details.ssid) - 1 ?
                              connected->ssid_len : sizeof(network_details.ssid) - 1;
        memcpy(network_details.ssid, connected->ssid, length);
        network_details.ssid[length] = '\0';
    }
    taskEXIT_CRITICAL(&network_details_lock);
}

void gadget_services_network_details(gadget_network_details_t *result)
{
    taskENTER_CRITICAL(&network_details_lock);
    *result = network_details;
    taskEXIT_CRITICAL(&network_details_lock);
}

static void sample_network_details(void)
{
    if (xSemaphoreTake(net_lock, 0) != pdTRUE) {
        return;
    }
    gadget_network_details_t sample;
    taskENTER_CRITICAL(&network_details_lock);
    unsigned generation = network_generation;
    sample = network_details;
    taskEXIT_CRITICAL(&network_details_lock);

    esp_netif_t *netif = wifi_started ? wifi_get_netif() : NULL;
    if (netif != NULL) {
        sample.mac_valid = esp_netif_get_mac(netif, sample.mac) == ESP_OK;
        sample.signal_valid = false;
        sample.ipv4[0] = '\0';
        sample.ipv6_count = 0;
        const bool up = esp_netif_is_netif_up(netif);
        if (sample.associated || (!wifi_events_registered && up)) {
            wifi_ap_record_t ap;
            const esp_err_t result = esp_wifi_sta_get_ap_info(&ap);
            if (result == ESP_OK) {
                memcpy(sample.ssid, ap.ssid, sizeof(sample.ssid) - 1);
                sample.ssid[sizeof(sample.ssid) - 1] = '\0';
                sample.rssi_dbm = ap.rssi;
                sample.associated = sample.signal_valid = true;
            } else if (result == ESP_ERR_WIFI_NOT_CONNECT) {
                sample.associated = false;
                sample.ssid[0] = '\0';
            }
        }
        if (sample.associated && up) {
            esp_netif_ip_info_t address;
            if (esp_netif_get_ip_info(netif, &address) == ESP_OK && address.ip.addr != 0) {
                esp_ip4addr_ntoa(&address.ip, sample.ipv4, sizeof(sample.ipv4));
            }
#if CONFIG_LWIP_IPV6
            esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
            const int count = esp_netif_get_all_ip6(netif, addresses);
            for (int i = 0; i < count && sample.ipv6_count < GADGET_NETWORK_IPV6_LIMIT; ++i) {
                gadget_ipv6_address_t *ipv6 = &sample.ipv6[sample.ipv6_count];
                if (inet_ntop(AF_INET6, addresses[i].addr, ipv6->address, sizeof(ipv6->address)) != NULL) {
                    ipv6->link_local = esp_netif_ip6_get_addr_type(&addresses[i]) == ESP_IP6_ADDR_IS_LINK_LOCAL;
                    ++sample.ipv6_count;
                }
            }
#endif
        }
    }
    taskENTER_CRITICAL(&network_details_lock);
    if (generation == network_generation) {
        network_details = sample;
    }
    taskEXIT_CRITICAL(&network_details_lock);
    xSemaphoreGive(net_lock);
}

static void run_network_sampler(void *arg)
{
    (void)arg;
    const gadget_event_t event = {.topic = GADGET_EVENT_SYSTEM_SAMPLE};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load(&network_sampler_stopping)) {
            break;
        }
        sample_network_details();
        atomic_store(&network_sample_pending, false);
        gadget_services_publish(&event);
    }
    xSemaphoreGive(network_sampler_done);
    vTaskDelete(NULL);
}

bool gadget_services_request_network_sample(void)
{
    if (atomic_load(&network_sample_pending)) {
        return true;
    }
    if (xSemaphoreTake(net_lock, 0) != pdTRUE) {
        return false;
    }
    const bool started = wifi_started;
    xSemaphoreGive(net_lock);
    if (!started) {
        return false;
    }
    if (network_sampler == NULL &&
        xTaskCreate(run_network_sampler, "gadget_netstat", 4096, NULL, 2, &network_sampler) != pdPASS) {
        return false;
    }
    atomic_store(&network_sample_pending, true);
    xTaskNotifyGive(network_sampler);
    return true;
}

static bool connect_wifi(const atomic_bool *stopping)
{
    if (wifi_connected()) {
        return true;
    }
    const uint64_t now = gadget_services_uptime_ms();
    if (now < next_wifi_attempt) {
        return false;
    }
    if (!wifi_started) {
        radio_used = true;
        if (wifi_remote_initialize() != ESP_OK || wifi_connection_init_stack() != ESP_OK) {
            ESP_LOGW(TAG, "Unable to start shared Wi-Fi");
            next_wifi_attempt = now + WIFI_ATTEMPT_MS;
            return false;
        }
        wifi_started = true;
        wifi_events_registered = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_details_event, NULL) == ESP_OK;
    }
    const uint64_t deadline = gadget_services_uptime_ms() + WIFI_ATTEMPT_MS;
    for (unsigned tried = 0; tried < WIFI_SETTINGS_MAX && !atomic_load(stopping) &&
         gadget_services_uptime_ms() < deadline; ++tried) {
        const unsigned slot = next_wifi_slot;
        next_wifi_slot = (next_wifi_slot + 1) % WIFI_SETTINGS_MAX;
        if (wifi_connection_connect(slot, 1) != ESP_OK) {
            continue;
        }
        for (unsigned wait = 0; wait < 25 && !atomic_load(stopping) &&
             gadget_services_uptime_ms() < deadline; ++wait) {
            if (wifi_connected()) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!atomic_load(stopping) && wifi_connected()) {
        return true;
    }
    wifi_connection_disconnect();
    next_wifi_attempt = gadget_services_uptime_ms() + 1000;
    return false;
}

static bool ensure_network(const atomic_bool *stopping)
{
    if (atomic_load(stopping) || xSemaphoreTake(net_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    const bool connected = connect_wifi(stopping);
    xSemaphoreGive(net_lock);
    return connected && !atomic_load(stopping);
}

int gadget_services_dns_query(const void *query, size_t length, void *reply, size_t capacity,
                              const atomic_bool *stopping)
{
    if (query == NULL || reply == NULL || length < 12 || length > 512 || capacity < 12 || capacity > 1536 ||
        !ensure_network(stopping)) {
        return -1;
    }
    esp_netif_t *netif = wifi_get_netif();
    for (unsigned attempt = 0; attempt < 2 && !atomic_load(stopping); ++attempt) {
        esp_netif_dns_info_t server;
        if (esp_netif_get_dns_info(netif, attempt == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &server) != ESP_OK ||
            server.ip.type != ESP_IPADDR_TYPE_V4 || server.ip.u_addr.ip4.addr == 0) {
            continue;
        }
        const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            continue;
        }
        const struct timeval timeout = {.tv_usec = 650000};
        const struct sockaddr_in destination = {
            .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server.ip.u_addr.ip4.addr,
        };
        int bytes = -1;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            connect(fd, (const struct sockaddr *)&destination, sizeof(destination)) == 0 &&
            send(fd, query, length, 0) == (int)length) {
            bytes = recv(fd, reply, capacity, 0);
        }
        close(fd);
        if (bytes >= 12) {
            return bytes;
        }
    }
    return -1;
}

static esp_err_t read_icy_interval(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER || event->header_key == NULL || event->header_value == NULL ||
        strcasecmp(event->header_key, "icy-metaint") != 0) {
        return ESP_OK;
    }
    gadget_http_stream_t *stream = event->user_data;
    const char *value = event->header_value;
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    uint32_t interval = 0;
    while (*value >= '0' && *value <= '9' && interval <= ICY_INTERVAL_LIMIT) {
        interval = interval * 10 + (uint32_t)(*value++ - '0');
    }
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    if (*value != '\0' || interval == 0 || interval > ICY_INTERVAL_LIMIT ||
        (stream->icy_interval != 0 && stream->icy_interval != interval)) {
        stream->icy_interval = UINT32_MAX;
    } else {
        stream->icy_interval = interval;
    }
    return ESP_OK;
}

static gadget_http_stream_t *open_http(const char *url, int *status, uint32_t *interval, bool request_metadata,
                                       const atomic_bool *stopping)
{
    if (status != NULL) {
        *status = 0;
    }
    if (interval != NULL) {
        *interval = 0;
    }
    if (url == NULL || atomic_load(stopping)) {
        return NULL;
    }
    const bool https = strncmp(url, "https://", 8) == 0;
    if ((!https && strncmp(url, "http://", 7) != 0) || !ensure_network(stopping)) {
        return NULL;
    }

    gadget_http_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NULL;
    }
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = IO_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Gadget/0.3",
        .disable_auto_redirect = true,
        .event_handler = interval != NULL ? read_icy_interval : NULL,
        .user_data = stream,
    };
    stream->client = esp_http_client_init(&config);
    if (request_metadata && stream->client != NULL &&
        esp_http_client_set_header(stream->client, "Icy-MetaData", "1") != ESP_OK) {
        gadget_services_http_close(stream);
        return NULL;
    }
    for (unsigned redirects = 0; stream->client != NULL && redirects <= 3 && !atomic_load(stopping); ++redirects) {
        stream->icy_interval = 0;
        if (esp_http_client_open(stream->client, 0) != ESP_OK || esp_http_client_fetch_headers(stream->client) < 0) {
            break;
        }
        const int code = esp_http_client_get_status_code(stream->client);
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) {
            if (interval != NULL && code == 200) {
                *interval = stream->icy_interval;
                if (*interval == UINT32_MAX) {
                    break;
                }
            }
            if (status != NULL) {
                *status = code;
            }
            return stream;
        }
        char destination[640];
        if (esp_http_client_close(stream->client) != ESP_OK ||
            esp_http_client_clear_response_buffer(stream->client) != ESP_OK ||
            esp_http_client_set_redirection(stream->client) != ESP_OK ||
            esp_http_client_get_url(stream->client, destination, sizeof(destination)) != ESP_OK ||
            (strncmp(destination, "https://", 8) != 0 && (https || strncmp(destination, "http://", 7) != 0))) {
            break;
        }
    }
    gadget_services_http_close(stream);
    return NULL;
}

gadget_http_stream_t *gadget_services_http_open(const char *url, int *status, const atomic_bool *stopping)
{
    return open_http(url, status, NULL, false, stopping);
}

gadget_http_stream_t *gadget_services_http_open_icy(const char *url, int *status, uint32_t *interval,
                                                   const atomic_bool *stopping)
{
    if (interval == NULL) {
        return gadget_services_http_open(url, status, stopping);
    }
    int code;
    gadget_http_stream_t *stream = open_http(url, &code, interval, true, stopping);
    if (*interval == UINT32_MAX || code == 400 || code == 406) {
        gadget_services_http_close(stream);
        stream = open_http(url, &code, interval, false, stopping);
    }
    if (status != NULL) {
        *status = code;
    }
    return stream;
}

int gadget_services_http_read(gadget_http_stream_t *stream, void *buffer, size_t size)
{
    return gadget_services_http_read_timed(stream, buffer, size, IO_TIMEOUT_MS);
}

int gadget_services_http_read_timed(gadget_http_stream_t *stream, void *buffer, size_t size, uint32_t timeout_ms)
{
    if (stream == NULL || buffer == NULL || size > INT_MAX || timeout_ms == 0 || timeout_ms > IO_TIMEOUT_MS) {
        return -1;
    }
    esp_http_client_set_timeout_ms(stream->client, timeout_ms);
    const int count = esp_http_client_read(stream->client, buffer, (int)size);
    return count == -ESP_ERR_HTTP_EAGAIN ? GADGET_HTTP_AGAIN : count;
}

void gadget_services_http_close(gadget_http_stream_t *stream)
{
    if (stream != NULL) {
        if (stream->client != NULL) {
            esp_http_client_cleanup(stream->client);
        }
        free(stream);
    }
}
