#include "nrf24.h"
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NRF24_DRIVER";

static inline void csn_low(nrf24_t *dev)  { gpio_set_level(dev->csn_pin, 0); }
static inline void csn_high(nrf24_t *dev) { gpio_set_level(dev->csn_pin, 1); }
static inline void ce_low(nrf24_t *dev)   { gpio_set_level(dev->ce_pin, 0); }
static inline void ce_high(nrf24_t *dev)  { gpio_set_level(dev->ce_pin, 1); }

static uint8_t spi_transfer_byte(nrf24_t *dev, uint8_t data) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
        .rx_buffer = &data,
    };
    spi_device_polling_transmit(dev->spi, &t);
    return data;
}

static esp_err_t write_register(nrf24_t *dev, uint8_t reg, uint8_t val) {
    csn_low(dev);
    spi_transfer_byte(dev, NRF24_CMD_W_REGISTER | (reg & 0x1F));
    spi_transfer_byte(dev, val);
    csn_high(dev);
    return ESP_OK;
}

static uint8_t read_register(nrf24_t *dev, uint8_t reg) {
    csn_low(dev);
    spi_transfer_byte(dev, NRF24_CMD_R_REGISTER | (reg & 0x1F));
    uint8_t val = spi_transfer_byte(dev, NRF24_CMD_NOP);
    csn_high(dev);
    return val;
}

esp_err_t nrf24_init(nrf24_t *dev, spi_host_device_t host,
                    gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso,
                    gpio_num_t csn, gpio_num_t ce) {
    if (!dev) return ESP_ERR_INVALID_ARG;

    dev->ce_pin = ce;
    dev->csn_pin = csn;

    gpio_reset_pin(dev->ce_pin);
    gpio_set_direction(dev->ce_pin, GPIO_MODE_OUTPUT);
    ce_low(dev);

    gpio_reset_pin(dev->csn_pin);
    gpio_set_direction(dev->csn_pin, GPIO_MODE_OUTPUT);
    csn_high(dev);

    spi_bus_config_t buscfg = {
        .miso_io_num = miso,
        .mosi_io_num = mosi,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // nRF24 itself only ever needs a few bytes per transfer, but this
        // caps the whole bus - other devices sharing it (an SD card doing
        // 512-byte sector transfers) need the larger size here.
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8 * 1000 * 1000, // 8 MHz
        .mode = 0,                         // SPI Mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = -1,                // Manual software CSN control
        .queue_size = 1,
    };
    err = spi_bus_add_device(host, &devcfg, &dev->spi);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(100)); // nRF24 internal power-on reset wait

    // Basic RF Setup: 5-byte address width, 250kbps rate, 0dBm power
    write_register(dev, NRF24_REG_SETUP_AW, 0x03);   // 5 bytes address
    write_register(dev, NRF24_REG_SETUP_RETR, 0x5F); // 1500us retry delay, 15 retries
    write_register(dev, NRF24_REG_RF_SETUP, 0x26);   // 250 kbps, 0 dBm output power
    write_register(dev, NRF24_REG_EN_AA, 0x01);      // Auto-acknowledgment on Pipe 0
    write_register(dev, NRF24_REG_EN_RXADDR, 0x01);  // Enable Pipe 0
    write_register(dev, NRF24_REG_CONFIG, 0x0E);     // PWR_UP=1, PRIM_RX=0, CRC 2-byte enabled

    // Quick SPI sanity check: read back CONFIG register
    uint8_t cfg = read_register(dev, NRF24_REG_CONFIG);
    if (cfg != 0x0E) {
        ESP_LOGE(TAG, "Hardware failure: Expected CONFIG 0x0E, read 0x%02X. Check SPI wiring!", cfg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "nRF24L01+ initialized in TX Standby.");
    return ESP_OK;
}

esp_err_t nrf24_set_channel(nrf24_t *dev, uint8_t channel) {
    if (channel > 125) channel = 125;
    return write_register(dev, NRF24_REG_RF_CH, channel);
}

