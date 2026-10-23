#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_modem.h"
#include "esp_modem_netif.h"
#include "esp_modem_dce_service.h"
#include "sim800.h"  // the SIM900A speaks the same AT dialect

#include "api.h"
#include "node_config.h"

static const char *TAG = "API";

#define INGEST_BATCH 20
#define INGEST_QUEUE_LEN 128  // 10 min of own (30 s) and slave readings fits several times over
static QueueHandle_t s_ingest_q;

// Gates the SIM900A's power (logic-level N-MOSFET on its ground, high = on).
// IRFZ gate is driven from 5 V by an NPN (GPIO16 -> 1k -> base, 10k from GPIO16 to 3V3): see MODEM_PWR_ACTIVE.
// The mini board powers up by itself when supplied (PWRKEY is not driven).
#define MODEM_PWR_PIN    GPIO_NUM_16
#define MODEM_PWR_ACTIVE 0            // gate level that powers the modem; 0 if a transistor inverts the gate drive
#define MODEM_POWER(on)  gpio_set_level(MODEM_PWR_PIN, (on) ? MODEM_PWR_ACTIVE : !MODEM_PWR_ACTIVE)
#define MODEM_TX_PIN     GPIO_NUM_17  // ESP32 TX -> board pin 3VT (labels are from the MCU side)
#define MODEM_RX_PIN     GPIO_NUM_27  // ESP32 RX <- board pin 3VR, idles at 4 V: keep a 1k in series
#define MODEM_USE_KEY    0            // 1: press the key if no AT answer. Off: this board boots by itself and the key toggles it OFF
#define MODEM_KEY_PIN    GPIO_NUM_25  // -> board's D6 (power key): a ~1.5 s pulse toggles the module on/off
#define MODEM_KEY_ACTIVE 1            // level that counts as pressed; flip to 0 if the pulse does nothing
#define MODEM_KEY_MS     1500
#define MODEM_SYNC_ROUNDS 20 // tries of 115200/19200/9600, about 12 s each
#define MODEM_REG_TRIES  30           // CGREG polls, 3 s apart
#define MODEM_BOOT_MS    8000         // power-up to first AT
#define MODEM_ALWAYS_ON  1            // 1: gate held high (USB power); 0: modem only powered during a push
#define MODEM_START_DELAY_S 600       // modem stays unpowered this long after boot so the supply caps charge first
#define PUSH_OFF_S       120  // modem stays off this long, then powers up and pushes
#define PUSH_GIVE_UP_S   300  // no successful push by then: count a failure, power down
#define TIME_VALID_S     1600000000   // system clock is real (SNTP) above this

#define MODEM_AT_TEST    0  // 1: only send AT at several baud rates and print what comes back; no pushing

#define BIT_GOT_IP  BIT0
#define BIT_STOPPED BIT1
static EventGroupHandle_t s_ev;
static uint32_t s_push_fails;
static esp_pm_lock_handle_t s_awake;  // no light sleep while the modem is up: UART and PPP need the CPU

static bool s_rtc_ok, s_sd_ok, s_radio_ok;
static int s_sim_rssi_dbm;  // from the last AT+CSQ, 0 = none yet

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "PPP up, IP " IPSTR, IP2STR(&event->ip_info.ip));
        // HTTPS cert validation needs a real clock; resync every session, the modem is off in between.
        if (sntp_enabled()) sntp_stop();
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "pool.ntp.org");
        sntp_init();
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
    }
}

static void on_modem_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == ESP_MODEM_EVENT_PPP_STOP) xEventGroupSetBits(s_ev, BIT_STOPPED);
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
    if (r->battery_mv) cJSON_AddNumberToObject(o, "battery_v", r->battery_mv / 1000.0);
    if (r->link_retries != 255) cJSON_AddNumberToObject(o, "link_retries", r->link_retries);
    // ponytail: the master's own rows get the newest SIM signal, not the one from when they were sampled
    if (r->node_id == NODE_ID && s_sim_rssi_dbm) cJSON_AddNumberToObject(o, "rssi_dbm", s_sim_rssi_dbm);
    return o;
}

static cJSON *status_json(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "now_s", current_timestamp_s());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(o, "rtc_ok", s_rtc_ok);
    cJSON_AddBoolToObject(o, "sd_ok", s_sd_ok);
    cJSON_AddBoolToObject(o, "radio_ok", s_radio_ok);
    cJSON_AddNumberToObject(o, "push_fails", s_push_fails);
    return o;
}

