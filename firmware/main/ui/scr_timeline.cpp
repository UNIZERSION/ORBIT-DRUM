// SCENES timeline (#l-timeline, bottom): scene buttons A-D (+ copy / clear menu), the song
// arranger (16 slots x rows Scene A..D + FX) and FOLLOW / loop, GRID and LENGTH.
//
// The slot header 1..16, the 5x16 cell matrix, the playhead and its triangle are ONE custom-drawn
// object with arithmetic hit testing. Cells keep the state they were drawn with; only cells that
// change are invalidated (the playing cell's pulse invalidates that cell alone, ~30 Hz).
// Cell (row r < 4, slot i) is lit when song.slot[i] == r: accent on the lane of the scene that is
// playing (the edited scene while stopped), clip grey with an accent outline on the lane of the
// edited scene when another one plays, clip grey otherwise; the FX row is lit when song.fx[i].
// A queued scene button blinks at 2 Hz until it plays; the playing scene gets a small dot.
//
// Cells are 20 px tall at a 22 px pitch (no room for more), so a cell edit is slide-to-correct:
// the cell under the finger is outlined while pressed and follows the finger; the release cell
// is committed (released outside the matrix = cancelled). Replacing another scene in a slot
// toasts it, and the "+" menu offers a one-level undo of the last cell edit.
#include "ui/ui.h"

#include <math.h>

#include "ui/theme.h"
#include "ui/ui_internal.h"
#include "ui/widgets.h"

