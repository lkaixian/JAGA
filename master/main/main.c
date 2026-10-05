#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#include "dht22.h"
#include "jsnsr40t.h"
#include "nrf24.h"
#include "ds1307.h"
#include "sdcard.h"
#include "ssd1306.h"
#include "api.h"
#include "node_config.h"

static const char *TAG = "MASTER_NODE";

// Struct layout matches exactly across over-the-air binary packets - must
// stay identical to the slave nodes' telemetry_packet_t or received packets
// decode as garbage.
typedef struct __attribute__((packed)) {
    uint8_t node_id;
    float temperature;
    float humidity;
    uint32_t distance_mm;
    uint32_t distance_stddev_mm;
    int32_t water_level_mm;
    uint32_t timestamp_s;
} telemetry_packet_t;

#define SONAR_SAMPLE_COUNT 10
// 8.3 name: FATFS_LFN_NONE (the ESP-IDF default) rejects longer filenames.
#define TELEMETRY_CSV_PATH "/sdcard/telem.csv"
#define TELEMETRY_CSV_HEADER \
    "log_timestamp_s,node_id,sender_timestamp_s,temperature_c,humidity_pct,distance_mm,distance_stddev_mm,water_level_mm"

// Sensor Pin Definitions (same physical layout as the slave nodes)
#define DHT_GPIO        GPIO_NUM_4
#define SONAR_TX_PIN    GPIO_NUM_14
#define SONAR_RX_PIN    GPIO_NUM_26

// nRF24 SPI Pin Definitions
#define NRF_SCK         GPIO_NUM_18
#define NRF_MISO        GPIO_NUM_19
#define NRF_MOSI        GPIO_NUM_23
#define NRF_CSN         GPIO_NUM_22
#define NRF_CE          GPIO_NUM_5

// microSD card shares the nRF24's SPI bus, own CS line
#define SD_CS_PIN       GPIO_NUM_13

// DS1307 RTC I2C Pin Definitions
#define RTC_I2C_PORT    I2C_NUM_0
#define RTC_SDA_PIN     GPIO_NUM_21
#define RTC_SCL_PIN     GPIO_NUM_27

// Status LEDs
#define LED_BLUE        GPIO_NUM_32 // lit while this cycle's own scan is in progress
#define LED_RED         GPIO_NUM_33 // lit when this cycle's own sensor read failed
#define LED_GREEN       GPIO_NUM_25 // lit when this cycle's own data was logged successfully

// Own periodic self-measurement interval. The master never deep-sleeps (it
// must stay awake to receive from the slave nodes), so this is a plain
// vTaskDelay between iterations of an infinite loop, not deep sleep.
#define CYCLE_PERIOD_US (60LL * 1000000LL)

// Both slave nodes transmit to this one shared address; distinguished by
// the node_id field inside the packet, not by which pipe received it.
static const uint8_t MASTER_ADDR[5] = {'M', 'A', 'S', 'T', '1'};

static ds1307_t rtc;
static bool rtc_available = false;
static ssd1306_t oled;
static bool oled_available = false;
static bool sd_available = false;
static bool radio_ok = false;

// Latest known reading per node_id (1-3), kept across cycles so the OLED
// still shows a slave's last report even on a cycle it didn't transmit.
static telemetry_packet_t last_reading[4];
static bool has_reading[4] = {false, false, false, false};