void api_set_health(bool rtc_ok, bool sd_ok, bool radio_ok) {
    s_radio_ok = radio_ok;
    s_rtc_ok = rtc_ok;
    s_sd_ok = sd_ok;
}

void api_record_reading(const api_reading_t *r) {
    if (!s_ingest_q || r->node_id < 1 || r->node_id > MAX_NODES) return;
    if (xQueueSend(s_ingest_q, r, 0) != pdTRUE) {  // full (offline): drop the oldest, keep the newest
        api_reading_t old;
        xQueueReceive(s_ingest_q, &old, 0);
        xQueueSend(s_ingest_q, r, 0);
    }
}

static esp_err_t ingest_post(const api_reading_t *r, int n) {
    cJSON *root = cJSON_CreateObject(), *rows = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(rows, reading_to_json(&r[i]));
    cJSON_AddItemToObject(root, "status", status_json());
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = INGEST_URL, .method = HTTP_METHOD_POST, .timeout_ms = 30000,  // GPRS: slow TLS handshake
        .crt_bundle_attach = esp_crt_bundle_attach,  // verifies the server certificate against the built-in roots
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_err_t e = h ? ESP_OK : ESP_ERR_NO_MEM;
    int code = 0;
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "Authorization", "Bearer " INGEST_TOKEN);
        esp_http_client_set_post_field(h, body, strlen(body));
        e = esp_http_client_perform(h);
        code = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
    }
    free(body);
    if (e == ESP_OK && code != 200) {
        ESP_LOGW(TAG, "Ingest rejected: HTTP %d%s", code, code == 401 ? " (check INGEST_TOKEN)" : "");
        return ESP_FAIL;
    }
    return e;
}

// The SIM900A locks its auto-baud on the first few "AT"s, so send a burst before the driver's single sync.
static void modem_wake(int baud) {
    uart_set_baudrate(UART_NUM_2, baud);
    uint8_t buf[64];
    for (int i = 0; i < 5; i++) {
        uart_write_bytes(UART_NUM_2, "AT\r", 3);
        int n = uart_read_bytes(UART_NUM_2, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (n > 0) ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_INFO);
        else ESP_LOGW(TAG, "baud %d: no reply", baud);
    }
    uart_flush_input(UART_NUM_2);
}

// Log the reply lines of one AT command (used to see why a data call does not come up).
static int s_reg_stat = -1;  // last "+CGREG: <n>,<stat>" seen

static esp_err_t log_reply_line(modem_dce_t *dce, const char *line) {
    if (strstr(line, "OK")) return esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    if (strstr(line, "ERROR")) return esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    const char *p = strstr(line, "+CGREG:");
    if (p && (p = strchr(p, ','))) s_reg_stat = atoi(p + 1);
    ESP_LOGI(TAG, "modem reply: %.*s", (int)strcspn(line, "\r\n"), line);
    return ESP_OK;
}

static void modem_ask(modem_dte_t *dte, modem_dce_t *dce, const char *cmd, uint32_t timeout_ms) {
    dce->handle_line = log_reply_line;
    ESP_LOGI(TAG, "modem ask: %.*s", (int)strcspn(cmd, "\r"), cmd);
    if (dte->send_cmd(dte, cmd, timeout_ms) != ESP_OK) ESP_LOGW(TAG, "modem ask: no answer");
    dce->handle_line = esp_modem_dce_handle_response_default;
}

