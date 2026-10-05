#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "cJSON.h"

#include "api.h"
#include "node_config.h"

static const char *TAG = "API";

#define HISTORY_LEN             90 // matches the dashboard's own rolling window

// Static per-node metadata for /api/nodes - the only place node coordinates
// live. TODO: replace with real surveyed GPS fixes.
typedef struct {
    uint8_t     node_id;
    const char *name;
    float       lat;
    float       lng;
} node_meta_t;

static const node_meta_t NODE_META[] = {
    { 1, "Slave 1 (Upstream)",  1.8710f, 103.0790f },
    { 2, "Slave 2 (Midstream)", 1.8656f, 103.0850f },
    { 3, "Master (Downstream)", 1.8620f, 103.0900f },
};
#define NODE_META_COUNT (sizeof(NODE_META) / sizeof(NODE_META[0]))

// Per-node circular buffer of recent readings, indexed 1-3 (slot 0 unused -
// mirrors main.c's own last_reading[4]/has_reading[4] convention).
static api_reading_t s_history[4][HISTORY_LEN];
static int s_history_count[4] = {0};
static int s_history_next[4]  = {0}; // next slot this node will write
static SemaphoreHandle_t s_history_lock;

static bool s_rtc_ok, s_sd_ok, s_radio_ok;

static esp_timer_handle_t s_wifi_timer, s_httpd_timer;
static int s_wifi_retry_s = 1; // next reconnect delay; 1, 2, 4, 8, 16, 30 s
static esp_err_t start_httpd(void);

static void wifi_retry_cb(void *arg) { esp_wifi_connect(); }
static void httpd_retry_cb(void *arg) { start_httpd(); }

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // ponytail: doubling delay capped at 30 s via a one-shot timer, so the
        // event loop never blocks; no jitter, fine for a single station.
        esp_timer_start_once(s_wifi_timer, (uint64_t)s_wifi_retry_s * 1000000);
        s_wifi_retry_s = s_wifi_retry_s >= 16 ? 30 : s_wifi_retry_s * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_s = 1;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_connect(void) {
    const esp_timer_create_args_t wt = { .callback = wifi_retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&wt, &s_wifi_timer));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static void set_cors(httpd_req_t *req) {
    // The browser dashboard and the Flutter app both fetch this cross-origin.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static void send_error(httpd_req_t *req, const char *status, const char *body) {
    set_cors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
}

static void send_oom(httpd_req_t *req) {
    send_error(req, "500 Internal Server Error",
               "{\"error\":{\"code\":\"internal\",\"message\":\"The master ran out of memory.\"}}");
}

static void send_json(httpd_req_t *req, cJSON *root) {
    char *out = root ? cJSON_PrintUnformatted(root) : NULL;
    if (!out) {
        send_oom(req);
        cJSON_Delete(root);
        return;
    }
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    cJSON_Delete(root);
}

static esp_err_t nodes_get_handler(httpd_req_t *req) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < NODE_META_COUNT; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "node_id", NODE_META[i].node_id);
        cJSON_AddStringToObject(o, "name", NODE_META[i].name);
        cJSON_AddNumberToObject(o, "lat", NODE_META[i].lat);
        cJSON_AddNumberToObject(o, "lng", NODE_META[i].lng);
        cJSON_AddItemToArray(arr, o);
    }
    send_json(req, arr);
    return ESP_OK;
}

static cJSON *reading_to_json(const api_reading_t *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "log_timestamp_s", r->log_timestamp_s);
    cJSON_AddNumberToObject(o, "node_id", r->node_id);
    cJSON_AddNumberToObject(o, "sender_timestamp_s", r->sender_timestamp_s);
    cJSON_AddNumberToObject(o, "temperature_c", r->temperature_c);
    cJSON_AddNumberToObject(o, "humidity_pct", r->humidity_pct);
    cJSON_AddNumberToObject(o, "distance_mm", r->distance_mm);
    cJSON_AddNumberToObject(o, "distance_stddev_mm", r->distance_stddev_mm);
    cJSON_AddNumberToObject(o, "water_level_mm", r->water_level_mm);
    return o;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "now_s", current_timestamp_s());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(o, "rtc_ok", s_rtc_ok);
    cJSON_AddBoolToObject(o, "sd_ok", s_sd_ok);
    cJSON_AddBoolToObject(o, "radio_ok", s_radio_ok);
    send_json(req, o);
    return ESP_OK;
}