uint32_t current_timestamp_s(void) { // also used by api.c
    if (rtc_available) {
        struct tm t;
        if (ds1307_get_time(&rtc, &t) == ESP_OK) {
            return (uint32_t)mktime(&t);
        }
    }
    // No RTC fitted (or it dropped out): fall back to seconds since this
    // node's own boot - the master never deep-sleeps, so this stays
    // monotonic for as long as it's been running.
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void log_telemetry(uint32_t log_timestamp_s, const telemetry_packet_t *p) {
    char line[160];
    snprintf(line, sizeof(line), "%u,%u,%u,%.2f,%.2f,%u,%u,%ld",
             log_timestamp_s, p->node_id, p->timestamp_s,
             p->temperature, p->humidity, p->distance_mm, p->distance_stddev_mm,
             (long)p->water_level_mm);
    if (sdcard_append_line(TELEMETRY_CSV_PATH, TELEMETRY_CSV_HEADER, line) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to log node %u's reading to SD card.", p->node_id);
    }

    if (p->node_id >= 1 && p->node_id <= 3) {
        last_reading[p->node_id] = *p;
        has_reading[p->node_id] = true;
    }

    api_record_reading(&(api_reading_t){
        .log_timestamp_s = log_timestamp_s,
        .node_id = p->node_id,
        .sender_timestamp_s = p->timestamp_s,
        .temperature_c = p->temperature,
        .humidity_pct = p->humidity,
        .distance_mm = p->distance_mm,
        .distance_stddev_mm = p->distance_stddev_mm,
        .water_level_mm = p->water_level_mm,
    });
}

// All values below are bounded in practice (node id 1-3, tenths-precision
// temp/humidity, tank-scale water level) and the 21-char OLED line already
// truncates anything longer - GCC's static worst-case estimate for %f/%d
// doesn't know that, so silence the false positive rather than pad buffers.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void update_oled(int received_this_cycle) {
    if (!oled_available) return;

    char line[24];
    ssd1306_write_line(&oled, 0, "JAGA MASTER NODE");

    for (int n = 1; n <= 3; n++) {
        if (has_reading[n]) {
            const telemetry_packet_t *p = &last_reading[n];
            snprintf(line, sizeof(line), "N%d T%.1fC H%.0f%% W%ldMM",
                     n, p->temperature, p->humidity, (long)p->water_level_mm);
        } else {
            snprintf(line, sizeof(line), "N%d NO DATA YET", n);
        }
        ssd1306_write_line(&oled, n, line);
    }

    snprintf(line, sizeof(line), "SD%s RTC%s",
             sd_available ? "OK" : "FAIL", rtc_available ? "OK" : "NO");
    ssd1306_write_line(&oled, 5, line);

    if (radio_ok) snprintf(line, sizeof(line), "RX THIS CYCLE %d", received_this_cycle);
    else snprintf(line, sizeof(line), "RADIO FAIL");
    ssd1306_write_line(&oled, 6, line);

    snprintf(line, sizeof(line), "UPTIME %luS", (unsigned long)(esp_timer_get_time() / 1000000));
    ssd1306_write_line(&oled, 7, line);
}
#pragma GCC diagnostic pop

// Pull every payload out of the nRF24's hardware FIFO, log it, return the count.
static int drain_radio(nrf24_t *radio) {
    int received = 0;
    while (nrf24_available(radio)) {
        telemetry_packet_t rx_packet;
        nrf24_receive_packet(radio, &rx_packet, sizeof(rx_packet));
        uint32_t received_at = current_timestamp_s();

        ESP_LOGI(TAG, "Node %u (received) -> Temp: %.1f C | Hum: %.1f %% | Dist: %u+-%u mm | Water: %ld mm | sent t=%us",
                 rx_packet.node_id, rx_packet.temperature, rx_packet.humidity, rx_packet.distance_mm,
                 rx_packet.distance_stddev_mm, (long)rx_packet.water_level_mm, rx_packet.timestamp_s);

        log_telemetry(received_at, &rx_packet);
        received++;
    }
    if (received > 0) {
        ESP_LOGI(TAG, "Logged %d packet(s) from slave nodes.", received);
    }
    return received;
}

static const char *reset_reason_str(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON (fresh power-up, or a brownout severe enough to look like one)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (supply sagged below the detector threshold)";
        case ESP_RST_PANIC:     return "PANIC (firmware crash - exception/assert)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (a task stalled and the watchdog fired)";
        case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
        case ESP_RST_SW:        return "SW (esp_restart() was called)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (woke from deep sleep)";
        default:                return "OTHER";
    }
}

