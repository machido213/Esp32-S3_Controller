idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server esp_http_client esp_https_ota esp_adc esp_netif nvs_flash esp_wifi                       
                       PRIV_REQUIRES esp_driver_gpio esp_driver_uart
                       EMBED_TXTFILES "index.html")