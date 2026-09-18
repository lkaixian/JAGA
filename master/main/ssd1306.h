#pragma once

#include <stdint.h>
#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t i2c_port;
} ssd1306_t;

/**
 * @brief Configure a 128x64 SSD1306 OLED at I2C address 0x3C.
 * @param i2c_port A port ALREADY brought up (e.g. by ds1307_init on the same
 *                  bus) - this does not install the I2C driver itself.
 */
esp_err_t ssd1306_init(ssd1306_t *dev, i2c_port_t i2c_port);

esp_err_t ssd1306_clear(ssd1306_t *dev);

/**
 * @brief Render one line of text. Supported characters: A-Z (case folded),
 *        0-9, space, ':' '.' '-' '%' '/' - anything else renders blank.
 * @param row 0-7 (each row is 8px tall). Up to 21 chars fit at 128px wide.
 */
esp_err_t ssd1306_write_line(ssd1306_t *dev, uint8_t row, const char *text);

#ifdef __cplusplus
}
#endif
