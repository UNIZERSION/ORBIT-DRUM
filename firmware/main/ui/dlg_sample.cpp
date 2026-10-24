// SAMPLE DETAILS dialog (pencil in the SAMPLE view head, "SAMPLE DETAILS" in the browser).
// Prototype: .sample-detail grid "1fr 200px", gap 22, padding 24 inside an 824 px modal
// (reference/shots/modal-sample-details.png). Left: file name, info line, waveform with START/END
// handles, knobs FADE IN / FADE OUT / GAIN / LOOP and the CHOKE / OUTPUT / POLY / LAYERS selects.
// Right: "Actions" — the five template buttons (TRIM, SLICE, NORMALIZE, REVERSE, DUPLICATE) and
// a compact grid with FADE, MONO, TRIM SILENCE, PRINT CRUSH, UNDO and SAVE WAV.
// Everything acts on the selected pad + layer through App; refresh() redraws only what changed.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/kits.h"
#include "core/sampleio.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/wave_view.h"
#include "ui/widgets.h"

namespace orbit::ui::dlg_sample {

namespace {

constexpr int kLeftW = 552, kRightW = 200, kGap = 22, kPad = 24;
constexpr int kWaveH = 180;
constexpr int kKnobD = 46;
constexpr int kSelW = 123, kSelH = 34;

struct Drawn {
    uint32_t selection = 0, pads = 0, params = 0, samples = 0;
    uint32_t info_id = 0xFFFFFFFFu;
    int info_pad = -1, info_layer = -1, info_undo = -1;
    int choke = -1, bus = -1, poly = -1, lmode = -1, undo = -1;
};
Drawn s_d;

bool s_open = false;
lv_obj_t* s_file = nullptr;
lv_obj_t* s_info = nullptr;
lv_obj_t* s_wave = nullptr;
lv_obj_t* s_knob[4] = {};
lv_obj_t* s_choke = nullptr;
lv_obj_t* s_out = nullptr;
lv_obj_t* s_poly = nullptr;
lv_obj_t* s_layers = nullptr;
lv_obj_t* s_undo = nullptr;

const int kPolyValues[4] = {1, 2, 4, 8};

const Pad& curPad() { return app().project().pad[app().ui().pad]; }
const Sample* layerSample() { return app().padSample(app().ui().pad, app().ui().layer); }

void setTextIfChanged(lv_obj_t* l, const char* t)
{
    if (!l || !t) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

lv_obj_t* box(lv_obj_t* parent, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// ---------------------------------------------------------------- info line
void fmtKHz(char* out, size_t len, uint32_t rate)
{
    if (rate % 1000u == 0) snprintf(out, len, "%u kHz", (unsigned)(rate / 1000u));
    else snprintf(out, len, "%.1f kHz", (double)rate / 1000.0);
}

const char* builtinKitName(const Sample* s)
{
    if (!s || !s->embedded) return nullptr;
    for (int k = 0; k < builtinKitCount(); ++k) {
        const BuiltinKit& kit = builtinKit(k);
        for (int i = 0; kit.sounds && i < kit.count; ++i)
            if (kit.sounds[i].pcm == s->data) return kit.name;
    }
    return nullptr;
}

const char* baseName(const char* path)
{
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

void updateInfo()
{
    App& a = app();
    const Sample* s = layerSample();
    const uint32_t id = s ? s->id : 0;
    const int undo = a.canUndo() ? 1 : 0;
    if (id == s_d.info_id && a.ui().pad == s_d.info_pad && a.ui().layer == s_d.info_layer && undo == s_d.info_undo)
        return;
    s_d.info_id = id;
    s_d.info_pad = a.ui().pad;
    s_d.info_layer = a.ui().layer;
    s_d.info_undo = undo;
    char file[kPathLen];
    char info[160];
    if (!s) {
        snprintf(file, sizeof file, "LAYER %d \xE2\x80\x94", a.ui().layer + 1);
        snprintf(info, sizeof info, "Pusta warstwa \xE2\x80\x94 wczytaj sampel z BROWSER");
    } else {
        char khz[16];
        fmtKHz(khz, sizeof khz, s->rate);
        const double secs = s->rate ? (double)s->frames / (double)s->rate : 0.0;
        const char* ch = s->channels > 1 ? "stereo" : "mono";
        const char* edited = undo ? " \xE2\x80\xA2 edytowany" : "";
        if (const char* kit = builtinKitName(s)) {
            snprintf(file, sizeof file, "%s", s->name[0] ? s->name : curPad().name);
            snprintf(info, sizeof info, "wbudowany zestaw: %s \xE2\x80\xA2 %s \xE2\x80\xA2 %.3f s \xE2\x80\xA2 %s%s", kit, khz,
                     secs, ch, edited);
        } else if (s->path[0]) {
            snprintf(file, sizeof file, "%s", baseName(s->path));
            WavInfo wi;
            const int bits = wavProbe(s->path, wi) == WavError::Ok ? (int)wi.bits : 16;
            snprintf(info, sizeof info, "%s \xE2\x80\xA2 WAV \xE2\x80\xA2 %d-bit%s \xE2\x80\xA2 %.3f s \xE2\x80\xA2 %s%s", khz, bits,
                     wi.is_float ? " float" : "", secs, ch, edited);
        } else {
            snprintf(file, sizeof file, "%s", s->name[0] ? s->name : curPad().name);
            snprintf(info, sizeof info, "%s \xE2\x80\xA2 16-bit \xE2\x80\xA2 %.3f s \xE2\x80\xA2 %s \xE2\x80\xA2 edytowany (niezapisany)", khz,
                     secs, ch);
        }
    }
    setTextIfChanged(s_file, file);
    setTextIfChanged(s_info, info);
}

// ---------------------------------------------------------------- actions
enum Action : uint8_t { A_TRIM = 0, A_SLICE, A_NORMALIZE, A_REVERSE, A_DUPLICATE, A_FADE, A_MONO, A_SILENCE,
                        A_CRUSH, A_UNDO, A_SAVE, A_COUNT };

void actionCb(lv_event_t* e)
{
    App& a = app();
    const int act = (int)(intptr_t)lv_event_get_user_data(e);
    switch (act) {
    case A_TRIM: a.applyOp(SampleOp::Trim); break;
    case A_SLICE:
        a.setView(View::Sample);
        a.setEditor(EditorTab::Slice);
        modalClose();
        break;
    case A_NORMALIZE: a.applyOp(SampleOp::Normalize); break;
    case A_REVERSE: a.applyOp(SampleOp::Reverse); break;
    case A_DUPLICATE: a.applyOp(SampleOp::Duplicate); break;   // first empty pad after this one, toast if none
    case A_FADE: a.applyOp(SampleOp::FadeInOut); break;
    case A_MONO: a.applyOp(SampleOp::ToMono); break;
    case A_SILENCE: a.applyOp(SampleOp::TrimSilence); break;
    case A_CRUSH: a.applyOp(SampleOp::PrintCrush); break;
    case A_UNDO:
        if (a.canUndo()) a.applyOp(SampleOp::Undo);
        break;
    case A_SAVE: a.saveSampleWav(); break;   // async: ORBIT/USER/<name>.wav, toast, rescan
    default: break;
    }
}

// ---------------------------------------------------------------- selects
void chokePicked(int i, void*) { app().setChoke(i); }
void outPicked(int i, void*) { app().setBus(i); }
void polyPicked(int i, void*)
{
    if (i >= 0 && i < 4) app().setPoly(kPolyValues[i]);
}
void layersPicked(int i, void*)
{
    if (i >= 0 && i < (int)LayerMode::Count) app().setLayerMode((LayerMode)i);
}

int polyIndex(int poly)
{
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        const int d = kPolyValues[i] - poly, db = kPolyValues[best] - poly;
        if ((d < 0 ? -d : d) < (db < 0 ? -db : db)) best = i;
    }
    return best;
}

lv_obj_t* selectWithCaption(lv_obj_t* parent, int x, int y, const char* caption, const char* const* opts, int n,
                            int sel, void (*cb)(int, void*))
{
    lv_obj_t* cap = overline(parent, caption);
    lv_obj_set_style_text_font(cap, f_s13, 0);   // 13 px caption (1.2x rule): the box has room
    lv_obj_set_style_text_letter_space(cap, ls(13, .13f), 0);
    lv_obj_set_pos(cap, x, y - 1);
    lv_obj_t* s = selectCreate(parent, kSelW, kSelH, opts, n, sel, cb, nullptr);
    lv_obj_set_pos(s, x, y + 17);
    return s;
}

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
    const Sample* s = layerSample();
    if (!s) return;
    const Pad& p = curPad();
    const float cur = which == 0 ? p.p[PP_START] : p.p[PP_END];
    float pos = snapToZeroCrossing(s, cur);
    if (which == 0 && pos > p.p[PP_END] - 0.01f) pos = cur;
    if (which == 1 && pos < p.p[PP_START] + 0.01f) pos = cur;
    a.paramSet(ParamRef::pad(which == 0 ? PP_START : PP_END), pos);
}

void waveTap(float, void*) { app().previewPad(); }

void onClose(void*)
{
    s_open = false;
    s_file = s_info = s_wave = nullptr;
    for (auto& k : s_knob) k = nullptr;
    s_choke = s_out = s_poly = s_layers = s_undo = nullptr;
}

}  // namespace

// ================================================================ public
void open()
{
    App& a = app();
    lv_obj_t* body = modalOpen("SAMPLE DETAILS", 824, 410, 650);
    if (!body) return;
    modalOnClose(onClose, nullptr);
    s_open = true;
    s_d = Drawn{};

    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_style_pad_column(body, kGap, 0);

    const Pad& p = curPad();
    // ---- left column
    lv_obj_t* left = box(body, kLeftW, 356);
    s_file = label(left, "", f_s15, col::ink);
    lv_label_set_long_mode(s_file, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_file, kLeftW);
    lv_obj_set_pos(s_file, 0, 0);
    s_info = label(left, "", f_r14, col::muted);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_info, kLeftW);
    lv_obj_set_pos(s_info, 0, 27);

