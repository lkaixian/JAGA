#include "jsnsr40t.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static const char *TAG = "JSN_SR40T_DRIVER";

esp_err_t jsnsr40t_init(jsnsr40t_t *dev, gpio_num_t trig_pin, gpio_num_t echo_pin, uint32_t max_range_mm) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->trig_pin = trig_pin;
    dev->echo_pin = echo_pin;
    dev->max_range_mm = max_range_mm;

    gpio_reset_pin(trig_pin);
    gpio_set_direction(trig_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(trig_pin, 0);

    gpio_reset_pin(echo_pin);
    gpio_set_direction(echo_pin, GPIO_MODE_INPUT);
    gpio_pullup_dis(echo_pin);
    gpio_pulldown_en(echo_pin);

    ESP_LOGI(TAG, "Initialized in Trigger/Echo mode (Trig: GPIO %d, Echo: GPIO %d)",
             trig_pin, echo_pin);

    return ESP_OK;
}

esp_err_t jsnsr40t_read(jsnsr40t_t *dev, uint32_t *distance_mm, uint32_t timeout_ms) {
    if (!dev || !distance_mm) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t timeout_us = (int64_t)timeout_ms * 1000;

    gpio_set_level(dev->trig_pin, 1);
    esp_rom_delay_us(20);
    gpio_set_level(dev->trig_pin, 0);

    // 1. Wait for the sensor to start its echo pulse
    int64_t wait_start = esp_timer_get_time();
    while (gpio_get_level(dev->echo_pin) == 0) {
        if (esp_timer_get_time() - wait_start > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }

    // 2. Time how long it stays high
    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(dev->echo_pin) == 1) {
        if (esp_timer_get_time() - echo_start > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }

    // 3. Distance = (pulse width * speed of sound) / 2 (round trip)
    int64_t echo_us = esp_timer_get_time() - echo_start;
    uint32_t distance = (uint32_t)(echo_us * 340 / 2 / 1000);

    // Anything beyond this node's configured ceiling is a stray reflection
    // or noise, not a valid distance.
    if (distance > dev->max_range_mm) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *distance_mm = distance;
    return ESP_OK;
}

esp_err_t jsnsr40t_deinit(jsnsr40t_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
