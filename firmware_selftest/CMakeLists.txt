cmake_minimum_required(VERSION 3.16)
# ORBIT DRUM hardware self-test: the firmware's hal/*.cpp with a test screen (display, touch, audio,
# SD, battery, USB-MIDI). Builds offline with the firmware's components:
#  - main/idf_component.yml has the same dependencies as ../firmware/main/idf_component.yml, so the
#    component manager accepts the copied ../firmware/dependencies.lock without asking the registry;
#  - managed_components is a directory junction (Windows) / symlink to ../firmware/managed_components.
set(IDF_TARGET "esp32p4" CACHE STRING "M5Stack Tab5")
set(PROJECT_VER "0.2.0")
get_filename_component(ORBIT_FW_DIR "${CMAKE_CURRENT_LIST_DIR}/../firmware" ABSOLUTE)

configure_file("${ORBIT_FW_DIR}/dependencies.lock" "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock" COPYONLY)
set(_orbit_mc "${CMAKE_CURRENT_LIST_DIR}/managed_components")
if(NOT EXISTS "${_orbit_mc}")
    if(CMAKE_HOST_WIN32)
        file(TO_NATIVE_PATH "${_orbit_mc}" _orbit_link)
        file(TO_NATIVE_PATH "${ORBIT_FW_DIR}/managed_components" _orbit_target)
        execute_process(COMMAND cmd /c mklink /J "${_orbit_link}" "${_orbit_target}"
                        RESULT_VARIABLE _orbit_r OUTPUT_QUIET ERROR_QUIET)
    else()
        execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${ORBIT_FW_DIR}/managed_components" "${_orbit_mc}"
                        RESULT_VARIABLE _orbit_r)
    endif()
    if(NOT _orbit_r EQUAL 0 OR NOT EXISTS "${_orbit_mc}/espressif__m5stack_tab5")
        message(FATAL_ERROR "Nie udało się podlinkować ../firmware/managed_components (${_orbit_r}).")
    endif()
endif()

include("${ORBIT_FW_DIR}/cmake/orbit_project.cmake")
set(SDKCONFIG_DEFAULTS "${ORBIT_FW_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
orbit_refresh_sdkconfig("${CMAKE_CURRENT_LIST_DIR}" ${SDKCONFIG_DEFAULTS})

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orbit_selftest)

orbit_project_fixups()
