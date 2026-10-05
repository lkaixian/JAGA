#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach and mount a FAT-formatted microSD card over SPI at /sdcard.
 * @param host SPI host that has ALREADY been initialized via spi_bus_initialize()
 *             (e.g. by nrf24_init on the same bus) - this only attaches the
 *             card as a second device on that bus, it does not init the bus.
 * @param cs_pin Card's dedicated chip-select pin.
 */
esp_err_t sdcard_init(spi_host_device_t host, gpio_num_t cs_pin);

/** @brief Unmount the card (e.g. after a write failure) so sdcard_init can retry. */
void sdcard_deinit(void);

/**
 * @brief Append one line to a file under /sdcard, adding a trailing newline.
 *        Creates the file (and writes `header` first) if it doesn't exist yet.
 * @param header Written once as the first line of a new file; pass NULL to skip.
 */
esp_err_t sdcard_append_line(const char *path, const char *header, const char *line);

#ifdef __cplusplus
}
#endif
