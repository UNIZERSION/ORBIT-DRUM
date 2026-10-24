#pragma once
// Reusable waveform widget (SAMPLE editor, SAMPLE DETAILS dialog, BROWSER preview).
//
// One LVGL object, custom drawn:
//   dark frame #1d2022 + border #343638, 10x4 grid (white ~5 %),
//   waveform (Sample::pk_min/pk_max resampled to the width) rendered once per sample / size /
//   color change into a cached ARGB8888 draw buffer (PSRAM on the device) and blitted,
//   region outside START..END dimmed (~45 % black), START/END markers (2 px accent, rounded
//   handle tabs top and bottom, 15 % inner glow band), slice markers (thin lines + numbers,
//   selected slice highlighted), optional AHDSR envelope overlay, time label on a dark plate
//   ("0.000s — 0.420s   48 000 Hz"): top-right, or top-left / between the markers when a
//   START/END handle would run through it.
// The playhead is a separate small child object moved with lv_obj_set_x only when its pixel
// changes, so the waveform itself is never redrawn at playback rate.
//
// Interaction (only when callbacks are set):
//   press near START/END (±18 px) and drag -> Callbacks::region(start, end, done=false) while
//   moving, done=true on release; double tap on a handle -> Callbacks::snap(0 start / 1 end);
//   any other tap -> Callbacks::tap(pos01). Positions are 0..1 fractions of the sample.
#include "lvgl.h"
#include "core/sample.h"

namespace orbit::ui::wave {

lv_obj_t* create(lv_obj_t* parent, int w, int h);
// Sample to show (nullptr = empty frame, "—"). The widget re-renders its cache only when the
// pointer, Sample::id or the size changed, so calling it every refresh is cheap.
void setSample(lv_obj_t* obj, const Sample* s);
void setRegion(lv_obj_t* obj, float start, float end);        // 0..1
void setSlices(lv_obj_t* obj, const float* pos, int n, int sel); // n = 0 hides; sel -1 none
enum class Overlay : uint8_t { None, Envelope };
void setOverlay(lv_obj_t* obj, Overlay o);
void setEnvelope(lv_obj_t* obj, float a_ms, float h_ms, float d_ms, float s, float r_ms);
void setPlayhead(lv_obj_t* obj, float pos01 /* <0 hides */);
void setShowTime(lv_obj_t* obj, bool on);                      // time label (default on)
void setHandles(lv_obj_t* obj, bool on);                       // START/END markers (default on)
void setAccent(lv_obj_t* obj, lv_color_t c);                   // waveform/marker color (default theme accent)

// Interaction callbacks (any may be nullptr).
struct Callbacks {
    void (*region)(float start, float end, bool done, void* u) = nullptr;
    void (*snap)(int which /*0 start 1 end*/, void* u) = nullptr;
    void (*tap)(float pos01, void* u) = nullptr;
    void* user = nullptr;
};
void setCallbacks(lv_obj_t* obj, const Callbacks& cb);

// Introspection (tests / hit testing): x pixel (relative to the widget) of a 0..1 position.
int posToX(lv_obj_t* obj, float pos01);
float xToPos(lv_obj_t* obj, int x);
bool dragging(lv_obj_t* obj);                                  // a handle drag is in progress

}  // namespace orbit::ui::wave
