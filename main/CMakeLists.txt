idf_component_register(SRCS "main.cpp" "bme280.cpp" "ssd1306.cpp" "net_time.cpp" "telemetry.cpp" "history.cpp" "web_server.cpp" "health.cpp" "charts.cpp" "identity.cpp"
                       INCLUDE_DIRS "."
                       EMBED_FILES "web_page.html"
                       REQUIRES esp_driver_i2c esp_driver_gpio log freertos
                                esp_wifi esp_netif esp_event nvs_flash lwip
                                esp_partition console esp_http_server esp_timer esp_app_format
                                espressif__mqtt)

# FR-8.2: the web page is also embedded gzip-compressed (about a third of the
# size), and sent that way to browsers that accept it.
idf_build_get_property(python PYTHON)
set(web_page_gz "${CMAKE_CURRENT_BINARY_DIR}/web_page.html.gz")
add_custom_command(OUTPUT "${web_page_gz}"
                   COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/gzip_file.py"
                           "${CMAKE_CURRENT_SOURCE_DIR}/web_page.html" "${web_page_gz}"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web_page.html" "${CMAKE_CURRENT_SOURCE_DIR}/gzip_file.py"
                   VERBATIM)
add_custom_target(web_page_gz DEPENDS "${web_page_gz}")
add_dependencies(${COMPONENT_LIB} web_page_gz)
target_add_binary_data(${COMPONENT_LIB} "${web_page_gz}" BINARY)
