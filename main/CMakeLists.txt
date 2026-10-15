idf_component_register(SRCS "main.cpp" "bme280.cpp" "ssd1306.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_gpio log freertos)
