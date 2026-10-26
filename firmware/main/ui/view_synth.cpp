// SYNTH view (work area, 0.4.9): the 4-voice SYNTH (core/synth) in the language of the BASS view — its knobs in
// pages, a note line per scene (chords of up to 4 notes per step, each with its own length) and a two-octave
// keyboard to play it live with several fingers.
//   head      "SYNTH / SCENE A", "POLY · 16 STEPS" (the MODE knob); KEYS, MUTE, CLEAR (UNDO for a few seconds),
//             PLAY, and the corner-marks button: the whole screen for the synth, and back
//   roll      the 16 steps of the shown bar (the same bar as SEQUENCE and BASS) over the keyboard's two octaves:
//             a bar per note, as long as the note; the selected step is outlined, the playing step lights up;
//             notes outside the two octaves are marks at the top / bottom edge. Tap or drag = select the step.
//   step row  OCT - [range] OCT +, LEN - n +, VEL, DEL, BAR < n/m >
//   chord kb  the same two octaves: the selected step's notes are lit; a key adds its note (and plays it while
//             held) or takes it out; at most 4 notes a step. LEN and VEL set the step's notes and the next new ones.
//   pages     OSC 1, OSC 2, FILTER, ENV, LFO, VOICE: 8 knobs each (GP_SYN_*); PATCH < name >: factory patches
// KEYS (the head's toggle): the roll, the step row and the chord keyboard make room for a row (OCT - / +, VEL,
// ALL OFF) and a larger two-octave keyboard that only plays the synth live: nothing is written into the line.
// Every finger sounds its key (chords; past 4 notes core/synth takes the oldest voice); a finger that slides onto
// another key moves its note there. As in BASS the touch task plays it (touchInput) and it is the touch task's
// exclusive zone, so knobs and buttons work while keys are held. On the PC the LVGL pointer is one finger.
// The mode, the range, VEL / LEN, the knob page and the patch live outside View: they survive the rebuild on
// maximize.
#include <atomic>
#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "core/synthmap.h"
#include "hal/hal.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui::view_synth {

namespace {

constexpr int kCols = kStepsPerPage;
constexpr int kX = 329, kW = 710;
constexpr int kRollY = 130, kRollH = 134;                       // 130..263
constexpr int kNumY = 130, kPitchY0 = 150, kPitchY1 = 256;      // pitch area: the top key at Y0, the lowest at Y1
constexpr int kRowY = 270;                                      // step row 270..303
constexpr int kPageY = 396, kPageH = 28;                        // knob pages 396..423
constexpr int kKnobD = 46, kKnobY = 432, kKnobs = 8, kKnobSlot = 88, kKnobX = 329 + (710 - kKnobs * kKnobSlot) / 2;
constexpr int kLiveRowY = 134;
constexpr uint32_t kUndoMs = 4000;
constexpr uint32_t kSparkMs = 360;
constexpr int kRange = 24;                                      // the keyboards: C .. C over two octaves (25 keys)
constexpr int kBaseLo = 12, kBaseHi = kSynthNoteMax - kRange;   // the lowest key: C0 .. C7
constexpr int kWhites = 15;
constexpr int kWhiteSemi[kWhites] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24};
constexpr int kBlackSemi[10] = {1, 3, 6, 8, 10, 13, 15, 18, 20, 22};
constexpr int kBlackAfter[10] = {0, 1, 3, 4, 5, 7, 8, 10, 11, 12};   // right of this white key
constexpr int kVels[4] = {40, 70, 100, 127};                    // VEL steps

// The two keyboards: the chord keyboard under the roll and the KEYS keyboard (design px).
struct KbGeo {
    int x, y, w, h;
};
constexpr KbGeo kChordKb = {329, 310, 710, 80};   // 310..389
constexpr KbGeo kLiveKb = {329, 176, 710, 214};   // 176..389

// Knob pages (8 knobs each; captions where the parameter's own name is not enough on the page)
struct PageKnob {
    GlobalParam p;
    const char* cap;   // nullptr: the parameter's name
};
constexpr int kPages = 6;
const char* const kPageNames[kPages] = {"OSC 1", "OSC 2", "FILTER", "ENV", "LFO", "VOICE"};
const PageKnob kPageKnobs[kPages][kKnobs] = {
    {{GP_SYN_OSC1_WAVE, nullptr}, {GP_SYN_OSC1_COLOR, nullptr}, {GP_SYN_OSC1_LEVEL, nullptr},
     {GP_SYN_SUB_OCT, nullptr}, {GP_SYN_SUB_LEVEL, nullptr}, {GP_SYN_NOISE, nullptr}, {GP_SYN_XMOD, nullptr},
     {GP_SYN_TUNE, nullptr}},
    {{GP_SYN_OSC2_WAVE, nullptr}, {GP_SYN_OSC2_COLOR, nullptr}, {GP_SYN_OSC2_LEVEL, nullptr},
     {GP_SYN_OSC2_COARSE, nullptr}, {GP_SYN_OSC2_FINE, nullptr}, {GP_SYN_OSC2_ENV, "ENV MOD"},
     {GP_SYN_SYNC, nullptr}, {GP_SYN_RING, nullptr}},
    {{GP_SYN_FILTER, "TYPE"}, {GP_SYN_CUTOFF, nullptr}, {GP_SYN_RESO, nullptr}, {GP_SYN_ENV_AMT, nullptr},
     {GP_SYN_KEY_FOLLOW, nullptr}, {GP_SYN_VEL_CUTOFF, nullptr}, {GP_SYN_VEL_AMP, nullptr},
     {GP_SYN_DRIVE, nullptr}},
    {{GP_SYN_F_ATTACK, "F ATTACK"}, {GP_SYN_F_DECAY, "F DECAY"}, {GP_SYN_F_SUSTAIN, "F SUSTAIN"},
     {GP_SYN_F_RELEASE, "F RELEASE"}, {GP_SYN_A_ATTACK, "A ATTACK"}, {GP_SYN_A_DECAY, "A DECAY"},
     {GP_SYN_A_SUSTAIN, "A SUSTAIN"}, {GP_SYN_A_RELEASE, "A RELEASE"}},
    {{GP_SYN_LFO_WAVE, "WAVE"}, {GP_SYN_LFO_RATE, nullptr}, {GP_SYN_LFO_SYNC, nullptr}, {GP_SYN_LFO_FADE, nullptr},
     {GP_SYN_LFO_PITCH, nullptr}, {GP_SYN_LFO_CUTOFF, nullptr}, {GP_SYN_LFO_AMP, nullptr},
     {GP_SYN_LFO_PW, nullptr}},
    {{GP_SYN_MODE, nullptr}, {GP_SYN_LEGATO, nullptr}, {GP_SYN_GLIDE, nullptr}, {GP_SYN_DETUNE, nullptr},
     {GP_SYN_SPREAD, nullptr}, {GP_SYN_LFO_KEY, "LFO TRIG"}, {GP_SYN_LEVEL, nullptr}, {GP_SYN_SEND, nullptr}},
};

struct Live {
    bool on = false;    // KEYS mode shown
    int base = 60;      // MIDI note of the lowest key (C4: the demo's chords fit)
    int vel = 100;      // VEL of new notes and of the live keys
    int len = 1;        // LEN of new notes (steps)
    int page = 2;       // knob page (FILTER)
    int preset = -1;    // the factory patch chosen last, -1 none
    char preset_proj[32] = {};   // ... in this project (another project loaded: no patch name)
};
Live s_live;

// KEYS with several fingers: the touch task reads where the keyboard is, the range and VEL, and publishes the notes
// held (a finger keeps its note when the range changes); the LVGL task draws those (refresh polls them).
struct KeysShared {
    std::atomic<bool> en{false};                     // KEYS on screen, no overlay: fingers play it
    std::atomic<int32_t> x1{0}, y1{0}, w{1}, h{1};   // the keyboard on the screen
    std::atomic<int32_t> base{60};
    std::atomic<int32_t> vel{100};
    std::atomic<uint32_t> held[4] = {};              // bit n: a finger holds MIDI note n
    std::atomic<uint32_t> all_off{0};                // ALL OFF taps: the held fingers play no more
};
KeysShared s_kb;

struct View {
    lv_obj_t* root = nullptr;
    int ox = geo::work_x + 1, oy = geo::work_y + 1;
    lv_obj_t* over = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* mode = nullptr;     // KEYS toggle (head)
    lv_obj_t* mute = nullptr;
    lv_obj_t* clear = nullptr;
    lv_obj_t* play = nullptr;
    lv_obj_t* cur = nullptr;      // parent of the next button()
    lv_obj_t* edit = nullptr;     // roll, step row, chord keyboard (hidden in KEYS mode)
    lv_obj_t* live = nullptr;     // KEYS row and keyboard
    // edit layer
    lv_obj_t* roll = nullptr;
    lv_obj_t* chord = nullptr;
    lv_obj_t* range = nullptr;
    lv_obj_t* len = nullptr;
    lv_obj_t* vel = nullptr;
    lv_obj_t* bar = nullptr;
    // live layer
    lv_obj_t* lkb = nullptr;
    lv_obj_t* l_range = nullptr;
    lv_obj_t* l_vel = nullptr;
    // pages + knobs (both modes)
    lv_obj_t* tab[kPages] = {};
    lv_obj_t* patch = nullptr;
    lv_obj_t* knob[kKnobs] = {};
    int sel = 0;                  // selected step (absolute)
    int ph = -1;                  // playhead column drawn (-1 none)
    int key_down = -1;            // chord keyboard: the pressed key 0..24
    int key_note = -1;            // chord keyboard: the note it auditions, -1 none
    uint32_t undo_until = 0;      // CLEAR reads UNDO until then (lv_tick ms)
    int ph_step = -1;             // the scene step playing (-1 stopped)
    int spark_col = -1, spark_end = -1;   // the step that lit up and the last column its notes reach
    uint32_t spark_t0 = 0;
    uint32_t kb_held = 0;         // KEYS: keys drawn as held (s_kb.held)
    uint32_t line_keys = 0;       // KEYS: keys the line plays now (bit k = base + k)
    bool kb_pub = false;          // KEYS: the touch task was told the keyboard plays at kb_area
    lv_area_t kb_area = {0, 0, -1, -1};
    Revs drawn{};
    bool drawn_valid = false;
    uint32_t last_tick_ms = 0;
};
View v;

inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

void place(lv_obj_t* o, int x, int y) { lv_obj_set_pos(o, x - v.ox, y - v.oy); }

void setTextIfChanged(lv_obj_t* l, const char* t)
{
    if (!l || !t) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

const Scene& curScene()
{
    const Project& p = app().project();
    return p.scene_data[p.scene < kScenes ? p.scene : 0];
}
int sceneLen() { return clampi(curScene().length, 1, kMaxSteps); }
int pageCount() { return (sceneLen() + kStepsPerPage - 1) / kStepsPerPage; }
int curPage() { return clampi(app().ui().seq_page, 0, pageCount() - 1); }
bool muted() { return app().paramGet(ParamRef::global(GP_SYN_MUTE)) >= 0.5f; }

int colX(int c) { return kX + c * kW / kCols; }
int colW(int c) { return (c + 1) * kW / kCols - c * kW / kCols - 4; }
// semitone r (0..kRange) above the lowest key -> the centre of its row
float rowY(int r) { return (float)kPitchY1 - (float)r * (float)(kPitchY1 - kPitchY0) / (float)kRange; }

// MIDI note -> "C4", "D#3" (C4 = 60)
void noteName(int midi, char* out, int len)
{
    static const char* const kN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int m = clampi(midi, 0, 127);
    snprintf(out, (size_t)len, "%s%d", kN[m % 12], m / 12 - 1);
}
void rangeText(char* out, int len)
{
    char lo[8], hi[8];
    noteName(s_live.base, lo, sizeof lo);
    noteName(s_live.base + kRange, hi, sizeof hi);
    snprintf(out, (size_t)len, "%s \xE2\x80\x93 %s", lo, hi);
}

int stepNotes(const SynthStep& st)
{
    int n = 0;
    for (const SynthNote& x : st.n) n += x.note ? 1 : 0;
    return n;
}
const SynthNote* firstNote(const SynthStep& st)
{
    for (const SynthNote& x : st.n)
        if (x.note) return &x;
    return nullptr;
}
// LEN / VEL shown: the selected step's (its first note), else the one for new notes
int curLen()
{
    const SynthNote* n = firstNote(app().synthStep(v.sel));
    return n ? n->len : s_live.len;
}
int curVel()
{
    const SynthNote* n = firstNote(app().synthStep(v.sel));
    return n ? n->vel : s_live.vel;
}

// a*w + b*(256-w) per channel (w = 0..256)
uint32_t mixHex(uint32_t a, uint32_t b, int w)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const int ca = (int)((a >> sh) & 0xFFu), cb = (int)((b >> sh) & 0xFFu);
        out |= (uint32_t)((ca * w + cb * (256 - w) + 128) >> 8) << sh;
    }
    return out;
}

void fillRect(lv_layer_t* layer, const lv_area_t& a, uint32_t color, int radius, lv_opa_t opa = LV_OPA_COVER)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void outline(lv_layer_t* layer, const lv_area_t& a, uint32_t color, int width, int radius, lv_opa_t opa = LV_OPA_COVER)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c(color);
    d.border_width = width;
    d.border_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void text(lv_layer_t* layer, const lv_area_t& a, const char* t, const lv_font_t* f, uint32_t color)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;
    d.font = f;
    d.color = c(color);
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(layer, &d, &a);
}

float sparkLevel(int col)
{
    if (col != v.spark_col) return 0.f;
    const uint32_t el = lv_tick_get() - v.spark_t0;
    if (el >= kSparkMs) return 0.f;
    const float t = 1.f - (float)el / (float)kSparkMs;
    return t * t;
}

