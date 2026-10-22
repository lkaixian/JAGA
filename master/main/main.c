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
#include "esp_pm.h"

#include "dht22.h"
#include "jsnsr40t.h"
#include "nrf24.h"
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
// Sensor Pin Definitions (same physical layout as the slave nodes)
#define DHT_GPIO        GPIO_NUM_4
#define SONAR_TX_PIN    GPIO_NUM_14
#define SONAR_RX_PIN    GPIO_NUM_26

// nRF24 SPI Pin Definitions
#define NRF_SCK         GPIO_NUM_19
#define NRF_MISO        GPIO_NUM_21
#define NRF_MOSI        GPIO_NUM_23
#define NRF_CSN         GPIO_NUM_22
#define NRF_CE          GPIO_NUM_18

// Own periodic self-measurement interval. The master never deep-sleeps (it
// must stay awake to receive from the slave nodes), so this is a plain
// vTaskDelay between iterations of an infinite loop; the chip light-sleeps
// in between (see the esp_pm_configure call in app_main). The SIM900A's
// power and the push every ~10 min live in api.c.
#define CYCLE_PERIOD_US (30LL * 1000000LL)

// Both slave nodes transmit to this one shared address; distinguished by
// the node_id field inside the packet, not by which pipe received it.
static const uint8_t MASTER_ADDR[5] = {'M', 'A', 'S', 'T', '1'};

static bool radio_ok = false;

// Latest known reading per node_id (1-3), kept across cycles, so a slave that skipped a cycle keeps its last report
static telemetry_packet_t last_reading[MAX_NODES + 1];
static bool has_reading[MAX_NODES + 1];
static uint8_t last_heard_id;

uint32_t current_timestamp_s(void) { // also used by api.c
    time_t now = time(NULL);
    if (now > 1600000000) return (uint32_t)now; // SNTP has set the clock (see api.c)
    return (uint32_t)(esp_timer_get_time() / 1000000); // not synced yet: seconds since boot
}

static void log_telemetry(uint32_t log_timestamp_s, const telemetry_packet_t *p) {
    if (p->node_id >= 1 && p->node_id <= MAX_NODES) {
        last_heard_id = p->node_id;
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

    // Light sleep whenever every task is blocked. Clock is fixed at 160 MHz so the
    // bit-banged DHT22 and UART timings don't move. Needs CONFIG_PM_ENABLE and
    // CONFIG_FREERTOS_USE_TICKLESS_IDLE (sdkconfig.defaults).
    esp_pm_config_esp32_t pm_cfg = { .max_freq_mhz = 160, .min_freq_mhz = 160, .light_sleep_enable = true };
    if (esp_pm_configure(&pm_cfg) != ESP_OK) ESP_LOGW(TAG, "Light sleep not enabled (PM_ENABLE off in sdkconfig?).");
    // The sonar UART cannot receive while asleep, so hold light sleep off while sampling.
    esp_pm_lock_handle_t sample_lock = NULL;
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sample", &sample_lock);

    // A failed init must not abort and reboot-loop: log it, skip that sensor
    // and report its sentinel instead.
    bool dht_ok = (dht22_init(&dht, DHT_GPIO) == ESP_OK);
    if (!dht_ok) ESP_LOGW(TAG, "DHT22 init failed. Reporting -999 for temperature and humidity.");
    bool sonar_ok = (jsnsr40t_init(&sonar, SONAR_TX_PIN, SONAR_RX_PIN, SONAR_MAX_RANGE_MM) == ESP_OK);
    if (!sonar_ok) ESP_LOGW(TAG, "Sonar init failed. Reporting the water level sentinel.");

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

    // A missing modem shouldn't block booting either -
    // same "degrade, don't crash-loop" treatment as the SD card and RTC above.
    api_set_health(current_timestamp_s() > 1600000000, false, radio_ok);
    if (api_start() != ESP_OK) {
        ESP_LOGW(TAG, "API failed to start - continuing without push.");
    }

    // A hung main loop now reboots the master after 30 s (panic = true).
    if (esp_task_wdt_init(30, true) != ESP_OK) ESP_LOGW(TAG, "Task watchdog setup failed. A hung loop will not reboot the node.");
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Master node ready (node_id=%u), listening + logging forever.", NODE_ID);

    for (;;) {
        int64_t cycle_start = esp_timer_get_time();
        esp_task_wdt_reset();
        api_set_health(current_timestamp_s() > 1600000000, false, radio_ok);

        telemetry_packet_t packet = {0};
        packet.node_id = NODE_ID;
        if (sample_lock) esp_pm_lock_acquire(sample_lock);

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
        }

        uint32_t sonar_samples[SONAR_SAMPLE_COUNT];
        int valid_samples = 0;
        for (int i = 0; sonar_ok && i < SONAR_SAMPLE_COUNT; i++) {
            uint32_t d;
            bool sample_ok = (jsnsr40t_read(&sonar, &d, 300) == ESP_OK);
            if (sample_ok) {
                sonar_samples[valid_samples++] = d;
            }
            vTaskDelay(pdMS_TO_TICKS(60)); // let any residual echo settle before retriggering
        }

        if (valid_samples == 0) {
            ESP_LOGW(TAG, "Sonar read failed (0/%d samples valid).", SONAR_SAMPLE_COUNT);
            packet.distance_mm = 0;
            packet.distance_stddev_mm = 0;
            packet.water_level_mm = INT32_MIN;
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

        if (sample_lock) esp_pm_lock_release(sample_lock);
        packet.timestamp_s = current_timestamp_s();

        ESP_LOGI(TAG, "Node %u (self) -> Temp: %.1f C | Hum: %.1f %% | Dist: %u+-%u mm | Water: %ld mm | t=%us",
                 packet.node_id, packet.temperature, packet.humidity, packet.distance_mm,
                 packet.distance_stddev_mm, (long)packet.water_level_mm, packet.timestamp_s);

        log_telemetry(packet.timestamp_s, &packet);


        // Drain anything the slave nodes sent while we were busy above - the
        // nRF24 buffers up to 3 payloads in hardware on its own as long as
        // CE stays high, no CPU involvement needed until we poll it here.
        int received = radio_ok ? drain_radio(&radio) : 0;


        
        // Drain the nRF24 FIFO every 1s
        // so a slave packet shows up within ~1s of arriving (no IRQ pin
        // wired, so this polls).
        while (esp_timer_get_time() - cycle_start < CYCLE_PERIOD_US) {
            vTaskDelay(pdMS_TO_TICKS(1000));

            esp_task_wdt_reset();
            if (radio_ok) received += drain_radio(&radio);
                    }
    }
}
