#include "dht22.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "DHT22_DRIVER";

// Helper: wait for line state transition within a timeout window
static int wait_for_level(gpio_num_t pin, int level, int timeout_us) {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1; // Timed out
        }
    }
    return (int)(esp_timer_get_time() - start);
}

esp_err_t dht22_init(dht22_t *dev, gpio_num_t pin) {
    if (!dev) {
        ESP_LOGE(TAG, "Initialization failed: dev handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    dev->pin = pin;

    gpio_reset_pin(dev->pin);
    gpio_set_direction(dev->pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(dev->pin, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Sensor initialized on GPIO %d", dev->pin);
    return ESP_OK;
}

esp_err_t dht22_read(dht22_t *dev, float *temperature, float *humidity) {
    if (!dev || !temperature || !humidity) {
        ESP_LOGE(TAG, "Read failed: invalid NULL pointers provided");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[5] = {0};
    gpio_num_t pin = dev->pin;

    // 1. Host start signal: pull line LOW for >= 18ms, then release
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);

    // 2. Switch pin to INPUT to read sensor pulses
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    // 3. Sensor handshake: pulls LOW (~80us), then HIGH (~80us)
    if (wait_for_level(pin, 0, 100) < 0 ||
        wait_for_level(pin, 1, 100) < 0 ||
        wait_for_level(pin, 0, 100) < 0) {
        ESP_LOGW(TAG, "Handshake timeout: sensor did not respond to start signal");
        return ESP_ERR_TIMEOUT;
    }

    // 4. Capture 40 data bits
    for (int i = 0; i < 40; i++) {
        // Bit prefix is ~50us LOW
        if (wait_for_level(pin, 1, 100) < 0) {
            ESP_LOGW(TAG, "Bit %d sync timeout (waiting for HIGH)", i);
            return ESP_ERR_TIMEOUT;
        }

        // Measure HIGH duration (~28us for '0', ~70us for '1')
        int64_t start = esp_timer_get_time();
        if (wait_for_level(pin, 0, 120) < 0) {
            ESP_LOGW(TAG, "Bit %d pulse width measurement timeout", i);
            return ESP_ERR_TIMEOUT;
        }
        int64_t duration = esp_timer_get_time() - start;

        if (duration > 45) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    // 5. Verify Checksum: Byte4 == (Byte0 + Byte1 + Byte2 + Byte3) & 0xFF
    uint8_t checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
    if (data[4] != checksum) {
        ESP_LOGW(TAG, "Checksum mismatch: calculated 0x%02X != received 0x%02X", checksum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    // 6. Decode values
    *humidity = (float)((data[0] << 8) | data[1]) / 10.0f;

    int16_t temp_raw = ((data[2] & 0x7F) << 8) | data[3];
    if (data[2] & 0x80) {
        temp_raw = -temp_raw; // Negative temperature flag
    }
    *temperature = (float)temp_raw / 10.0f;

    ESP_LOGD(TAG, "Read success -> Temp: %.1f C, Hum: %.1f %%", *temperature, *humidity);
    return ESP_OK;
}