static esp_err_t latest_get_handler(httpd_req_t *req) {
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    for (int n = 1; n <= 3; n++) {
        if (s_history_count[n] == 0) continue;
        int last_idx = (s_history_next[n] - 1 + HISTORY_LEN) % HISTORY_LEN;
        cJSON_AddItemToArray(arr, reading_to_json(&s_history[n][last_idx]));
    }
    xSemaphoreGive(s_history_lock);
    send_json(req, arr);
    return ESP_OK;
}

static esp_err_t history_get_handler(httpd_req_t *req) {
    int node_id = 0, limit = HISTORY_LEN;
    char query[64], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "node", val, sizeof(val)) == ESP_OK) node_id = atoi(val);
        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK) limit = atoi(val);
    }
    if (node_id < 1 || node_id > 3) {
        send_error(req, "400 Bad Request",
                   "{\"error\":{\"code\":\"bad_request\",\"message\":\"node must be 1, 2 or 3\"}}");
        return ESP_OK;
    }
    if (limit <= 0 || limit > HISTORY_LEN) limit = HISTORY_LEN;

    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    int available = s_history_count[node_id];
    int n = available < limit ? available : limit;
    for (int i = 0; i < n; i++) {
        // newest first
        int idx = (s_history_next[node_id] - 1 - i + HISTORY_LEN * 2) % HISTORY_LEN;
        cJSON_AddItemToArray(arr, reading_to_json(&s_history[node_id][idx]));
    }
    xSemaphoreGive(s_history_lock);
    send_json(req, arr);
    return ESP_OK;
}

void api_set_health(bool rtc_ok, bool sd_ok, bool radio_ok) {
    s_radio_ok = radio_ok;
    s_rtc_ok = rtc_ok;
    s_sd_ok = sd_ok;
}

void api_record_reading(const api_reading_t *r) {
    if (!s_history_lock || r->node_id < 1 || r->node_id > 3) return;
    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    int n = r->node_id;
    s_history[n][s_history_next[n]] = *r;
    s_history_next[n] = (s_history_next[n] + 1) % HISTORY_LEN;
    if (s_history_count[n] < HISTORY_LEN) s_history_count[n]++;
    xSemaphoreGive(s_history_lock);
}

static esp_err_t start_httpd(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server. Retrying in 30 s.");
        esp_timer_start_once(s_httpd_timer, 30 * 1000000ULL);
        return ESP_FAIL;
    }

    static const httpd_uri_t status_uri  = { .uri = "/api/status",  .method = HTTP_GET, .handler = status_get_handler };
    static const httpd_uri_t nodes_uri   = { .uri = "/api/nodes",   .method = HTTP_GET, .handler = nodes_get_handler };
    static const httpd_uri_t latest_uri  = { .uri = "/api/latest",  .method = HTTP_GET, .handler = latest_get_handler };
    static const httpd_uri_t history_uri = { .uri = "/api/history", .method = HTTP_GET, .handler = history_get_handler };
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &nodes_uri);
    httpd_register_uri_handler(server, &latest_uri);
    httpd_register_uri_handler(server, &history_uri);

    ESP_LOGI(TAG, "API up: GET /api/status, /api/nodes, /api/latest, /api/history?node=<1-3>&limit=<n>");
    return ESP_OK;
}

esp_err_t api_start(void) {
    s_history_lock = xSemaphoreCreateMutex();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Doesn't wait for an IP; httpd below serves once the retry handler gets one.
    wifi_connect();

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("jaga-master");
        mdns_instance_name_set("JAGA Master Node");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS up - reachable at http://jaga-master.local/");
    } else {
        ESP_LOGW(TAG, "mDNS init failed - use the IP address logged above instead.");
    }

    const esp_timer_create_args_t ht = { .callback = httpd_retry_cb, .name = "httpd_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ht, &s_httpd_timer));
    return start_httpd();
}
