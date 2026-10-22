#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"

#include "dht22.h"
#include "jsnsr40t.h"
#include "nrf24.h"
#include "node_config.h"

static const char *TAG = "MAIN_NODE";

// Struct layout matches exactly across over-the-air binary packets - update
// the master node's parser to match whenever this changes.
typedef struct __attribute__((packed)) {
    uint8_t node_id;             // which physical node this reading came from
    float temperature;
    float humidity;
    uint32_t distance_mm;        // mean of up to SONAR_SAMPLE_COUNT samples
    uint32_t distance_stddev_mm; // spread across those samples (0 if <2 valid)
    int32_t water_level_mm;      // INT32_MIN when every sonar sample failed
    uint32_t timestamp_s;        // seconds since this node's last power-on (the master stamps wall-clock time on receipt)
} telemetry_packet_t;

#define SONAR_SAMPLE_COUNT 10

// Sensor Pin Definitions
#define DHT_GPIO        GPIO_NUM_4
#define SONAR_TX_PIN    GPIO_NUM_14
#define SONAR_RX_PIN    GPIO_NUM_26

// nRF24 SPI Pin Definitions
#define NRF_SCK         GPIO_NUM_18
#define NRF_MISO        GPIO_NUM_19
#define NRF_MOSI        GPIO_NUM_23
#define NRF_CSN         GPIO_NUM_22
#define NRF_CE          GPIO_NUM_5

// Wake, scan, transmit, sleep off the remainder of this window
#define CYCLE_PERIOD_US (60LL * 1000000LL)

// Both slave nodes transmit to this one shared address; the master
// distinguishes them by the node_id field inside the packet, not by pipe.
static const uint8_t MASTER_ADDR[5] = {'M', 'A', 'S', 'T', '1'};

// Survives deep sleep (RTC memory), reset only on power-on/brownout - gives a
// monotonic relative clock since this node last powered up, not wall-clock time.
RTC_DATA_ATTR static int64_t uptime_us_at_wake = 0;

// Packets that got no ACK, oldest first. Each carries its own timestamp_s, so
// the master sees the original (uptime) time when they are finally delivered.
#define UNSENT_MAX 5
RTC_DATA_ATTR static telemetry_packet_t unsent[UNSENT_MAX];
RTC_DATA_ATTR static uint8_t unsent_n = 0;

// The nRF24 already retries 15 times in hardware; these are on top of that.
static bool send_with_retry(nrf24_t *radio, const telemetry_packet_t *p) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(300));
        if (nrf24_send_packet(radio, p, sizeof(*p)) == ESP_OK) return true;
    }
    return false;
}

void app_main(void) {
    int64_t cycle_start = esp_timer_get_time();
    uint32_t timestamp_s = (uint32_t)(uptime_us_at_wake / 1000000);

    dht22_t dht;
    jsnsr40t_t sonar;
    nrf24_t radio;

    ESP_LOGI(TAG, "Initializing hardware subsystems...");

    // A failed init must not abort: that reboot-loops and drains the battery.
    // Log it, skip that sensor, and send the sentinel for it instead.
    bool dht_ok = (dht22_init(&dht, DHT_GPIO) == ESP_OK);
    if (!dht_ok) ESP_LOGW(TAG, "DHT22 init failed. Sending -999 for temperature and humidity.");
    bool sonar_ok = (jsnsr40t_init(&sonar, SONAR_TX_PIN, SONAR_RX_PIN, SONAR_MAX_RANGE_MM) == ESP_OK);
    if (!sonar_ok) ESP_LOGW(TAG, "Sonar init failed. Sending the water level sentinel.");

    bool radio_ok = (nrf24_init(&radio, SPI2_HOST, NRF_SCK, NRF_MOSI, NRF_MISO, NRF_CSN, NRF_CE) == ESP_OK);
    if (radio_ok) {
        nrf24_set_channel(&radio, 76);
        radio_ok = (nrf24_open_tx_pipe(&radio, MASTER_ADDR) == ESP_OK);
    }
    if (!radio_ok) ESP_LOGW(TAG, "Radio init failed. Queuing the reading and retrying next wake.");

    telemetry_packet_t packet = {0};
    packet.node_id = NODE_ID;


    // DHT22 requires a minimum of 2 seconds since power-on before it responds
    vTaskDelay(pdMS_TO_TICKS(2000));

    // The bit-banged read is timing-sensitive and fails now and then, so retry
    // before giving up.
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

        // Sensor is mounted at a fixed height/angle; the beam measures slant
        // distance, so project it onto vertical before subtracting from the
        // mount height to get the actual water level.
        float vertical_mm = mean * cosf(SONAR_MOUNT_ANGLE_DEG * (float)M_PI / 180.0f);
        packet.water_level_mm = (int32_t)(SONAR_MOUNT_HEIGHT_MM - vertical_mm);

        if (valid_samples < SONAR_SAMPLE_COUNT) {
            ESP_LOGW(TAG, "Sonar: only %d/%d samples valid.", valid_samples, SONAR_SAMPLE_COUNT);
        }
    }
    packet.timestamp_s = timestamp_s;

    ESP_LOGI(TAG, "Node %u Data -> Temp: %.1f C | Hum: %.1f %% | Dist: %u+-%u mm | Water: %ld mm | t=%us",
             packet.node_id, packet.temperature, packet.humidity, packet.distance_mm, packet.distance_stddev_mm,
             (long)packet.water_level_mm, packet.timestamp_s);

    // All slaves share one nRF24 address, so offset each node's transmit time
    // or nodes powered on in the same second would collide every cycle.
#define MAX_STAGGER_SLOTS 8
    vTaskDelay(pdMS_TO_TICKS((NODE_ID % MAX_STAGGER_SLOTS) * 150));

    // Queued packets go first, oldest to newest, so the master sees them in
    // order. Stop at the first failure: the rest stay queued and the new
    // packet goes to the back.
    bool sent = radio_ok;
    while (sent && unsent_n > 0) {
        if (send_with_retry(&radio, &unsent[0])) {
            memmove(&unsent[0], &unsent[1], (--unsent_n) * sizeof(unsent[0]));
        } else {
            sent = false;
        }
    }
    if (sent) sent = send_with_retry(&radio, &packet);
    if (!sent) {
        if (unsent_n == UNSENT_MAX) { // full: drop the oldest
            memmove(&unsent[0], &unsent[1], (UNSENT_MAX - 1) * sizeof(unsent[0]));
            unsent_n--;
        }
        unsent[unsent_n++] = packet;
    }
    ESP_LOGI(TAG, "Radio -> %s (%u queued)",
             sent ? "Packet acknowledged by receiver." : "TX failed. Reading queued for the next wake.",
             unsent_n);



    // Deep sleep resets the chip on wake, so app_main() reruns this whole scan
    // from the top next time - there is no loop to return to.
    int64_t elapsed_us = esp_timer_get_time() - cycle_start;
    int64_t sleep_us = CYCLE_PERIOD_US - elapsed_us;
    if (sleep_us < 0) sleep_us = 0;

    ESP_LOGI(TAG, "Scan took %lld ms, deep sleeping for %lld ms.",
             (long long)(elapsed_us / 1000), (long long)(sleep_us / 1000));

    uptime_us_at_wake += elapsed_us + sleep_us;

    esp_sleep_enable_timer_wakeup((uint64_t)sleep_us);
    esp_deep_sleep_start();
}
