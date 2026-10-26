// BASS view (work area, 0.3 — no prototype screen): the ACID BASS, a 303-style monophonic line per scene, in the
// prototype's visual language (overline + title head, action buttons, dark editor knobs).
//   head      "ACID BASS / SCENE A", "303 STYLE · 16 STEPS"; MUTE, RANDOM, CLEAR (UNDO for a few seconds after
//             RANDOM / CLEAR), PLAY, and the corner-marks button: the whole screen for the bass (0.3.2) and back
//   line      the 16 steps of the shown bar (the same bar as SEQUENCE): note bars on a five-octave pitch scale
//             (C0..C5, 0.4.6), glide lines between slid notes, note names, accent / slide marks, the edit cursor and the
//             playhead; a note that sounds lights up like the drum steps. Tap = select the step; press and drag
//             = draw notes (absolute pitch, also across steps).
//   step row  GATE, ACCENT, SLIDE, OCT -, OCT + of the selected step; BAR < n/m > (longer scenes); WAVE SAW / SQR
//   keyboard  one octave + C: sets the selected step's note in its octave (and its gate), plays it, moves on
//   knobs     CUTOFF RESO ENV MOD DECAY ACCENT DRIVE FX SEND LEVEL (GP_BASS_*; 0.4.6: no TUNE, the range grew)
// Layout (screen px): head 84..121, line 134..300, step row 306..340, keyboard 348..424, knobs 432..
// KEYS (0.4.2, the head's toggle): the line, the step row and the step keyboard make room for a row (OCT - / +
// with the range, ACCENT and SLIDE latches, SAW / SQR) and a two-octave keyboard (176..424) that only plays the
// bass live: nothing is written into the line. A held key owns the voice (the line's notes wait, engine.cpp);
// moving the finger to another key slides to it with SLIDE on (303 legato), else starts a new note. The knobs
// stay. The mode and the latches live outside View: they survive the rebuild on maximize.
// 0.4.3: several fingers. On the Tab5 the touch task plays the keyboard (touchInput, like the pads): the newest
// finger's key sounds (mono, last-note priority), lifting it goes back to the key still held before (with
// SLIDE: a glide, a trill between two keys). The keyboard is the touch task's exclusive zone, so LVGL follows
// the other fingers: knobs and buttons work while keys are held. On the PC the LVGL pointer is one finger.
#include <atomic>
#include <stdio.h>
#include <string.h>

#include "hal/hal.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui::view_bass {

namespace {

constexpr int kCols = kStepsPerPage;
constexpr int kLineX = 329, kLineY = 134, kLineW = 710, kLineH = 166;
constexpr int kNumY = 134, kPitchY0 = 152, kPitchY1 = 262;     // pitch area (C0 at the bottom, C5 at the top)
constexpr int kNameY = 266, kMarkY = 284;
constexpr int kKeyX = 329, kKeyY = 348, kKeyW = 710, kKeyH = 76;
constexpr int kKnobD = 46, kKnobY = 432, kKnobs = 8, kKnobSlot = 88, kKnobX = 329 + (710 - kKnobs * kKnobSlot) / 2;
constexpr uint32_t kUndoMs = 4000;
constexpr uint32_t kSparkMs = 360;
constexpr int kNotes = kBassNoteMax - kBassNoteMin;   // 60 semitones span
// KEYS mode
constexpr int kLiveRowY = 134;
constexpr int kLiveX = 329, kLiveY = 176, kLiveW = 710, kLiveH = 248;
constexpr int kLiveWhites = 15;                       // C .. C over two octaves
constexpr int kLiveBaseLo = 12, kLiveBaseHi = 72;     // the lowest key: C0 .. C5 (0.4.6: an octave more each way)
constexpr int kLiveWhiteSemi[kLiveWhites] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24};
constexpr int kLiveBlackSemi[10] = {1, 3, 6, 8, 10, 13, 15, 18, 20, 22};
constexpr int kLiveBlackAfter[10] = {0, 1, 3, 4, 5, 7, 8, 10, 11, 12};   // right of this white key

struct Live {
    bool on = false;            // KEYS mode shown
    int base = kBassRootNote;   // MIDI note of the lowest key (C2)
    bool accent = false;
    bool slide = true;
};
Live s_live;

// KEYS with several fingers: the touch task reads where the keyboard is and the latches, and publishes the keys
// held and the one sounding; the LVGL task draws those (refresh polls them).
struct KeysShared {
    std::atomic<bool> en{false};                          // KEYS on screen, no overlay: fingers play it
    std::atomic<int32_t> x1{0}, y1{0}, w{1}, h{1};        // the keyboard on the screen
    std::atomic<int32_t> base{kBassRootNote};
    std::atomic<bool> accent{false}, slide{true};
    std::atomic<uint32_t> held{0};                        // bit k: a finger holds key k (0..24)
    std::atomic<int32_t> sound{-1};                       // the key sounding, -1 none
};
KeysShared s_kb;

struct View {
    lv_obj_t* root = nullptr;
    int ox = geo::work_x + 1, oy = geo::work_y + 1;
    lv_obj_t* over = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* mute = nullptr;
    lv_obj_t* random = nullptr;
    lv_obj_t* clear = nullptr;
    lv_obj_t* play = nullptr;
    lv_obj_t* line = nullptr;
    lv_obj_t* keys = nullptr;
    lv_obj_t* gate = nullptr;
    lv_obj_t* accent = nullptr;
    lv_obj_t* slide = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* saw = nullptr;
    lv_obj_t* sqr = nullptr;
    lv_obj_t* knob[kKnobs] = {};
    lv_obj_t* cur = nullptr;      // parent of the next button()
    lv_obj_t* edit = nullptr;     // line, step row, step keyboard (hidden in KEYS mode)
    lv_obj_t* live = nullptr;     // KEYS row and keyboard
    lv_obj_t* mode = nullptr;     // KEYS toggle (head)
    lv_obj_t* lkb = nullptr;      // the two-octave keyboard
    lv_obj_t* l_range = nullptr;
    lv_obj_t* l_accent = nullptr;
    lv_obj_t* l_slide = nullptr;
    lv_obj_t* l_saw = nullptr;
    lv_obj_t* l_sqr = nullptr;
    uint32_t kb_held = 0;         // KEYS: keys drawn as held (s_kb.held)
    int kb_sound = -1;            // KEYS: key drawn as sounding (s_kb.sound)
    bool kb_pub = false;          // KEYS: the touch task was told the keyboard plays at kb_area
    lv_area_t kb_area = {0, 0, -1, -1};
    int line_note = -1;           // KEYS: MIDI note of the line's playing step (lit on the keys), -1 none
    int sel = 0;                  // selected step (absolute)
    int ph = -1;                  // playhead column drawn (-1 none)
    int key_down = -1;            // pressed key 0..12
    uint32_t undo_until = 0;      // CLEAR reads UNDO until then (lv_tick ms)
    uint32_t notes_seen = 0;      // Telemetry::bass_notes last seen
    int spark_col = -1;
    uint32_t spark_t0 = 0;
    // line gesture
    bool drawing = false;
    int press_x = 0, press_y = 0, last_col = -1;
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

int colX(int c) { return kLineX + c * kLineW / kCols; }
int colW(int c) { return (c + 1) * kLineW / kCols - c * kLineW / kCols - 4; }
int noteY(int note) { return kPitchY1 - (note - kBassNoteMin) * (kPitchY1 - kPitchY0) / kNotes; }

// MIDI note -> "C2", "D#3" (C4 = 60)
void noteName(int midi, char* out, int len)
{
    static const char* const kN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int m = clampi(midi, 0, 127);   // octave -1..9: at most "C#-1" + NUL
    snprintf(out, (size_t)len, "%s%d", kN[m % 12], m / 12 - 1);
}

// Offset of the root from the constant screen geometry (0 when placed as designed).
void lineOffset(int* dx, int* dy)
{
    lv_area_t a;
    lv_obj_get_coords(v.line, &a);
    *dx = a.x1 - kLineX;
    *dy = a.y1 - kLineY;
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

void lineTo(lv_layer_t* layer, int x1, int y1, int x2, int y2, uint32_t color, int w, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c(color);
    d.width = w;
    d.opa = opa;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

float sparkLevel(int col)
{
    if (col != v.spark_col) return 0.f;
    const uint32_t el = lv_tick_get() - v.spark_t0;
    if (el >= kSparkMs) return 0.f;
    const float t = 1.f - (float)el / (float)kSparkMs;
    return t * t;
}

// ------------------------------------------------------------------------------ the line
void lineDraw(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const Xf t = xfOf(v.line, kLineX, kLineY);   // design px -> screen (maximized: larger, sharp)
    const lv_font_t* f12 = t.font(f_r12);
    const lv_font_t* m12 = t.font(f_m12);
    const int page = curPage(), len = sceneLen();
    const int sel_col = v.sel / kStepsPerPage == page ? v.sel % kStepsPerPage : -1;
    char buf[8];
    // octave guides (C1 .. C4) across the line
    for (int n = kBassNoteMin + 12; n < kBassNoteMax; n += 12) {
        lv_area_t g = {t.X(kLineX), t.Y(noteY(n)), t.X(kLineX + kLineW) - 1, t.Y(noteY(n)) + t.F(1) - 1};
        fillRect(layer, g, col::step_border, 0, 110);
    }
    for (int col = 0; col < kCols; ++col) {
        const int s = page * kStepsPerPage + col;
        const BassStep& b = app().bassStep(s);
        const bool dim = s >= len;
        const int x1 = t.X(colX(col)), x2 = t.X(colX(col) + colW(col)) - 1;
        // step number (the playing step gets an accent pill)
        lv_area_t na = {x1, t.Y(kNumY), x2, t.Y(kNumY + 14)};
        snprintf(buf, sizeof buf, "%d", s + 1);
        if (col == v.ph) {
            fillRect(layer, na, col::accent, t.F(4));
            text(layer, na, buf, m12, col::on_accent);
        } else {
            text(layer, na, buf, f12, dim ? mixHex(col::muted, col::seq_bg, 110) : col::muted);
        }
        // column track
        lv_area_t tr = {x1, t.Y(kPitchY0 - 4), x2, t.Y(kPitchY1 + 4)};
        fillRect(layer, tr, dim ? mixHex(col::step_off, col::seq_bg, 90) : mixHex(col::step_off, col::seq_bg, 150),
                 t.F(4));
        // note bar
        const int ny = t.Y(noteY(b.note));
        const int bh = t.H(b.accent ? 12 : 9);
        const int in3 = t.F(3);
        lv_area_t nb = {x1 + in3, ny - bh / 2, x2 - in3, ny - bh / 2 + bh - 1};
        if (b.gate && !dim) {
            const float f = sparkLevel(col);
            if (f > 0.f) {   // the note sounds: glow + a white-hot core, like the drum steps
                for (int i = 0; i < 3; ++i) {
                    lv_area_t o = nb;
                    lv_area_increase(&o, t.F((float)(i + 1)), t.F((float)(i + 1)));
                    static const uint8_t kGlow[3] = {190, 105, 45};
                    outline(layer, o, col::accent, t.F(1), t.F((float)(4 + i)), (lv_opa_t)((float)kGlow[i] * f + 0.5f));
                }
            }
            fillRect(layer, nb, b.accent ? col::accent_dark : col::accent, t.F(4));
            if (f > 0.f) {
                lv_area_t in = nb;
                lv_area_increase(&in, -t.F(2), -t.F(2));
                if (in.y2 > in.y1) fillRect(layer, in, 0xFFFFFF, t.F(3), (lv_opa_t)(120.f * f));
            }
        } else {
            outline(layer, nb, dim ? mixHex(col::step_border, col::seq_bg, 120) : col::step_border, t.F(1), t.F(4));
        }
        // glide line into the next step's note (SLIDE)
        if (b.gate && b.slide && !dim && col + 1 < kCols) {
            const BassStep& nx = app().bassStep(s + 1);
            if (nx.gate && s + 1 < len) {
                const int nx1 = t.X(colX(col + 1)) + in3, ny2 = t.Y(noteY(nx.note));
                lineTo(layer, x2 - in3, ny, nx1, ny2, col::accent, t.F(3), 200);
            }
        }
        // note name and marks
        const int e2 = t.F(2);
        lv_area_t tn = {x1 - e2, t.Y(kNameY), x2 + e2, t.Y(kNameY + 15)};
        if (b.gate) noteName(kBassRootNote + b.note, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "\xE2\x80\x93");
        text(layer, tn, buf, f12, b.gate && !dim ? col::ink : col::muted);
        const int half = (x2 - x1) / 2;
        lv_area_t ma = {x1, t.Y(kMarkY), x1 + half, t.Y(kMarkY + 13)};
        lv_area_t ms = {x1 + half + 1, t.Y(kMarkY), x2, t.Y(kMarkY + 13)};
        text(layer, ma, "A", m12, b.accent ? col::accent_dark : mixHex(col::muted, col::seq_bg, 70));
        text(layer, ms, "S", m12, b.slide ? col::accent_dark : mixHex(col::muted, col::seq_bg, 70));
        // edit cursor
        if (col == sel_col) {
            lv_area_t o = {x1 - e2, t.Y(kPitchY0 - 6), x2 + e2, t.Y(kMarkY + 15)};
            outline(layer, o, col::ink, t.F(2), t.F(5));
        }
    }
}

int colAt(int x)
{
    const int cx = x - kLineX;
    return clampi(cx * kCols / kLineW, 0, kCols - 1);
}
int noteAt(int y)
{
    const int d = clampi(kPitchY1 - y, 0, kPitchY1 - kPitchY0);
    return clampi(kBassNoteMin + (d * kNotes + (kPitchY1 - kPitchY0) / 2) / (kPitchY1 - kPitchY0), kBassNoteMin,
                  kBassNoteMax);
}

void setSel(int s)
{
    s = clampi(s, 0, sceneLen() - 1);
    if (s == v.sel) return;
    v.sel = s;
    if (v.line) lv_obj_invalidate(v.line);
    if (v.keys) lv_obj_invalidate(v.keys);
}

// draws the note of column col at height y (gate on)
void drawAt(int col, int y)
{
    const int s = curPage() * kStepsPerPage + col;
    if (s >= sceneLen()) return;
    BassStep b = app().bassStep(s);
    const int n = noteAt(y);
    if (b.gate && b.note == n) return;
    b.gate = 1;
    b.note = (int8_t)n;
    app().setBassStep(s, b);
}

void lineEvent(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    int dx, dy;
    lineOffset(&dx, &dy);
    const lv_point_t p0 = pointerIn(v.line);   // the work panel may be drawn scaled (maximize)
    const int x = p0.x - dx, y = p0.y - dy;
    if (code == LV_EVENT_PRESSED) {
        v.drawing = false;
        v.press_x = x;
        v.press_y = y;
        v.last_col = colAt(x);
        setSel(curPage() * kStepsPerPage + v.last_col);
    } else if (code == LV_EVENT_PRESSING) {
        const int ddx = x - v.press_x, ddy = y - v.press_y;
        if (!v.drawing && ddx * ddx + ddy * ddy > 64) v.drawing = true;
        if (!v.drawing) return;
        const int col = colAt(x);
        // every column between the last one and this one gets the note under the finger (fast sweeps)
        const int a = v.last_col < 0 ? col : v.last_col;
        const int step = col >= a ? 1 : -1;
        for (int c2 = a; ; c2 += step) {
            drawAt(c2, y);
            if (c2 == col) break;
        }
        v.last_col = col;
        setSel(curPage() * kStepsPerPage + col);
    }
}

// ------------------------------------------------------------------------------ the keyboard
// 8 white keys (C D E F G A B C') with 5 black ones; key index = semitone 0..12.
constexpr int kWhiteSemi[8] = {0, 2, 4, 5, 7, 9, 11, 12};
constexpr int kBlackSemi[5] = {1, 3, 6, 8, 10};
constexpr int kBlackAfter[5] = {0, 1, 3, 4, 5};   // black key sits after this white key

int whiteX(int i) { return kKeyX + i * kKeyW / 8; }
int whiteW(int i) { return (i + 1) * kKeyW / 8 - i * kKeyW / 8 - 3; }
void blackArea(int k, lv_area_t& a)
{
    const int cx = whiteX(kBlackAfter[k] + 1) - 2;
    a.x1 = cx - 27;
    a.x2 = cx + 27;
    a.y1 = kKeyY;
    a.y2 = kKeyY + kKeyH * 58 / 100;
}

int selOctave()   // octave of the selected step's note (relative to C2)
{
    const int n = app().bassStep(v.sel).note;
    return n >= 0 ? n / 12 : -((-n + 11) / 12);
}

void keysDraw(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const Xf t = xfOf(v.keys, kKeyX, kKeyY);
    const lv_font_t* f12 = t.font(f_r12);
    const BassStep& b = app().bassStep(v.sel);
    const int oct = selOctave();
    const int cur = b.gate ? b.note - oct * 12 : -1;   // key of the selected step's note (0..12), -1 rest
    char nm[8];
    for (int i = 0; i < 8; ++i) {
        lv_area_t a = {t.X(whiteX(i)), t.Y(kKeyY), t.X(whiteX(i) + whiteW(i)) - 1, t.Y(kKeyY + kKeyH) - 1};
        const int semi = kWhiteSemi[i];
        const bool on = semi == cur, down = semi == v.key_down;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c(down ? col::hover : (on ? col::active_bg : col::key_white));
        d.bg_opa = LV_OPA_COVER;
        d.border_color = c(on ? col::accent : col::line);
        d.border_width = t.F(on ? 2 : 1);
        d.radius = t.F(6);
        lv_draw_rect(layer, &d, &a);
        noteName(kBassRootNote + oct * 12 + semi, nm, sizeof nm);
        lv_area_t ta = {a.x1, a.y2 - t.H(22), a.x2, a.y2 - t.H(6)};
        text(layer, ta, nm, f12, on ? col::accent_dark : col::muted);
    }
    for (int k = 0; k < 5; ++k) {
        lv_area_t d;
        blackArea(k, d);
        const lv_area_t a = t.area((float)d.x1, (float)d.y1, (float)(d.x2 + 1), (float)(d.y2 + 1));
        const int semi = kBlackSemi[k];
        const bool on = semi == cur, down = semi == v.key_down;
        fillRect(layer, a, down ? col::key_black_down : (on ? col::accent : col::key_black), t.F(5));
    }
}

int keyAt(int x, int y)
{
    for (int k = 0; k < 5; ++k) {
        lv_area_t a;
        blackArea(k, a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return kBlackSemi[k];
    }
    const int i = clampi((x - kKeyX) * 8 / kKeyW, 0, 7);
    return kWhiteSemi[i];
}

void keysEvent(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {   // rebuilt (maximize) under the finger: no RELEASED comes
        if (lv_event_get_target(e) != v.keys) return;
        if (v.key_down >= 0) app().bassRelease();
        v.key_down = -1;
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        lv_area_t o;
        lv_obj_get_coords(v.keys, &o);
        const lv_point_t p = pointerIn(v.keys);
        const int semi = keyAt(p.x - (o.x1 - kKeyX), p.y - (o.y1 - kKeyY));
        BassStep b = app().bassStep(v.sel);
        const int note = clampi(selOctave() * 12 + semi, kBassNoteMin, kBassNoteMax);
        b.gate = 1;
        b.note = (int8_t)note;
        app().setBassStep(v.sel, b);
        app().bassAudition(kBassRootNote + note, b.accent != 0);
        v.key_down = semi;
        lv_obj_invalidate(v.keys);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (v.key_down < 0) return;
        v.key_down = -1;
        app().bassRelease();
        // step entry: on to the next step (and its bar)
        const int next = (v.sel + 1) % sceneLen();
        setSel(next);
        if (next / kStepsPerPage != curPage()) app().setSeqPage(next / kStepsPerPage);
        lv_obj_invalidate(v.keys);
    }
}

// ------------------------------------------------------------------------------ KEYS: the two-octave keyboard
// 15 white keys (C .. C) with 10 black ones; key = semitones above the lowest key (0..24).
int liveWhiteX(int i) { return kLiveX + i * kLiveW / kLiveWhites; }
int liveWhiteW(int i) { return (i + 1) * kLiveW / kLiveWhites - i * kLiveW / kLiveWhites - 3; }
void liveBlackArea(int k, lv_area_t& a)
{
    const int cx = liveWhiteX(kLiveBlackAfter[k] + 1) - 2;   // the gap between the two white keys
    const int hw = kLiveW / kLiveWhites * 34 / 100;           // about 2/3 of a white key wide
    a.x1 = cx - hw;
    a.x2 = cx + hw;
    a.y1 = kLiveY;
    a.y2 = kLiveY + kLiveH * 60 / 100;
}

void liveDraw(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const Xf t = xfOf(v.lkb, kLiveX, kLiveY);
    const lv_font_t* f12 = t.font(f_r12);
    const lv_font_t* m12 = t.font(f_m12);
    char nm[8];
    for (int i = 0; i < kLiveWhites; ++i) {
        lv_area_t a = {t.X(liveWhiteX(i)), t.Y(kLiveY), t.X(liveWhiteX(i) + liveWhiteW(i)) - 1,
                       t.Y(kLiveY + kLiveH) - 1};
        const int semi = kLiveWhiteSemi[i];
        const bool snd = semi == v.kb_sound;                            // sounding
        const bool held = !snd && ((v.kb_held >> semi) & 1u);          // held, waiting (an older finger)
        const bool lit = !snd && !held && s_live.base + semi == v.line_note;   // the line plays it now
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c(snd ? col::accent : (held ? mixHex(col::accent, col::key_white, 96)
                                                 : (lit ? col::active_bg : col::key_white)));
        d.bg_opa = LV_OPA_COVER;
        d.border_color = c(snd || held ? col::accent_dark : (lit ? col::accent : col::line));
        d.border_width = t.F(lit ? 2 : 1);
        d.radius = t.F(6);
        lv_draw_rect(layer, &d, &a);
        // note names: every C stands out (orientation), the other keys stay quiet
        const bool is_c = semi % 12 == 0;
        noteName(s_live.base + semi, nm, sizeof nm);
        lv_area_t ta = {a.x1, a.y2 - t.H(24), a.x2, a.y2 - t.H(8)};
        text(layer, ta, nm, is_c ? m12 : f12, snd ? col::on_accent : (is_c || held ? col::ink : col::muted));
    }
    for (int k = 0; k < 10; ++k) {
        lv_area_t d;
        liveBlackArea(k, d);
        const lv_area_t a = t.area((float)d.x1, (float)d.y1, (float)(d.x2 + 1), (float)(d.y2 + 1));
        const int semi = kLiveBlackSemi[k];
        const bool snd = semi == v.kb_sound;
        const bool held = !snd && ((v.kb_held >> semi) & 1u);
        const bool lit = !snd && !held && s_live.base + semi == v.line_note;
        const uint32_t fill = snd ? col::accent
                                  : (held ? mixHex(col::accent, col::key_black, 170)
                                          : (lit ? mixHex(col::accent, col::key_black, 110) : col::key_black));
        fillRect(layer, a, fill, t.F(5));
    }
}

// design px (also outside the keyboard while the finger is held) -> key 0..24
int liveKeyAt(int x, int y)
{
    for (int k = 0; k < 10; ++k) {
        lv_area_t a;
        liveBlackArea(k, a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return kLiveBlackSemi[k];
    }
    const int i = clampi((x - kLiveX) * kLiveWhites / kLiveW, 0, kLiveWhites - 1);
    return kLiveWhiteSemi[i];
}

// Redraws only key `semi` (0..24; the keyboard draws all keys clipped to it, black keys stay on top).
void liveInvalidateKey(int semi)
{
    if (!v.lkb || semi < 0 || semi > 24) return;
    lv_area_t d = {0, 0, -1, -1};
    for (int k = 0; k < 10; ++k)
        if (kLiveBlackSemi[k] == semi) liveBlackArea(k, d);
    for (int i = 0; i < kLiveWhites; ++i)
        if (kLiveWhiteSemi[i] == semi)
            d = {liveWhiteX(i), kLiveY, liveWhiteX(i) + liveWhiteW(i) - 1, kLiveY + kLiveH - 1};
    if (d.x2 < d.x1) return;
    const Xf t = xfOf(v.lkb, kLiveX, kLiveY);
    lv_area_t a = t.area((float)d.x1, (float)d.y1, (float)(d.x2 + 1), (float)(d.y2 + 1));
    lv_area_increase(&a, t.F(2), t.F(2));
    lv_obj_invalidate_area(v.lkb, &a);
}
void liveInvalidateNote(int midi) { liveInvalidateKey(midi - s_live.base); }

// ---- fingers [the touch task on the Tab5, the LVGL task on the PC]. A finger plays when it comes down on the
// keyboard and changes its key when it moves (also off the keyboard: the nearest key); the newest finger's key
// sounds. No LVGL calls here.
struct KeyFinger {
    uint8_t id;
    int8_t key;       // -1: not a key finger (came down elsewhere, or KEYS went away under it)
    bool used;
    uint32_t order;   // larger: came down on / moved to its key later
};
KeyFinger s_kf[5];
uint32_t s_kf_order = 0;
int s_kf_sound = -1;   // the key sounding (the finger logic's own copy of s_kb.sound)

void keysTouch(const hal::TouchPoint* pts, int n)
{
    const bool en = s_kb.en.load(std::memory_order_acquire);
    const int32_t x1 = s_kb.x1.load(std::memory_order_relaxed), y1 = s_kb.y1.load(std::memory_order_relaxed);
    const int32_t w = s_kb.w.load(std::memory_order_relaxed), h = s_kb.h.load(std::memory_order_relaxed);
    bool changed = false;
    for (KeyFinger& f : s_kf) {   // lifted fingers
        if (!f.used) continue;
        bool present = false;
        for (int k = 0; k < n; ++k) present = present || pts[k].id == f.id;
        if (present) continue;
        changed = changed || f.key >= 0;
        f.used = false;
    }
    for (int k = 0; k < n; ++k) {
        KeyFinger* f = nullptr;
        for (KeyFinger& g : s_kf) {
            if (g.used && g.id == pts[k].id) f = &g;
        }
        const int dx = kLiveX + (int)((int64_t)(pts[k].x - x1) * kLiveW / (w > 0 ? w : 1));   // design px
        const int dy = kLiveY + (int)((int64_t)(pts[k].y - y1) * kLiveH / (h > 0 ? h : 1));
        const int key = liveKeyAt(dx, dy);
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
                f->key = (int8_t)key;
                f->order = ++s_kf_order;
                changed = true;
            }
        } else if (f->key >= 0 && !en) {   // KEYS went away under the finger: it plays no more
            f->key = -1;
            changed = true;
        } else if (f->key >= 0 && key != f->key) {   // moved on to another key: the newest again
            f->key = (int8_t)key;
            f->order = ++s_kf_order;
            changed = true;
        }
    }
    if (!changed) return;
    // last-note priority: the newest finger's key sounds; lifting it goes back to the key held before
    int want = -1;
    uint32_t newest = 0, held = 0;
    for (const KeyFinger& f : s_kf) {
        if (!f.used || f.key < 0) continue;
        held |= 1u << f.key;
        if (want < 0 || (int32_t)(f.order - newest) > 0) {
            newest = f.order;
            want = f.key;
        }
    }
    if (want != s_kf_sound) {
        // from one key to another: SLIDE glides (303 legato, no new envelope), else a new note
        if (want < 0) app().bassKeyOff(Producer::Touch);
        else
            app().bassKeyOn(s_kb.base.load(std::memory_order_relaxed) + want,
                            s_kb.accent.load(std::memory_order_relaxed),
                            s_kf_sound >= 0 && s_kb.slide.load(std::memory_order_relaxed), Producer::Touch);
        s_kf_sound = want;
    }
    s_kb.held.store(held, std::memory_order_relaxed);
    s_kb.sound.store(want, std::memory_order_release);
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
    s_kb.accent.store(s_live.accent, std::memory_order_relaxed);
    s_kb.slide.store(s_live.slide, std::memory_order_relaxed);
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

// ------------------------------------------------------------------------------ buttons
void editSel(void (*fn)(BassStep&))
{
    BassStep b = app().bassStep(v.sel);
    fn(b);
    app().setBassStep(v.sel, b);
}
void onGate(lv_event_t*) { editSel([](BassStep& b) { b.gate = b.gate ? 0 : 1; }); }
void onAccent(lv_event_t*) { editSel([](BassStep& b) { b.accent = b.accent ? 0 : 1; }); }
void onSlide(lv_event_t*) { editSel([](BassStep& b) { b.slide = b.slide ? 0 : 1; }); }
void onOctDown(lv_event_t*)
{
    editSel([](BassStep& b) { b.note = (int8_t)(b.note - 12 >= kBassNoteMin ? b.note - 12 : b.note); });
}
void onOctUp(lv_event_t*)
{
    editSel([](BassStep& b) { b.note = (int8_t)(b.note + 12 <= kBassNoteMax ? b.note + 12 : b.note); });
}
void onBar(lv_event_t* e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int pages = pageCount();
    const int page = (curPage() + d + pages) % pages;
    app().setSeqPage(page);
    setSel(page * kStepsPerPage + v.sel % kStepsPerPage);
}
void onWave(lv_event_t* e)
{
    app().paramSet(ParamRef::global(GP_BASS_WAVE), (float)(int)(intptr_t)lv_event_get_user_data(e));
}
void onMute(lv_event_t*)
{
    const bool on = app().paramGet(ParamRef::global(GP_BASS_MUTE)) >= 0.5f;
    app().paramSet(ParamRef::global(GP_BASS_MUTE), on ? 0.f : 1.f);
}
void armUndo()
{
    v.undo_until = lv_tick_get() + kUndoMs;
    if (!v.undo_until) v.undo_until = 1;
    btnSetText(v.clear, "UNDO");
}
void onRandom(lv_event_t*)
{
    app().bassRandom();
    armUndo();
}
void onClear(lv_event_t*)
{
    if (v.undo_until) {
        v.undo_until = 0;
        btnSetText(v.clear, "CLEAR");
        if (app().canUndoPattern() && app().undoPatternScene() == app().project().scene) app().undoPattern();
        return;
    }
    app().bassClear();
    armUndo();
}
void onPlay(lv_event_t*) { app().togglePlay(); }

void updateTexts();
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
    if (v.key_down >= 0) {
        v.key_down = -1;
        app().bassRelease();
    }
    s_live.on = !s_live.on;
    v.line_note = -1;
    showMode();
    kbPublish();   // KEYS off: fingers still on the keyboard stop playing (the touch task, next sample)
    updateTexts();
}
// (fingers may hold keys meanwhile: the sounding note keeps its pitch, the next key plays in the new range)
void onLiveOct(lv_event_t* e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int base = clampi(s_live.base + d * 12, kLiveBaseLo, kLiveBaseHi);
    if (base == s_live.base) return;
    s_live.base = base;
    kbLatches();
    updateTexts();
    if (v.lkb) lv_obj_invalidate(v.lkb);
}
void onLiveAccent(lv_event_t*)
{
    s_live.accent = !s_live.accent;
    kbLatches();
    updateTexts();
}
void onLiveSlide(lv_event_t*)
{
    s_live.slide = !s_live.slide;
    kbLatches();
    updateTexts();
}

lv_obj_t* button(const char* t, int x, int y, int w, lv_event_cb_t cb, void* user = nullptr, bool primary = false)
{
    lv_obj_t* b = actionBtn(v.cur ? v.cur : v.root, t, primary, w, 34);
    place(b, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

// ------------------------------------------------------------------------------ refresh
void updateTexts()
{
    char buf[64];
    snprintf(buf, sizeof buf, "ACID BASS / SCENE %c", (char)('A' + (app().project().scene % kScenes)));
    setTextIfChanged(v.over, buf);
    const int len = sceneLen(), pages = pageCount();
    // (the bar of a longer scene: the step row's BAR n/m; the head is narrower since the KEYS toggle)
    if (s_live.on) snprintf(buf, sizeof buf, "303 STYLE \xC2\xB7 LIVE KEYS");
    else snprintf(buf, sizeof buf, "303 STYLE \xC2\xB7 %d STEPS", len);
    setTextIfChanged(v.title, buf);
    snprintf(buf, sizeof buf, "BAR %d/%d", curPage() + 1, pages);
    if (v.bar) btnSetText(v.bar, buf);   // (the step row: built with the edit layer)
    const BassStep& b = app().bassStep(v.sel);
    btnSetActive(v.gate, b.gate != 0);
    btnSetActive(v.accent, b.accent != 0);
    btnSetActive(v.slide, b.slide != 0);
    const bool sq = app().paramGet(ParamRef::global(GP_BASS_WAVE)) >= 0.5f;
    btnSetActive(v.saw, !sq);
    btnSetActive(v.sqr, sq);
    // KEYS
    btnSetActive(v.mode, s_live.on);
    char lo[8], hi[8];
    noteName(s_live.base, lo, sizeof lo);
    noteName(s_live.base + 24, hi, sizeof hi);
    snprintf(buf, sizeof buf, "%s \xE2\x80\x93 %s", lo, hi);
    setTextIfChanged(v.l_range, buf);
    btnSetActive(v.l_accent, s_live.accent);
    btnSetActive(v.l_slide, s_live.slide);
    btnSetActive(v.l_saw, !sq);
    btnSetActive(v.l_sqr, sq);
    btnSetActive(v.mute, app().paramGet(ParamRef::global(GP_BASS_MUTE)) >= 0.5f);
    btnSetText(v.play, app().playing() ? "PAUSE \xE2\x85\xA1" : "PLAY \xE2\x96\xB6");
    for (auto* k : v.knob)
        if (k) knobRefresh(k);
}

void tickPlayhead()
{
    const Telemetry& t = app().engine().telemetry();
    const int step = t.step.load(std::memory_order_relaxed);
    const bool show = t.playing.load(std::memory_order_relaxed) && step >= 0 &&
                      t.scene.load(std::memory_order_relaxed) == (int)app().project().scene;
    int col = -1;
    if (show) {
        const int s = step % sceneLen();
        if (s / kStepsPerPage == curPage()) col = s % kStepsPerPage;
    }
    // a bass note started since the last tick: the playing step lights up
    const uint32_t notes = app().bassNotes();
    if (notes != v.notes_seen) {
        v.notes_seen = notes;
        if (col >= 0) {
            v.spark_col = col;
            v.spark_t0 = lv_tick_get();
        }
    }
    const bool sparking = v.spark_col >= 0 && lv_tick_get() - v.spark_t0 < kSparkMs + 40;
    if (col != v.ph || sparking) {
        v.ph = col;
        if (!s_live.on && v.line) lv_obj_invalidate(v.line);
    }
    if (!sparking) v.spark_col = -1;
    // KEYS: the key of the note the line plays now (not while a held key owns the voice, nor muted)
    int ln = -1;
    if (s_live.on && show && v.kb_sound < 0 && app().paramGet(ParamRef::global(GP_BASS_MUTE)) < 0.5f) {
        const BassStep& b = app().bassStep(step % sceneLen());
        if (b.gate) ln = kBassRootNote + b.note;
    }
    if (ln != v.line_note) {
        liveInvalidateNote(v.line_note);
        v.line_note = ln;
        liveInvalidateNote(ln);
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

// A one-line label centred in a w x 34 cell at (x, y).
lv_obj_t* cellLabel(lv_obj_t* parent, const char* t, int x, int y, int w, const lv_font_t* f, uint32_t color)
{
    lv_obj_t* l = label(parent, t, f, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    const int lh = lv_font_get_line_height(f);
    lv_obj_set_size(l, w, lh);
    place(l, x, y + (34 - lh) / 2);
    return l;
}

// The KEYS row and the two-octave keyboard (into v.live).
void buildLive()
{
    // ---- KEYS: OCT - [C2 – C4] OCT +  ACCENT SLIDE  (hint)  SAW SQR, the two-octave keyboard
    {
        const int y = kLiveRowY;
        button("OCT -", 329, y, 70, onLiveOct, (void*)(intptr_t)-1);
        v.l_range = cellLabel(v.live, "C2 \xE2\x80\x93 C4", 405, y, 96, f_m16, col::ink);
        button("OCT +", 507, y, 70, onLiveOct, (void*)(intptr_t)1);
        v.l_accent = button("ACCENT", 589, y, 86, onLiveAccent);
        v.l_slide = button("SLIDE", 681, y, 76, onLiveSlide);
        cellLabel(v.live, "LIVE \xC2\xB7 NOT RECORDED", 763, y, 164, f_r12, col::muted);
        v.l_sqr = button("SQR", 1039 - 52, y, 52, onWave, (void*)(intptr_t)1);
        v.l_saw = button("SAW", 1039 - 52 - 4 - 52, y, 52, onWave, (void*)(intptr_t)0);
    }
    v.lkb = lv_obj_create(v.live);
    lv_obj_remove_style_all(v.lkb);
    place(v.lkb, kLiveX, kLiveY);
    lv_obj_set_size(v.lkb, kLiveW, kLiveH);
    lv_obj_add_flag(v.lkb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v.lkb, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(v.lkb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v.lkb, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(v.lkb, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v.lkb, liveDraw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(v.lkb, liveEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(v.lkb, liveEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(v.lkb, liveEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(v.lkb, liveEvent, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(v.lkb, liveEvent, LV_EVENT_DELETE, nullptr);
}

// Step editing: the line, the step row, the keyboard (into v.edit).
void buildEdit()
{
    // ---- the line (one custom-drawn object)
    lv_obj_t* bg = lv_obj_create(v.edit);
    lv_obj_remove_style_all(bg);
    place(bg, kLineX - 8, kLineY - 5);
    lv_obj_set_size(bg, kLineW + 16, kLineH + 9);
    lv_obj_set_style_bg_color(bg, c(col::seq_bg), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bg, 8, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    v.line = lv_obj_create(v.edit);
    lv_obj_remove_style_all(v.line);
    place(v.line, kLineX, kLineY);
    lv_obj_set_size(v.line, kLineW, kLineH);
    lv_obj_add_flag(v.line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v.line, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(v.line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v.line, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(v.line, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v.line, lineDraw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(v.line, lineEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(v.line, lineEvent, LV_EVENT_PRESSING, nullptr);

    // ---- step row: GATE ACCENT SLIDE OCT- OCT+ | BAR < n/m > | SAW SQR
    {
        int x = 329;
        const int y = 306;
        v.gate = button("GATE", x, y, 76, onGate);
        x += 76 + 6;
        v.accent = button("ACCENT", x, y, 86, onAccent);
        x += 86 + 6;
        v.slide = button("SLIDE", x, y, 76, onSlide);
        x += 76 + 6;
        button("OCT -", x, y, 70, onOctDown);
        x += 70 + 6;
        button("OCT +", x, y, 70, onOctUp);
        int xr = 1039 - 52;
        v.sqr = button("SQR", xr, y, 52, onWave, (void*)(intptr_t)1);
        xr -= 52 + 4;
        v.saw = button("SAW", xr, y, 52, onWave, (void*)(intptr_t)0);
        xr -= 14 + 36;
        button(">", xr, y, 36, onBar, (void*)(intptr_t)1);
        xr -= 4 + 92;
        v.bar = button("BAR 1/1", xr, y, 92, onBar, (void*)(intptr_t)1);
        xr -= 4 + 36;
        button("<", xr, y, 36, onBar, (void*)(intptr_t)-1);
    }

    // ---- keyboard (one custom-drawn object)
    v.keys = lv_obj_create(v.edit);
    lv_obj_remove_style_all(v.keys);
    place(v.keys, kKeyX, kKeyY);
    lv_obj_set_size(v.keys, kKeyW, kKeyH);
    lv_obj_add_flag(v.keys, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v.keys, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(v.keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v.keys, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v.keys, keysDraw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(v.keys, keysEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(v.keys, keysEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(v.keys, keysEvent, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(v.keys, keysEvent, LV_EVENT_DELETE, nullptr);
}

// The work panel is rebuilt (maximize / restore): the view goes, maybe for a while (a maximized work area builds
// only the views shown). Its root goes first (LVGL sends DELETE to a parent before its children): the audition and
// the keyboard zone end here, and nothing outside (keys.cpp: setLiveBase) touches the objects any more (0.4.9:
// Z / X in another maximized view did).
void rootDeleted(lv_event_t* e)
{
    if (lv_event_get_target(e) != v.root) return;
    if (v.key_down >= 0) app().bassRelease();
    if (v.kb_pub) kbSet(false, v.kb_area);
    v = View{};
}

// Only the shown mode's layer is built (create(), the KEYS toggle): both made the view twice as slow to build
// on the Tab5 (maximize 220 ms instead of 121). Inside a maximized view it is laid out at the view's scale.
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

    // ---- head (like SEQUENCE): KEYS  MUTE  RANDOM  CLEAR  PLAY (right edge 1039, gap 6)
    v.over = overline(v.root, "ACID BASS / SCENE A");
    place(v.over, 329, 84);
    v.title = label(v.root, "303 STYLE \xC2\xB7 16 STEPS", f_m20, col::ink);
    lv_label_set_long_mode(v.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(v.title, 252, lv_font_get_line_height(f_m20));
    place(v.title, 329, 100);
    // corner marks (0.3.2): the bass on the whole screen, and back
    lv_obj_t* mx = panelMaxButton(v.root, Panel::Work, true);
    place(mx, 1039 - 26, 87 + (34 - 26) / 2);
    constexpr int kHeadR = 1039 - 26 - 6;   // right edge of the button row
    v.play = button("PLAY \xE2\x96\xB6", kHeadR - 86, 87, 86, onPlay, nullptr, true);
    v.clear = button("CLEAR", kHeadR - 86 - 6 - 74, 87, 74, onClear);
    v.random = button("RANDOM", kHeadR - 86 - 6 - 74 - 6 - 94, 87, 94, onRandom);
    v.mute = button("MUTE", kHeadR - 86 - 6 - 74 - 6 - 94 - 6 - 70, 87, 70, onMute);
    v.mode = button("KEYS", kHeadR - 86 - 6 - 74 - 6 - 94 - 6 - 70 - 6 - 70, 87, 70, onMode);

    v.cur = nullptr;

    // ---- knobs (both modes)
    static const GlobalParam kP[kKnobs] = {GP_BASS_CUTOFF, GP_BASS_RESO,  GP_BASS_ENVMOD, GP_BASS_DECAY,
                                           GP_BASS_ACCENT, GP_BASS_DRIVE, GP_BASS_SEND,   GP_BASS_LEVEL};
    for (int i = 0; i < kKnobs; ++i) {
        lv_obj_t* k = knobCreate(v.root, KnobLook::Dark, kKnobD, ParamRef::global(kP[i]), nullptr);
        lv_obj_update_layout(k);
        const int w = lv_obj_get_width(k);
        place(k, kKnobX + i * kKnobSlot + (kKnobSlot - w) / 2, kKnobY);
        v.knob[i] = k;
    }

    lv_obj_add_event_cb(v.root, rootDeleted, LV_EVENT_DELETE, nullptr);
    v.sel = curPage() * kStepsPerPage;
    v.notes_seen = app().bassNotes();
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
    // The line and the step keyboard show the pattern (and the bar / the selected step), not the knobs: a turned
    // knob only updates the knobs and buttons (0.4.3: redrawing them on every knob step made the knobs stutter).
    const bool lines = all || r.pattern != v.drawn.pattern || r.view != v.drawn.view ||
                       r.transport != v.drawn.transport || r.project != v.drawn.project;
    if (lines || r.params != v.drawn.params || r.fx != v.drawn.fx) {
        if (v.sel >= sceneLen()) v.sel = 0;
        if (v.sel / kStepsPerPage != curPage()) v.sel = curPage() * kStepsPerPage + v.sel % kStepsPerPage;
        updateTexts();
        if (lines) {
            if (v.line) lv_obj_invalidate(v.line);
            if (v.keys) lv_obj_invalidate(v.keys);
        }
        if (all && v.lkb) lv_obj_invalidate(v.lkb);
    }
    v.drawn = r;
    v.drawn_valid = true;
    // KEYS: where the keyboard plays (touch task), then the keys the fingers hold now
    kbPublish();
    const uint32_t held = s_kb.held.load(std::memory_order_relaxed);
    const int snd = s_kb.sound.load(std::memory_order_acquire);
    if (held != v.kb_held || snd != v.kb_sound) {
        const uint32_t diff = held ^ v.kb_held;
        for (int k = 0; k <= 24; ++k) {
            if (diff & (1u << k)) liveInvalidateKey(k);
        }
        if (snd != v.kb_sound) {
            liveInvalidateKey(v.kb_sound);
            liveInvalidateKey(snd);
        }
        v.kb_held = held;
        v.kb_sound = snd;
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
    if (v.key_down >= 0) {   // the step keyboard's audition
        v.key_down = -1;
        app().bassRelease();
    }
    v.kb_pub = false;
    kbSet(false, v.kb_area);   // fingers on the keyboard stop playing; the zone is the other views' again
}

void touchInput(const hal::TouchPoint* pts, int n) { keysTouch(pts, n); }

bool keysOn() { return s_live.on; }

// The physical keyboard (ui/keys.cpp) plays the bass like KEYS: the same range, accent and slide.
int liveBase() { return s_live.base; }
bool liveAccent() { return s_live.accent; }
bool liveSlide() { return s_live.slide; }
void setLiveBase(int midi)
{
    const int base = clampi(midi, kLiveBaseLo, kLiveBaseHi);
    if (base == s_live.base) return;
    s_live.base = base;
    kbLatches();
    if (!v.root) return;
    updateTexts();
    if (v.lkb) lv_obj_invalidate(v.lkb);
}

}  // namespace orbit::ui::view_bass