    s_wave = wave::create(left, kLeftW, kWaveH);
    lv_obj_set_pos(s_wave, 0, 53);
    wave::Callbacks cb;
    cb.region = waveRegion;
    cb.snap = waveSnap;
    cb.tap = waveTap;
    wave::setCallbacks(s_wave, cb);

    // knobs FADE IN / FADE OUT / GAIN / LOOP (dark, 46 px), 70 px slots
    static const PadParam kp[4] = {PP_FADE_IN, PP_FADE_OUT, PP_GAIN, PP_LOOP};
    const int row_y = 247;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* k = knobCreate(left, KnobLook::Dark, kKnobD, ParamRef::pad(kp[i]), nullptr);
        if (!k) continue;
        lv_obj_set_width(k, 70);
        lv_obj_add_flag(k, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_pos(k, i * 70, row_y);
        s_knob[i] = k;
    }

    // selects CHOKE / OUTPUT / POLY / LAYERS (2 x 2)
    static const char* const choke_opts[9] = {"OFF", "1", "2", "3", "4", "5", "6", "7", "8"};
    char bus_names[kBuses][16];
    const char* bus_opts[kBuses];
    for (int b = 0; b < kBuses; ++b) {
        const char* nm = a.project().bus[b].name;
        if (nm[0]) snprintf(bus_names[b], sizeof bus_names[b], "%.8s", nm);
        else snprintf(bus_names[b], sizeof bus_names[b], "BUS %d", b + 1);
        bus_opts[b] = bus_names[b];
    }
    static const char* const poly_opts[4] = {"1", "2", "4", "8"};
    static const char* const layer_opts[(int)LayerMode::Count] = {"Single", "Velocity", "Round Robin", "Random",
                                                                   "Stack"};
    const int sx0 = kLeftW - 2 * kSelW - 10, sx1 = kLeftW - kSelW;
    s_choke = selectWithCaption(left, sx0, row_y, "CHOKE", choke_opts, 9, p.choke, chokePicked);
    s_out = selectWithCaption(left, sx1, row_y, "OUTPUT", bus_opts, kBuses, p.bus, outPicked);
    s_poly = selectWithCaption(left, sx0, row_y + 58, "POLY", poly_opts, 4, polyIndex(p.poly), polyPicked);
    s_layers = selectWithCaption(left, sx1, row_y + 58, "LAYERS", layer_opts, (int)LayerMode::Count,
                                 (int)p.layer_mode, layersPicked);

