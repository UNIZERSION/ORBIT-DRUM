// SEQUENCE view (work area): pattern editor of the current scene.
//   head: "PATTERN EDITOR / SCENE A", "DRUM MACHINE · 16 STEPS" (+ " · BAR n/m" when the scene is
//         longer than one bar; in a value lane: "VELOCITY · KICK"), FILL (hold), RANDOM, CLEAR
//         (-> UNDO for 8 s after any pattern edit the App can undo: RANDOM, CLEAR, CLEAR TRACK,
//         timeline COPY), PLAY ▶ / PAUSE Ⅱ.
//   grid: bank toggle A1–B4 | C1–D4 (top-left corner), 8 row labels (tap: select pad, plays it when
//         Settings > Pad audition is on, never recorded; long press: CLEAR TRACK / LENGTH / MUTE menu),
//         the step grid (step_grid.*: one custom-drawn object);
//         in a value lane the label column names the edited track (tap: list of pads, no sound).
//   footer cards: PATTERN SETTINGS [1/16 ▾] [SWING 54%] [16 STEPS] [BAR n/m ▾ when > 16 steps: list of bars],
//         SELECTED TRACK [pad] [VELOCITY] [PROBABILITY] [MORE ▾ -> RATCHET / NUDGE / PITCH / COND].
// Geometry follows reference/geometry.json (view-sequence); positions below are screen px.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/step_grid.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui::view_sequence {