esp_err_t nrf24_open_tx_pipe(nrf24_t *dev, const uint8_t *address) {
    // Pipe 0 must match TX address for auto-acknowledgment packets
    csn_low(dev);
    spi_transfer_byte(dev, NRF24_CMD_W_REGISTER | NRF24_REG_RX_ADDR_P0);
    for (int i = 0; i < 5; i++) spi_transfer_byte(dev, address[i]);
    csn_high(dev);

    csn_low(dev);
    spi_transfer_byte(dev, NRF24_CMD_W_REGISTER | NRF24_REG_TX_ADDR);
    for (int i = 0; i < 5; i++) spi_transfer_byte(dev, address[i]);
    csn_high(dev);

    return ESP_OK;
}

esp_err_t nrf24_send_packet(nrf24_t *dev, const void *data, uint8_t len) {
    if (len > 32) return ESP_ERR_INVALID_SIZE;

    // Load payload into TX FIFO
    csn_low(dev);
    spi_transfer_byte(dev, NRF24_CMD_W_TX_PAYLOAD);
    const uint8_t *p = (const uint8_t *)data;
    for (int i = 0; i < len; i++) spi_transfer_byte(dev, p[i]);
    csn_high(dev);

    // Strobe CE pin for >10us to trigger TX
    ce_high(dev);
    esp_rom_delay_us(15);
    ce_low(dev);

    // Poll STATUS register until TX_DS (Data Sent) or MAX_RT (Max Retries) bit is set
    int64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) < 50000) { // 50ms timeout window
        uint8_t status = read_register(dev, NRF24_REG_STATUS);

        if (status & (1 << 5)) { // TX_DS: Ack received
            write_register(dev, NRF24_REG_STATUS, (1 << 5)); // Clear bit
            return ESP_OK;
        }

        if (status & (1 << 4)) { // MAX_RT: No Ack received within retry window
            write_register(dev, NRF24_REG_STATUS, (1 << 4)); // Clear bit
            // Flush TX FIFO to unblock future transmissions
            csn_low(dev);
            spi_transfer_byte(dev, NRF24_CMD_FLUSH_TX);
            csn_high(dev);
            return ESP_FAIL;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t nrf24_open_rx_pipe(nrf24_t *dev, uint8_t pipe, const uint8_t *address, uint8_t payload_size) {
    if (pipe > 5 || payload_size > 32) return ESP_ERR_INVALID_ARG;

    uint8_t addr_reg = NRF24_REG_RX_ADDR_P0 + pipe;

    if (pipe < 2) {
        // Pipes 0/1 have a full 5-byte address
        csn_low(dev);
        spi_transfer_byte(dev, NRF24_CMD_W_REGISTER | addr_reg);
        for (int i = 0; i < 5; i++) spi_transfer_byte(dev, address[i]);
        csn_high(dev);
    } else {
        // Pipes 2-5 only carry 1 configurable byte on top of pipe 1's address
        write_register(dev, addr_reg, address[0]);
    }

    write_register(dev, NRF24_REG_RX_PW_P0 + pipe, payload_size);

    uint8_t en_rxaddr = read_register(dev, NRF24_REG_EN_RXADDR);
    write_register(dev, NRF24_REG_EN_RXADDR, en_rxaddr | (1 << pipe));

    return ESP_OK;
}

esp_err_t nrf24_start_listening(nrf24_t *dev) {
    uint8_t cfg = read_register(dev, NRF24_REG_CONFIG);
    write_register(dev, NRF24_REG_CONFIG, cfg | 0x01); // PRIM_RX = 1
    ce_high(dev);
    vTaskDelay(pdMS_TO_TICKS(2)); // Rx settling time
    return ESP_OK;
}

bool nrf24_available(nrf24_t *dev) {
    uint8_t fifo_status = read_register(dev, NRF24_REG_FIFO_STATUS);
    return !(fifo_status & 0x01); // RX_EMPTY bit clear -> a payload is waiting
}

esp_err_t nrf24_receive_packet(nrf24_t *dev, void *data, uint8_t len) {
    csn_low(dev);
    spi_transfer_byte(dev, NRF24_CMD_R_RX_PAYLOAD);
    uint8_t *p = (uint8_t *)data;
    for (int i = 0; i < len; i++) p[i] = spi_transfer_byte(dev, NRF24_CMD_NOP);
    csn_high(dev);

    write_register(dev, NRF24_REG_STATUS, (1 << 6)); // Clear RX_DR
    return ESP_OK;
}