namespace orbit::ui::timeline {

namespace {

// ---- matrix geometry (object at panel-content 240,7 = absolute 552,576)
constexpr int kMatX = 240, kMatY = 7, kMatW = 595, kMatH = 125;
constexpr int kCellX0 = 8, kCellsW = 587;     // cells span 8..594 (absolute 560..1146)
constexpr int kHeadH = 16;                    // header strip with the slot numbers
constexpr int kRowH = 20, kRowGap = 2, kGap = 2;
constexpr int kRows = 5;                      // A, B, C, D, FX
constexpr uint32_t kPulseMs = 1100;
constexpr int kPulseLevels = 16;

enum CellState : uint8_t { CellEmpty = 0, CellClip, CellAccent, CellEdit };

lv_obj_t* s_panel = nullptr;
lv_obj_t* s_scene[kScenes] = {};
lv_obj_t* s_scene_dot[kScenes] = {};
lv_obj_t* s_plus = nullptr;
lv_obj_t* s_mat = nullptr;
lv_obj_t* s_follow = nullptr;
lv_obj_t* s_link = nullptr;
lv_obj_t* s_link_icon = nullptr;
lv_obj_t* s_grid_sel = nullptr;
lv_obj_t* s_len_sel = nullptr;

uint8_t s_cell[kRows][kSongSlots];   // drawn state
int s_play_slot = -1;                // pulsing cell's slot (FOLLOW + playing), -1 none
int s_play_row = -1;
int s_pulse_level = 0;
int s_head_x = -1;                   // drawn playhead x (matrix-local), -1 hidden
int s_press_cell = -1;               // r * 16 + i under the pressing finger (outlined), -1 none
bool s_pressing = false;
int s_accent_row = -1;               // lane drawn in accent (playing scene, else the edited one)
// One-level undo of the last cell edit (the "+" menu). Offered only while the cell still holds
// the value the edit left and the project was not replaced (revs.project) since.
struct SongUndo {
    bool valid;
    bool fx;            // FX row (before/after: 0/1) or a scene row (before/after: -1..3)
    int8_t slot, before, after;
    uint32_t rev_project;
};
SongUndo s_undo = {};
uint32_t s_rev_song = 0, s_rev_pattern = 0, s_rev_transport = 0, s_rev_project = 0;
bool s_first = true;
int s_scene_drawn = -1;              // P().scene drawn as active
int s_blink_drawn = -1;              // queued scene drawn in its "off" phase
int s_dot_drawn = -1;
int s_follow_drawn = -1;
int s_div_drawn = -1, s_len_drawn = -1;

const char* const kDivNames[] = {"1/8", "1/16", "1/32", "1/8T", "1/16T"};
const char* const kLenNames[] = {"1 Bar", "2 Bars", "3 Bars", "4 Bars"};
const char* const kNums[kSongSlots] = {"1", "2", "3", "4", "5", "6", "7", "8",
                                       "9", "10", "11", "12", "13", "14", "15", "16"};

// ---------------------------------------------------------------- geometry
int colX(int i) { return kCellX0 + (i * (kCellsW + kGap) + kSongSlots / 2) / kSongSlots; }
int cellW(int i) { return colX(i + 1) - colX(i) - kGap; }
int rowY(int r) { return kHeadH + r * (kRowH + kRowGap); }

// matrix-local design px -> screen (the matrix may be drawn larger: maximize, ui_scale.h)
lv_area_t cellRect(const Xf& t, int r, int i)
{
    return t.area((float)colX(i), (float)rowY(r), (float)(colX(i) + cellW(i)), (float)(rowY(r) + kRowH));
}

void cellArea(int r, int i, lv_area_t& out)
{
    const Xf t = xfOf(s_mat);
    out = cellRect(t, r, i);
    if (!t.s.unit()) lv_area_increase(&out, 1, 1);   // rounding of the scaled edges
}

void invalidateCell(int r, int i)
{
    if (r < 0 || r >= kRows || i < 0 || i >= kSongSlots) return;
    lv_area_t a;
    cellArea(r, i, a);
    lv_obj_invalidate_area(s_mat, &a);
}

void invalidateHead(int x)
{
    if (x < 0) return;
    lv_area_t m;
    lv_obj_get_coords(s_mat, &m);
    const Xf t = xfOf(s_mat);
    const int32_t hx = t.X((float)x), w = t.F(6) + 1;
    lv_area_t a = {hx - w, m.y1, hx + w, m.y2};
    lv_obj_invalidate_area(s_mat, &a);
}

// matrix-local point -> r * 16 + i, or -1
int cellAt(int32_t lx, int32_t ly)
{
    if (ly < kHeadH - 1 || lx < kCellX0 - 1) return -1;
    int r = (ly - kHeadH + kRowGap / 2) / (kRowH + kRowGap);
    if (r < 0) r = 0;
    if (r >= kRows) return -1;
    int i = -1;
    for (int k = 0; k < kSongSlots; ++k) {
        if (lx >= colX(k) - kGap / 2 - 1 && lx < colX(k + 1) - kGap / 2) i = k;
    }
    if (i < 0) return -1;
    return r * kSongSlots + i;
}

void invalidateCellIdx(int cell)
{
    if (cell >= 0) invalidateCell(cell / kSongSlots, cell % kSongSlots);
}

// ---------------------------------------------------------------- state
// accent_row: the playing scene (the edited one while stopped); the edited scene's lane is
// outlined when it differs, so FOLLOW playback shows what plays and what the buttons edit.
uint8_t wantCell(const Project& p, int r, int i, int accent_row)
{
    if (r == 4) return p.song.fx[i] ? CellAccent : CellEmpty;
    if (p.song.slot[i] != r) return CellEmpty;
    if (r == accent_row) return CellAccent;
    return r == p.scene ? CellEdit : CellClip;
}

int accentRow(const App& a, const Project& p, bool playing)
{
    const int ps = playing ? a.playScene() : -1;
    if (ps >= 0 && ps < kScenes) return ps;
    return p.scene < kScenes ? p.scene : 0;
}

lv_color_t mixc(uint32_t a, uint32_t b, int num, int den)
{
    const int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    const int br = (b >> 16) & 255, bgc = (b >> 8) & 255, bb = b & 255;
    const uint32_t r = (uint32_t)(ar + (br - ar) * num / den), g = (uint32_t)(ag + (bgc - ag) * num / den),
                   bl = (uint32_t)(ab + (bb - ab) * num / den);
    return c((r << 16) | (g << 8) | bl);
}

// ---------------------------------------------------------------- drawing
void matDrawCb(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    lv_area_t m;
    lv_obj_get_coords(s_mat, &m);
    const lv_area_t clip = layer->_clip_area;
    const Xf t = xfOf(s_mat);

    // header numbers (centred over their column)
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = t.font(f_r12);
    ld.color = c(col::song_num);
    ld.align = LV_TEXT_ALIGN_CENTER;
    for (int i = 0; i < kSongSlots; ++i) {
        const lv_area_t a = t.area((float)colX(i), 0.f, (float)(colX(i) + cellW(i)), (float)(kHeadH - 1));
        if (!areasOverlap(a, clip)) continue;
        ld.text = kNums[i];
        lv_draw_label(layer, &ld, &a);
    }

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.border_width = t.F(1);
    d.border_opa = LV_OPA_COVER;
    d.radius = 0;
    for (int r = 0; r < kRows; ++r) {
        for (int i = 0; i < kSongSlots; ++i) {
            const lv_area_t a = cellRect(t, r, i);
            if (!areasOverlap(a, clip)) continue;
            const uint8_t st = s_cell[r][i];
            d.border_width = t.F(1);
            if (r == s_play_row && i == s_play_slot) {
                // .timeline-cell.playing #ff8869, pulsing towards a light tint (1.1 s)
                d.bg_color = mixc(col::cell_playing, col::cell_pulse, s_pulse_level, kPulseLevels);
                d.border_color = c(col::accent);
            } else if (st == CellAccent) {
                d.bg_color = c(col::accent);
                d.border_color = c(col::accent);
            } else if (st == CellEdit) {   // edited scene's lane while another scene plays
                d.bg_color = c(col::cell_clip);
                d.border_color = c(col::accent);
                d.border_width = t.F(2);
            } else if (st == CellClip) {
                d.bg_color = c(col::cell_clip);
                d.border_color = c(col::cell_border);
            } else {
                d.bg_color = c(col::cell);
                d.border_color = c(col::cell_border);
            }
            lv_draw_rect(layer, &d, &a);
            if (r * kSongSlots + i == s_press_cell) {   // slide-to-correct: the cell under the finger
                lv_draw_rect_dsc_t ring;
                lv_draw_rect_dsc_init(&ring);
                ring.bg_opa = LV_OPA_TRANSP;
                ring.border_color = c(col::ink);
                ring.border_opa = LV_OPA_COVER;
                ring.border_width = t.F(2);
                lv_draw_rect(layer, &ring, &a);
            }
        }
    }

    if (s_head_x >= 0) {   // playhead: 2 px accent line + triangle in the header strip
        const int32_t hx = t.X((float)s_head_x), lw = t.F(2);
        lv_area_t l = {hx - lw / 2, t.Y((float)(kHeadH - 4)), hx - lw / 2 + lw - 1, m.y2};
        if (areasOverlap(l, clip)) {
            lv_draw_rect_dsc_t ln;
            lv_draw_rect_dsc_init(&ln);
            ln.bg_color = c(col::accent);
            ln.bg_opa = LV_OPA_COVER;
            lv_draw_rect(layer, &ln, &l);
        }
        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = c(col::accent);
        tri.opa = LV_OPA_COVER;
        const int32_t cx = hx, ty = t.Y((float)(kHeadH - 11));
        tri.p[0].x = (lv_value_precise_t)(cx - t.F(5));
        tri.p[0].y = (lv_value_precise_t)ty;
        tri.p[1].x = (lv_value_precise_t)(cx + t.F(4));
        tri.p[1].y = (lv_value_precise_t)ty;
        tri.p[2].x = (lv_value_precise_t)cx;
        tri.p[2].y = (lv_value_precise_t)(ty + t.F(7));
        lv_draw_triangle(layer, &tri);
    }
}

void setPressCell(int cell)
{
    if (cell == s_press_cell) return;
    invalidateCellIdx(s_press_cell);
    s_press_cell = cell;
    invalidateCellIdx(s_press_cell);
}

// Applies a released cell: scene rows set / clear the slot (the lit cell clears), the FX row
// toggles. Remembers the edit for the undo item; replacing another scene is toasted.
void commitCell(int cell)
{
    App& a = app();
    const Project& p = a.project();
    const int r = cell / kSongSlots, i = cell % kSongSlots;
    SongUndo u = {};
    u.slot = (int8_t)i;
    u.fx = r == 4;
    if (u.fx) {
        u.before = p.song.fx[i] ? 1 : 0;
        a.toggleSongFx(i);
        u.after = p.song.fx[i] ? 1 : 0;
    } else {
        u.before = p.song.slot[i];
        a.setSongSlot(i, r);
        u.after = p.song.slot[i];
        if (u.before >= 0 && u.before < kScenes && u.after >= 0 && u.after < kScenes && u.after != u.before)
            a.toast("Slot %d: Scene %c \xE2\x86\x92 %c", i + 1, (char)('A' + u.before), (char)('A' + u.after));
    }
    u.valid = u.before != u.after;
    u.rev_project = a.revs().project;
    s_undo = u;
}

bool undoAvailable()
{
    if (!s_undo.valid) return false;
    App& a = app();
    if (a.revs().project != s_undo.rev_project) return false;
    const Project& p = a.project();
    const int i = s_undo.slot;
    const int cur = s_undo.fx ? (p.song.fx[i] ? 1 : 0) : p.song.slot[i];
    return cur == s_undo.after;
}

void undoCell()
{
    if (!undoAvailable()) {
        s_undo.valid = false;
        return;
    }
    App& a = app();
    const int i = s_undo.slot;
    if (s_undo.fx) a.toggleSongFx(i);
    else a.setSongSlot(i, s_undo.before);   // -1 clears, a scene sets it (it differs from now)
    s_undo.valid = false;
    a.toast("Undid slot %d", i + 1);
}

// matrix-local cell under the active pointer, -1 outside the cells
int pointerCell()
{
    const lv_point_t p = pointerIn(s_mat);   // the panel may be drawn scaled (maximize)
    lv_area_t m;
    lv_obj_get_coords(s_mat, &m);
    return cellAt(p.x - m.x1, p.y - m.y1);
}

void matEventCb(lv_event_t* e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_pressing = true;
        setPressCell(pointerCell());
        break;
    case LV_EVENT_PRESSING:   // the outline follows the finger (PRESS_LOCK keeps the events coming)
        if (s_pressing) setPressCell(pointerCell());
        break;
    case LV_EVENT_RELEASED: {
        if (!s_pressing) break;
        s_pressing = false;
        const int cell = pointerCell();   // the release point decides; outside = cancel
        setPressCell(-1);
        if (cell >= 0) commitCell(cell);
        break;
    }
    case LV_EVENT_PRESS_LOST:
        s_pressing = false;
        setPressCell(-1);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- scene buttons + menus
void sceneCb(lv_event_t* e)
{
    const int s = (int)(intptr_t)lv_event_get_user_data(e);
    App& a = app();
    a.selectScene(s);
    // FOLLOW playback: the song chain picks the playing scene and the sequencer ignores the
    // queue request, so say why nothing changes audibly (the button only selects for editing).
    if (a.project().song.follow && a.playing() && a.songSlot() >= 0)
        a.toast("FOLLOW: the song picks the scene \xE2\x80\x94 %c for editing only", (char)('A' + s));
}

int s_menu_to[3] = {1, 2, 3};
enum PlusAction : uint8_t { kPlusCopy0 = 0, kPlusCopy1, kPlusCopy2, kPlusClear, kPlusUndoSlot, kPlusUndoScene };
uint8_t s_plus_act[6] = {};   // menu row -> PlusAction (the UNDO rows are optional)
void plusPicked(int index, void*)
{
    App& a = app();
    const int cur = a.project().scene;
    if (index < 0 || index >= 6) return;
    switch (s_plus_act[index]) {
    case kPlusCopy0:
    case kPlusCopy1:
    case kPlusCopy2: a.copyScene(cur, s_menu_to[s_plus_act[index]]); break;
    case kPlusClear: a.clearScene(); break;
    case kPlusUndoSlot: undoCell(); break;
    case kPlusUndoScene: a.undoPattern(); break;   // the App names the restored scene
    default: break;
    }
}

void plusCb(lv_event_t*)
{
    const int cur = app().project().scene < kScenes ? app().project().scene : 0;
    static char items[6][24];
    const char* ptrs[6] = {};
    int n = 0;
    for (int s = 0; s < kScenes; ++s) {
        if (s == cur) continue;
        s_menu_to[n] = s;
        snprintf(items[n], sizeof items[n], "COPY %c \xE2\x86\x92 %c", (char)('A' + cur), (char)('A' + s));
        ptrs[n] = items[n];
        s_plus_act[n] = (uint8_t)(kPlusCopy0 + n);
        ++n;
    }
    snprintf(items[n], sizeof items[n], "CLEAR SCENE %c", (char)('A' + cur));
    ptrs[n] = items[n];
    s_plus_act[n++] = kPlusClear;
    if (undoAvailable()) {   // undo the last song cell edit
        snprintf(items[n], sizeof items[n], "UNDO SLOT %d", s_undo.slot + 1);
        ptrs[n] = items[n];
        s_plus_act[n++] = kPlusUndoSlot;
    }
    // Undo the last destructive pattern edit (COPY, CLEAR SCENE, RANDOM, CLEAR TRACK) from any view.
    const int us = app().undoPatternScene();
    if (app().canUndoPattern() && us >= 0 && us < kScenes) {
        snprintf(items[n], sizeof items[n], "UNDO %c", (char)('A' + us));
        ptrs[n] = items[n];
        s_plus_act[n++] = kPlusUndoScene;
    }
    popupMenu(s_plus, ptrs, n, -1, plusPicked, nullptr);
}

void followCb(lv_event_t*) { app().setFollow(!app().project().song.follow); }
void linkCb(lv_event_t*) { app().setFollow(false); }
void divPicked(int index, void*) { app().setDivision((Division)index); }
void lenPicked(int index, void*) { app().setSceneLength(16 * (index + 1)); }

// .l-scene-buttons button: tab bg, 1 px line, radius 4, 13 px; .active: accent.
lv_obj_t* sceneButton(lv_obj_t* parent, const char* text, int x)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, 35);
    lv_obj_set_size(b, 24, 34);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, c(col::tab), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, c(col::line), 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_text_font(b, f_m14, 0);
    lv_obj_set_style_text_color(b, c(col::scene_text), 0);
    lv_obj_set_style_text_color(b, c(col::scene_text_on), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, c(col::accent), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, c(col::accent), LV_STATE_CHECKED);
    lv_obj_add_style(b, &styles().pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    // 24 px wide at a 29 px pitch: sideways only the gap (2 + 3 px, no overlap); up and down
    // there is free panel space, so the target grows to ~52 px tall.
    setHitBox(b, 2, 8, 3, 10);
    rippleAttach(b);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -1);
    return b;
}

void setSceneLook(int s, bool lit)
{
    if (s >= 0 && s < kScenes) lv_obj_set_state(s_scene[s], LV_STATE_CHECKED, lit);
}

void updateDotColors()
{
    for (int s = 0; s < kScenes; ++s) {
        const bool lit = lv_obj_has_state(s_scene[s], LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_scene_dot[s], c(lit ? col::on_accent : col::accent), 0);
    }
}

}  // namespace

