#include "sdcard.h"
#include <stdio.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

static const char *TAG = "SDCARD_DRIVER";
static sdmmc_card_t *s_card = NULL;

esp_err_t sdcard_init(spi_host_device_t host, gpio_num_t cs_pin) {
    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = host;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = host;
    slot_config.gpio_cs = cs_pin;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host_config, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed on GPIO%d CS (err 0x%x) - is a card inserted?", cs_pin, err);
        s_card = NULL;
        return err;
    }

    ESP_LOGI(TAG, "SD card mounted at /sdcard (%s, %llu MB)", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t sdcard_append_line(const char *path, const char *header, const char *line) {
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    bool exists = (stat(path, &st) == 0);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s for append", path);
        return ESP_FAIL;
    }

    if (!exists && header) {
        fprintf(f, "%s\n", header);
    }
    fprintf(f, "%s\n", line);

    fclose(f);
    return ESP_OK;
}