// ------------------------------------------------------------------------------ the roll
void rollDraw(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const Xf t = xfOf(v.roll, kX, kRollY);   // design px -> screen (maximized: larger, sharp)
    const lv_font_t* f12 = t.font(f_r12);
    const lv_font_t* m12 = t.font(f_m12);
    const int page = curPage(), len = sceneLen();
    const int p0 = page * kStepsPerPage, p1 = p0 + kCols;   // the bar's steps [p0, p1)
    const int sel_col = v.sel / kStepsPerPage == page ? v.sel % kStepsPerPage : -1;
    char buf[8];
    // step numbers and column tracks
    for (int col = 0; col < kCols; ++col) {
        const int s = p0 + col;
        const bool dim = s >= len;
        const int x1 = t.X(colX(col)), x2 = t.X(colX(col) + colW(col)) - 1;
        lv_area_t na = {x1, t.Y(kNumY), x2, t.Y(kNumY + 14)};
        snprintf(buf, sizeof buf, "%d", s + 1);
        if (col == v.ph) {
            fillRect(layer, na, col::accent, t.F(4));
            text(layer, na, buf, m12, col::on_accent);
        } else {
            text(layer, na, buf, f12, dim ? mixHex(col::muted, col::seq_bg, 110) : col::muted);
        }
        lv_area_t tr = {x1, t.Y(kPitchY0 - 5), x2, t.Y(kPitchY1 + 5)};
        fillRect(layer, tr, dim ? mixHex(col::step_off, col::seq_bg, 90) : mixHex(col::step_off, col::seq_bg, 150),
                 t.F(4));
    }
    // the black keys' rows (a darker band across the roll) and the C rows (a line)
    const int rx1 = t.X(kX), rx2 = t.X(kX + kW) - 1;
    const int bh = t.H(4);
    for (int r = 0; r <= kRange; ++r) {
        const int pc = (s_live.base + r) % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        if (!black && pc != 0) continue;
        const int y = t.Y(rowY(r));
        if (black) {
            lv_area_t b = {rx1, y - bh / 2, rx2, y - bh / 2 + bh - 1};
            fillRect(layer, b, col::step_border, 0, 70);
        } else {
            lv_area_t g = {rx1, y, rx2, y + t.F(1) - 1};
            fillRect(layer, g, col::step_border, 0, 150);
        }
    }
    // the notes: every note of the scene that sounds in this bar, as long as it is (cut at the scene end)
    const int nh = t.H(4) < 3 ? 3 : t.H(4);
    const int in2 = t.F(2);
    for (int s = 0; s < len; ++s) {
        const SynthStep& st = app().synthStep(s);
        for (const SynthNote& n : st.n) {
            if (!n.note) continue;
            const int end = s + n.len < len ? s + n.len : len;   // steps [s, end)
            if (end <= p0 || s >= p1) continue;
            const int c0 = (s > p0 ? s : p0) - p0, c1 = (end < p1 ? end : p1) - p0 - 1;
            const bool starts = s >= p0;   // starts in this bar (else it rings in from before)
            const int r = (int)n.note - s_live.base;
            const bool mine = s == v.sel;
            const uint32_t fill = mine ? col::accent_dark : col::accent;
            if (r < 0 || r > kRange) {   // outside the two octaves: a mark at the edge of its first column
                if (!starts) continue;
                const int cx = t.X(colX(c0) + colW(c0) / 2);
                const int y = r < 0 ? t.Y(kPitchY1 + 2) : t.Y(kPitchY0 - 5);
                lv_area_t m = {cx - t.F(5), y, cx + t.F(5), y + t.F(3) - 1};
                fillRect(layer, m, fill, t.F(1), 210);
                continue;
            }
            const int y = t.Y(rowY(r));
            lv_area_t nb = {t.X(colX(c0)) + (starts ? in2 : 0), y - nh / 2, t.X(colX(c1) + colW(c1)) - 1 - in2,
                            y - nh / 2 + nh - 1};
            if (nb.x2 < nb.x1) nb.x2 = nb.x1;
            const float f = starts ? sparkLevel(c0) : 0.f;
            if (f > 0.f) {   // it sounds: a glow, like the drum steps
                lv_area_t o = nb;
                lv_area_increase(&o, t.F(2), t.F(2));
                outline(layer, o, col::accent, t.F(2), t.F(3), (lv_opa_t)(170.f * f + 0.5f));
            }
            fillRect(layer, nb, fill, t.F(2));
            if (f > 0.f) fillRect(layer, nb, 0xFFFFFF, t.F(2), (lv_opa_t)(110.f * f));
        }
    }
    // edit cursor
    if (sel_col >= 0) {
        const int e2 = t.F(2);
        lv_area_t o = {t.X(colX(sel_col)) - e2, t.Y(kPitchY0 - 7), t.X(colX(sel_col) + colW(sel_col)) - 1 + e2,
                       t.Y(kPitchY1 + 7)};
        outline(layer, o, col::ink, t.F(2), t.F(5));
    }
}

int colAt(int x) { return clampi((x - kX) * kCols / kW, 0, kCols - 1); }

void updateTexts();

void setSel(int s)
{
    s = clampi(s, 0, sceneLen() - 1);
    if (s == v.sel) return;
    v.sel = s;
    if (v.roll) lv_obj_invalidate(v.roll);
    if (v.chord) lv_obj_invalidate(v.chord);
    updateTexts();   // LEN / VEL of the new step
}

void rollEvent(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_area_t a;
    lv_obj_get_coords(v.roll, &a);
    const lv_point_t p = pointerIn(v.roll);   // the work panel may be drawn scaled (maximize)
    setSel(curPage() * kStepsPerPage + colAt(p.x - (a.x1 - kX)));
}

// ------------------------------------------------------------------------------ the keyboards
// 15 white keys (C .. C) with 10 black ones; key = semitones above the lowest key (0..24).
int whiteX(const KbGeo& g, int i) { return g.x + i * g.w / kWhites; }
int whiteW(const KbGeo& g, int i) { return (i + 1) * g.w / kWhites - i * g.w / kWhites - 3; }
void blackArea(const KbGeo& g, int k, lv_area_t& a)
{
    const int cx = whiteX(g, kBlackAfter[k] + 1) - 2;   // the gap between the two white keys
    const int hw = g.w / kWhites * 34 / 100;             // about 2/3 of a white key wide
    a.x1 = cx - hw;
    a.x2 = cx + hw;
    a.y1 = g.y;
    a.y2 = g.y + g.h * 60 / 100;
}
// design px (also outside the keyboard while a finger is held) -> key 0..24
int keyAt(const KbGeo& g, int x, int y)
{
    for (int k = 0; k < 10; ++k) {
        lv_area_t a;
        blackArea(g, k, a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return kBlackSemi[k];
    }
    return kWhiteSemi[clampi((x - g.x) * kWhites / g.w, 0, kWhites - 1)];
}

enum class KeyLook : uint8_t { Normal, Lit, On, Down };   // Lit: the line plays it; On: in the step / held

// Draws the keyboard g of obj; look(semi) gives each key's look.
void kbDraw(lv_event_t* e, lv_obj_t* obj, const KbGeo& g, KeyLook (*look)(int semi))
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const Xf t = xfOf(obj, g.x, g.y);
    const lv_font_t* f12 = t.font(f_r12);
    const lv_font_t* m12 = t.font(f_m12);
    const int name_h = g.h >= 120 ? 24 : 20;
    char nm[8];
    for (int i = 0; i < kWhites; ++i) {
        lv_area_t a = {t.X(whiteX(g, i)), t.Y(g.y), t.X(whiteX(g, i) + whiteW(g, i)) - 1, t.Y(g.y + g.h) - 1};
        const int semi = kWhiteSemi[i];
        const KeyLook k = look(semi);
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c(k == KeyLook::On ? col::accent
                                        : (k == KeyLook::Down ? col::hover
                                                              : (k == KeyLook::Lit ? col::active_bg : col::key_white)));
        d.bg_opa = LV_OPA_COVER;
        d.border_color = c(k == KeyLook::On ? col::accent_dark : (k == KeyLook::Lit ? col::accent : col::line));
        d.border_width = t.F(k == KeyLook::Lit ? 2 : 1);
        d.radius = t.F(6);
        lv_draw_rect(layer, &d, &a);
        // note names: every C stands out (orientation), the other keys stay quiet
        const bool is_c = semi % 12 == 0;
        noteName(s_live.base + semi, nm, sizeof nm);
        lv_area_t ta = {a.x1, a.y2 - t.H(name_h), a.x2, a.y2 - t.H(name_h - 16)};
        text(layer, ta, nm, is_c ? m12 : f12,
             k == KeyLook::On ? col::on_accent : (is_c || k != KeyLook::Normal ? col::ink : col::muted));
    }
    for (int i = 0; i < 10; ++i) {
        lv_area_t d;
        blackArea(g, i, d);
        const lv_area_t a = t.area((float)d.x1, (float)d.y1, (float)(d.x2 + 1), (float)(d.y2 + 1));
        const KeyLook k = look(kBlackSemi[i]);
        const uint32_t fill = k == KeyLook::On ? col::accent
                                               : (k == KeyLook::Down ? col::key_black_down
                                                                     : (k == KeyLook::Lit ? mixHex(col::accent, col::key_black, 110)
                                                                                          : col::key_black));
        fillRect(layer, a, fill, t.F(5));
    }
}

// Redraws only key `semi` of the keyboard g of obj (all keys are drawn clipped to it, black keys stay on top).
void kbInvalidateKey(lv_obj_t* obj, const KbGeo& g, int semi)
{
    if (!obj || semi < 0 || semi > kRange) return;
    lv_area_t d = {0, 0, -1, -1};
    for (int k = 0; k < 10; ++k)
        if (kBlackSemi[k] == semi) blackArea(g, k, d);
    for (int i = 0; i < kWhites; ++i)
        if (kWhiteSemi[i] == semi) d = {whiteX(g, i), g.y, whiteX(g, i) + whiteW(g, i) - 1, g.y + g.h - 1};
    if (d.x2 < d.x1) return;
    const Xf t = xfOf(obj, g.x, g.y);
    lv_area_t a = t.area((float)d.x1, (float)d.y1, (float)(d.x2 + 1), (float)(d.y2 + 1));
    lv_area_increase(&a, t.F(2), t.F(2));
    lv_obj_invalidate_area(obj, &a);
}

// ---- the chord keyboard: the selected step's notes
int stepSlotOf(const SynthStep& st, int note)
{
    for (int i = 0; i < kSynthStepNotes; ++i)
        if (st.n[i].note == note) return i;
    return -1;
}

KeyLook chordLook(int semi)
{
    if (stepSlotOf(app().synthStep(v.sel), s_live.base + semi) >= 0) return KeyLook::On;
    return semi == v.key_down ? KeyLook::Down : KeyLook::Normal;
}
void chordDraw(lv_event_t* e) { kbDraw(e, v.chord, kChordKb, chordLook); }

void chordRelease()
{
    if (v.key_note >= 0) app().synthKeyOff(v.key_note, Producer::Ui);
    v.key_note = -1;
    if (v.key_down >= 0) kbInvalidateKey(v.chord, kChordKb, v.key_down);
    v.key_down = -1;
}

void chordEvent(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {   // rebuilt (maximize) under the finger: no RELEASED comes
        if (lv_event_get_target(e) != v.chord) return;
        if (v.key_note >= 0) app().synthKeyOff(v.key_note, Producer::Ui);
        v.key_note = v.key_down = -1;
        v.chord = nullptr;
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        chordRelease();
        lv_area_t o;
        lv_obj_get_coords(v.chord, &o);
        const lv_point_t p = pointerIn(v.chord);
        const int semi = keyAt(kChordKb, p.x - (o.x1 - kChordKb.x), p.y - (o.y1 - kChordKb.y));
        const int note = s_live.base + semi;
        SynthStep st = app().synthStep(v.sel);
        const int slot = stepSlotOf(st, note);
        v.key_down = semi;
        if (slot >= 0) {   // in the step: out it goes (the rest move up: the first note sets LEN / VEL shown)
            for (int i = slot; i + 1 < kSynthStepNotes; ++i) st.n[i] = st.n[i + 1];
            st.n[kSynthStepNotes - 1] = SynthNote{};
            app().setSynthStep(v.sel, st);
        } else {
            const int free = stepSlotOf(st, 0);
            if (free < 0) {
                app().toast("%d NOTES PER STEP", kSynthStepNotes);
            } else {
                st.n[free].note = (uint8_t)note;
                st.n[free].vel = (uint8_t)curVel();
                st.n[free].len = (uint8_t)curLen();
                app().setSynthStep(v.sel, st);
                app().synthKeyOn(note, st.n[free].vel, Producer::Ui);   // heard while the key is held
                v.key_note = note;
            }
        }
        kbInvalidateKey(v.chord, kChordKb, semi);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        chordRelease();
    }
}

// ---- KEYS: the two-octave keyboard
KeyLook liveLook(int semi)
{
    if ((v.kb_held >> semi) & 1u) return KeyLook::On;
    return (v.line_keys >> semi) & 1u ? KeyLook::Lit : KeyLook::Normal;
}
void liveDraw(lv_event_t* e) { kbDraw(e, v.lkb, kLiveKb, liveLook); }
void liveInvalidateKeys(uint32_t keys)
{
    for (int k = 0; k <= kRange; ++k)
        if (keys & (1u << k)) kbInvalidateKey(v.lkb, kLiveKb, k);
}

// ---- fingers [the touch task on the Tab5, the LVGL task on the PC]. A finger plays when it comes down on the
// keyboard and moves its note when it slides onto another key (also off the keyboard: the nearest key). Every
// finger sounds; a note two fingers hold sounds once and ends with the last of them. No LVGL calls here.
struct KeyFinger {
    uint8_t id;
    int8_t key;     // -1: not a key finger (came down elsewhere, or KEYS went away under it)
    uint8_t note;   // the MIDI note it plays (the range may change while it is held)
    bool used;
};
KeyFinger s_kf[5];
uint32_t s_kf_all_off = 0;   // s_kb.all_off seen

bool noteHeld(int note)
{
    for (const KeyFinger& f : s_kf)
        if (f.used && f.key >= 0 && f.note == note) return true;
    return false;
}
void fingerOff(KeyFinger& f)
{
    const int note = f.note;
    f.key = -1;
    if (!noteHeld(note)) app().synthKeyOff(note, Producer::Touch);
}
void fingerOn(KeyFinger& f, int key)
{
    const int note = clampi(s_kb.base.load(std::memory_order_relaxed) + key, kSynthNoteMin, kSynthNoteMax);
    const bool sounding = noteHeld(note);
    f.key = (int8_t)key;
    f.note = (uint8_t)note;
    if (!sounding) app().synthKeyOn(note, s_kb.vel.load(std::memory_order_relaxed), Producer::Touch);
}