void app_main(void) {
    dht22_t dht;
    jsnsr40t_t sonar;
    nrf24_t radio;

    ESP_LOGI(TAG, "Initializing hardware subsystems...");
    ESP_LOGI(TAG, "Boot reason: %s", reset_reason_str(esp_reset_reason()));

    gpio_reset_pin(LED_BLUE);
    gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);
    gpio_reset_pin(LED_RED);
    gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_RED, 0);
    gpio_reset_pin(LED_GREEN);
    gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GREEN, 0);

    // A failed init must not abort and reboot-loop: log it, skip that sensor
    // and report its sentinel instead.
    bool dht_ok = (dht22_init(&dht, DHT_GPIO) == ESP_OK);
    if (!dht_ok) ESP_LOGW(TAG, "DHT22 init failed. Reporting -999 for temperature and humidity.");
    bool sonar_ok = (jsnsr40t_init(&sonar, SONAR_TX_PIN, SONAR_RX_PIN, SONAR_MAX_RANGE_MM) == ESP_OK);
    if (!sonar_ok) ESP_LOGW(TAG, "Sonar init failed. Reporting the water level sentinel.");

    // Not fitted yet? ds1307_init fails cleanly (NACK/timeout) rather than
    // aborting, and current_timestamp_s() just keeps the relative fallback.
    // The I2C bus stays up either way, so the OLED below still works.
    rtc_available = (ds1307_init(&rtc, RTC_I2C_PORT, RTC_SDA_PIN, RTC_SCL_PIN) == ESP_OK);
    if (rtc_available) ds1307_set_if_unset(&rtc);
#if OLED_ENABLED
    oled_available = (ssd1306_init(&oled, RTC_I2C_PORT) == ESP_OK);
