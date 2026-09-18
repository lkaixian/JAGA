idf_component_register(SRCS "main.c" "dht22.c" "jsnsr40t.c" "nrf24.c" "ds1307.c" "sdcard.c" "ssd1306.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc)
