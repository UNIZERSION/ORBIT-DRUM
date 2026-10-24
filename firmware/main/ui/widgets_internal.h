#pragma once
// Internals shared by the widget kit sources (widgets*.cpp). Not for screens.
#include "lvgl.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace orbit::ui::wk {

// Widget state lives in the user_data of an event callback registered by the kit (so the
// object's own user_data stays free for the screens). Returns that user_data or nullptr.
void* findState(lv_obj_t* obj, lv_event_cb_t cb);

bool feedbackOn();   // Settings.touch_feedback && !low_power
bool animOn();       // Settings.animations && !low_power
// Keeps the shared :active style in line with Settings (called on every kit press).
void syncFeedback();
// Shared button setup: press look, ripple, ext hit area, no scrolling of its own.
void buttonCommon(lv_obj_t* btn, int hit);
// First child of class lv_label (or nullptr).
lv_obj_t* firstLabel(lv_obj_t* obj);
// Chevron-down stroke icon (not part of enum Icon), used by selects.
lv_obj_t* chevDown(lv_obj_t* parent, int size, uint32_t color);
// Full-screen transparent catcher on lv_layer_top() that calls on_tap when tapped (popup close).
lv_obj_t* overlayCreate(lv_opa_t scrim_opa);
// Text helper: sets the label text only when it differs (avoids relayout + redraw).
void setTextIfChanged(lv_obj_t* label, const char* text);

}  // namespace orbit::ui::wk
