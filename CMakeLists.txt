# ESP32 Weather Station (ideaspark ESP32 0.96" OLED board)
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build the components actually required by "main"
idf_build_set_property(MINIMAL_BUILD ON)
project(weather_station)