// One modem session: power up, dial PPP, push everything queued, hang up, power off.
// Returns true if the queue was emptied. Rows that did not go stay in batch[0..*n) and the queue.
static bool push_session(esp_netif_t *netif, modem_dte_t *dte, api_reading_t *batch, int *n) {
    MODEM_POWER(1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_MS));  // also covers boot of a module that was powered at startup
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_STOPPED);

    bool done = false;
    // The module may still be booting (or resetting on a weak supply), so keep trying for a while.
    static const int try_bauds[] = {115200, 19200, 9600};  // boards ship at 115200, 19200 or 9600
    modem_dce_t *dce = NULL;
    for (int round = 0; !dce && round < MODEM_SYNC_ROUNDS; round++) {
        for (int i = 0; !dce && i < 3; i++) {
            modem_wake(try_bauds[i]);
            dce = sim800_init(dte);
            if (dce && try_bauds[i] != 115200) {  // switch the module to 115200 and keep it there
                dce->handle_line = esp_modem_dce_handle_response_default;
                dte->send_cmd(dte, "AT+IPR=115200\r", MODEM_COMMAND_TIMEOUT_DEFAULT);
                uart_set_baudrate(UART_NUM_2, 115200);
                vTaskDelay(pdMS_TO_TICKS(500));
                dce->handle_line = esp_modem_dce_handle_response_default;
                dte->send_cmd(dte, "AT&W\r", MODEM_COMMAND_TIMEOUT_DEFAULT);
                ESP_LOGI(TAG, "Module set to 115200 baud");
            }
        }
        if (!dce && round == 0 && MODEM_USE_KEY) {  // the key toggles, so only press it once the module has failed to answer
            ESP_LOGW(TAG, "Modem did not answer AT, pressing the power key");
            gpio_set_level(MODEM_KEY_PIN, MODEM_KEY_ACTIVE);
            vTaskDelay(pdMS_TO_TICKS(MODEM_KEY_MS));
            gpio_set_level(MODEM_KEY_PIN, !MODEM_KEY_ACTIVE);
            vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_MS));
        }
        if (!dce) vTaskDelay(pdMS_TO_TICKS(3000));
    }
    uart_set_baudrate(UART_NUM_2, 115200);
    if (!dce) {
        ESP_LOGW(TAG, "Modem still silent (power, wiring, baud?)");
        goto off;
    }
    dce->set_flow_ctrl(dce, MODEM_FLOW_CONTROL_NONE);
    dce->store_profile(dce);
    uint32_t rssi = 0, ber = 0;
    if (dce->get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
        if (rssi <= 31) s_sim_rssi_dbm = -113 + 2 * (int)rssi;  // 99 = unknown
        ESP_LOGI(TAG, "Modem %s, operator '%s', rssi %u (0-31, 99 = none)", dce->name, dce->oper, (unsigned)rssi);
    }

    // Dialing before the module has registered gives no CONNECT, so wait for it (1 home, 5 roaming).
    s_reg_stat = -1;
    for (int i = 0; i < MODEM_REG_TRIES && s_reg_stat != 1 && s_reg_stat != 5; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(3000));
        modem_ask(dte, dce, "AT+CGREG?\r", 2000);
    }
    if (s_reg_stat != 1 && s_reg_stat != 5) {
        ESP_LOGW(TAG, "Not registered on the network (CGREG %d): antenna, signal, SIM or no 2G coverage?", s_reg_stat);
        dce->deinit(dce);
        goto off;
    }
    modem_ask(dte, dce, "AT+CGATT=1\r", 30000);  // attach to GPRS before dialing

    esp_modem_apn = MODEM_APN;
    void *adapter = esp_modem_netif_setup(dte);
    esp_modem_netif_set_default_handlers(adapter, netif);
    esp_netif_attach(netif, adapter);  // dials PPP

    if (xEventGroupWaitBits(s_ev, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000)) & BIT_GOT_IP) {
        int64_t give_up = esp_timer_get_time() + PUSH_GIVE_UP_S * 1000000LL;
        while (!done && esp_timer_get_time() < give_up) {
            if (time(NULL) < TIME_VALID_S) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }  // waiting for SNTP
            while (*n < INGEST_BATCH && xQueueReceive(s_ingest_q, &batch[*n], 0) == pdTRUE) (*n)++;
            if (*n == 0) done = true;
            else if (ingest_post(batch, *n) == ESP_OK) *n = 0;
            else vTaskDelay(pdMS_TO_TICKS(5000));
        }
    } else {
        ESP_LOGW(TAG, "No PPP link (SIM, APN, signal, or no 2G coverage?)");
    }

    if (esp_modem_stop_ppp(dte) == ESP_OK) {
        xEventGroupWaitBits(s_ev, BIT_STOPPED, pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    } else if (!done) {
        // The modem never left data mode, so its driver task is still feeding lines to a dce that the teardown below
        // frees (that crashed in the heap). Restart cleanly instead; the unsent rows in RAM are lost either way.
        ESP_LOGE(TAG, "Could not leave PPP, restarting");
        esp_restart();
    }
    esp_modem_netif_clear_default_handlers(adapter);
    esp_modem_netif_teardown(adapter);
    dce->deinit(dce);
off:
    if (!MODEM_ALWAYS_ON) MODEM_POWER(0);
    return done;
}

