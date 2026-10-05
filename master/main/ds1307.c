#include "ds1307.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define DS1307_ADDR 0x68

static const char *TAG = "DS1307_DRIVER";

static uint8_t bcd_to_dec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec) { return (uint8_t)(((dec / 10) << 4) | (dec % 10)); }

// IDF v4.3.6's driver/i2c.h has no i2c_master_write_to_device()/
// i2c_master_write_read_device() convenience wrappers yet - build the same
// transactions by hand with the command-link API.
static esp_err_t i2c_write(i2c_port_t port, const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_write_then_read(i2c_port_t port, uint8_t reg, uint8_t *out, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // repeated start
    i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, out, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t ds1307_init(ds1307_t *dev, i2c_port_t i2c_port, gpio_num_t sda_pin, gpio_num_t scl_pin) {
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->i2c_port = i2c_port;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    esp_err_t err = i2c_param_config(i2c_port, &conf);
    if (err != ESP_OK) return err;

    err = i2c_driver_install(i2c_port, conf.mode, 0, 0, 0);
    if (err != ESP_OK) return err;

    // Probe the seconds register; a DS1307 that isn't actually on the bus
    // NACKs/times out here instead of silently returning garbage later.
    // The I2C driver stays installed either way - other devices on this
    // bus (e.g. an OLED) may still need it even if the RTC isn't fitted.
    uint8_t dummy;
    err = i2c_write_then_read(i2c_port, 0x00, &dummy, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No response from DS1307 on SDA=%d SCL=%d", sda_pin, scl_pin);
        return err;
    }

    ESP_LOGI(TAG, "DS1307 detected on I2C port %d (SDA: GPIO %d, SCL: GPIO %d)", i2c_port, sda_pin, scl_pin);
    return ESP_OK;
}

esp_err_t ds1307_get_time(ds1307_t *dev, struct tm *time) {
    if (!dev || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[7];
    esp_err_t err = i2c_write_then_read(dev->i2c_port, 0x00, raw, sizeof(raw));
    if (err != ESP_OK) return err;

    time->tm_sec  = bcd_to_dec(raw[0] & 0x7F); // bit 7 is the clock-halt flag, not part of the value
    time->tm_min  = bcd_to_dec(raw[1]);
    time->tm_hour = bcd_to_dec(raw[2] & 0x3F); // force 24h interpretation regardless of the 12/24h mode bit
    time->tm_wday = bcd_to_dec(raw[3]) - 1;
    time->tm_mday = bcd_to_dec(raw[4]);
    time->tm_mon  = bcd_to_dec(raw[5]) - 1;
    time->tm_year = bcd_to_dec(raw[6]) + 100; // DS1307 only stores 2 digits; assumes 20xx

    return ESP_OK;
}

esp_err_t ds1307_set_time(ds1307_t *dev, const struct tm *time) {
    if (!dev || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[8] = {
        0x00, // start register (seconds)
        dec_to_bcd(time->tm_sec),
        dec_to_bcd(time->tm_min),
        dec_to_bcd(time->tm_hour),
        dec_to_bcd(time->tm_wday + 1),
        dec_to_bcd(time->tm_mday),
        dec_to_bcd(time->tm_mon + 1),
        dec_to_bcd(time->tm_year - 100),
    };

    return i2c_write(dev->i2c_port, data, sizeof(data));
}

// A fresh/battery-less DS1307 sits halted at ~year 2000. If the stored year
// looks unset, seed it from this file's build timestamp (__DATE__/__TIME__).
// ponytail: build time, not real time - off by the build-to-flash gap and only
// refreshed when this file is recompiled; add a proper sync source if it matters.
esp_err_t ds1307_set_if_unset(ds1307_t *dev) {
    struct tm t;
    if (ds1307_get_time(dev, &t) == ESP_OK && t.tm_year + 1900 >= 2024) return ESP_OK;

    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4];
    int d, y, h, m, s;
    if (sscanf(__DATE__, "%3s %d %d", mon, &d, &y) != 3 || sscanf(__TIME__, "%d:%d:%d", &h, &m, &s) != 3) {
        return ESP_FAIL;
    }
    t = (struct tm){ .tm_sec = s, .tm_min = m, .tm_hour = h, .tm_mday = d,
                     .tm_mon = (int)(strstr(months, mon) - months) / 3, .tm_year = y - 1900 };
    mktime(&t); // fills in tm_wday
    ESP_LOGW(TAG, "RTC unset - seeding from build time %s %s", __DATE__, __TIME__);
    return ds1307_set_time(dev, &t);
}
