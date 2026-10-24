#pragma once
// SEQUENCE step grid (used by view_sequence.cpp): ONE custom-drawn LVGL object that shows the
// step-number row and 8 rows x 16 cells of the visible bank (A1..B4 / C1..D4) and bar, or — in a
// value lane (UiState::lane != Trig) — the selected track's 16 steps of the bar as value bars.
//
// Drawing cost: the object keeps a key per cell (on, velocity tint, marks, dimmed) and the
// playhead column per row; refresh() / tickPlayhead() invalidate only the cells whose key or
// playhead changed, and the draw callback skips every cell outside the clip area.
//
// Touch (Trig lane): the first touched cell decides on/off, every cell crossed by the drag is set
// once (App::setStepOn). The drag stays on the first cell's row (a slightly diagonal swipe never
// paints the neighbouring track); a drag that starts out vertical paints that column instead.
// Value lanes (COND too): absolute vertical position sets the value of the column under the
// finger, also while sweeping across columns (App::setStepLane); COND only once the finger has
// moved (> 8 px) — a tap on a COND column opens the list of conditions instead.
#include "lvgl.h"

namespace orbit::ui::step_grid {

constexpr int kRows = 8;
constexpr int kCols = 16;

// Geometry in screen px: the prototype's .seq-grid-wrap (329,142 710x312) with 8 rows of 29 px
// (pitch 34) fitted where it had 6 rows of 35 px; columns as in the prototype (32.5 px, gap 5).
constexpr int kCellX0 = 432;    // left edge of column 0
constexpr int kNumY = 152;      // top of the step-number row
constexpr int kRowY0 = 174;     // top of row 0
constexpr int kRowPitch = 34;
constexpr int kCellH = 29;
constexpr int kMargin = 4;      // object margin around the cells (room for the playhead outline)
constexpr int kLaneTextH = 17;  // lane mode: value text row on top of the bars
constexpr int kLaneBarsY0 = kRowY0 + 20;                                  // 194
constexpr int kLaneBarsY1 = kRowY0 + (kRows - 1) * kRowPitch + kCellH - 1; // 441

inline int colX(int c) { return kCellX0 + (c * 75) / 2; }               // screen x of column c
inline int colW(int c) { return (c * 75 + 65) / 2 - (c * 75) / 2; }     // 32 / 33 px
inline int rowY(int r) { return kRowY0 + r * kRowPitch; }               // screen y of row r

// Object bounds (screen px).
constexpr int kObjX = kCellX0 - kMargin;
constexpr int kObjY = kNumY - kMargin;
constexpr int kObjW = (15 * 75 + 65) / 2 + 2 * kMargin;                  // 603
constexpr int kObjH = (kLaneBarsY1 + 1 - kNumY) + 2 * kMargin;          // 298

// Creates the grid in `parent`; (x, y) is the parent-local position that corresponds to the
// screen point (kObjX, kObjY).
lv_obj_t* create(lv_obj_t* parent, int x, int y);
// Re-reads the pattern / lane / bank / bar and invalidates only what changed. Cheap when nothing
// changed (128 key compares); call after any App revision change.
void refresh();
// ~30 Hz: playhead cells from Engine telemetry (polymeter aware, only for the edited scene).
void tickPlayhead();
// Every UI frame: steps that just sounded light up and fade out (App::takeGridHits; Trig lane,
// Settings > Display > Animations, not in low power mode). Only the fading cells are redrawn.
void tickSparks();
// Forces a full redraw (e.g. after the view becomes visible again).
void invalidateAll();
// Column currently outlined in row r (-1: none) — for tests.
int playheadCol(int row);
// Brightness of a cell's spark now, 0..100 (0: none) — for tests.
int sparkPct(int row, int col);

}  // namespace orbit::ui::step_grid
