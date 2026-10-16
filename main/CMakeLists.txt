idf_component_register(SRCS "main.cpp" "bme280.cpp" "ssd1306.cpp" "net_time.cpp" "telemetry.cpp" "history.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_gpio log freertos
                                esp_wifi esp_netif esp_event nvs_flash lwip
                                esp_partition console
                                espressif__mqtt)
