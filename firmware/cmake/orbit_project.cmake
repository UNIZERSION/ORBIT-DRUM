# Shared build helpers of the ORBIT DRUM firmware and the hardware self-test (firmware_selftest).
#
#   include(<firmware>/cmake/orbit_project.cmake)
#   orbit_refresh_sdkconfig(<project dir> <defaults file>...)   # before project.cmake
#   ... include(project.cmake) / project(...)
#   orbit_project_fixups()                                       # after project()

include("${CMAKE_CURRENT_LIST_DIR}/exfat.cmake")

# Offline component manager. managed_components/ and dependencies.lock are complete, but the component
# manager still asks the registry for newer versions and re-checks every locked version on each configure.
# Both checks are skipped (the second one treats an unreachable registry as "no changes": a proxy that
# refuses connections makes it fail at once instead of waiting for a timeout). Configure with
# -DORBIT_ALLOW_NETWORK=ON (or set ORBIT_ALLOW_NETWORK=1 in the environment) to update dependencies.
# A fresh clone has no managed_components/ (it is not in git): the first configure downloads the locked
# versions (dependencies.lock), so the network stays open until the folder exists.
if(NOT ORBIT_ALLOW_NETWORK AND NOT "$ENV{ORBIT_ALLOW_NETWORK}" STREQUAL "1" AND EXISTS "${CMAKE_SOURCE_DIR}/managed_components")
    set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} "0")
    set(ENV{HTTPS_PROXY} "http://127.0.0.1:9")
    set(ENV{HTTP_PROXY} "http://127.0.0.1:9")
    set(ENV{https_proxy} "http://127.0.0.1:9")
    set(ENV{http_proxy} "http://127.0.0.1:9")
    set(ENV{NO_PROXY} "")
    set(ENV{no_proxy} "")
endif()

# The defaults files are the configuration source of this project. kconfgen applies them only to
# options missing from sdkconfig, so a sdkconfig written before the defaults changed keeps stale values.
# When the defaults changed since the last configure (or are newer than a sdkconfig this build folder
# never saw), sdkconfig is renamed to sdkconfig.old and regenerated from the defaults.
function(orbit_refresh_sdkconfig project_dir)
    set(_cfg "${project_dir}/sdkconfig")
    set(_all "")
    foreach(_d ${ARGN})
        if(EXISTS "${_d}")
            file(READ "${_d}" _txt)
            string(APPEND _all "${_txt}\n")
        endif()
    endforeach()
    string(MD5 _md5 "${_all}")
    set(_stale FALSE)
    if(EXISTS "${_cfg}")
        if(DEFINED ORBIT_SDKCONFIG_DEFAULTS_MD5)
            if(NOT ORBIT_SDKCONFIG_DEFAULTS_MD5 STREQUAL _md5)
                set(_stale TRUE)
            endif()
        else()
            foreach(_d ${ARGN})
                if(EXISTS "${_d}" AND "${_d}" IS_NEWER_THAN "${_cfg}")
                    set(_stale TRUE)
                endif()
            endforeach()
        endif()
    endif()
    if(_stale)
        message(STATUS "ORBIT: sdkconfig.defaults changed, sdkconfig regenerated (previous one: sdkconfig.old)")
        file(RENAME "${_cfg}" "${_cfg}.old")
    endif()
    set(ORBIT_SDKCONFIG_DEFAULTS_MD5 "${_md5}" CACHE INTERNAL "defaults used for sdkconfig")
endfunction()

macro(orbit_project_fixups)
    # BSP 1.3.2 warns "Long filenames on SD card are disabled" by testing CONFIG_FATFS_LONG_FILENAMES,
    # the name of a Kconfig choice that never becomes a macro. Long names are enabled here.
    if(CONFIG_FATFS_LFN_HEAP OR CONFIG_FATFS_LFN_STACK)
        idf_component_get_property(_orbit_bsp espressif__m5stack_tab5 COMPONENT_LIB)
        target_compile_definitions(${_orbit_bsp} PRIVATE CONFIG_FATFS_LONG_FILENAMES=1)
    endif()

    # main/idf_component.yml lists espressif/esp_hosted (kept identical to the offline lock), so the
    # component is built and linked WHOLE_ARCHIVE. Its constructor (port_esp_hosted_host_init.c) would
    # start ESP-Hosted on SDIO before app_main and assert without a configured C6. ORBIT DRUM does not use
    # Wi-Fi and keeps the ESP32-C6 powered down: the constructor is removed from the build.
    if(TARGET __idf_espressif__esp_hosted)
        idf_component_get_property(_orbit_hosted espressif__esp_hosted COMPONENT_LIB)
        get_target_property(_orbit_hosted_srcs ${_orbit_hosted} SOURCES)
        list(FILTER _orbit_hosted_srcs EXCLUDE REGEX "port_esp_hosted_host_init\\.c$")
        set_target_properties(${_orbit_hosted} PROPERTIES SOURCES "${_orbit_hosted_srcs}")
        # nothing of it runs: build it small
        target_compile_options(${_orbit_hosted} PRIVATE -Os)
    endif()
endmacro()
