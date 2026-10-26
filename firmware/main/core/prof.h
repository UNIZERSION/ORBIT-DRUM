#pragma once
// CPU cycle counter for the profiles (Telemetry::prof*, ui::tickProf): the core's own counter on the Tab5 (cycles of
// the core that runs the code); on the PC the steady clock in units of 1/360 us (as if the PC ran at 360 MHz), so
// the same code reports milliseconds there too.
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#else
#include <chrono>
#endif

namespace orbit {

#ifdef ESP_PLATFORM
inline uint32_t profCycles() { return (uint32_t)esp_cpu_get_cycle_count(); }
#else
inline uint32_t profCycles()
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    return (uint32_t)((uint64_t)ns * 36u / 100u);
}
#endif

}  // namespace orbit
