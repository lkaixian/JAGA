#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Driver object representation
typedef struct {
    gpio_num_t pin;
} dht22_t;

/**
 * @brief Initialize the DHT22 driver instance.
 * @param dev Pointer to dht22_t struct.
 * @param pin GPIO connected to the DHT22 data line.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid pointer.
 */
esp_err_t dht22_init(dht22_t *dev, gpio_num_t pin);

/**
 * @brief Trigger reading and decode temperature and humidity.
 * @param dev Pointer to initialized dht22_t struct.
 * @param temperature Pointer to store temperature in Celsius.
 * @param humidity Pointer to store relative humidity in %.
 * @return ESP_OK, ESP_ERR_TIMEOUT, or ESP_ERR_INVALID_CRC.
 */
esp_err_t dht22_read(dht22_t *dev, float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif