// SAMPLE view (editor) in the 746x486 work panel, laid out like the prototype's .l-work sample
// editor (reference/geometry.json, states view-sample and editor-*):
//   head: sample name of the selected layer + pencil (SAMPLE DETAILS), LAYER 1..4
//   waveform (wave_view) with START/END handles, slices, envelope overlay and live playhead
//   tabs TRIM / SLICE / ENVELOPE / FILTER / PITCH / LFO
//   params row: three tools + ONE SHOT ▾ + "<TAB> · EDIT MODE" | 4 knobs | 2 knobs (SLICE: AUTO ▾,
//   GRID ▾, TO PADS)
// Everything is created once; a tab change re-binds the same six knob objects. refresh() compares
// the App revision counters with the values it last drew and touches only what changed.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/sampleio.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/wave_view.h"
#include "ui/widgets.h"

namespace orbit::ui::view_sample {

namespace {

// The work panel content area starts at (312, 74) on the stage (panel 311,73 + 1 px border).
constexpr int kOX = 312, kOY = 74;
constexpr int X(float stage_x) { return (int)(stage_x + 0.5f) - kOX; }
constexpr int Y(float stage_y) { return (int)(stage_y + 0.5f) - kOY; }

constexpr int kTabs = (int)EditorTab::Count;
const char* const kTabNames[kTabs] = {"TRIM", "SLICE", "ENVELOPE", "FILTER", "PITCH", "LFO"};
const float kTabX[kTabs] = {324.f, 445.f, 565.5f, 686.5f, 807.5f, 928.f};
// Knob parameters per tab: 4 (main group) | 2 (right group; SLICE shows buttons instead).
const PadParam kTabParams[kTabs][6] = {
    /* TRIM     */ {PP_VOLUME, PP_ATTACK, PP_DECAY, PP_RELEASE, PP_CUTOFF, PP_RESO},
    /* SLICE    */ {PP_TUNE, PP_START, PP_END, PP_GAIN, PP_CUTOFF, PP_RESO},
    /* ENVELOPE */ {PP_ATTACK, PP_HOLD, PP_DECAY, PP_SUSTAIN, PP_RELEASE, PP_VEL_AMP},
    /* FILTER   */ {PP_CUTOFF, PP_RESO, PP_DRIVE, PP_FTYPE, PP_FENV, PP_FDECAY},
    /* PITCH    */ {PP_TUNE, PP_FINE, PP_PENV, PP_PDECAY, PP_BITS, PP_DOWNSAMPLE},
    /* LFO      */ {PP_LFO_RATE, PP_LFO_DEPTH, PP_LFO_SHAPE, PP_LFO_TARGET, PP_LFO_SYNC, PP_LFO_FADE},
};
const float kKnobX[6] = {550.f, 624.f, 698.f, 772.f, 892.f, 971.f};   // knob bodies (stage x)
constexpr float kKnobY = 418.f;
constexpr int kKnobD = 53;

const uint32_t &kLayerBorder = col::layer_border, &kLayerBg = col::layer_bg, &kLayerOn = col::layer_on,
               &kLayerLoaded = col::layer_loaded, &kLayerEmpty = col::layer_empty;
const uint32_t& kDivider = col::divider;
constexpr uint32_t kPlayheadMs = 30;   // ~30 Hz playhead updates

struct Drawn {
    uint32_t view = 0, selection = 0, pads = 0, params = 0, samples = 0;
    int tab = -1;
    int layer_state[kLayers] = {-1, -1, -1, -1};
};
Drawn s_d;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_name = nullptr;
lv_obj_t* s_layer_row = nullptr;
lv_obj_t* s_layer_btn[kLayers] = {};
lv_obj_t* s_layer_dot[kLayers] = {};
lv_obj_t* s_layer_txt[kLayers] = {};
lv_obj_t* s_wave = nullptr;
// TRIM's first knob is the pad's own volume (0.3: the macro panel's VOLUME knob is the device output level).
const char* tabCaption(int t, int i) { return t == (int)EditorTab::Trim && i == 0 ? "PAD VOL" : nullptr; }

lv_obj_t* s_tab[kTabs] = {};
lv_obj_t* s_tool[3] = {};
lv_obj_t* s_tool_icon[3] = {};
lv_obj_t* s_mode = nullptr;
lv_obj_t* s_overline = nullptr;
lv_obj_t* s_knob[6] = {};
lv_obj_t* s_slice_col = nullptr;
lv_obj_t* s_auto = nullptr;
lv_obj_t* s_grid = nullptr;
lv_obj_t* s_topads = nullptr;
lv_obj_t* s_menu_anchor = nullptr;   // invisible, wider anchor for the LAYER menu
uint32_t s_play_ms = 0;
// TO PADS over pads that hold samples: armed by the first tap (source pad, time); afterwards the
// button reads UNDO TO PADS for kUndoMs (App::undoPads).
constexpr uint32_t kConfirmMs = 3000, kUndoMs = 8000;
int s_topads_armed_pad = -1;
uint32_t s_topads_armed_ms = 0;
uint32_t s_topads_undo_until = 0;   // lv_tick while TO PADS reads UNDO (0: not)
lv_style_t s_layer_style;
bool s_styles = false;

const Pad& curPad() { return app().project().pad[app().ui().pad]; }

// Sample of the selected layer, or (for operations that need one) the first loaded layer.
const Sample* layerSample() { return app().padSample(app().ui().pad, app().ui().layer); }
const Sample* anySample()
{
    const Sample* s = layerSample();
    return s ? s : app().padSample(app().ui().pad, -1);
}

void setTextIfChanged(lv_obj_t* l, const char* t)
{
    if (!l || !t) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

void setHidden(lv_obj_t* o, bool hidden)
{
    if (!o) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* box(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// Dark 53 px knob whose BODY lands on the prototype position; the wrap has a fixed width so a
// re-bind with another caption never moves it.
lv_obj_t* knobAt(lv_obj_t* parent, const ParamRef& ref, float stage_x, int slot_w, const char* caption = nullptr)
{
    lv_obj_t* k = knobCreate(parent, KnobLook::Dark, kKnobD, ref, caption);
    if (!k) return nullptr;
    lv_obj_set_width(k, slot_w);
    lv_obj_add_flag(k, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_update_layout(k);
    lv_obj_t* body = lv_obj_get_child(k, 1);
    const int bx = body ? lv_obj_get_x(body) : (slot_w - kKnobD) / 2;
    const int by = body ? lv_obj_get_y(body) : 21;
    lv_obj_set_pos(k, X(stage_x) - bx, Y(kKnobY) - by);
    return k;
}

// ---------------------------------------------------------------- popups
void layerMenuPicked(int index, void* user)
{
    const int layer = (int)(intptr_t)user;
    App& a = app();
    switch (index) {
    case 0:   // LOAD FROM BROWSER: the browser loads into the selected pad + layer
        a.selectLayer(layer);
        a.setView(View::Browser);
        break;
    case 1: a.clearPadLayer(a.ui().pad, layer); break;
    default:
        if (index >= 2 && index < 2 + (int)LayerMode::Count) a.setLayerMode((LayerMode)(index - 2));
        break;
    }
}

void openLayerMenu(lv_obj_t* btn, int layer)
{
    static const char* const items[] = {"LOAD FROM BROWSER", "CLEAR LAYER", "SINGLE", "VELOCITY",
                                        "ROUND ROBIN", "RANDOM", "STACK"};
    // The menu takes the anchor's width (min 150 px): anchor it to an invisible 210 px box whose
    // right edge is the button's, so "LOAD FROM BROWSER" fits on one line.
    lv_obj_t* anchor = btn;
    if (s_menu_anchor && s_root) {
        lv_area_t b, r;
        lv_obj_get_coords(btn, &b);
        lv_obj_get_coords(s_root, &r);
        lv_obj_set_pos(s_menu_anchor, b.x2 + 1 - 210 - r.x1, b.y1 - r.y1);
        lv_obj_update_layout(s_menu_anchor);
        anchor = s_menu_anchor;
    }
    popupMenu(anchor, items, 7, 2 + (int)curPad().layer_mode, layerMenuPicked, (void*)(intptr_t)layer);
}

void modeMenuPicked(int index, void*)
{
    App& a = app();
    if (index >= 0 && index < (int)PlayMode::Count) a.setPlayMode((PlayMode)index);
    else if (index == 3) a.setReverse(!curPad().reverse);
}

void autoMenuPicked(int index, void*)
{
    static const float sens[3] = {0.3f, 0.55f, 0.8f};
    if (index < 0 || index > 2) return;
    app().setSliceSens(sens[index]);
    app().sliceAuto();
}

void gridMenuPicked(int index, void*)
{
    static const int counts[4] = {2, 4, 8, 16};
    if (index >= 0 && index < 4) app().sliceGrid(counts[index]);
}

// ---------------------------------------------------------------- events
void layerCb(lv_event_t* e)
{
    const int layer = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_current_target(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SHORT_CLICKED: app().selectLayer(layer); break;
    case LV_EVENT_LONG_PRESSED:
        app().selectLayer(layer);
        openLayerMenu(btn, layer);
        break;
    default: break;
    }
}

void tabCb(lv_event_t* e)
{
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    app().setEditor((EditorTab)t);
}

void toolCb(lv_event_t* e)
{
    static const EditorTab tabs[3] = {EditorTab::Trim, EditorTab::Envelope, EditorTab::Slice};
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < 3) app().setEditor(tabs[i]);
}

void modeCb(lv_event_t* e)
{
    const Pad& p = curPad();
    char rev[24];
    snprintf(rev, sizeof rev, "%sREVERSE", p.reverse ? "\xE2\x97\x8F " : "");
    const char* items[4] = {"ONE SHOT", "GATE", "LOOP", rev};
    popupMenu((lv_obj_t*)lv_event_get_current_target(e), items, 4, (int)p.mode, modeMenuPicked, nullptr);
}

void autoCb(lv_event_t* e)
{
    static const char* const items[] = {"LOW", "MED", "HIGH"};
    const float s = app().sliceSens();
    const int sel = s < 0.425f ? 0 : (s < 0.675f ? 1 : 2);
    popupMenu((lv_obj_t*)lv_event_get_current_target(e), items, 3, sel, autoMenuPicked, nullptr);
}

void gridCb(lv_event_t* e)
{
    static const char* const items[] = {"2", "4", "8", "16"};
    const int n = curPad().slice_count;
    const int sel = n == 2 ? 0 : n == 4 ? 1 : n == 8 ? 2 : n == 16 ? 3 : -1;
    popupMenu((lv_obj_t*)lv_event_get_current_target(e), items, 4, sel, gridMenuPicked, nullptr);
}

bool toPadsUndoShown()
{
    return s_topads_undo_until && app().canUndoPads() && strcmp(app().undoPadsLabel(), "TO PADS") == 0 &&
           (int32_t)(lv_tick_get() - s_topads_undo_until) < 0;
}

void toPadsLabel()
{
    if (s_topads) btnSetText(s_topads, toPadsUndoShown() ? "UNDO TO PADS" : "TO PADS");
}

// TO PADS copies slice 2..n onto the pads after the selected one. Overwriting pads that hold samples
// needs a second tap within kConfirmMs (the first names them); afterwards UNDO TO PADS for kUndoMs.
void toPadsCb(lv_event_t*)
{
    App& a = app();
    if (toPadsUndoShown()) {
        s_topads_undo_until = 0;
        a.undoPads();   // the App says what came back
        toPadsLabel();
        return;
    }
    const int src = a.ui().pad;
    const int n = curPad().slice_count;
    int busy = 0, first = -1, last = -1;
    for (int i = 1; i < n && src + i < kPads; ++i)
        if (a.padSample(src + i, -1)) {
            ++busy;
            if (first < 0) first = src + i;
            last = src + i;
        }
    const bool confirmed = s_topads_armed_pad == src && lv_tick_elaps(s_topads_armed_ms) < kConfirmMs;
    if (busy > 0 && !confirmed) {
        s_topads_armed_pad = src;
        s_topads_armed_ms = lv_tick_get();
        const int m10 = busy % 10, m100 = busy % 100;
        const char* word = busy == 1 ? "pad" : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? "pads" : "pads";
        if (busy == 1)
            a.toast("TO PADS overwrites pad %c%d \xE2\x80\x94 tap again", 'A' + first / 4, first % 4 + 1);
        else
            a.toast("TO PADS overwrites %d %s (%c%d\xE2\x80\x93%c%d) \xE2\x80\x94 tap again", busy, word,
                    'A' + first / 4, first % 4 + 1, 'A' + last / 4, last % 4 + 1);
        return;
    }
    s_topads_armed_pad = -1;
    a.slicesToPads();   // no slices: the App explains
    if (a.canUndoPads() && strcmp(a.undoPadsLabel(), "TO PADS") == 0) {
        s_topads_undo_until = lv_tick_get() + kUndoMs;
        if (!s_topads_undo_until) s_topads_undo_until = 1;
    }
    toPadsLabel();
}

void penCb(lv_event_t*) { dlg_sample::open(); }

// ---------------------------------------------------------------- waveform interaction
void waveRegion(float start, float end, bool, void*)
{
    App& a = app();
    const Pad& p = curPad();
    if (start != p.p[PP_START]) a.paramSet(ParamRef::pad(PP_START), start);
    if (end != p.p[PP_END]) a.paramSet(ParamRef::pad(PP_END), end);
}

void waveSnap(int which, void*)
{
    App& a = app();
    const Sample* s = anySample();
    if (!s) return;
    const Pad& p = curPad();
    const float cur = which == 0 ? p.p[PP_START] : p.p[PP_END];
    float pos = snapToZeroCrossing(s, cur);
    if (which == 0 && pos > p.p[PP_END] - 0.01f) pos = cur;
    if (which == 1 && pos < p.p[PP_START] + 0.01f) pos = cur;
    a.paramSet(ParamRef::pad(which == 0 ? PP_START : PP_END), pos);
    a.toast("%s: snapped to a zero crossing" "", which == 0 ? "START" : "END");
}

// Slice index under a 0..1 position (-1 outside the sliced range).
int sliceAt(const Pad& p, float pos)
{
    const int n = p.slice_count;
    if (n <= 0) return -1;
    float last_end = 1.0f;   // Pad::slice_end (GRID 16 has no room after the markers), else the stored end
    if (p.slice_end > p.slices[n - 1]) last_end = p.slice_end;
    else if (n < kMaxSlices && p.slices[n] > p.slices[n - 1]) last_end = p.slices[n];
    if (pos < p.slices[0] || pos > last_end) return -1;
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (p.slices[i] <= pos) idx = i;
    return idx;
}

void waveTap(float pos, void*)
{
    App& a = app();
    if (a.ui().editor == EditorTab::Slice) {
        const int i = sliceAt(curPad(), pos);
        if (i >= 0) {
            a.sliceSelect(i);
            a.previewPad();   // hear the slice that was picked
            return;
        }
    }
    a.previewPad();
}

// ---------------------------------------------------------------- building
lv_obj_t* layerButton(lv_obj_t* parent, int i)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_layer_style, 0);
    lv_obj_add_style(b, &styles().pressed, LV_STATE_PRESSED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    setHit(b, 6);
    rippleAttach(b);
    lv_obj_t* dot = box(b);   // the ● before the active layer: a 6 px accent dot
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, c(col::accent), 0);
    lv_obj_t* t = label(b, "", f_m12, kLayerEmpty);
    char txt[24];
    snprintf(txt, sizeof txt, "LAYER %d", i + 1);
    lv_label_set_text(t, txt);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(b, layerCb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    lv_obj_add_event_cb(b, layerCb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
    s_layer_dot[i] = dot;
    s_layer_txt[i] = t;
    return b;
}

void initStyles()
{
    if (s_styles) return;
    s_styles = true;
    // .l-layers button: padding 7px 11px, 1 px #c4c7c8, radius 5, #e6e8ea, 10 px -> 12 px
    lv_style_init(&s_layer_style);
    lv_style_set_bg_opa(&s_layer_style, LV_OPA_COVER);
    lv_style_set_bg_color(&s_layer_style, c(kLayerBg));
    lv_style_set_border_width(&s_layer_style, 1);
    lv_style_set_border_color(&s_layer_style, c(kLayerBorder));
    lv_style_set_radius(&s_layer_style, 5);
    lv_style_set_pad_hor(&s_layer_style, 11);
    lv_style_set_pad_ver(&s_layer_style, 0);
}

lv_obj_t* divider(lv_obj_t* parent, float stage_x)
{
    lv_obj_t* d = box(parent);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, c(kDivider), 0);
    lv_obj_set_pos(d, X(stage_x), Y(397.f));
    lv_obj_set_size(d, 1, 114);
    return d;
}

void setToolOn(int i, bool on)
{
    lv_obj_t* b = s_tool[i];
    if (!b) return;
    lv_obj_set_style_border_color(b, c(on ? col::accent : col::line), 0);
    lv_obj_set_style_image_recolor(s_tool_icon[i], c(on ? col::accent : col::ink), 0);
}

// ---------------------------------------------------------------- refresh parts
void applyTab(int t)
{
    if (t < 0 || t >= kTabs || t == s_d.tab) return;
    s_d.tab = t;
    for (int i = 0; i < kTabs; ++i) btnSetActive(s_tab[i], i == t);
    setToolOn(0, t == (int)EditorTab::Trim);
    setToolOn(1, t == (int)EditorTab::Envelope);
    setToolOn(2, t == (int)EditorTab::Slice);
    char ov[40];
    snprintf(ov, sizeof ov, "%s \xC2\xB7 EDIT MODE", kTabNames[t]);
    setTextIfChanged(s_overline, ov);
    const bool slice = t == (int)EditorTab::Slice;
    for (int i = 0; i < 6; ++i) {
        if (i >= 4 && slice) continue;
        knobBind(s_knob[i], ParamRef::pad(kTabParams[t][i]), tabCaption(t, i));
    }
    setHidden(s_knob[4], slice);
    setHidden(s_knob[5], slice);
    setHidden(s_slice_col, !slice);
    wave::setOverlay(s_wave, t == (int)EditorTab::Envelope ? wave::Overlay::Envelope : wave::Overlay::None);
}

void updateHead()
{
    App& a = app();
    const UiState& u = a.ui();
    const Pad& p = a.project().pad[u.pad];
    const Sample* s = a.padSample(u.pad, u.layer);
    const char* name = "\xE2\x80\x94";   // —
    if (s) name = s->name[0] ? s->name : (p.name[0] ? p.name : "\xE2\x80\x94");
    setTextIfChanged(s_name, name);
    for (int l = 0; l < kLayers; ++l) {
        const bool active = l == u.layer;
        const bool loaded = a.padSample(u.pad, l) != nullptr;
        const int st = active ? 2 : (loaded ? 1 : 0);
        if (st == s_d.layer_state[l]) continue;
        s_d.layer_state[l] = st;
        setHidden(s_layer_dot[l], !active);
        lv_obj_set_style_text_color(s_layer_txt[l], c(active ? kLayerOn : (loaded ? kLayerLoaded : kLayerEmpty)), 0);
    }
    const char* mode = p.mode == PlayMode::Gate ? "GATE" : (p.mode == PlayMode::Loop ? "LOOP" : "ONE SHOT");
    char t[40];
    snprintf(t, sizeof t, "%s%s \xE2\x96\xBE", mode, p.reverse ? " \xC2\xB7 REV" : "");   // ▾
    btnSetText(s_mode, t);
}

void updateParams()
{
    App& a = app();
    const UiState& u = a.ui();
    const Pad& p = a.project().pad[u.pad];
    for (int i = 0; i < 6; ++i)
        if (s_knob[i] && !lv_obj_has_flag(s_knob[i], LV_OBJ_FLAG_HIDDEN)) knobRefresh(s_knob[i]);
    wave::setRegion(s_wave, p.p[PP_START], p.p[PP_END]);
    if (u.editor == EditorTab::Slice) wave::setSlices(s_wave, p.slices, p.slice_count, p.slice_count ? p.slice_sel : -1);
    else wave::setSlices(s_wave, nullptr, 0, -1);
    wave::setEnvelope(s_wave, p.p[PP_ATTACK], p.p[PP_HOLD], p.p[PP_DECAY], p.p[PP_SUSTAIN], p.p[PP_RELEASE]);
}

}  // namespace

// ================================================================ public
lv_obj_t* create(lv_obj_t* work)
{
    initStyles();
    s_d = Drawn{};
    lv_obj_t* root = box(work);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    s_root = root;

    // ---- head: name + pencil (flex row, gap 13), LAYER 1..4 (right aligned)
    lv_obj_t* nrow = box(root);
    lv_obj_set_size(nrow, LV_SIZE_CONTENT, 30);
    lv_obj_set_pos(nrow, X(330.f), Y(82.f));
    lv_obj_set_flex_flow(nrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nrow, 13, 0);
    s_name = label(nrow, "\xE2\x80\x94", f_m24, col::ink);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(s_name, 380, 0);
    lv_obj_t* pen = iconBtn(nrow, Icon::Pencil, 16, 30);
    lv_obj_add_event_cb(pen, penCb, LV_EVENT_CLICKED, nullptr);

    s_layer_row = box(root);
    lv_obj_set_size(s_layer_row, LV_SIZE_CONTENT, 30);
    lv_obj_set_flex_flow(s_layer_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_layer_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_layer_row, 7, 0);
    for (int i = 0; i < kLayers; ++i) s_layer_btn[i] = layerButton(s_layer_row, i);
    // right edge at stage x 1038 (content width 744)
    lv_obj_align(s_layer_row, LV_ALIGN_TOP_RIGHT, -(744 - X(1038.f)), Y(82.f));
    s_menu_anchor = box(root);
    lv_obj_set_size(s_menu_anchor, 210, 30);
    lv_obj_add_flag(s_menu_anchor, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // ---- waveform
    s_wave = wave::create(root, 720, 213);
    lv_obj_set_pos(s_wave, X(324.f), Y(120.f));
    wave::Callbacks cb;
    cb.region = waveRegion;
    cb.snap = waveSnap;
    cb.tap = waveTap;
    cb.user = nullptr;
    wave::setCallbacks(s_wave, cb);

    // ---- tabs
    for (int i = 0; i < kTabs; ++i) {
        lv_obj_t* t = tabBtn(root, kTabNames[i], 116, 32);
        lv_obj_set_pos(t, X(kTabX[i]), Y(341.f));
        lv_obj_add_event_cb(t, tabCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_tab[i] = t;
    }

    // ---- left mini actions
    static const Icon tools[3] = {Icon::Wave, Icon::Envelope, Icon::Slice};
    static const float tool_x[3] = {347.5f, 391.5f, 435.5f};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* b = actionBtn(root, "", false, 39, 38);
        lv_obj_set_pos(b, X(tool_x[i]), Y(397.f));
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_t* ic = iconCreate(b, tools[i], 18, col::ink);
        lv_obj_center(ic);
        lv_obj_add_event_cb(b, toolCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_tool[i] = b;
        s_tool_icon[i] = ic;
    }
    s_mode = actionBtn(root, "ONE SHOT \xE2\x96\xBE", false, 135, 37);
    lv_obj_set_pos(s_mode, X(343.5f), Y(443.f));
    lv_obj_set_style_bg_color(s_mode, c(col::dark_btn), 0);
    lv_obj_set_style_border_color(s_mode, c(col::dark_btn), 0);
    lv_obj_set_style_text_color(s_mode, c(col::dark_btn_fg), 0);
    lv_obj_set_style_pad_hor(s_mode, 4, 0);
    lv_obj_add_event_cb(s_mode, modeCb, LV_EVENT_CLICKED, nullptr);
    s_overline = overline(root, "TRIM \xC2\xB7 EDIT MODE");
    lv_obj_set_width(s_overline, 170);
    lv_obj_set_style_text_align(s_overline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_overline, X(411.f) - 85, Y(491.f));

    divider(root, 516.5f);
    divider(root, 858.f);

    // ---- knobs (4 | 2)
    for (int i = 0; i < 6; ++i)
        s_knob[i] = knobAt(root, ParamRef::pad(kTabParams[0][i]), kKnobX[i], 92, tabCaption(0, i));   // wider than the 74 px pitch: RESONANCE / DOWNSAMPLE fit (wraps are not clickable)

    // ---- SLICE button column (replaces the 2-knob group)
    s_slice_col = box(root);
    lv_obj_set_pos(s_slice_col, X(892.f), Y(398.f));
    lv_obj_set_size(s_slice_col, 138, 112);
    s_auto = actionBtn(s_slice_col, "AUTO \xE2\x96\xBE", false, 138, 32);
    lv_obj_set_pos(s_auto, 0, 0);
    lv_obj_add_event_cb(s_auto, autoCb, LV_EVENT_CLICKED, nullptr);
    s_grid = actionBtn(s_slice_col, "GRID \xE2\x96\xBE", false, 138, 32);
    lv_obj_set_pos(s_grid, 0, 40);
    lv_obj_add_event_cb(s_grid, gridCb, LV_EVENT_CLICKED, nullptr);
    s_topads = actionBtn(s_slice_col, "TO PADS", false, 138, 32);
    lv_obj_set_pos(s_topads, 0, 80);
    lv_obj_add_event_cb(s_topads, toPadsCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_slice_col, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(
        root,
        [](lv_event_t*) {
            for (auto& p : s_layer_btn) p = nullptr;
            for (auto& p : s_layer_dot) p = nullptr;
            for (auto& p : s_layer_txt) p = nullptr;
            for (auto& p : s_tab) p = nullptr;
            for (auto& p : s_tool) p = nullptr;
            for (auto& p : s_tool_icon) p = nullptr;
            for (auto& p : s_knob) p = nullptr;
            s_root = s_name = s_layer_row = s_wave = s_mode = s_overline = nullptr;
            s_slice_col = s_auto = s_grid = s_topads = s_menu_anchor = nullptr;
        },
        LV_EVENT_DELETE, nullptr);

    refresh();
    return root;
}

void refresh()
{
    if (!s_root) return;
    App& a = app();
    const Revs& r = a.revs();
    if (s_topads_undo_until && !toPadsUndoShown()) {   // expired, undone elsewhere or replaced by another undo
        s_topads_undo_until = 0;
        toPadsLabel();
    }
    const bool view_ch = r.view != s_d.view;
    const bool head_ch = view_ch || r.selection != s_d.selection || r.samples != s_d.samples || r.pads != s_d.pads;
    const bool par_ch = head_ch || r.params != s_d.params;
    if (view_ch) applyTab((int)a.ui().editor);
    if (head_ch) {
        updateHead();
        wave::setSample(s_wave, layerSample());
    }
    if (par_ch) updateParams();
    s_d.view = r.view;
    s_d.selection = r.selection;
    s_d.samples = r.samples;
    s_d.pads = r.pads;
    s_d.params = r.params;

    // playhead from the engine (newest voice of the watched pad), ~30 Hz
    if (lv_tick_elaps(s_play_ms) >= kPlayheadMs) {
        s_play_ms = lv_tick_get();
        wave::setPlayhead(s_wave, a.engine().telemetry().play_pos.load(std::memory_order_relaxed));
    }
}

void onShow()
{
    // Changes made while hidden were not drawn: force a full comparison.
    const int tab = s_d.tab;
    s_d = Drawn{};
    s_d.tab = tab;
    refresh();
}

}  // namespace orbit::ui::view_sample