#endif

    // Park the SD card's CS high before touching the shared SPI bus. Until
    // sdcard_init() runs the pin floats, and a selected card drives MISO and
    // corrupts the nRF24's reads (CONFIG reads back 0xFF).
    gpio_reset_pin(SD_CS_PIN);
    gpio_set_direction(SD_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_CS_PIN, 1);

    radio_ok = (nrf24_init(&radio, SPI2_HOST, NRF_SCK, NRF_MOSI, NRF_MISO, NRF_CSN, NRF_CE) == ESP_OK);
    if (radio_ok) {
        nrf24_set_channel(&radio, 76); // must match the slave (main/main.c)
        radio_ok = (nrf24_open_rx_pipe(&radio, 0, MASTER_ADDR, sizeof(telemetry_packet_t)) == ESP_OK);
    }
    if (radio_ok) {
        nrf24_start_listening(&radio); // CE stays high from here - keeps receiving even while we're busy below
    } else {
        ESP_LOGW(TAG, "Radio init failed. Slave nodes cannot be received; own readings still logged.");
    }

    // A missing/bad SD card shouldn't crash-loop the whole node - log a
    // warning and keep running with SD logging disabled (status still shows
    // on the OLED and in the serial log either way).
    sd_available = (sdcard_init(SPI2_HOST, SD_CS_PIN) == ESP_OK);

    // A missing/misconfigured Wi-Fi network shouldn't block booting either -
    // same "degrade, don't crash-loop" treatment as the SD card and RTC above.
    api_set_health(rtc_available, sd_available, radio_ok);
    if (api_start() != ESP_OK) {
        ESP_LOGW(TAG, "API failed to start - continuing with SD-only logging.");
    }

    // A hung main loop now reboots the master after 30 s (panic = true).
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Master node ready (node_id=%u), listening + logging forever.", NODE_ID);

    while (1) {
        int64_t cycle_start = esp_timer_get_time();
        esp_task_wdt_reset();
        api_set_health(rtc_available, sd_available, radio_ok);

        telemetry_packet_t packet = {0};
        packet.node_id = NODE_ID;
        bool had_error = false;

        gpio_set_level(LED_BLUE, 1);

        // DHT22 requires a minimum of 2 seconds between reads
        vTaskDelay(pdMS_TO_TICKS(2000));

        // The bit-banged read is timing-sensitive and fails now and then
        // (e.g. "pulse width measurement timeout"), so retry before giving up.
        esp_err_t dht_err = ESP_FAIL;
        for (int attempt = 0; attempt < 3 && dht_ok && dht_err != ESP_OK; attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(2000)); // DHT22 needs >= 2s between reads
            dht_err = dht22_read(&dht, &packet.temperature, &packet.humidity);
        }
        if (dht_err != ESP_OK) {
            ESP_LOGW(TAG, "DHT22 read failed.");
            packet.temperature = -999.0f;
            packet.humidity = -999.0f;
            had_error = true;
        }

        uint32_t sonar_samples[SONAR_SAMPLE_COUNT];
        int valid_samples = 0;
        for (int i = 0; sonar_ok && i < SONAR_SAMPLE_COUNT; i++) {
            gpio_set_level(LED_BLUE, 1); // pulse once per sampling attempt
            uint32_t d;
            bool sample_ok = (jsnsr40t_read(&sonar, &d, 300) == ESP_OK);
            if (sample_ok) {
                sonar_samples[valid_samples++] = d;
                gpio_set_level(LED_GREEN, 1); // pulse once per successful sample
            }
            vTaskDelay(pdMS_TO_TICKS(60)); // let any residual echo settle before retriggering
            gpio_set_level(LED_BLUE, 0);
            gpio_set_level(LED_GREEN, 0);
        }

        if (valid_samples == 0) {
            ESP_LOGW(TAG, "Sonar read failed (0/%d samples valid).", SONAR_SAMPLE_COUNT);
            packet.distance_mm = 0;
            packet.distance_stddev_mm = 0;
            packet.water_level_mm = INT32_MIN;
            had_error = true;
        } else {
            uint32_t sum = 0;
            for (int i = 0; i < valid_samples; i++) sum += sonar_samples[i];
            uint32_t mean = sum / valid_samples;

            float variance = 0;
            for (int i = 0; i < valid_samples; i++) {
                float diff = (float)sonar_samples[i] - (float)mean;
                variance += diff * diff;
            }
            variance /= valid_samples;

            packet.distance_mm = mean;
            packet.distance_stddev_mm = (uint32_t)sqrtf(variance);

            float vertical_mm = mean * cosf(SONAR_MOUNT_ANGLE_DEG * (float)M_PI / 180.0f);
            packet.water_level_mm = (int32_t)(SONAR_MOUNT_HEIGHT_MM - vertical_mm);

            if (valid_samples < SONAR_SAMPLE_COUNT) {
                ESP_LOGW(TAG, "Sonar: only %d/%d samples valid.", valid_samples, SONAR_SAMPLE_COUNT);
            }
        }

        packet.timestamp_s = current_timestamp_s();

        ESP_LOGI(TAG, "Node %u (self) -> Temp: %.1f C | Hum: %.1f %% | Dist: %u+-%u mm | Water: %ld mm | t=%us",
                 packet.node_id, packet.temperature, packet.humidity, packet.distance_mm,
                 packet.distance_stddev_mm, (long)packet.water_level_mm, packet.timestamp_s);

        log_telemetry(packet.timestamp_s, &packet);

        gpio_set_level(LED_BLUE, 0);
        gpio_set_level(LED_RED, had_error ? 1 : 0);
        gpio_set_level(LED_GREEN, had_error ? 0 : 1);

        // Drain anything the slave nodes sent while we were busy above - the
        // nRF24 buffers up to 3 payloads in hardware on its own as long as
        // CE stays high, no CPU involvement needed until we poll it here.
        int received = radio_ok ? drain_radio(&radio) : 0;

        update_oled(received);

        // Hold the status LEDs for 5s, then turn them off.
        // The OLED stays on the whole time for now (no blanking).
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_task_wdt_reset();
        gpio_set_level(LED_RED, 0);
        gpio_set_level(LED_GREEN, 0);

        // Stay awake (no light sleep) so the HTTP API keeps answering; Wi-Fi's
        // default min-modem-sleep handles power. Drain the nRF24 FIFO every 1s
        // so a slave packet shows up within ~1s of arriving (no IRQ pin
        // wired, so this polls).
        while (esp_timer_get_time() - cycle_start < CYCLE_PERIOD_US) {
            vTaskDelay(pdMS_TO_TICKS(1000));

            esp_task_wdt_reset();
            if (radio_ok) received += drain_radio(&radio);
            update_oled(received); // also keeps the UPTIME line ticking
        }
    }
}