lv_obj_t* create(lv_obj_t* scr)
{
    lv_obj_t* panel = panelCreate(scr, geo::timeline_x, geo::timeline_y, geo::timeline_w, geo::timeline_h);
    if (bg::shadowsBaked()) lv_obj_set_style_shadow_width(panel, 0, 0);
    s_panel = panel;
    panelTopHighlight(panel, geo::timeline_w);

    // ---- left: SCENES title + A B C D +
    lv_obj_t* ic = iconCreate(panel, Icon::Grid, 16, col::ink);
    lv_obj_set_pos(ic, 8, 11);
    lv_obj_t* t = label(panel, "SCENES", f_s15, col::ink);
    lv_obj_set_pos(t, 31, 8);
    lv_obj_set_pos(panelMaxButton(panel, Panel::Timeline), 108, 3);   // 0.3: maximize
    for (int s = 0; s < kScenes; ++s) {
        char txt[2] = {(char)('A' + s), 0};
        s_scene[s] = sceneButton(panel, txt, 8 + s * 29);
        lv_obj_add_event_cb(s_scene[s], sceneCb, LV_EVENT_CLICKED, (void*)(intptr_t)s);
        lv_obj_t* dot = lv_obj_create(s_scene[s]);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, c(col::accent), 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_scene_dot[s] = dot;
    }
    s_plus = sceneButton(panel, "+", 8 + 4 * 29);
    lv_obj_add_event_cb(s_plus, plusCb, LV_EVENT_CLICKED, nullptr);

    // ---- row labels (static)
    const Icon icons[kRows] = {Icon::Record, Icon::Grid, Icon::Wave, Icon::Eq, Icon::Spark};
    const char* const names[kRows] = {"Scene A", "Scene B", "Scene C", "Scene D", "FX"};
    for (int r = 0; r < kRows; ++r) {
        const int y = kMatY + rowY(r);
        lv_obj_t* ri = iconCreate(panel, icons[r], 14, col::ink);
        lv_obj_set_pos(ri, 158, y + 3);
        lv_obj_t* rl = label(panel, names[r], f_r12, col::ink);
        lv_obj_set_pos(rl, 177, y + 2);
    }

    // ---- matrix
    lv_obj_t* m = lv_obj_create(panel);
    lv_obj_remove_style_all(m);
    lv_obj_set_pos(m, kMatX, kMatY);
    lv_obj_set_size(m, kMatW, kMatH);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(m, LV_OBJ_FLAG_PRESS_LOCK);   // keep PRESSING / RELEASED while the finger slides off
    lv_obj_add_event_cb(m, matDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(m, matEventCb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(m, matEventCb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(m, matEventCb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(m, matEventCb, LV_EVENT_PRESS_LOST, nullptr);
    s_mat = m;

    // ---- right: FOLLOW + loop, GRID, LENGTH (.l-track-side: 1 px line on the left)
    lv_obj_t* side = lv_obj_create(panel);
    lv_obj_remove_style_all(side);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(side, c(col::line), 0);
    lv_obj_set_pos(side, 841, 8);
    lv_obj_set_size(side, 1, 121);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_CLICKABLE);

    s_follow = actionBtn(panel, "FOLLOW", false, 61, 30);
    lv_obj_set_pos(s_follow, 850, 8);
    lv_obj_set_style_pad_hor(s_follow, 0, 0);
    lv_obj_set_style_text_font(s_follow, f_m12, 0);
    lv_obj_add_event_cb(s_follow, followCb, LV_EVENT_CLICKED, nullptr);
    s_link = actionBtn(panel, "", false, 30, 30);
    lv_obj_set_pos(s_link, 915, 8);
    lv_obj_set_style_pad_hor(s_link, 0, 0);
    lv_obj_set_style_bg_color(s_link, c(col::control), LV_STATE_CHECKED);
    s_link_icon = iconCreate(s_link, Icon::Link, 18, col::ink);
    lv_obj_center(s_link_icon);
    lv_obj_add_event_cb(s_link, linkCb, LV_EVENT_CLICKED, nullptr);

    // Captions at 13 px (the 1.2x rule; 17 px line between the FOLLOW row and each select).
    lv_obj_t* og = overline(panel, "GRID");
    lv_obj_set_style_text_font(og, f_s13, 0);
    lv_obj_set_style_text_letter_space(og, ls(13, 0.13f), 0);
    lv_obj_set_pos(og, 850, 41);
    const Scene& sc = app().project().scene_data[app().project().scene % kScenes];
    s_grid_sel = selectCreate(panel, 95, 24, kDivNames, 5, (int)sc.div, divPicked, nullptr);
    lv_obj_set_pos(s_grid_sel, 850, 58);
    lv_obj_t* ol = overline(panel, "LENGTH");
    lv_obj_set_style_text_font(ol, f_s13, 0);
    lv_obj_set_style_text_letter_space(ol, ls(13, 0.13f), 0);
    lv_obj_set_pos(ol, 850, 84);
    s_len_sel = selectCreate(panel, 95, 24, kLenNames, 4, 0, lenPicked, nullptr);
    lv_obj_set_pos(s_len_sel, 850, 101);

    for (int r = 0; r < kRows; ++r)
        for (int i = 0; i < kSongSlots; ++i) s_cell[r][i] = 0xFF;   // force the first sync
    s_first = true;
    s_scene_drawn = s_blink_drawn = s_dot_drawn = s_follow_drawn = s_div_drawn = s_len_drawn = -1;
    s_play_slot = s_play_row = s_head_x = -1;
    s_press_cell = s_accent_row = -1;
    s_pressing = false;
    s_undo = SongUndo{};
    refresh();
    return panel;
}

void refresh()
{
    if (!s_mat) return;
    App& a = app();
    const Revs& rv = a.revs();
    const Project& p = a.project();
    const bool playing = a.playing();
    const uint32_t now = lv_tick_get();
    const int accent = accentRow(a, p, playing);   // the playing scene changes without a revision

    if (s_first || rv.song != s_rev_song || rv.pattern != s_rev_pattern || rv.transport != s_rev_transport ||
        rv.project != s_rev_project || accent != s_accent_row) {
        s_rev_song = rv.song;
        s_rev_pattern = rv.pattern;
        s_rev_transport = rv.transport;
        s_rev_project = rv.project;
        s_accent_row = accent;
        for (int r = 0; r < kRows; ++r) {
            for (int i = 0; i < kSongSlots; ++i) {
                const uint8_t w = wantCell(p, r, i, accent);
                if (w != s_cell[r][i]) {
                    s_cell[r][i] = w;
                    if (!s_first) invalidateCell(r, i);
                }
            }
        }
        const int scene = p.scene < kScenes ? p.scene : 0;
        if (scene != s_scene_drawn) {
            setSceneLook(s_scene_drawn, false);
            setSceneLook(scene, true);
            s_scene_drawn = scene;
            s_blink_drawn = -1;
            updateDotColors();
        }
        const int follow = p.song.follow ? 1 : 0;
        if (follow != s_follow_drawn) {
            s_follow_drawn = follow;
            btnSetActive(s_follow, follow != 0);
            btnSetActive(s_link, follow == 0);
            lv_obj_set_style_image_recolor(s_link_icon, c(follow ? col::ink : col::accent), 0);
        }
        const Scene& sc = p.scene_data[scene];
        if ((int)sc.div != s_div_drawn) {
            s_div_drawn = (int)sc.div;
            selectSet(s_grid_sel, s_div_drawn);
        }
        int li = ((int)sc.length + 15) / 16 - 1;
        li = li < 0 ? 0 : (li > 3 ? 3 : li);
        if (li != s_len_drawn) {
            s_len_drawn = li;
            selectSet(s_len_sel, li);
        }
        if (s_first) lv_obj_invalidate(s_mat);
        s_first = false;
    }

    // ---- queued scene: blink at 2 Hz (250 ms on / off) until it plays
    const int queued = playing ? a.queuedScene() : -1;
    const int blink_off = (queued >= 0 && queued < kScenes && ((now / 250u) & 1u)) ? queued : -1;
    if (blink_off != s_blink_drawn) {
        if (s_blink_drawn >= 0) setSceneLook(s_blink_drawn, s_blink_drawn == s_scene_drawn);
        if (blink_off >= 0) setSceneLook(blink_off, blink_off != s_scene_drawn);
        s_blink_drawn = blink_off;
        updateDotColors();
    }
    // ---- playing scene dot
    const int ps = playing ? a.playScene() : -1;
    const int dot = (ps >= 0 && ps < kScenes) ? ps : -1;
    if (dot != s_dot_drawn) {
        if (s_dot_drawn >= 0) lv_obj_add_flag(s_scene_dot[s_dot_drawn], LV_OBJ_FLAG_HIDDEN);
        if (dot >= 0) lv_obj_remove_flag(s_scene_dot[dot], LV_OBJ_FLAG_HIDDEN);
        s_dot_drawn = dot;
    }

    // ---- song position (FOLLOW + playing): pulsing cell + playhead
    int slot = -1, row = -1, hx = -1;
    if (playing && p.song.follow) {
        slot = a.songSlot();
        if (slot >= 0 && slot < kSongSlots) {
            row = p.song.slot[slot] >= 0 && p.song.slot[slot] < kScenes ? p.song.slot[slot] : -1;
            const int sc = a.playScene();
            int len = (sc >= 0 && sc < kScenes) ? (int)p.scene_data[sc].length : 16;
            if (len < 1) len = 16;
            int step = a.playStep();
            if (step < 0) step = 0;
            hx = colX(slot) + (cellW(slot) * step + len / 2) / len;
        } else {
            slot = -1;
        }
    }
    if (slot != s_play_slot || row != s_play_row) {
        invalidateCell(s_play_row, s_play_slot);
        s_play_slot = slot;
        s_play_row = row;
        s_pulse_level = -1;
    }
    if (s_play_slot >= 0 && s_play_row >= 0) {
        const float ph = (float)(now % kPulseMs) / (float)kPulseMs;
        const int lvl = (int)((0.5f - 0.5f * cosf(ph * 6.2831853f)) * (float)kPulseLevels + 0.5f);
        if (lvl != s_pulse_level) {
            s_pulse_level = lvl;
            invalidateCell(s_play_row, s_play_slot);
        }
    }
    if (hx != s_head_x) {
        invalidateHead(s_head_x);
        s_head_x = hx;
        invalidateHead(s_head_x);
    }
}

}  // namespace orbit::ui::timeline
