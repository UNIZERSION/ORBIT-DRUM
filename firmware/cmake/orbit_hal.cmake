# Compile options of the component that builds the ORBIT sources (firmware/main and firmware_selftest/main).
#
# ORBIT_HOT: placement attribute for the hot audio code (engine/voice render loops). On the Tab5 it is
# IRAM_ATTR (a unique .iram1.N section per function, the same macro ESP-IDF uses), so the render loop
# runs from internal RAM and never waits for a flash cache refill. esp_attr.h is force-included so the
# portable core/ code needs no ESP-IDF include; core headers must only define ORBIT_HOT when it is not
# defined yet (#ifndef ORBIT_HOT / #define ORBIT_HOT / #endif) for the PC build.
# ESP-IDF compiles C++ as gnu++2b, where LVGL's selector idiom (LV_PART_MAIN | LV_STATE_PRESSED, two
# different unnamed enums) is a deprecation warning; the code itself is C++17.
function(orbit_hal_compile_options target)
    target_compile_options(${target} PRIVATE -O2 -Wall "SHELL:-include esp_attr.h"
                           $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>)
    target_compile_definitions(${target} PRIVATE "ORBIT_HOT=IRAM_ATTR")
endfunction()