// Loop: modem off for PUSH_OFF_S, then one session. If nothing got through, count one failure
// and carry on; the rows stay queued for the next round.
static void ingest_task(void *arg) {
    api_reading_t batch[INGEST_BATCH];
    int n = 0;  // rows held between sessions

    esp_modem_dte_config_t cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    cfg.port_num = UART_NUM_2;
    cfg.tx_io_num = MODEM_TX_PIN;
    cfg.rx_io_num = MODEM_RX_PIN;
    cfg.rts_io_num = UART_PIN_NO_CHANGE;  // no flow control wired
    cfg.cts_io_num = UART_PIN_NO_CHANGE;
    cfg.rx_buffer_size = 4096;
    cfg.tx_buffer_size = 1024;
    cfg.line_buffer_size = 2048;
    cfg.event_task_stack_size = 4096;
    modem_dte_t *dte = esp_modem_dte_init(&cfg);
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    if (!dte || !netif) {
        ESP_LOGE(TAG, "Modem could not start");
        vTaskDelete(NULL);
    }
    esp_modem_set_event_handler(dte, on_modem_event, ESP_EVENT_ANY_ID, NULL);
    ESP_LOGI(TAG, "Modem stays off for %d s so the supply can charge", MODEM_START_DELAY_S);
    vTaskDelay(pdMS_TO_TICKS(MODEM_START_DELAY_S * 1000));  // push_session powers it on

    for (;;) {
        esp_pm_lock_acquire(s_awake);
        bool done = push_session(netif, dte, batch, &n);
        esp_pm_lock_release(s_awake);
        if (done) {
            ESP_LOGI(TAG, "Push done, modem off for %d s (fails so far: %u)", PUSH_OFF_S, (unsigned)s_push_fails);
        } else {
            s_push_fails++;
            ESP_LOGW(TAG, "No push, modem off for %d s (fails so far: %u)", PUSH_OFF_S, (unsigned)s_push_fails);
        }
        vTaskDelay(pdMS_TO_TICKS(PUSH_OFF_S * 1000));
    }
}

#if MODEM_AT_TEST
static void at_test_task(void *arg) {
    static const int bauds[] = {115200, 9600, 57600, 19200, 38400};
    esp_pm_lock_handle_t awake;
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "attest", &awake);
    esp_pm_lock_acquire(awake);
    uint8_t buf[128];
    uart_config_t c = { .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_APB };
    c.baud_rate = 115200;
    uart_param_config(UART_NUM_2, &c);
    uart_set_pin(UART_NUM_2, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_2, 1024, 0, 0, NULL, 0);
    for (int round = 0;; round++) {
        // alternate the pin order, in case the wires are swapped
        int tx = (round & 1) ? MODEM_RX_PIN : MODEM_TX_PIN;
        int rx = (round & 1) ? MODEM_TX_PIN : MODEM_RX_PIN;
        uart_set_pin(UART_NUM_2, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        ESP_LOGI(TAG, "AT test: TX on GPIO%d, RX on GPIO%d, RX idles %s", tx, rx,
                 gpio_get_level(rx) ? "HIGH" : "LOW");
        for (int i = 0; i < 5; i++) {
            uart_set_baudrate(UART_NUM_2, bauds[i]);
            for (int t = 0; t < 3; t++) {
                uart_flush_input(UART_NUM_2);
                uart_write_bytes(UART_NUM_2, "AT\r\n", 4);
                int n = uart_read_bytes(UART_NUM_2, buf, sizeof(buf) - 1, pdMS_TO_TICKS(700));
                if (n > 0) {
                    ESP_LOGI(TAG, "baud %d: %d bytes:", bauds[i], n);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_INFO);
                } else {
                    ESP_LOGW(TAG, "baud %d: nothing", bauds[i]);
                }
            }
        }
    }
}
#endif

esp_err_t api_start(void) {
    gpio_reset_pin(MODEM_PWR_PIN);
    gpio_set_direction(MODEM_PWR_PIN, GPIO_MODE_OUTPUT);
    MODEM_POWER(MODEM_ALWAYS_ON && !MODEM_START_DELAY_S);
    gpio_reset_pin(MODEM_KEY_PIN);
    gpio_set_direction(MODEM_KEY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MODEM_KEY_PIN, !MODEM_KEY_ACTIVE);

#if MODEM_AT_TEST
    xTaskCreate(at_test_task, "attest", 4096, NULL, 4, NULL);
    return ESP_OK;
#endif
    if (!INGEST_URL[0]) return ESP_OK;  // nothing to push to
    s_ingest_q = xQueueCreate(INGEST_QUEUE_LEN, sizeof(api_reading_t));
    s_ev = xEventGroupCreate();
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "modem", &s_awake);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));

    if (xTaskCreate(ingest_task, "ingest", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Ingest push could not start");
        vQueueDelete(s_ingest_q);
        s_ingest_q = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}
