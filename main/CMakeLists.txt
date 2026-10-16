idf_component_register(SRCS "main.cpp" "bme280.cpp" "ssd1306.cpp" "net_time.cpp" "telemetry.cpp" "history.cpp" "web_server.cpp"
                       INCLUDE_DIRS "."
                       EMBED_FILES "web_page.html"
                       REQUIRES esp_driver_i2c esp_driver_gpio log freertos
                                esp_wifi esp_netif esp_event nvs_flash lwip
                                esp_partition console esp_http_server
                                espressif__mqtt)
