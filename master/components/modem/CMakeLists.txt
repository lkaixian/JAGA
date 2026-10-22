idf_component_register(SRCS "src/esp_modem.c" "src/esp_modem_dce_service.c" "src/esp_modem_netif.c"
                            "src/esp_modem_compat.c" "src/sim800.c"
                       INCLUDE_DIRS include
                       REQUIRES driver esp_netif)