void keysTouch(const hal::TouchPoint* pts, int n)
{
    const bool en = s_kb.en.load(std::memory_order_acquire);
    const int32_t x1 = s_kb.x1.load(std::memory_order_relaxed), y1 = s_kb.y1.load(std::memory_order_relaxed);
    const int32_t w = s_kb.w.load(std::memory_order_relaxed), h = s_kb.h.load(std::memory_order_relaxed);
    bool changed = false;
    const uint32_t all_off = s_kb.all_off.load(std::memory_order_acquire);
    if (all_off != s_kf_all_off) {   // ALL OFF ended every note: the held fingers play no more (no offs)
        s_kf_all_off = all_off;
        for (KeyFinger& f : s_kf) f.key = -1;
        changed = true;
    }
    for (KeyFinger& f : s_kf) {   // lifted fingers
        if (!f.used) continue;
        bool present = false;
        for (int k = 0; k < n; ++k) present = present || pts[k].id == f.id;
        if (present) continue;
        if (f.key >= 0) {
            fingerOff(f);
            changed = true;
        }
        f.used = false;
    }
    for (int k = 0; k < n; ++k) {
        KeyFinger* f = nullptr;
        for (KeyFinger& g : s_kf) {
            if (g.used && g.id == pts[k].id) f = &g;
        }
        const int dx = kLiveKb.x + (int)((int64_t)(pts[k].x - x1) * kLiveKb.w / (w > 0 ? w : 1));   // design px
        const int dy = kLiveKb.y + (int)((int64_t)(pts[k].y - y1) * kLiveKb.h / (h > 0 ? h : 1));
        const int key = keyAt(kLiveKb, dx, dy);
        if (!f) {   // a new finger: a key finger when it comes down on the keyboard
            for (KeyFinger& g : s_kf) {
                if (!g.used) {
                    f = &g;
                    break;
                }
            }
            if (!f) continue;   // more fingers than tracked
            f->used = true;
            f->id = pts[k].id;
            f->key = -1;
            if (en && pts[k].x >= x1 && pts[k].x < x1 + w && pts[k].y >= y1 && pts[k].y < y1 + h) {
                fingerOn(*f, key);
                changed = true;
            }
        } else if (f->key >= 0 && !en) {   // KEYS went away under the finger: it plays no more
            fingerOff(*f);
            changed = true;
        } else if (f->key >= 0 && key != f->key) {   // slid onto another key: the note moves there
            fingerOff(*f);
            fingerOn(*f, key);
            changed = true;
        }
    }
    if (!changed) return;
    uint32_t held[4] = {};
    for (const KeyFinger& f : s_kf)
        if (f.used && f.key >= 0) held[f.note >> 5] |= 1u << (f.note & 31);
    for (int i = 0; i < 4; ++i) s_kb.held[i].store(held[i], std::memory_order_release);
}

// The keys of the KEYS range the fingers hold now (bit k = s_live.base + k).
uint32_t heldKeys()
{
    uint32_t held[4];
    for (int i = 0; i < 4; ++i) held[i] = s_kb.held[i].load(std::memory_order_acquire);
    uint32_t keys = 0;
    for (int k = 0; k <= kRange; ++k) {
        const int n = s_live.base + k;
        if (n < 128 && ((held[n >> 5] >> (n & 31)) & 1u)) keys |= 1u << k;
    }
    return keys;
}

// ---- the LVGL side: where the keyboard is and whether fingers may play it (also the touch task's exclusive
// zone: LVGL leaves those fingers alone, so the other fingers work on knobs and buttons)
void kbSet(bool en, const lv_area_t& a)
{
    if (en) {
        s_kb.x1.store(a.x1, std::memory_order_relaxed);
        s_kb.y1.store(a.y1, std::memory_order_relaxed);
        s_kb.w.store(a.x2 - a.x1 + 1, std::memory_order_relaxed);
        s_kb.h.store(a.y2 - a.y1 + 1, std::memory_order_relaxed);
        hal::setTouchExclusive(a.x1, a.y1, a.x2, a.y2);
    } else {
        hal::setTouchExclusive(0, 0, -1, -1);
    }
    s_kb.en.store(en, std::memory_order_release);
}

bool shownNow(lv_obj_t* o)
{
    if (!o || lv_obj_get_screen(o) != lv_screen_active()) return false;
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

void kbPublish()   // refresh(): tells the touch task what changed
{
    lv_area_t a = {0, 0, -1, -1};
    const bool en = s_live.on && shownNow(v.lkb) && !overlayOpen();
    if (en) lv_obj_get_coords(v.lkb, &a);
    if (en == v.kb_pub && (!en || memcmp(&a, &v.kb_area, sizeof a) == 0)) return;
    v.kb_pub = en;
    v.kb_area = a;
    kbSet(en, a);
}

void kbLatches()   // s_live -> the touch task
{
    s_kb.base.store(s_live.base, std::memory_order_relaxed);
    s_kb.vel.store(s_live.vel, std::memory_order_relaxed);
}

void liveEvent(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {   // rebuilt (maximize): the fingers on it stop playing
        if (lv_event_get_target(e) != v.lkb) return;
        v.lkb = nullptr;
        v.kb_pub = false;
        kbSet(false, v.kb_area);
        return;
    }
#ifdef ORBIT_HOST
    // No touch task on the PC: the LVGL pointer is one finger (id 15) of the same logic.
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        keysTouch(nullptr, 0);
        return;
    }
    lv_point_t p = {0, 0};
    if (lv_indev_t* in = lv_indev_active()) lv_indev_get_point(in, &p);
    const hal::TouchPoint tp = {(int16_t)p.x, (int16_t)p.y, 15, 0};
    keysTouch(&tp, 1);
#endif
}

// The notes the line plays at step `step` (started there, still ringing, or ringing over the loop's end from the
// steps after it), as keys of the KEYS range.
uint32_t lineKeysAt(int step)
{
    uint32_t keys = 0;
    const int len = sceneLen();
    for (int s = 0; s < len; ++s) {
        for (const SynthNote& n : app().synthStep(s).n) {
            if (!n.note) continue;
            const bool rings = s <= step ? s + n.len > step : s + n.len - len > step;
            const int r = (int)n.note - s_live.base;
            if (rings && r >= 0 && r <= kRange) keys |= 1u << r;
        }
    }
    return keys;
}

// ------------------------------------------------------------------------------ buttons
void invalidateKeyboards()
{
    if (v.roll) lv_obj_invalidate(v.roll);
    if (v.chord) lv_obj_invalidate(v.chord);
    if (v.lkb) lv_obj_invalidate(v.lkb);
}

