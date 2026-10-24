#pragma once
// Headless LVGL 9.3 on the PC for screenshots and UI tests (no window). Implemented in
// host/lvgl_host.cpp. The display is 1280x720 RGB565 like the Tab5 (landscape).
#include "lvgl.h"

lv_display_t* lvhost_init(int w = 1280, int h = 720);  // lv_init + display + pointer indev + tick
void lvhost_run(int ms);                               // advance virtual time, run timers (5 ms steps)
bool lvhost_screenshot(const char* png_path);          // full frame after a refresh
void lvhost_pointer(int x, int y, bool pressed);       // set the pointer state (takes effect on run)
void lvhost_tap(int x, int y);                         // press 60 ms, release, settle 100 ms
void lvhost_double_tap(int x, int y);
void lvhost_drag(int x0, int y0, int x1, int y1, int ms);
uint32_t lvhost_ms();                                  // virtual time
