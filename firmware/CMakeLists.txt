cmake_minimum_required(VERSION 3.16)
# ORBIT DRUM: drum machine / sampler for the M5Stack Tab5 (ESP32-P4, ESP-IDF 5.5.2, LVGL 9.3).
# Offline build: managed_components/ + dependencies.lock are copied in and match main/idf_component.yml.
set(IDF_TARGET "esp32p4" CACHE STRING "M5Stack Tab5")
set(PROJECT_VER "0.4.8")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/orbit_project.cmake)
orbit_refresh_sdkconfig("${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

# Build only what main needs (the copied component set also holds camera / Wi-Fi parts we do not use).
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orbit_drum)

orbit_project_fixups()
