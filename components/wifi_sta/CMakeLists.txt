idf_component_register(SRCS "wifi_sta.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event nvs_flash esp_netif freertos lwip)
