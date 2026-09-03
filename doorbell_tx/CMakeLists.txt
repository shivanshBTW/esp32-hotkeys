cmake_minimum_required(VERSION 3.16)

# Slim ESP-NOW doorbell transmitter (classic ESP32 / ESP-IDF).
# ESP8266 is the same product via doorbell_tx_8266/ (PlatformIO). Not LumosOS.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/lumos_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/lumos_ota"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doorbell_tx)
