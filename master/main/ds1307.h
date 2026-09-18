#pragma once

#include <stdint.h>
#include <time.h>
#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t i2c_port;
} ds1307_t;

/**
 * @brief Bring up the I2C bus and probe for a DS1307 on it.
 * @return ESP_OK if a DS1307 answered, or an I2C error (e.g. ESP_ERR_TIMEOUT)
 *         if nothing is on the bus - callers should treat that as "no RTC
 *         fitted yet" and fall back to another time source, not abort.
 */
esp_err_t ds1307_init(ds1307_t *dev, i2c_port_t i2c_port, gpio_num_t sda_pin, gpio_num_t scl_pin);

/** @brief Read the current calendar time. tm_year/tm_mon follow struct tm's convention (years since 1900, 0-11 months). */
esp_err_t ds1307_get_time(ds1307_t *dev, struct tm *time);

/** @brief Program the calendar time, e.g. once at first install. */
esp_err_t ds1307_set_time(ds1307_t *dev, const struct tm *time);

#ifdef __cplusplus
}
#endif
