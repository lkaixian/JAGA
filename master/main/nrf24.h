#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Core nRF24 register addresses & SPI command opcodes
#define NRF24_CMD_R_REGISTER    0x00
#define NRF24_CMD_W_REGISTER    0x20
#define NRF24_CMD_R_RX_PAYLOAD  0x61
#define NRF24_CMD_W_TX_PAYLOAD  0xA0
#define NRF24_CMD_FLUSH_TX      0xE1
#define NRF24_CMD_FLUSH_RX      0xE2
#define NRF24_CMD_NOP           0xFF

#define NRF24_REG_CONFIG        0x00
#define NRF24_REG_EN_AA         0x01
#define NRF24_REG_EN_RXADDR     0x02
#define NRF24_REG_SETUP_AW      0x03
#define NRF24_REG_SETUP_RETR    0x04
#define NRF24_REG_RF_CH         0x05
#define NRF24_REG_RF_SETUP      0x06
#define NRF24_REG_STATUS        0x07
#define NRF24_REG_RX_ADDR_P0    0x0A
#define NRF24_REG_TX_ADDR       0x10
#define NRF24_REG_RX_PW_P0      0x11
#define NRF24_REG_FIFO_STATUS   0x17

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t ce_pin;
    gpio_num_t csn_pin;
} nrf24_t;

/**
 * @brief Initialize the SPI bus and configure nRF24 registers into TX standby.
 */
esp_err_t nrf24_init(nrf24_t *dev, spi_host_device_t host,
                    gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso,
                    gpio_num_t csn, gpio_num_t ce);

/**
 * @brief Set RF transmission frequency channel (Channel 0 to 125).
 */
esp_err_t nrf24_set_channel(nrf24_t *dev, uint8_t channel);

/**
 * @brief Set 5-byte target transmission address.
 */
esp_err_t nrf24_open_tx_pipe(nrf24_t *dev, const uint8_t *address);

/**
 * @brief Transmit payload over RF (blocking until ACK received or retry limit reached).
 * @return ESP_OK on ACK received, ESP_FAIL on MAX_RT timeout.
 */
esp_err_t nrf24_send_packet(nrf24_t *dev, const void *data, uint8_t len);

/**
 * @brief Configure a receive pipe's address and fixed payload size.
 * @param pipe 0-5. Pipes 2-5 only have 1 configurable address byte - they
 *             share pipe 1's upper 4 address bytes, so open pipe 1 first.
 */
esp_err_t nrf24_open_rx_pipe(nrf24_t *dev, uint8_t pipe, const uint8_t *address, uint8_t payload_size);

/**
 * @brief Switch into RX mode and start listening (CE held high).
 */
esp_err_t nrf24_start_listening(nrf24_t *dev);

/**
 * @brief Check whether the hardware RX FIFO has a packet waiting.
 */
bool nrf24_available(nrf24_t *dev);

/**
 * @brief Pop one payload off the RX FIFO into `data` (exactly `len` bytes,
 *        matching whatever payload_size was set on the pipe it arrived on).
 */
esp_err_t nrf24_receive_packet(nrf24_t *dev, void *data, uint8_t len);

#ifdef __cplusplus
}
#endif