void setBase(int base)
{
    base = clampi(base, kBaseLo, kBaseHi);
    if (base == s_live.base) return;
    if (v.key_down >= 0) chordRelease();   // (the chord key under the finger names another note now)
    s_live.base = base;
    v.line_keys = 0;
    kbLatches();
    if (!v.root) return;
    updateTexts();
    invalidateKeyboards();
}
// (fingers may hold keys meanwhile: a held note keeps its pitch, the next key plays in the new range)
void onOct(lv_event_t* e) { setBase(s_live.base + 12 * (int)(intptr_t)lv_event_get_user_data(e)); }

// the selected step's notes get fn (LEN, VEL)
void editNotes(void (*fn)(SynthNote&, int), int x)
{
    SynthStep st = app().synthStep(v.sel);
    if (!stepNotes(st)) return;
    for (SynthNote& n : st.n)
        if (n.note) fn(n, x);
    app().setSynthStep(v.sel, st);
}
void onLen(lv_event_t* e)
{
    const int len = clampi(curLen() + (int)(intptr_t)lv_event_get_user_data(e), 1, kMaxSteps);
    s_live.len = len;
    editNotes([](SynthNote& n, int x) { n.len = (uint8_t)x; }, len);
    updateTexts();
}
void onVel(lv_event_t*)
{
    const int cur = curVel();
    int vel = kVels[0];
    for (int x : kVels) {
        if (x > cur) {
            vel = x;
            break;
        }
    }
    s_live.vel = vel;
    kbLatches();
    editNotes([](SynthNote& n, int x) { n.vel = (uint8_t)x; }, vel);
    updateTexts();
}
void onLiveVel(lv_event_t*)
{
    int vel = kVels[0];
    for (int x : kVels) {
        if (x > s_live.vel) {
            vel = x;
            break;
        }
    }
    s_live.vel = vel;
    kbLatches();
    updateTexts();
}
void onDel(lv_event_t*) { app().setSynthStep(v.sel, SynthStep{}); }
void onBar(lv_event_t* e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int pages = pageCount();
    const int page = (curPage() + d + pages) % pages;
    app().setSeqPage(page);
    setSel(page * kStepsPerPage + v.sel % kStepsPerPage);
}
void onMute(lv_event_t*) { app().paramSet(ParamRef::global(GP_SYN_MUTE), muted() ? 0.f : 1.f); }
void onAllOff(lv_event_t*)
{
    app().synthKeyOff(-1, Producer::Ui);
    s_kb.all_off.fetch_add(1, std::memory_order_release);   // the touch task lets the held fingers go
}
void onClear(lv_event_t*)
{
    if (v.undo_until) {
        v.undo_until = 0;
        btnSetText(v.clear, "CLEAR");
        if (app().canUndoPattern() && app().undoPatternScene() == app().project().scene) app().undoPattern();
        return;
    }
    app().synthClear();
    v.undo_until = lv_tick_get() + kUndoMs;
    if (!v.undo_until) v.undo_until = 1;
    btnSetText(v.clear, "UNDO");
}
void onPlay(lv_event_t*) { app().togglePlay(); }

// The knobs are bound to the page's parameters (their columns are one slot wide: a caption never moves them).
void showPage()
{
    const int pg = clampi(s_live.page, 0, kPages - 1);
    for (int i = 0; i < kPages; ++i) btnSetActive(v.tab[i], i == pg);
    for (int i = 0; i < kKnobs; ++i) {
        const PageKnob& pk = kPageKnobs[pg][i];
        if (v.knob[i] && knobRef(v.knob[i]).id != (uint8_t)pk.p) knobBind(v.knob[i], ParamRef::global(pk.p), pk.cap);
    }
}
void onPage(lv_event_t* e)
{
    const int pg = (int)(intptr_t)lv_event_get_user_data(e);
    if (pg == s_live.page) return;
    s_live.page = pg;
    showPage();
}
void onPatch(lv_event_t* e)
{
    const int n = synthPresetCount();
    if (n <= 0) return;
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int i = s_live.preset < 0 ? (d > 0 ? 0 : n - 1) : (s_live.preset + d + n) % n;
    s_live.preset = i;
    snprintf(s_live.preset_proj, sizeof s_live.preset_proj, "%s", app().project().name);
    app().synthPreset(i);   // the knobs follow on revs.params
    updateTexts();
}

void ensureLayer(bool live);

