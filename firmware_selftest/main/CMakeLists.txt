# Self-test main: the firmware's hardware layer (hal/*.cpp + core/settings.cpp) and the test screen.
get_filename_component(_fw_main "${CMAKE_CURRENT_LIST_DIR}/../../firmware/main" ABSOLUTE)
file(GLOB _hal_srcs "${_fw_main}/hal/*.cpp")

idf_component_register(
    SRCS "selftest_main.cpp" ${_hal_srcs} "${_fw_main}/core/settings.cpp"
    INCLUDE_DIRS "." "${_fw_main}"
    REQUIRES esp_lcd esp_driver_ppa esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_driver_sdmmc
             esp_mm esp_hw_support esp_system esp_timer esp_psram esp_rom heap vfs
             fatfs sdmmc nvs_flash json usb
             espressif__m5stack_tab5 lvgl__lvgl espressif__esp_lvgl_port espressif__esp_codec_dev
             espressif__esp_lcd_touch espressif__esp_io_expander
)

include("${_fw_main}/../cmake/orbit_hal.cmake")
orbit_hal_compile_options(${COMPONENT_LIB})
