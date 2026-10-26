#pragma once
// UI root and screen modules. The whole UI is one LVGL screen laid out like the prototype:
//   top bar | PADS (left top) | BROWSER categories (left bottom) | work area (center, one of the
//   five views) | macros + transport (right) | SCENES timeline (bottom).
// All five views are created once at start and shown/hidden (instant switching, screen-enter
// animation on show). Every module exposes create() and refresh(); ui.cpp runs one LVGL timer
// (16 ms, 33 ms in low power) that calls refresh() of the visible modules, which compare App
// revision counters / telemetry with what they last drew and update only what changed.
#include "lvgl.h"
#include "ui/app.h"

namespace orbit::ui {

void build(lv_display_t* disp);   // after themeInit() and app().init()
// 0.4.9: CPU cycles of each part of the UI tick (60 Hz) since boot, summed (Tab5; zeros on the PC). Read by the
// test console's "uistat" (diagnostics: torn reads are acceptable).
struct TickProf {
    enum Part { kApp = 0, kKeys, kTop, kPads, kCat, kMacros, kTimeline, kView, kDialogs, kParts };
    uint64_t cyc[kParts] = {};
    uint32_t ticks = 0;
};
const TickProf& tickProf();
// 0.4.9: CPU cycles inside the custom draw callbacks (their lv_draw_* calls run at once: LV_OS_NONE), since boot.
struct DrawProf {
    enum Part { kPads = 0, kStepGrid, kWave, kParts };
    uint64_t cyc[kParts] = {};
    uint32_t calls[kParts] = {};
};
DrawProf& drawProf();
// Bakes the stage background (ui_bg.cpp, ~0.9 Mpx of float math) ahead of build(), from any task and
// without LVGL calls: app_main runs it on core 1 while core 0 mounts the card, loads the project and
// starts the display; build() then only waits for it (or bakes itself when this never ran). Once.
void bgPrebake(int w, int h);
// Views are created lazily (only the visible one during build(), the rest after the first frame): makes sure
// view v exists before code outside the view module talks to it.
void ensureView(View v);

// ---- panel maximize (0.3, sharp since 0.3.2): every main-screen panel has a corner-marks button (the work
// area's sits in the top bar after the view tabs; the BASS view has its own for the whole screen). Tap: that
// panel is built again natively larger (ui_scale.h: sharp text and lines) in the area under the top bar or, from
// the BASS view, on the whole screen (top bar hidden); the other panels hide; the same button brings everything
// back. A view tab keeps a maximized work area (it shows that view) and restores any other maximized panel.
enum class Panel : uint8_t { Pads = 0, Browser, Work, Macros, Timeline, Count };
// 26x26, corner marks out (maximize) / in (restore). full_screen: the tap maximizes onto the whole screen and,
// when anything is maximized, restores it.
lv_obj_t* panelMaxButton(lv_obj_t* parent, Panel p, bool full_screen = false);
void panelToggleMax(Panel p);                          // under the top bar
void panelToggleMaxEx(Panel p, bool full_screen);
Panel panelMaxed();                                    // Panel::Count: none
bool panelMaxedFull();                                 // a panel covers the whole screen
lv_obj_t* panelRoot(Panel p);                          // the panel's current root object (tests)

// Persistent panels
namespace topbar   { lv_obj_t* create(lv_obj_t* scr); void refresh(); }
// touchGeometry(): the touch task's pad rectangles follow the drawn panel (after a maximize / restore).
// installTouchHook(): the touch task's hook hands every sample to pads::touchInput and
// view_bass::touchInput (0.4.3: the BASS KEYS keyboard with several fingers).
// setCulling (0.4.9): draw only what reaches the redrawn area (default on; off = the old way, A/B measurements);
// invalidateForTest(i): pad i's redraw area (PC tests).
// setDebugSkip: diagnostics only, parts left out (1 images, 2 hit rings, 4 tints, 8 labels) to time the rest.
namespace pads     { void setCulling(bool on); bool culling(); void invalidateForTest(int i);
                     void setDebugSkip(uint8_t mask);
                     lv_obj_t* create(lv_obj_t* scr); void refresh(); void installTouchHook(); void touchGeometry();
                     void touchInput(const hal::TouchPoint* pts, int n); }
namespace catpanel { lv_obj_t* create(lv_obj_t* scr); void refresh(); }   // left-bottom BROWSER
namespace macros   { lv_obj_t* create(lv_obj_t* scr); void refresh(); }   // right panel
namespace timeline { lv_obj_t* create(lv_obj_t* scr); void refresh(); }   // bottom SCENES

// Work-area views (parent = the 746x486 work panel)
namespace view_sample   { lv_obj_t* create(lv_obj_t* work); void refresh(); void onShow(); }
namespace view_sequence { lv_obj_t* create(lv_obj_t* work); void refresh(); void onShow(); }
namespace view_mixer    { lv_obj_t* create(lv_obj_t* work); void refresh(); void onShow(); }
namespace view_browser  { lv_obj_t* create(lv_obj_t* work); void refresh(); void onShow(); void focusSearch(); }
namespace view_fx       { lv_obj_t* create(lv_obj_t* work); void refresh(); void onShow(); }
// ACID BASS (0.3). onHide(): another view is shown; touchInput(): [touch task] fingers on the KEYS keyboard.
namespace view_bass     { lv_obj_t* create(lv_obj_t* work); void refresh(); void onShow(); void onHide();
                          void touchInput(const hal::TouchPoint* pts, int n); bool keysOn();
                          int liveBase(); bool liveAccent(); bool liveSlide(); void setLiveBase(int midi); }

// Physical keyboard (0.4.6, keys.cpp): the M5Stack Tab5 Keyboard with Ableton Live's shortcuts. poll(): every UI
// tick; handle(): one key (the test console and the PC tests inject through hal::kbdInject, then poll).
namespace keys { void poll(); void handle(const hal::KeyEvent& e); }

// Test console (0.4.4, test_console.cpp): commands from the PC over the USB console (tools/device_test.py).
// start(): its task, after the UI is built; handleLine(): one command (also from host tests).
namespace testcon { void start(); void handleLine(const char* line); }

// Dialogs (built on modalOpen())
namespace dlg_settings { void open(); void refresh(); }
namespace dlg_project  { void open(); }
namespace dlg_help     { void open(); }
namespace dlg_sample   { void open(); void refresh(); }   // SAMPLE DETAILS
// [added] SETTINGS opened on a given section (0 General .. 7 About); the top-bar BT chip opens Bluetooth.
// Inline over open() (which shows the remembered UiState::settings_tab): no new symbol for test builds that
// replace the dialog with a stub.
namespace dlg_settings {
constexpr int kTabBluetooth = 3;
inline void openAt(int tab)
{
    app().ui().settings_tab = tab;
    open();
}
}  // namespace dlg_settings

}  // namespace orbit::ui
