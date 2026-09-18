#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sensor's spec sheet tops out around 4.5-6m; used as jsnsr40t_init's default
// when a node doesn't need a tighter, mount-specific clamp.
#define JSNSR40T_DEFAULT_MAX_RANGE_MM 6000

typedef struct {
    gpio_num_t trig_pin;
    gpio_num_t echo_pin;
    uint32_t max_range_mm;
} jsnsr40t_t;

/**
 * @brief Configure the trigger (output) and echo (input) GPIOs.
 * @param dev Pointer to driver struct.
 * @param trig_pin ESP32 output pin, wired directly to the sensor's TRIG/RX pin.
 * @param echo_pin ESP32 input pin, wired to the sensor's ECHO/TX pin through a
 *                 5V->3.3V voltage divider (the sensor's output is 5V logic).
 * @param max_range_mm Readings beyond this are rejected as noise/stray
 *                      reflections. Pass JSNSR40T_DEFAULT_MAX_RANGE_MM unless
 *                      this node's mount height/tank is known to be shorter.
 * @return ESP_OK on success, or ESP-IDF GPIO error code.
 */
esp_err_t jsnsr40t_init(jsnsr40t_t *dev, gpio_num_t trig_pin, gpio_num_t echo_pin, uint32_t max_range_mm);

/**
 * @brief Fire a trigger pulse and measure the echo pulse width.
 * @param dev Pointer to driver struct.
 * @param distance_mm Output pointer for distance in millimeters.
 * @param timeout_ms Max time to wait for the rising edge and for the echo
 *                    pulse to end (e.g. 300 ms; the sensor's own no-target
 *                    ceiling is 60 ms).
 * @return ESP_OK, or ESP_ERR_TIMEOUT.
 */
esp_err_t jsnsr40t_read(jsnsr40t_t *dev, uint32_t *distance_mm, uint32_t timeout_ms);

/**
 * @brief No-op, kept for API symmetry with other sensor drivers.
 */
esp_err_t jsnsr40t_deinit(jsnsr40t_t *dev);

#ifdef __cplusplus
}
#endif