void showMode()
{
    ensureLayer(s_live.on);   // the other layer may not exist yet
    if (s_live.on) {
        if (v.edit) lv_obj_add_flag(v.edit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v.live, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(v.edit, LV_OBJ_FLAG_HIDDEN);
        if (v.live) lv_obj_add_flag(v.live, LV_OBJ_FLAG_HIDDEN);
    }
}
void onMode(lv_event_t*)
{
    chordRelease();
    s_live.on = !s_live.on;
    v.line_keys = 0;
    showMode();
    kbPublish();   // KEYS off: fingers still on the keyboard stop playing (the touch task, next sample)
    updateTexts();
}

lv_obj_t* button(const char* t, int x, int y, int w, lv_event_cb_t cb, void* user = nullptr, bool primary = false,
                 int h = 34)
{
    lv_obj_t* b = actionBtn(v.cur ? v.cur : v.root, t, primary, w, h);
    place(b, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

// A one-line label centred in a w x h cell at (x, y).
lv_obj_t* cellLabel(lv_obj_t* parent, const char* t, int x, int y, int w, const lv_font_t* f, uint32_t color,
                    int h = 34)
{
    lv_obj_t* l = label(parent, t, f, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    const int lh = lv_font_get_line_height(f);
    lv_obj_set_size(l, w, lh);
    place(l, x, y + (h - lh) / 2);
    return l;
}

// ------------------------------------------------------------------------------ refresh
// The PATCH cell: the factory patch chosen last; "*" once a sound knob differs from it; none in another project.
const char* patchText(char* buf, int len)
{
    if (s_live.preset < 0) return "\xE2\x80\x93";
    if (strncmp(s_live.preset_proj, app().project().name, sizeof s_live.preset_proj) != 0) {
        s_live.preset = -1;
        return "\xE2\x80\x93";
    }
    static float g[kGlobalParamCount];   // (LVGL task only)
    memcpy(g, app().project().g, sizeof g);
    synthApplyPreset(s_live.preset, g);
    const bool same = memcmp(g, app().project().g, sizeof g) == 0;
    snprintf(buf, (size_t)len, "%s%s", synthPresetName(s_live.preset), same ? "" : " *");
    return buf;
}

void updateTexts()
{
    char buf[64], mode[24];
    snprintf(buf, sizeof buf, "SYNTH / SCENE %c", (char)('A' + (app().project().scene % kScenes)));
    setTextIfChanged(v.over, buf);
    app().paramText(ParamRef::global(GP_SYN_MODE), mode, sizeof mode);
    if (s_live.on) snprintf(buf, sizeof buf, "%s \xC2\xB7 LIVE KEYS", mode);
    else snprintf(buf, sizeof buf, "%s \xC2\xB7 %d STEPS", mode, sceneLen());
    setTextIfChanged(v.title, buf);
    btnSetActive(v.mode, s_live.on);
    btnSetActive(v.mute, muted());
    btnSetText(v.play, app().playing() ? "PAUSE \xE2\x85\xA1" : "PLAY \xE2\x96\xB6");
    rangeText(buf, sizeof buf);
    setTextIfChanged(v.range, buf);
    setTextIfChanged(v.l_range, buf);
    // edit layer (built with it)
    if (v.len) {
        snprintf(buf, sizeof buf, "LEN %d", curLen());
        setTextIfChanged(v.len, buf);
    }
    if (v.vel) {
        snprintf(buf, sizeof buf, "VEL %d", curVel());
        btnSetText(v.vel, buf);
    }
    if (v.bar) {
        snprintf(buf, sizeof buf, "BAR %d/%d", curPage() + 1, pageCount());
        btnSetText(v.bar, buf);
    }
    if (v.l_vel) {
        snprintf(buf, sizeof buf, "VEL %d", s_live.vel);
        btnSetText(v.l_vel, buf);
    }
    setTextIfChanged(v.patch, patchText(buf, sizeof buf));
    for (auto* k : v.knob)
        if (k) knobRefresh(k);
}

// Redraws the roll's columns c0..c1: their step numbers (the playhead) or their notes (a spark).
void rollInvalidate(int c0, int c1, bool numbers)
{
    if (!v.roll || s_live.on || c0 < 0) return;
    c1 = clampi(c1, c0, kCols - 1);
    const Xf t = xfOf(v.roll, kX, kRollY);
    lv_area_t a = numbers ? t.area((float)colX(c0), (float)kNumY, (float)(colX(c1) + colW(c1)), (float)(kNumY + 15))
                          : t.area((float)(colX(c0) - 4), (float)(kPitchY0 - 8), (float)(colX(c1) + colW(c1) + 4),
                                   (float)(kPitchY1 + 8));
    lv_obj_invalidate_area(v.roll, &a);
}

void tickPlayhead()
{
    const Telemetry& t = app().engine().telemetry();
    const int step = t.step.load(std::memory_order_relaxed);
    const bool show = t.playing.load(std::memory_order_relaxed) && step >= 0 &&
                      t.scene.load(std::memory_order_relaxed) == (int)app().project().scene;
    const int s = show ? step % sceneLen() : -1;
    const int col = s >= 0 && s / kStepsPerPage == curPage() ? s % kStepsPerPage : -1;
    if (col != v.ph) {
        rollInvalidate(v.ph, v.ph, true);
        rollInvalidate(col, col, true);
        v.ph = col;
    }
    // the line enters a step with notes (not muted): its notes light up, as far as they reach in the bar
    if (s != v.ph_step) {
        v.ph_step = s;
        const SynthStep& st = app().synthStep(s);
        if (col >= 0 && stepNotes(st) && !muted()) {
            if (v.spark_col >= 0) rollInvalidate(v.spark_col, v.spark_end, false);   // the last one goes out
            int reach = 1;
            for (const SynthNote& n : st.n)
                if (n.note && n.len > reach) reach = n.len;
            v.spark_col = col;
            v.spark_end = col + reach - 1;
            v.spark_t0 = lv_tick_get();
        }
    }
    const bool sparking = v.spark_col >= 0 && lv_tick_get() - v.spark_t0 < kSparkMs + 40;
    if (v.spark_col >= 0) rollInvalidate(v.spark_col, v.spark_end, false);
    if (!sparking) v.spark_col = -1;
    // KEYS: the keys of the notes the line plays now (not while muted)
    const uint32_t lk = s_live.on && s >= 0 && !muted() ? lineKeysAt(s) : 0;
    if (lk != v.line_keys) {
        const uint32_t diff = lk ^ v.line_keys;
        v.line_keys = lk;
        liveInvalidateKeys(diff);
    }
}

// A layer over the whole view for one mode's objects (placed like the root's own children).
lv_obj_t* layerObj()
{
    lv_obj_t* o = lv_obj_create(v.root);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, geo::work_w - 2, geo::work_h - 2);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// A custom-drawn input object (the roll, the keyboards).
lv_obj_t* drawObj(lv_obj_t* parent, int x, int y, int w, int h, lv_event_cb_t draw)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    place(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(o, draw, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

// The KEYS row and the two-octave keyboard (into v.live).
void buildLive()
{
    // ---- KEYS: OCT - [C4 – C6] OCT +  VEL  (hint)  ALL OFF
    const int y = kLiveRowY;
    button("OCT -", 329, y, 70, onOct, (void*)(intptr_t)-1);
    v.l_range = cellLabel(v.live, "C4 \xE2\x80\x93 C6", 405, y, 96, f_m16, col::ink);
    button("OCT +", 507, y, 70, onOct, (void*)(intptr_t)1);
    v.l_vel = button("VEL 100", 589, y, 86, onLiveVel);
    cellLabel(v.live, "LIVE \xC2\xB7 NOT RECORDED", 687, y, 180, f_r12, col::muted);
    button("ALL OFF", 1039 - 86, y, 86, onAllOff);
    v.lkb = drawObj(v.live, kLiveKb.x, kLiveKb.y, kLiveKb.w, kLiveKb.h, liveDraw);
    for (lv_event_code_t ev : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                               LV_EVENT_DELETE})
        lv_obj_add_event_cb(v.lkb, liveEvent, ev, nullptr);
}

// Step editing: the roll, the step row, the chord keyboard (into v.edit).
void buildEdit()
{
    // ---- the roll (one custom-drawn object on the sequencer background)
    lv_obj_t* bg = lv_obj_create(v.edit);
    lv_obj_remove_style_all(bg);
    place(bg, kX - 8, kRollY - 5);
    lv_obj_set_size(bg, kW + 16, kRollH + 9);
    lv_obj_set_style_bg_color(bg, c(col::seq_bg), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bg, 8, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    v.roll = drawObj(v.edit, kX, kRollY, kW, kRollH, rollDraw);
    lv_obj_add_event_cb(v.roll, rollEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(v.roll, rollEvent, LV_EVENT_PRESSING, nullptr);

    // ---- step row: OCT - [range] OCT + | LEN - n + | VEL | DEL | BAR < n/m >
    {
        const int y = kRowY;
        button("OCT -", 329, y, 60, onOct, (void*)(intptr_t)-1);
        v.range = cellLabel(v.edit, "C4 \xE2\x80\x93 C6", 391, y, 80, f_m14, col::ink);
        button("OCT +", 473, y, 60, onOct, (void*)(intptr_t)1);
        button("LEN -", 545, y, 56, onLen, (void*)(intptr_t)-1);
        v.len = cellLabel(v.edit, "LEN 1", 603, y, 56, f_m14, col::ink);
        button("LEN +", 661, y, 56, onLen, (void*)(intptr_t)1);
        v.vel = button("VEL 100", 729, y, 80, onVel);
        button("DEL", 821, y, 52, onDel);
        button(">", 1039 - 32, y, 32, onBar, (void*)(intptr_t)1);
        v.bar = button("BAR 1/1", 1039 - 32 - 4 - 70, y, 70, onBar, (void*)(intptr_t)1);
        button("<", 1039 - 32 - 4 - 70 - 4 - 32, y, 32, onBar, (void*)(intptr_t)-1);
    }

    // ---- the chord keyboard
    v.chord = drawObj(v.edit, kChordKb.x, kChordKb.y, kChordKb.w, kChordKb.h, chordDraw);
    for (lv_event_code_t ev : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_DELETE})
        lv_obj_add_event_cb(v.chord, chordEvent, ev, nullptr);
}

// The work panel is rebuilt (maximize / restore): the view goes, maybe for a while (a maximized work area builds
// only the views shown). Its root goes first (LVGL sends DELETE to a parent before its children): the audition and
// the keyboard zone end here, and nothing outside (keys.cpp: setLiveBase) touches the objects any more.
void rootDeleted(lv_event_t* e)
{
    if (lv_event_get_target(e) != v.root) return;
    if (v.key_note >= 0) app().synthKeyOff(v.key_note, Producer::Ui);
    if (v.kb_pub) kbSet(false, v.kb_area);
    v = View{};
}

// Only the shown mode's layer is built (create(), the KEYS toggle), like BASS. Inside a maximized view it is laid
// out at the view's scale.
void ensureLayer(bool live)
{
    lv_obj_t*& layer = live ? v.live : v.edit;
    if (layer || !v.root) return;
    const UiScale s = uiScaleOf(v.root);   // (unit while create() builds in the stand-in panel: ui.cpp)
    {
        UiBuildScope scope(s);
        layer = layerObj();
        v.cur = layer;
        if (live) buildLive();
        else buildEdit();
        v.cur = nullptr;
    }
    uiScaleTree(layer, s);   // no-op at the normal size
    updateTexts();
}

}  // namespace

lv_obj_t* create(lv_obj_t* work)
{
    v = View{};
    v.root = lv_obj_create(work);
    lv_obj_remove_style_all(v.root);
    lv_obj_set_pos(v.root, 0, 0);
    lv_obj_set_size(v.root, geo::work_w - 2, geo::work_h - 2);
    lv_obj_remove_flag(v.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(v.root);
    lv_area_t ra;
    lv_obj_get_coords(v.root, &ra);
    if (ra.x1 > 0 || ra.y1 > 0) {
        v.ox = ra.x1;
        v.oy = ra.y1;
    }

    // ---- head (like BASS): KEYS  MUTE  CLEAR  PLAY (right edge 1039, gap 6)
    v.over = overline(v.root, "SYNTH / SCENE A");
    place(v.over, 329, 84);
    v.title = label(v.root, "POLY \xC2\xB7 16 STEPS", f_m20, col::ink);
    lv_label_set_long_mode(v.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(v.title, 340, lv_font_get_line_height(f_m20));
    place(v.title, 329, 100);
    lv_obj_t* mx = panelMaxButton(v.root, Panel::Work, true);   // the whole screen, and back
    place(mx, 1039 - 26, 87 + (34 - 26) / 2);
    constexpr int kHeadR = 1039 - 26 - 6;   // right edge of the button row
    v.play = button("PLAY \xE2\x96\xB6", kHeadR - 86, 87, 86, onPlay, nullptr, true);
    v.clear = button("CLEAR", kHeadR - 86 - 6 - 74, 87, 74, onClear);
    v.mute = button("MUTE", kHeadR - 86 - 6 - 74 - 6 - 70, 87, 70, onMute);
    v.mode = button("KEYS", kHeadR - 86 - 6 - 74 - 6 - 70 - 6 - 70, 87, 70, onMode);

    // ---- knob pages + PATCH < name > (both modes)
    for (int i = 0; i < kPages; ++i) {
        lv_obj_t* b = tabBtn(v.root, kPageNames[i], 64, kPageH);
        place(b, 329 + i * (64 + 4), kPageY);
        lv_obj_add_event_cb(b, onPage, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        v.tab[i] = b;
    }
    cellLabel(v.root, "PATCH", 745, kPageY, 64, f_m12, col::muted, kPageH);
    button("<", 813, kPageY, 32, onPatch, (void*)(intptr_t)-1, false, kPageH);
    v.patch = cellLabel(v.root, "\xE2\x80\x93", 849, kPageY, 154, f_m14, col::ink, kPageH);
    button(">", 1039 - 32, kPageY, 32, onPatch, (void*)(intptr_t)1, false, kPageH);

    // ---- knobs of the page (each column one slot wide, its parts centred)
    const int pg = clampi(s_live.page, 0, kPages - 1);
    for (int i = 0; i < kKnobs; ++i) {
        const PageKnob& pk = kPageKnobs[pg][i];
        lv_obj_t* k = knobCreate(v.root, KnobLook::Dark, kKnobD, ParamRef::global(pk.p), pk.cap);
        if (!k) continue;
        lv_obj_set_width(k, kKnobSlot);
        place(k, kKnobX + i * kKnobSlot, kKnobY);
        v.knob[i] = k;
    }
    showPage();
    lv_obj_add_event_cb(v.root, rootDeleted, LV_EVENT_DELETE, nullptr);

    v.sel = curPage() * kStepsPerPage;
    v.drawn_valid = false;
    kbLatches();
    showMode();
    refresh();
    return v.root;
}

void refresh()
{
    if (!v.root) return;
    const Revs& r = app().revs();
    const bool all = !v.drawn_valid;
    // The roll and the chord keyboard show the pattern (and the bar / the selected step), not the knobs: a turned
    // knob only updates the knobs and the texts.
    const bool lines = all || r.pattern != v.drawn.pattern || r.view != v.drawn.view ||
                       r.transport != v.drawn.transport || r.project != v.drawn.project;
    if (lines || r.params != v.drawn.params || r.fx != v.drawn.fx) {
        if (v.sel >= sceneLen()) v.sel = 0;
        if (v.sel / kStepsPerPage != curPage()) v.sel = curPage() * kStepsPerPage + v.sel % kStepsPerPage;
        updateTexts();
        if (lines) {
            if (v.roll) lv_obj_invalidate(v.roll);
            if (v.chord) lv_obj_invalidate(v.chord);
        }
        if (all && v.lkb) lv_obj_invalidate(v.lkb);
    }
    v.drawn = r;
    v.drawn_valid = true;
    // KEYS: where the keyboard plays (touch task), then the keys the fingers hold now
    kbPublish();
    const uint32_t held = heldKeys();
    if (held != v.kb_held) {
        const uint32_t diff = held ^ v.kb_held;
        v.kb_held = held;
        liveInvalidateKeys(diff);
    }
    const uint32_t now = lv_tick_get();
    if (v.undo_until && (int32_t)(now - v.undo_until) >= 0) {
        v.undo_until = 0;
        btnSetText(v.clear, "CLEAR");
    }
    if (now - v.last_tick_ms >= 16) {
        v.last_tick_ms = now;
        tickPlayhead();
    }
}

void onShow()
{
    v.drawn_valid = false;
    refresh();
}

void onHide()
{
    chordRelease();   // the chord keyboard's audition
    v.kb_pub = false;
    kbSet(false, v.kb_area);   // fingers on the keyboard stop playing; the zone is the other views' again
}

void touchInput(const hal::TouchPoint* pts, int n) { keysTouch(pts, n); }

bool keysOn() { return s_live.on; }

// The physical keyboard (ui/keys.cpp) plays the synth like KEYS: the same range and VEL.
int liveBase() { return s_live.base; }
int liveVel() { return s_live.vel; }
void setLiveBase(int midi) { setBase(midi); }

}  // namespace orbit::ui::view_synth
