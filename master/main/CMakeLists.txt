idf_component_register(SRCS "main.c" "dht22.c" "jsnsr40t.c" "nrf24.c" "api.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_pm esp_event esp_netif json esp_http_client esp-tls modem)
