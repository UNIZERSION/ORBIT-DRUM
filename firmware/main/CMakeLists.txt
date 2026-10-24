# ORBIT DRUM firmware: portable core (core/), Tab5 hardware layer (hal/), LVGL UI (ui/) and app_main.
# Sources are collected recursively; after adding a file run `idf.py reconfigure` (or any build that
# re-runs CMake) so it is picked up.
set(_orbit_dirs core hal ui)
set(_orbit_srcs "")
foreach(_d ${_orbit_dirs})
    file(GLOB_RECURSE _found
         "${CMAKE_CURRENT_LIST_DIR}/${_d}/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/${_d}/*.cpp")
    list(APPEND _orbit_srcs ${_found})
endforeach()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/app_main.cpp")
    list(APPEND _orbit_srcs "${CMAKE_CURRENT_LIST_DIR}/app_main.cpp")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/app_main.c")
    list(APPEND _orbit_srcs "${CMAKE_CURRENT_LIST_DIR}/app_main.c")
endif()

idf_component_register(
    SRCS ${_orbit_srcs}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_ppa esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_driver_sdmmc
             esp_mm esp_hw_support esp_system esp_timer esp_psram esp_rom heap vfs
             fatfs sdmmc nvs_flash json usb app_update
             espressif__m5stack_tab5 lvgl__lvgl espressif__esp_lvgl_port espressif__esp_codec_dev
             espressif__esp_lcd_touch espressif__esp_io_expander
)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/orbit_hal.cmake")
orbit_hal_compile_options(${COMPONENT_LIB})

# With an LVGL OS layer (CONFIG_LV_OS_FREERTOS), osal/lv_freertos.c includes FreeRTOS atomic.h, whose unused
# static inline helpers warn (-Wunused-function); third-party code, silenced so the build log stays clean
# (sdkconfig.defaults now selects CONFIG_LV_OS_NONE, the option is kept for a later OS layer).
idf_component_get_property(_orbit_lvgl lvgl__lvgl COMPONENT_LIB)
target_compile_options(${_orbit_lvgl} PRIVATE -Wno-unused-function)