namespace {

// ------------------------------------------------------------------ optional App extras
// canUndoPattern()/undoPattern() come with the App extras; patternUndoGen() (counts the App's
// pattern snapshots: RANDOM, CLEAR, CLEAR TRACK, COPY) and undoPatternScene() (the scene the undo
// restores) are requested additions. All are detected at compile time so this file builds
// against an app.h with or without them.
template <class A>
auto undoAvailable(A& a, int) -> decltype(a.canUndoPattern(), bool())
{
    return a.canUndoPattern();
}
template <class A>
bool undoAvailable(A&, long)
{
    return false;
}
template <class A>
auto undoRun(A& a, int) -> decltype(a.undoPattern(), bool())
{
    a.undoPattern();
    return true;
}
template <class A>
bool undoRun(A&, long)
{
    return false;
}
template <class A>
auto undoGenOf(A& a, int) -> decltype((int64_t)a.patternUndoGen())
{
    return (int64_t)a.patternUndoGen();
}
template <class A>
int64_t undoGenOf(A&, long)
{
    return -1;   // unknown: fall back to watching canUndoPattern() turn true
}
template <class A>
auto undoSceneOf(A& a, int) -> decltype((int)a.undoPatternScene())
{
    return (int)a.undoPatternScene();
}
template <class A>
int undoSceneOf(A&, long)
{
    return -1;
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr int kUndoMs = 8000;   // CLEAR reads UNDO this long after a destructive pattern edit
constexpr int kSwingPx = 6;   // vertical drag px per 1 % swing

const char* const kLaneNames[(int)StepLane::Count] = {"TRIG", "VELOCITY", "PROBABILITY", "RATCHET",
                                                      "NUDGE", "PITCH", "COND"};
const char* const kMoreItems[4] = {"RATCHET", "NUDGE", "PITCH", "COND"};
const StepLane kMoreLanes[4] = {StepLane::Ratchet, StepLane::Nudge, StepLane::Pitch, StepLane::Cond};
const char* const kDivItems[(int)Division::Count] = {"1/8", "1/16", "1/32", "1/8T", "1/16T"};
const int kSwingVals[7] = {50, 54, 58, 62, 66, 71, 75};
const char* const kSwingItems[7] = {"50%", "54%", "58%", "62%", "66%", "71%", "75%"};
const char* const kStepsItems[4] = {"16 STEPS", "32 STEPS", "48 STEPS", "64 STEPS"};
const int kLenVals[12] = {0, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32, 64};
const char* const kLenItems[12] = {"FOLLOW", "3", "4", "5", "6", "7", "8", "12", "16", "24", "32", "64"};

struct RowUi {
    lv_obj_t* box;
    lv_obj_t* dot;
    lv_obj_t* name;
    int8_t sel;     // drawn state (-1 unknown)
    int8_t muted;
    char txt[kNameLen + 8];   // text drawn (labels in DOTS mode change their own text)
};

struct View {
    lv_obj_t* root = nullptr;
    int ox = geo::work_x + 1, oy = geo::work_y + 1;   // screen position of root (0,0)
    lv_obj_t* over = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* fill = nullptr;
    lv_obj_t* random = nullptr;
    lv_obj_t* clear = nullptr;
    lv_obj_t* play = nullptr;
    lv_obj_t* bank[2] = {};
    RowUi row[step_grid::kRows] = {};
    lv_obj_t* lane_box = nullptr;
    lv_obj_t* lane_dot = nullptr;
    lv_obj_t* lane_pad = nullptr;
    lv_obj_t* lane_name = nullptr;
    lv_obj_t* lane_zero = nullptr;
    lv_obj_t* grid = nullptr;
    lv_obj_t* div = nullptr;
    lv_obj_t* swing = nullptr;
    lv_obj_t* steps = nullptr;
    lv_obj_t* bar = nullptr;   // BAR n/m ▾ (scenes longer than one bar)
    lv_obj_t* track = nullptr;
    lv_obj_t* vel = nullptr;
    lv_obj_t* prob = nullptr;
    lv_obj_t* more = nullptr;
    // texts drawn last (compared instead of lv_label_get_text: DOTS labels rewrite their text)
    char over_txt[96] = "";
    char title_txt[96] = "";
    char lane_pad_txt[kNameLen + 8] = "";
    char track_txt[kNameLen + 8] = "";
    // last drawn revisions
    Revs drawn{};
    bool drawn_valid = false;
    int8_t footer_compact = -1;   // PATTERN SETTINGS layout drawn (-1 unknown)
    uint32_t undo_until = 0;   // lv_tick ms while CLEAR shows UNDO (0: not)
    int64_t undo_gen = -1;     // App's pattern snapshot counter seen last (-1: the App has none)
    bool undo_can = false;     // canUndoPattern() seen last
    uint32_t last_ph_ms = 0;
    // swing drag
    int32_t swing_acc = 0;
    int swing_start = 50;
    bool swing_moved = false;
};
View v;

const Scene& curScene()
{
    const Project& p = app().project();
    return p.scene_data[p.scene < kScenes ? p.scene : 0];
}
char sceneLetter() { return (char)('A' + clampi(app().project().scene, 0, kScenes - 1)); }

// Pad name in uppercase (ASCII only; Polish letters stay as they are).
void padName(int pad, char* out, int cap)
{
    const char* src = app().project().pad[clampi(pad, 0, kPads - 1)].name;
    int i = 0;
    for (; src[i] && i < cap - 1; ++i) {
        const char ch = src[i];
        out[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    out[i] = 0;
    if (!out[0]) snprintf(out, (size_t)cap, "PAD %c%d", 'A' + pad / 4, pad % 4 + 1);
}

void place(lv_obj_t* o, int x, int y) { lv_obj_set_pos(o, x - v.ox, y - v.oy); }

// Card captions with room for it: 13 px (the 1.2x scale rule) instead of the 11 px overline.
lv_obj_t* caption13(lv_obj_t* o)
{
    lv_obj_set_style_text_font(o, f_s13, 0);
    lv_obj_set_style_text_letter_space(o, ls(13, .13f), 0);
    return o;
}

// Sets a label's text only when it differs from what was drawn last (cache = that text).
void setCached(lv_obj_t* l, char* cache, int cap, const char* text)
{
    if (!l || strcmp(cache, text) == 0) return;
    snprintf(cache, (size_t)cap, "%s", text);
    lv_label_set_text(l, text);
}

lv_obj_t* box(int x, int y, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t* o = lv_obj_create(v.root);
    lv_obj_remove_style_all(o);
    place(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c(bg), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c(col::line), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// Footer button: action-btn look with 12 px text (the footer cards are too narrow for 13 px).
lv_obj_t* footBtn(const char* text, int x, int w, bool primary = false)
{
    lv_obj_t* b = actionBtn(v.root, text, primary, w, 34);
    place(b, x, 507);
    lv_obj_set_style_text_font(b, f_m12, 0);
    lv_obj_set_style_text_letter_space(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 5, 0);
    return b;
}

// ------------------------------------------------------------------ pattern undo (CLEAR <-> UNDO)
// Every destructive pattern edit the App can undo (RANDOM, CLEAR, CLEAR TRACK, timeline COPY)
// turns CLEAR into UNDO for kUndoMs. The App's snapshot counter (or, without it, canUndoPattern()
// becoming true) tells refresh() that a new snapshot exists; the view's own actions arm it at once.
void clearToClear()
{
    v.undo_until = 0;
    btnSetText(v.clear, "CLEAR");
}

// "UNDO", or "UNDO C" when the undo restores a scene other than the one shown.
void undoLabel()
{
    const int sc = undoSceneOf(app(), 0);
    char t[16];
    if (sc >= 0 && sc < kScenes && sc != (int)app().project().scene) snprintf(t, sizeof t, "UNDO %c", 'A' + sc);
    else snprintf(t, sizeof t, "UNDO");
    btnSetText(v.clear, t);
}

void armUndo()
{
    if (!undoAvailable(app(), 0)) return;
    undoLabel();
    v.undo_until = lv_tick_get() + kUndoMs;
    if (!v.undo_until) v.undo_until = 1;
}

// Reads the App's undo state; arm: show UNDO when a new snapshot appeared since the last read.
void syncUndo(bool arm)
{
    const bool can = undoAvailable(app(), 0);
    const int64_t gen = undoGenOf(app(), 0);
    const bool fresh = can && (gen >= 0 ? gen != v.undo_gen : !v.undo_can);
    v.undo_gen = gen;
    v.undo_can = can;
    if (!can) {
        if (v.undo_until) clearToClear();   // undone elsewhere, or the project changed
        return;
    }
    if (fresh && arm) armUndo();
}

// ------------------------------------------------------------------ callbacks
void onPlay(lv_event_t*) { app().togglePlay(); }

void onRandom(lv_event_t*)
{
    app().randomScene();
    if (!undoAvailable(app(), 0)) return;   // the App's own toast stays
    armUndo();
    syncUndo(false);
    char msg[96];
    snprintf(msg, sizeof msg, "Random scene %c — tap UNDO to take it back", sceneLetter());
    toast(msg);
}

void onClear(lv_event_t*)
{
    if (v.undo_until) {   // the button currently reads UNDO: never clears
        clearToClear();
        if (undoAvailable(app(), 0)) undoRun(app(), 0);   // the App names the restored scene
        else toast("No pattern change to undo");
        syncUndo(false);
        return;
    }
    char msg[96];
    app().clearScene();
    if (undoAvailable(app(), 0)) {
        snprintf(msg, sizeof msg, "Cleared scene %c — tap UNDO to take it back", sceneLetter());
        armUndo();
        syncUndo(false);
    } else {
        snprintf(msg, sizeof msg, "Cleared scene %c", sceneLetter());
    }
    toast(msg);
}

void onFill(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) app().setFill(true);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) app().setFill(false);
}

void onBank(lv_event_t* e) { app().setSeqBank((int)(intptr_t)lv_event_get_user_data(e)); }

// BAR n/m ▾: one 34 px footer button with the list of bars (four 22 px tabs did not fit a finger).
void barPicked(int index, void*)
{
    if (index >= 0 && index < kMaxSteps / kStepsPerPage) app().setSeqPage(index);
}
void onBar(lv_event_t*)
{
    static const char* const kItems[kMaxSteps / kStepsPerPage] = {"BAR 1", "BAR 2", "BAR 3", "BAR 4"};
    const int len = clampi(curScene().length, 1, kMaxSteps);
    const int pages = (len + kStepsPerPage - 1) / kStepsPerPage;
    popupMenu(v.bar, kItems, pages, clampi(app().ui().seq_page, 0, pages - 1), barPicked, nullptr);
}

// Row label: tap selects the pad, long press opens the track menu.
void lengthPicked(int index, void* user)
{
    const int pad = (int)(intptr_t)user;
    if (index < 0 || index >= 12) return;
    app().setTrackLength(pad, kLenVals[index]);
    char name[32], msg[96];
    padName(pad, name, sizeof name);
    if (kLenVals[index] == 0) snprintf(msg, sizeof msg, "%s: length as the scene", name);
    else snprintf(msg, sizeof msg, "%s: %d steps", name, kLenVals[index]);
    toast(msg);
}

void trackMenuPicked(int index, void* user)
{
    const int pad = (int)(intptr_t)user;
    char name[32], msg[96];
    padName(pad, name, sizeof name);
    switch (index) {
    case 0:
        app().clearTrack(pad);
        if (undoAvailable(app(), 0)) {
            armUndo();
            syncUndo(false);
            snprintf(msg, sizeof msg, "Cleared track %s — tap UNDO to take it back", name);
        } else {
            snprintf(msg, sizeof msg, "Cleared track %s", name);
        }
        toast(msg);
        break;
    case 1: {
        const int len = curScene().track[pad].length;
        int sel = -1;
        for (int i = 0; i < 12; ++i)
            if (kLenVals[i] == len) sel = i;
        popupMenu(v.row[pad % step_grid::kRows].box, kLenItems, 12, sel, lengthPicked, user);
        break;
    }
    case 2:
        app().toggleMute(pad);
        snprintf(msg, sizeof msg, "%s: %s", name, app().project().pad[pad].mute ? "muted" : "on");
        toast(msg);
        break;
    default: break;
    }
}

void onRow(lv_event_t* e)
{
    const int r = (int)(intptr_t)lv_event_get_user_data(e);
    const int pad = clampi(app().ui().seq_bank, 0, 1) * step_grid::kRows + r;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED) {
        app().selectPad(pad, true);   // audition per Settings > Pad audition (not recorded while REC)
    } else if (code == LV_EVENT_LONG_PRESSED) {
        static const char* items[3] = {"CLEAR TRACK", "LENGTH \xE2\x86\x92", "MUTE"};
        items[2] = app().project().pad[pad].mute ? "UNMUTE" : "MUTE";
        popupMenu(v.row[r].box, items, 3, -1, trackMenuPicked, (void*)(intptr_t)pad);
    }
}

void divPicked(int index, void*)
{
    if (index >= 0 && index < (int)Division::Count) app().setDivision((Division)index);
}
void onDiv(lv_event_t*) { popupMenu(v.div, kDivItems, (int)Division::Count, (int)curScene().div, divPicked, nullptr); }

void swingPicked(int index, void*)
{
    if (index >= 0 && index < 7) app().setSwing(kSwingVals[index]);
}
void onSwing(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        v.swing_acc = 0;
        v.swing_moved = false;
        v.swing_start = curScene().swing;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t d;
        lv_indev_get_vect(indev, &d);
        if (d.y == 0) return;
        v.swing_acc -= d.y;   // up = more swing
        if (!v.swing_moved && (v.swing_acc > kSwingPx || v.swing_acc < -kSwingPx)) v.swing_moved = true;
        if (v.swing_moved) {
            const int target = clampi(v.swing_start + v.swing_acc / kSwingPx, 50, 75);
            if (target != curScene().swing) app().setSwing(target);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (v.swing_moved) return;
        int sel = -1;
        for (int i = 0; i < 7; ++i)
            if (kSwingVals[i] == curScene().swing) sel = i;
        popupMenu(v.swing, kSwingItems, 7, sel, swingPicked, nullptr);
    }
}

void stepsPicked(int index, void*)
{
    if (index >= 0 && index < 4) app().setSceneLength((index + 1) * kStepsPerPage);
}
void onSteps(lv_event_t*)
{
    const int len = curScene().length;
    const int sel = (len % kStepsPerPage == 0 && len >= 16 && len <= 64) ? len / kStepsPerPage - 1 : -1;
    popupMenu(v.steps, kStepsItems, 4, sel, stepsPicked, nullptr);
}

void toggleLane(StepLane l) { app().setLane(app().ui().lane == l ? StepLane::Trig : l); }
void onTrack(lv_event_t*) { app().setLane(StepLane::Trig); }

// Value lane: the track name box opens the list of the 16 pads; picking one edits its lane
// without playing it (a pad touch would sound, and record while REC is on) and keeps the lane.
void lanePadPicked(int index, void*)
{
    if (index >= 0 && index < kPads) app().selectPad(index, false);
}
void onLanePad(lv_event_t*)
{
    static char names[kPads][48];
    static const char* items[kPads];
    for (int p = 0; p < kPads; ++p) {
        char nm[kNameLen + 8];
        padName(p, nm, sizeof nm);
        snprintf(names[p], sizeof names[p], "%c%d  %s", 'A' + p / 4, p % 4 + 1, nm);
        items[p] = names[p];
    }
    popupMenu(v.lane_box, items, kPads, clampi(app().ui().pad, 0, kPads - 1), lanePadPicked, nullptr);
}
void onVel(lv_event_t*) { toggleLane(StepLane::Velocity); }
void onProb(lv_event_t*) { toggleLane(StepLane::Probability); }
void morePicked(int index, void*)
{
    if (index >= 0 && index < 4) toggleLane(kMoreLanes[index]);
}
void onMore(lv_event_t*)
{
    int sel = -1;
    for (int i = 0; i < 4; ++i)
        if (app().ui().lane == kMoreLanes[i]) sel = i;
    popupMenu(v.more, kMoreItems, 4, sel, morePicked, nullptr);
}

// ------------------------------------------------------------------ refresh parts
void updateHead()
{
    char buf[96];
    snprintf(buf, sizeof buf, "PATTERN EDITOR / SCENE %c", sceneLetter());
    setCached(v.over, v.over_txt, sizeof v.over_txt, buf);

    const Scene& sc = curScene();
    const int len = clampi(sc.length, 1, kMaxSteps);
    const int pages = (len + kStepsPerPage - 1) / kStepsPerPage;
    const int page = clampi(app().ui().seq_page, 0, pages - 1);
    const StepLane lane = app().ui().lane;
    char bar[24] = "";
    if (len > kStepsPerPage) snprintf(bar, sizeof bar, " \xC2\xB7 BAR %d/%d", page + 1, pages);
    if (lane == StepLane::Trig) {
        snprintf(buf, sizeof buf, "DRUM MACHINE \xC2\xB7 %d STEPS%s", len, bar);
    } else {
        char name[32];
        padName(app().ui().pad, name, sizeof name);
        snprintf(buf, sizeof buf, "%s \xC2\xB7 %s%s", kLaneNames[(int)lane], name, bar);
    }
    setCached(v.title, v.title_txt, sizeof v.title_txt, buf);
}

void updateRows()
{
    const UiState& ui = app().ui();
    const bool trig = ui.lane == StepLane::Trig;
    const int bank = clampi(ui.seq_bank, 0, 1);
    for (int i = 0; i < 2; ++i) {
        btnSetActive(v.bank[i], bank == i);
        if (trig) lv_obj_remove_flag(v.bank[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(v.bank[i], LV_OBJ_FLAG_HIDDEN);
    }
    char name[32];
    for (int r = 0; r < step_grid::kRows; ++r) {
        RowUi& ru = v.row[r];
        if (!trig) {
            lv_obj_add_flag(ru.box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(ru.box, LV_OBJ_FLAG_HIDDEN);
        const int pad = bank * step_grid::kRows + r;
        padName(pad, name, sizeof name);
        setCached(ru.name, ru.txt, sizeof ru.txt, name);
        const int8_t sel = pad == ui.pad ? 1 : 0;
        const int8_t muted = app().project().pad[pad].mute ? 1 : 0;
        if (sel != ru.sel || muted != ru.muted) {
            ru.sel = sel;
            ru.muted = muted;
            lv_obj_set_style_text_font(ru.name, uiFont(ru.name, sel ? f_s13 : f_r13), 0);   // maximized: larger
            lv_obj_set_style_text_color(ru.name, c(muted ? col::muted : col::ink), 0);
            lv_obj_set_style_bg_color(ru.dot, c(muted ? col::muted : col::accent), 0);
            const float f = uiScaleOf(ru.dot).f;
            const int d = f == 1.f ? (sel ? 7 : 5) : (int)lroundf((sel ? 7.f : 5.f) * f);
            lv_obj_set_size(ru.dot, d, d);
            lv_obj_set_pos(ru.dot, uiSx(ru.dot, sel ? 0 : 1), (uiSy(ru.dot, step_grid::kCellH) - d) / 2);
        }
    }
    // Lane mode: the label column names the edited track and lane.
    if (trig) {
        lv_obj_add_flag(v.lane_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v.lane_zero, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(v.lane_box, LV_OBJ_FLAG_HIDDEN);
        padName(ui.pad, name, sizeof name);
        setCached(v.lane_pad, v.lane_pad_txt, sizeof v.lane_pad_txt, name);
        const char* ln = kLaneNames[(int)ui.lane];
        if (strcmp(lv_label_get_text(v.lane_name), ln) != 0) lv_label_set_text(v.lane_name, ln);
        const bool muted = app().project().pad[clampi(ui.pad, 0, kPads - 1)].mute != 0;
        lv_obj_set_style_bg_color(v.lane_dot, c(muted ? col::muted : col::accent), 0);
        if (ui.lane == StepLane::Nudge || ui.lane == StepLane::Pitch) lv_obj_remove_flag(v.lane_zero, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(v.lane_zero, LV_OBJ_FLAG_HIDDEN);
    }
}

void updateFooter()
{
    const Scene& sc = curScene();
    const UiState& ui = app().ui();
    char buf[32];
    snprintf(buf, sizeof buf, "%s \xE2\x96\xBE", divisionName(sc.div));
    btnSetText(v.div, buf);
    snprintf(buf, sizeof buf, "SWING %d%%", (int)sc.swing);
    btnSetText(v.swing, buf);
    const int len = clampi(sc.length, 1, kMaxSteps);
    snprintf(buf, sizeof buf, "%d STEPS", len);
    btnSetText(v.steps, buf);
    const int pages = (len + kStepsPerPage - 1) / kStepsPerPage;
    const int8_t compact = pages > 1 ? 1 : 0;
    if (compact != v.footer_compact) {
        // Prototype widths (67 / 92.5 / 77.5, gap 8) while there is no BAR button; narrower
        // buttons when BAR n/m ▾ has to fit into the card as well.
        v.footer_compact = compact;
        static const int16_t kX[2][3] = {{341, 416, 517}, {341, 403, 491}};
        static const int16_t kW[2][3] = {{67, 93, 78}, {56, 82, 68}};
        lv_obj_t* const b[3] = {v.div, v.swing, v.steps};
        for (int i = 0; i < 3; ++i) {   // design px, in what the (maybe maximized) panel draws now
            lv_obj_set_pos(b[i], uiSx(b[i], kX[compact][i] - v.ox), uiSy(b[i], 507 - v.oy));
            lv_obj_set_width(b[i], uiSx(b[i], kW[compact][i]));
        }
    }
    if (pages > 1) {
        snprintf(buf, sizeof buf, "BAR %d/%d \xE2\x96\xBE", clampi(ui.seq_page, 0, pages - 1) + 1, pages);
        btnSetText(v.bar, buf);
        lv_obj_remove_flag(v.bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v.bar, LV_OBJ_FLAG_HIDDEN);
    }
    char name[32];
    padName(ui.pad, name, sizeof name);
    setCached(lv_obj_get_child(v.track, 0), v.track_txt, sizeof v.track_txt, name);
    btnSetActive(v.track, ui.lane == StepLane::Trig);
    btnSetActive(v.vel, ui.lane == StepLane::Velocity);
    btnSetActive(v.prob, ui.lane == StepLane::Probability);
    int more = -1;
    for (int i = 0; i < 4; ++i)
        if (ui.lane == kMoreLanes[i]) more = i;
    if (more >= 0) snprintf(buf, sizeof buf, "%s \xE2\x96\xBE", kMoreItems[more]);
    else snprintf(buf, sizeof buf, "MORE \xE2\x96\xBE");
    btnSetText(v.more, buf);
    btnSetActive(v.more, more >= 0);
}

void updateTransport()
{
    btnSetText(v.play, app().playing() ? "PAUSE \xE2\x85\xA1" : "PLAY \xE2\x96\xB6");
    btnSetActive(v.fill, app().ui().fill != 0);
}

lv_obj_t* rowLabel(int r)
{
    RowUi& ru = v.row[r];
    ru.box = lv_obj_create(v.root);
    lv_obj_remove_style_all(ru.box);
    place(ru.box, 341, step_grid::rowY(r));
    lv_obj_set_size(ru.box, 86, step_grid::kCellH);
    lv_obj_add_flag(ru.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ru.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ru.box, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_style(ru.box, &styles().pressed, LV_STATE_PRESSED);
    setHit(ru.box, 2);
    ru.dot = lv_obj_create(ru.box);
    lv_obj_remove_style_all(ru.dot);
    lv_obj_set_size(ru.dot, 5, 5);
    lv_obj_set_pos(ru.dot, 1, (step_grid::kCellH - 5) / 2);
    lv_obj_set_style_radius(ru.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ru.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ru.dot, c(col::accent), 0);
    lv_obj_remove_flag(ru.dot, LV_OBJ_FLAG_CLICKABLE);
    ru.name = label(ru.box, "", f_r13, col::ink);
    lv_label_set_long_mode(ru.name, LV_LABEL_LONG_MODE_DOTS);   // fixed box: one line, dots
    lv_obj_set_size(ru.name, 74, lv_font_get_line_height(f_s13));
    lv_obj_align(ru.name, LV_ALIGN_LEFT_MID, 12, 0);
    ru.sel = -1;
    ru.muted = -1;
    lv_obj_add_event_cb(ru.box, onRow, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)r);
    lv_obj_add_event_cb(ru.box, onRow, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)r);
    return ru.box;
}

}  // namespace

lv_obj_t* create(lv_obj_t* work)
{
    v = View{};
    // Root fills the work panel; children are placed in screen px through place().
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

    // ---- head (.sequence-top)
    v.over = overline(v.root, "PATTERN EDITOR / SCENE A");
    place(v.over, 329, 84);
    v.title = label(v.root, "DRUM MACHINE \xC2\xB7 16 STEPS", f_m20, col::ink);
    lv_label_set_long_mode(v.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(v.title, 372, lv_font_get_line_height(f_m20));
    place(v.title, 329, 100);
    // right: FILL  RANDOM  CLEAR  PLAY (right edge 1039, gap 6)
    v.play = actionBtn(v.root, "PLAY \xE2\x96\xB6", true, 86, 34);
    place(v.play, 1039 - 86, 87);
    lv_obj_add_event_cb(v.play, onPlay, LV_EVENT_CLICKED, nullptr);
    v.clear = actionBtn(v.root, "CLEAR", false, 74, 34);
    place(v.clear, 1039 - 86 - 6 - 74, 87);
    lv_obj_add_event_cb(v.clear, onClear, LV_EVENT_CLICKED, nullptr);
    v.random = actionBtn(v.root, "RANDOM", false, 94, 34);
    place(v.random, 1039 - 86 - 6 - 74 - 6 - 94, 87);
    lv_obj_add_event_cb(v.random, onRandom, LV_EVENT_CLICKED, nullptr);
    v.fill = actionBtn(v.root, "FILL", false, 58, 34);
    place(v.fill, 1039 - 86 - 6 - 74 - 6 - 94 - 6 - 58, 87);
    lv_obj_add_event_cb(v.fill, onFill, LV_EVENT_ALL, nullptr);

    // ---- grid panel (.seq-grid-wrap)
    box(329, 142, 710, 312, col::seq_bg, 8);
    // the grid first: the bank toggle and the row labels (with their extended hit areas) stay on top
    v.grid = step_grid::create(v.root, step_grid::kObjX - v.ox, step_grid::kObjY - v.oy);
    static const char* const kBankText[2] = {"A1\xE2\x80\x93" "B4", "C1\xE2\x80\x93" "D4"};
    for (int i = 0; i < 2; ++i) {
        // 28 px tall (y 145..172): above row 0 (y 174), whose label wins the 1 px hit overlap
        v.bank[i] = microBtn(v.root, kBankText[i], 46, 28);
        place(v.bank[i], 333 + i * 48, 145);
        setHit(v.bank[i], 3);
        lv_obj_add_event_cb(v.bank[i], onBank, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    for (int r = 0; r < step_grid::kRows; ++r) rowLabel(r);
    // lane mode label column
    v.lane_box = lv_obj_create(v.root);
    lv_obj_remove_style_all(v.lane_box);
    place(v.lane_box, 341, step_grid::kRowY0);
    lv_obj_set_size(v.lane_box, 86, 44);
    lv_obj_add_flag(v.lane_box, LV_OBJ_FLAG_CLICKABLE);   // tap: list of pads (silent track switch)
    lv_obj_remove_flag(v.lane_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v.lane_box, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_style(v.lane_box, &styles().pressed, LV_STATE_PRESSED);
    setHit(v.lane_box, 4);
    lv_obj_add_event_cb(v.lane_box, onLanePad, LV_EVENT_CLICKED, nullptr);
    v.lane_dot = lv_obj_create(v.lane_box);
    lv_obj_remove_style_all(v.lane_dot);
    lv_obj_set_size(v.lane_dot, 7, 7);
    lv_obj_set_pos(v.lane_dot, 0, 5);
    lv_obj_set_style_radius(v.lane_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(v.lane_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v.lane_dot, c(col::accent), 0);
    v.lane_pad = label(v.lane_box, "", f_s13, col::ink);
    lv_label_set_long_mode(v.lane_pad, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(v.lane_pad, 62, lv_font_get_line_height(f_s13));
    lv_obj_set_pos(v.lane_pad, 12, 0);
    lv_obj_t* chev = label(v.lane_box, "\xE2\x96\xBE", f_m12, col::muted);   // ▾: the name opens a list
    lv_obj_set_pos(chev, 76, 1);
    v.lane_name = overline(v.lane_box, "");
    lv_obj_set_pos(v.lane_name, 0, 22);
    lv_obj_add_flag(v.lane_box, LV_OBJ_FLAG_HIDDEN);
    v.lane_zero = label(v.root, "0", f_r12, col::muted);
    lv_obj_set_width(v.lane_zero, 30);
    lv_obj_set_style_text_align(v.lane_zero, LV_TEXT_ALIGN_RIGHT, 0);
    place(v.lane_zero, 395, (step_grid::kLaneBarsY0 + step_grid::kLaneBarsY1) / 2 - 8);
    lv_obj_add_flag(v.lane_zero, LV_OBJ_FLAG_HIDDEN);

    // ---- footer cards (.l-seq-footer / .l-sec-card)
    box(329, 470, 349, 77, col::control, 6);
    place(caption13(overline(v.root, "PATTERN SETTINGS")), 341, 483);
    v.div = footBtn("1/16 \xE2\x96\xBE", 341, 56);
    lv_obj_add_event_cb(v.div, onDiv, LV_EVENT_CLICKED, nullptr);
    v.swing = footBtn("SWING 54%", 403, 82);
    lv_obj_add_flag(v.swing, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(v.swing, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(v.swing, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v.swing, onSwing, LV_EVENT_ALL, nullptr);
    v.steps = footBtn("16 STEPS", 491, 68);
    lv_obj_add_event_cb(v.steps, onSteps, LV_EVENT_CLICKED, nullptr);
    v.bar = footBtn("BAR 1/2 \xE2\x96\xBE", 567, 99);   // to the card's inner right edge (666)
    lv_obj_add_event_cb(v.bar, onBar, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(v.bar, LV_OBJ_FLAG_HIDDEN);

    box(690, 470, 349, 77, col::control, 6);
    place(caption13(overline(v.root, "SELECTED TRACK")), 702, 483);
    v.track = footBtn("KICK", 702, 62, false);
    if (lv_obj_t* l = lv_obj_get_child(v.track, 0)) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);   // one line, dots for long pad names
        lv_obj_set_size(l, 62 - 10, lv_font_get_line_height(f_m12));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_add_event_cb(v.track, onTrack, LV_EVENT_CLICKED, nullptr);
    v.vel = footBtn("VELOCITY", 769, 76);
    lv_obj_add_event_cb(v.vel, onVel, LV_EVENT_CLICKED, nullptr);
    v.prob = footBtn("PROBABILITY", 850, 96);
    lv_obj_add_event_cb(v.prob, onProb, LV_EVENT_CLICKED, nullptr);
    v.more = footBtn("MORE \xE2\x96\xBE", 951, 76);
    lv_obj_add_event_cb(v.more, onMore, LV_EVENT_CLICKED, nullptr);

    v.drawn_valid = false;
    syncUndo(false);   // an older snapshot does not turn CLEAR into UNDO
    refresh();
    return v.root;
}

void refresh()
{
    if (!v.root) return;
    const Revs& r = app().revs();
    const bool all = !v.drawn_valid;
    const bool pattern = all || r.pattern != v.drawn.pattern || r.view != v.drawn.view ||
                         r.selection != v.drawn.selection || r.pads != v.drawn.pads || r.project != v.drawn.project;
    if (pattern) {
        updateHead();
        updateRows();
        updateFooter();
        step_grid::refresh();
    }
    if (all || r.transport != v.drawn.transport) updateTransport();
    v.drawn = r;
    v.drawn_valid = true;

    syncUndo(true);   // RANDOM / CLEAR TRACK / timeline COPY ... -> UNDO
    const uint32_t now = lv_tick_get();
    if (v.undo_until) {
        if ((int32_t)(now - v.undo_until) >= 0) clearToClear();
        else undoLabel();   // "UNDO" / "UNDO B" follows the edited scene (text set only on change)
    }
    if (now - v.last_ph_ms >= 33) {   // playhead ~30 Hz
        v.last_ph_ms = now;
        step_grid::tickPlayhead();
    }
    step_grid::tickSparks();          // hit sparks fade every frame
}

void onShow()
{
    v.drawn_valid = false;
    syncUndo(false);   // snapshots taken while the view was hidden do not arm UNDO (timeline "+" has it)
    refresh();
}

}  // namespace orbit::ui::view_sequence
