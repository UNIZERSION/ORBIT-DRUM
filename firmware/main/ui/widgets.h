#pragma once
// ORBIT LIGHT widget kit (LVGL 9.3). Shared by all screens. Each widget reproduces the look and
// the interaction of its prototype counterpart (class names in brackets refer to the HTML/CSS):
// knob [.knob-wrap/.knob], fader [.vert-fader], meter [.meter], buttons [.action-btn, .micro-btn,
// .icon-btn, .tab], toggle [.toggle], select [.ui-select / .mini-select], modal [.modal-window],
// toast [#toast], ripple [.ripple], icons [ico()].
//
// Touch on a 5" 1280x720 panel: every interactive widget gets an invisible extended hit area
// (lv_obj_set_ext_click_area) so small graphics stay easy to hit.
#include <stdint.h>
#include "lvgl.h"
#include "ui/app.h"
#include "ui/ui_scale.h"

namespace orbit::ui {

// ---------------------------------------------------------------- icons
// Stroke icons of the prototype's ico() set, pre-rasterised (tools/icongen) as A8 images and
// recoloured with the text color of their parent.
enum class Icon : uint8_t {
    Play, Pause, Stop, Record, Folder, Settings, Search, Grid, Wave, Mixer, Spark, Close, Chev, Back,
    Save, Help, Layers, Key, Home, Minus, Plus, Headphones, Menu, Check, Reload, Trash, Info,
    Keyboard, Eq, Volume, Dots, Pencil, Envelope, Slice, Rewind, Link, Count
};
// Creates an lv_image showing the icon at size px (14..26 available, nearest is used).
lv_obj_t* iconCreate(lv_obj_t* parent, Icon icon, int size, uint32_t color);
void iconSet(lv_obj_t* img, Icon icon, int size);

// ---------------------------------------------------------------- buttons
// All buttons: press feedback (scale .963 + brightness, 120 ms), ripple (if enabled), ext hit area.
// Buttons wider than 120 px or taller than 44 px keep only the brightness part of the press look:
// a scale != 1 renders the object through an ARGB layer on every animation frame.
lv_obj_t* actionBtn(lv_obj_t* parent, const char* text, bool primary = false, int w = 0, int h = 34);
lv_obj_t* microBtn(lv_obj_t* parent, const char* text, int w = 25, int h = 24);
lv_obj_t* iconBtn(lv_obj_t* parent, Icon icon, int size = 18, int box = 30);
lv_obj_t* tabBtn(lv_obj_t* parent, const char* text, int w, int h);
void btnSetActive(lv_obj_t* btn, bool active);   // .active / .lit / .primary look
void btnSetText(lv_obj_t* btn, const char* text);
// Hold-to-repeat (BPM +/-): calls cb on press and then accelerating every 400 -> 60 ms.
void btnRepeat(lv_obj_t* btn, void (*cb)(void* user), void* user);

// ---------------------------------------------------------------- knob
enum class KnobLook : uint8_t { Dark = 0, White };
// Column: caption (uppercase), knob, value text. Diameter: 53/56 (editor), 69/78 (macro).
// Interaction: vertical drag changes the value (sensitivity from Settings), the arc and the
// pointer follow at once, the value text formats via the parameter spec; double tap resets to the
// parameter default (Settings > Touch > Double tap reset). Drag captures the pointer.
lv_obj_t* knobCreate(lv_obj_t* parent, KnobLook look, int diameter, const ParamRef& ref,
                     const char* caption = nullptr);
void knobBind(lv_obj_t* knob_wrap, const ParamRef& ref, const char* caption = nullptr);
void knobRefresh(lv_obj_t* knob_wrap);   // re-read the value (after model changes)
ParamRef knobRef(lv_obj_t* knob_wrap);

// ---------------------------------------------------------------- fader + meter
// Vertical fader (track + accent fill + thumb), drag anywhere on it; value 0..1.
lv_obj_t* faderCreate(lv_obj_t* parent, int w, int h, const ParamRef& ref);
void faderRefresh(lv_obj_t* fader);
// Segmented meter: accent 6 px segments with 2 px gaps from the bottom, meter-bg track.
// meterSet takes a linear peak; the widget applies dB scaling (-48..+3), a 300 ms peak decay
// and only invalidates when the lit height changes by >= 1 px.
lv_obj_t* meterCreate(lv_obj_t* parent, int w, int h);
void meterSet(lv_obj_t* meter, float peak_linear);

// ---------------------------------------------------------------- toggle + select + input
lv_obj_t* toggleCreate(lv_obj_t* parent, bool on, void (*cb)(bool on, void* user), void* user);
void toggleSet(lv_obj_t* toggle, bool on);
// Dropdown look of .ui-select/.mini-select. Tapping opens a popup list above other content.
lv_obj_t* selectCreate(lv_obj_t* parent, int w, int h, const char* const* options, int count, int selected,
                       void (*cb)(int index, void* user), void* user);
void selectSet(lv_obj_t* select, int index);
// Range slider (settings rows: brightness, knob sensitivity, volume).
lv_obj_t* hsliderCreate(lv_obj_t* parent, int w, int min, int max, int value, void (*cb)(int v, void* user), void* user);
// Text field that opens the on-screen keyboard dialog when tapped.
lv_obj_t* textFieldCreate(lv_obj_t* parent, int w, const char* text, int maxlen,
                          void (*on_done)(const char* text, void* user), void* user, const char* placeholder = nullptr);
void textFieldSet(lv_obj_t* field, const char* text);

// ---------------------------------------------------------------- overlays
// Modal: scrim #0009 + window shown at once (no fade / slide: on the Tab5 every animation step of
// a translucent full-screen scrim repaints all 1280x720 px), 56 px head with title + close icon,
// ESC/close/scrim tap closes. Returns the body container (flex column) to fill. Only one modal at
// a time; opening another replaces it.
lv_obj_t* modalOpen(const char* title, int w = 824, int min_h = 410, int max_h = 650);
lv_obj_t* modalFooter(const char* note);   // 58 px footer row (note left, buttons right)
void modalClose();
bool modalIsOpen();
// [added by the widget kit] Called once when the current modal closes (close button, scrim tap,
// ESC, modalClose() or replacement by another modal); widgets of the body are deleted right after.
void modalOnClose(void (*cb)(void* user), void* user);
// Toast: bottom center, slides up 15 px + fades in 240 ms, visible 2.6 s (1.6 s for short texts
// under 24 bytes). A new text with the same key as the visible one (the same text, or the same
// "PREFIX:" before a colon, e.g. "TAP TEMPO: 121 BPM") only replaces the text and keeps the
// running timer, so a stream of repeats does not keep the toast up.
void toast(const char* text);
// On-screen keyboard dialog (Polish letters on long press), returns via callback. The sheet is
// shown at once (no slide).
void keyboardOpen(const char* title, const char* initial, int maxlen,
                  void (*on_done)(const char* text, void* user), void* user);
// [added] Numeric variant of keyboardOpen (TEMPO and other numbers): the same sheet with a
// 1-2-3 / 4-5-6 / 7-8-9 pad plus 0, '.' and DEL in large keys; returns via the same callback.
void keyboardOpenNumeric(const char* title, const char* initial, int maxlen,
                         void (*on_done)(const char* text, void* user), void* user);
// Small popup menu anchored to an object (used by selects, "ONE SHOT ▾", scene "+").
void popupMenu(lv_obj_t* anchor, const char* const* items, int count, int selected,
               void (*cb)(int index, void* user), void* user);
// [added for the screen chrome] true while a modal, a popup menu / select list or the on-screen
// keyboard is open (any clickable overlay on lv_layer_top(); the toast does not count). The pad
// grid's touch hook stays silent while it is true.
bool overlayOpen();

// ---------------------------------------------------------------- feedback
// Ripple: 12 px accent dot at the touch point, scale x19 and fade to 0 in 560 ms (ease-out).
// Attach to any clickable object (buttons above call it already). Respects Settings.
void rippleAttach(lv_obj_t* obj);
// [added] Applies Settings.touch_feedback / low_power to the shared press style now. Call once
// after building the UI (before the first frame), so the first tap does not restyle every button;
// later changes are still picked up on the next kit press.
void pressFeedbackSync();
// Screen enter: translateY 4 -> 0 in two discrete steps over 90 ms, no opacity fade (a fade or a
// smooth slide repaints the whole view on every animation frame). Respects Settings.animations.
void animScreenEnter(lv_obj_t* obj);
// Pad/step "hit" pulse: glow + scale .96 -> 1 in 280 ms.
void animHit(lv_obj_t* obj);

// ---------------------------------------------------------------- misc helpers
lv_obj_t* panelCreate(lv_obj_t* parent, int x, int y, int w, int h);       // .panel
lv_obj_t* panelHead(lv_obj_t* panel, Icon icon, const char* title);        // 36 px head row; returns the right-side container
lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color);
lv_obj_t* overline(lv_obj_t* parent, const char* text);                    // .overline
void setHit(lv_obj_t* obj, int extra_px);                                  // ext click area

// ---------------------------------------------------------------- scaled panels (maximize, ui_scale.h)
// A maximized panel is laid out natively larger. Code that reads the pointer itself gets it from pointerIn():
// obj's top-left corner stays where it is on the screen and the offset from it is in design px, so
// "p.x - obj.x1 + design_x" is a design coordinate whether the panel is maximized or not.
lv_point_t pointerIn(const lv_obj_t* obj);
// The area of obj on the screen as drawn (after its own and its parents' transforms, e.g. the press scale).
void screenArea(const lv_obj_t* obj, lv_area_t* out);

}  // namespace orbit::ui
