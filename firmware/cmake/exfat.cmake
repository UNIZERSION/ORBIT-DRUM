# exFAT support for the SD card (cards above 32 GB usually ship as exFAT).
#
# ESP-IDF 5.5.2 builds FatFs with FF_FS_EXFAT hard-coded to 0 (no Kconfig option). This file copies the
# fatfs component of the active ESP-IDF into the build folder, switches FF_FS_EXFAT to 1 in that copy
# and adds it to EXTRA_COMPONENT_DIRS, so it replaces the built-in component (project components win
# over ESP-IDF components with the same name). ESP-IDF itself is never modified.
# Include this file BEFORE project.cmake. Needs CONFIG_FATFS_USE_LABEL=y and long file names.
set(_orbit_fatfs_src "$ENV{IDF_PATH}/components/fatfs")
set(_orbit_fatfs_dst "${CMAKE_BINARY_DIR}/orbit_components/fatfs")
if(NOT EXISTS "${_orbit_fatfs_src}/src/ffconf.h")
    message(FATAL_ERROR "ESP-IDF not found (IDF_PATH). Activate ESP-IDF 5.5.2 before configuring.")
endif()
file(READ "${_orbit_fatfs_src}/src/ffconf.h" _orbit_ffconf)
if(NOT _orbit_ffconf MATCHES "#define[ \t]+FF_FS_EXFAT[ \t]+0")
    message(FATAL_ERROR "Unexpected FatFs configuration (FF_FS_EXFAT) in ESP-IDF; use a pristine ESP-IDF 5.5.2.")
endif()
# file(COPY) only rewrites changed files, so a reconfigure does not rebuild FatFs
file(COPY "${_orbit_fatfs_src}/" DESTINATION "${_orbit_fatfs_dst}"
     PATTERN "test_apps" EXCLUDE PATTERN "host_test" EXCLUDE PATTERN "test_fatfsgen" EXCLUDE)
string(REGEX REPLACE "#define[ \t]+FF_FS_EXFAT[ \t]+0" "#define FF_FS_EXFAT 1" _orbit_ffconf "${_orbit_ffconf}")
set(_orbit_ffconf_tmp "${CMAKE_BINARY_DIR}/orbit_components/ffconf.h.tmp")
file(WRITE "${_orbit_ffconf_tmp}" "${_orbit_ffconf}")
configure_file("${_orbit_ffconf_tmp}" "${_orbit_fatfs_dst}/src/ffconf.h" COPYONLY)
list(APPEND EXTRA_COMPONENT_DIRS "${_orbit_fatfs_dst}")
message(STATUS "ORBIT: FatFs with exFAT from ${_orbit_fatfs_dst}")