    // ---- right column
    lv_obj_t* right = box(body, kRightW, 356);
    lv_obj_t* h = label(right, "Actions", f_s15, col::ink);
    lv_obj_set_pos(h, 0, 0);
    static const char* const main_names[5] = {"TRIM", "SLICE", "NORMALIZE", "REVERSE", "DUPLICATE"};
    for (int i = 0; i < 5; ++i) {
        lv_obj_t* b = actionBtn(right, main_names[i], false, kRightW, 33);
        lv_obj_set_pos(b, 0, 26 + i * 42);
        lv_obj_add_event_cb(b, actionCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    static const char* const more_names[6] = {"FADE", "MONO", "TRIM SILENCE", "PRINT CRUSH", "UNDO", "SAVE WAV"};
    static const uint8_t more_act[6] = {A_FADE, A_MONO, A_SILENCE, A_CRUSH, A_UNDO, A_SAVE};
    const int bw = (kRightW - 8) / 2;
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* b = actionBtn(right, more_names[i], false, bw, 32);
        lv_obj_set_pos(b, (i % 2) * (bw + 8), 248 + (i / 2) * 37);
        lv_obj_set_style_pad_hor(b, 2, 0);
        lv_obj_set_style_text_font(b, f_m12, 0);
        lv_obj_set_style_text_letter_space(b, 0, 0);
        lv_obj_add_event_cb(b, actionCb, LV_EVENT_CLICKED, (void*)(intptr_t)more_act[i]);
        if (more_act[i] == A_UNDO) {
            s_undo = b;
            lv_obj_set_style_text_color(b, c(col::disabled_fg), LV_STATE_DISABLED);
            lv_obj_set_style_border_color(b, c(col::disabled_line), LV_STATE_DISABLED);
        }
    }
    refresh();
}

void refresh()
{
    if (!s_open || !s_wave) return;
    App& a = app();
    const Revs& r = a.revs();
    const bool sel_ch = r.selection != s_d.selection;
    const bool smp_ch = sel_ch || r.samples != s_d.samples;
    const bool pads_ch = sel_ch || r.pads != s_d.pads;
    const bool par_ch = pads_ch || smp_ch || r.params != s_d.params;
    s_d.selection = r.selection;
    s_d.samples = r.samples;
    s_d.pads = r.pads;
    s_d.params = r.params;

    const Pad& p = curPad();
    if (smp_ch || pads_ch || par_ch) {
        updateInfo();   // compares the sample id / undo state itself
        wave::setSample(s_wave, layerSample());
    }
    if (par_ch) {
        wave::setRegion(s_wave, p.p[PP_START], p.p[PP_END]);
        for (lv_obj_t* k : s_knob)
            if (k) knobRefresh(k);
    }
    if (pads_ch) {
        if (p.choke != s_d.choke) selectSet(s_choke, s_d.choke = p.choke);
        if (p.bus != s_d.bus) selectSet(s_out, s_d.bus = p.bus);
        const int pi = polyIndex(p.poly);
        if (pi != s_d.poly) selectSet(s_poly, s_d.poly = pi);
        if ((int)p.layer_mode != s_d.lmode) selectSet(s_layers, s_d.lmode = (int)p.layer_mode);
    }
    const int undo = a.canUndo() ? 1 : 0;
    if (undo != s_d.undo && s_undo) {
        s_d.undo = undo;
        if (undo) lv_obj_remove_state(s_undo, LV_STATE_DISABLED);
        else lv_obj_add_state(s_undo, LV_STATE_DISABLED);
    }
    static uint32_t play_ms = 0;   // playhead at ~30 Hz
    if (lv_tick_elaps(play_ms) >= 30) {
        play_ms = lv_tick_get();
        wave::setPlayhead(s_wave, a.engine().telemetry().play_pos.load(std::memory_order_relaxed));
    }
}

}  // namespace orbit::ui::dlg_sample
