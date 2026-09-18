#include "ssd1306.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define SSD1306_ADDR   0x3C
#define SSD1306_WIDTH  128
#define SSD1306_PAGES  8

static const char *TAG = "SSD1306_DRIVER";

// 5x7 font (5 column bytes per glyph, LSB = top pixel), only the characters
// the status display actually needs. Hand-written, not render-tested on
// real hardware - if a character looks wrong on first boot, say which one
// so just that glyph can be fixed.
typedef struct {
    char ch;
    uint8_t cols[5];
} font_glyph_t;

static const font_glyph_t FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
    {'/', {0x03, 0x0C, 0x30, 0x40, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x30, 0x40, 0x40, 0x3F, 0x00}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
};
#define FONT_COUNT (sizeof(FONT) / sizeof(FONT[0]))

static const uint8_t *glyph_for(char c) {
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    if (c >= 'a' && c <= 'z') c -= 32; // fold lowercase to uppercase
    for (size_t i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].ch == c) return FONT[i].cols;
    }
    return blank;
}

static esp_err_t send(i2c_port_t port, uint8_t control, const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SSD1306_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, control, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t ssd1306_init(ssd1306_t *dev, i2c_port_t i2c_port) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    dev->i2c_port = i2c_port;

    static const uint8_t init_cmds[] = {
        0xAE,       // display off
        0xD5, 0x80, // clock divide ratio / oscillator freq
        0xA8, 0x3F, // multiplex ratio = 64
        0xD3, 0x00, // display offset = 0
        0x40,       // display start line = 0
        0x8D, 0x14, // charge pump enable
        0x20, 0x02, // memory addressing mode = page
        0xA1,       // segment remap
        0xC8,       // COM output scan direction remap
        0xDA, 0x12, // COM pins hardware config
        0x81, 0xCF, // contrast
        0xD9, 0xF1, // pre-charge period
        0xDB, 0x40, // VCOMH deselect level
        0xA4,       // resume to RAM content display
        0xA6,       // normal display (not inverted)
        0xAF,       // display on
    };

    esp_err_t err = send(i2c_port, 0x00, init_cmds, sizeof(init_cmds));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No response from SSD1306 at 0x%02X", SSD1306_ADDR);
        return err;
    }

    ssd1306_clear(dev);
    ESP_LOGI(TAG, "SSD1306 OLED initialized on I2C port %d", i2c_port);
    return ESP_OK;
}

static esp_err_t set_page_col0(i2c_port_t port, uint8_t page) {
    uint8_t addr_cmds[3] = {(uint8_t)(0xB0 | page), 0x00, 0x10};
    return send(port, 0x00, addr_cmds, sizeof(addr_cmds));
}

esp_err_t ssd1306_clear(ssd1306_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;

    uint8_t zeros[SSD1306_WIDTH] = {0};
    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        set_page_col0(dev->i2c_port, page);
        send(dev->i2c_port, 0x40, zeros, sizeof(zeros));
    }
    return ESP_OK;
}

esp_err_t ssd1306_write_line(ssd1306_t *dev, uint8_t row, const char *text) {
    if (!dev || !text || row > 7) return ESP_ERR_INVALID_ARG;

    set_page_col0(dev->i2c_port, row);

    uint8_t line[SSD1306_WIDTH] = {0};
    size_t col = 0;
    for (size_t i = 0; text[i] != '\0' && col + 6 <= SSD1306_WIDTH; i++) {
        memcpy(&line[col], glyph_for(text[i]), 5);
        col += 6; // 5 glyph columns + 1 blank spacer column
    }

    return send(dev->i2c_port, 0x40, line, sizeof(line));
}
