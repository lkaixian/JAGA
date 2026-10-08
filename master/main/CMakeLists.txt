idf_component_register(SRCS "main.c" "dht22.c" "jsnsr40t.c" "nrf24.c" "ds1307.c" "sdcard.c" "ssd1306.c" "api.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc esp_wifi esp_event esp_netif esp_http_server mdns nvs_flash json esp_http_client esp-tls)
