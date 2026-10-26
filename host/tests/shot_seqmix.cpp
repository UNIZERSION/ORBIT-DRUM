// Build + run (Git Bash, project root; first: bash host/build_lvgl.sh and bash tools/run_host_tests.sh app for host/bin/cJSON.o + kits_*.o):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; J=${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}/components/json/cJSON; C=firmware/main/core; U=firmware/main/ui; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -Ifirmware/main -Ihost -Ifirmware/managed_components/lvgl__lvgl -I$J host/tests/shot_seqmix.cpp $U/view_sequence.cpp $U/step_grid.cpp $U/view_mixer.cpp $U/view_fx.cpp $U/app.cpp $U/theme.cpp $U/widgets.cpp $U/widgets_knob.cpp $U/widgets_input.cpp $U/widgets_overlay.cpp $(ls $C/*.cpp) host/hal_host.cpp host/platform_host.cpp host/lvgl_host.cpp host/bin/cJSON.o host/bin/kits_808.o host/bin/kits_909.o host/lib/liborbit_assets.a host/lib/liblvgl.a -o host/bin/shot_seqmix.exe && host/bin/shot_seqmix.exe
//
// SEQUENCE, MIXER and FX views on the headless 1280x720 display at the prototype positions, with
// the REAL App (default project, TR-909 kit, sdcard library), the host HAL (audio rendered
// offline in step with the virtual LVGL clock) and the real widget kit. Writes
// host/shots/seqmix_*.png (compare with reference/shots/view-sequence.png, playing-sequence.png,
// view-mixer.png) and simulates touch (tap / drag / long press) asserting App state changes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#include "hal/hal.h"
#include "hal_host.h"
#include "lvgl_host.h"
#include "src/display/lv_display_private.h"
#include "ui/step_grid.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

using namespace orbit;
using namespace orbit::ui;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                           \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(c)) {                                                        \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);             \
            ++g_fails;                                                     \
        }                                                                  \
    } while (0)

// ------------------------------------------------------------------ harness
enum class Shown { Seq = 0, Mix, Fx };
static lv_obj_t* g_view[3] = {};
static Shown g_shown = Shown::Seq;
static uint32_t g_audio_ms = 0;
static int16_t g_audio[48 * 64 * 2];

// What ui.cpp does every frame: App::tick() + refresh() of the visible view. The audio "driver"
// renders exactly the virtual time that passed, so the sequencer runs in step with LVGL time.
static void frameTimer(lv_timer_t*)
{
    const uint32_t now = lvhost_ms();
    uint32_t ms = now - g_audio_ms;
    g_audio_ms = now;
    while (ms > 0) {
        const uint32_t n = ms > 60 ? 60 : ms;
        halHostRenderAudio((int)n * 48, g_audio);
        ms -= n;
    }
    app().tick();
    app().padFlashBits();   // pads::refresh() does this every frame (it feeds the step sparks too)
    switch (g_shown) {
    case Shown::Seq: view_sequence::refresh(); break;
    case Shown::Mix: view_mixer::refresh(); break;
    case Shown::Fx: view_fx::refresh(); break;
    }
}

static void show(Shown s)
{
    g_shown = s;
    for (int i = 0; i < 3; ++i) {
        if (i == (int)s) lv_obj_remove_flag(g_view[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_view[i], LV_OBJ_FLAG_HIDDEN);
    }
    switch (s) {
    case Shown::Seq: view_sequence::onShow(); break;
    case Shown::Mix: view_mixer::onShow(); break;
    case Shown::Fx: view_fx::onShow(); break;
    }
    lvhost_run(100);
}

static void shot(const char* name)
{
    char path[160];
    snprintf(path, sizeof path, "host/shots/seqmix_%s.png", name);
    CHECK(lvhost_screenshot(path));
}

// Depth-first search for a visible label with exactly this text (or containing it); with
// `clickable` only labels inside a clickable parent (buttons, menu rows) match.
static lv_obj_t* findLabel(lv_obj_t* root, const char* text, bool contains = false, bool clickable = false)
{
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char* t = lv_label_get_text(root);
        lv_obj_t* par = lv_obj_get_parent(root);
        const bool ok = !clickable || (par && lv_obj_has_flag(par, LV_OBJ_FLAG_CLICKABLE));
        if (ok && t && (contains ? strstr(t, text) != nullptr : strcmp(t, text) == 0)) return root;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i)
        if (lv_obj_t* f = findLabel(lv_obj_get_child(root, (int32_t)i), text, contains, clickable)) return f;
    return nullptr;
}
static bool center(lv_obj_t* o, int* x, int* y)
{
    if (!o) return false;
    lv_obj_update_layout(o);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
    return true;
}
// Taps the clickable object that shows `text` (its label's parent when the label is not clickable).
static bool tapText(lv_obj_t* root, const char* text)
{
    lv_obj_t* l = findLabel(root, text, false, true);
    if (!l) {
        printf("  (no '%s' on screen)\n", text);
        return false;
    }
    int x, y;
    center(l, &x, &y);
    lvhost_tap(x, y);
    lvhost_run(80);
    return true;
}
// Taps the SEQUENCE footer button showing `text` (the lane column repeats the lane / pad name in
// a clickable box above, so the search is limited to the footer row).
static lv_obj_t* findBelow(lv_obj_t* root, const char* text, int ymin)
{
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char* t = lv_label_get_text(root);
        int x, y;
        if (t && strcmp(t, text) == 0 && center(root, &x, &y) && y >= ymin) return root;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i)
        if (lv_obj_t* f = findBelow(lv_obj_get_child(root, (int32_t)i), text, ymin)) return f;
    return nullptr;
}
static bool tapFooter(const char* text)
{
    int x, y;
    if (!center(findBelow(lv_screen_active(), text, 495), &x, &y)) {
        printf("  (no footer button '%s')\n", text);
        return false;
    }
    lvhost_tap(x, y);
    lvhost_run(80);
    return true;
}
static void longPress(int x, int y)
{
    lvhost_pointer(x, y, true);
    lvhost_run(650);
    lvhost_pointer(x, y, false);
    lvhost_run(120);
}
static int cellX(int c) { return step_grid::colX(c) + step_grid::colW(c) / 2; }
static int cellY(int r) { return step_grid::rowY(r) + step_grid::kCellH / 2; }
static const Scene& scene() { return app().project().scene_data[app().project().scene]; }
static uint32_t patternHash()
{
    uint32_t h = 2166136261u;
    const Scene& s = scene();
    for (int t = 0; t < kPads; ++t)
        for (int i = 0; i < kMaxSteps; ++i) h = (h ^ (uint32_t)(s.track[t].step[i].on * 131 + s.track[t].step[i].vel)) * 16777619u;
    return h;
}
static lv_display_t* g_disp = nullptr;
// Invalidated area bookkeeping (LVGL's pending list for the next refresh).
static void flushInvalid() { lv_refr_now(g_disp); }
static int invalidCount() { return (int)g_disp->inv_p; }
static long invalidPixels()
{
    long px = 0;
    for (uint32_t i = 0; i < g_disp->inv_p; ++i)
        if (!g_disp->inv_area_joined[i]) px += (long)lv_area_get_width(&g_disp->inv_areas[i]) * lv_area_get_height(&g_disp->inv_areas[i]);
    return px;
}

// Static context panels so the screenshots read like the real screen (their modules belong to
// other agents and are not linked here).
static void contextPanels(lv_obj_t* scr)
{
    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 1280, geo::top_h);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(top, c(col::top_a), 0);
    lv_obj_set_style_bg_grad_color(top, c(col::top_b), 0);
    lv_obj_set_style_bg_grad_dir(top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_color(top, c(col::top_line), 0);
    lv_obj_t* logo = label(top, "TAB5", f_b34, col::ink);
    lv_obj_set_pos(logo, 21, 12);
    lv_obj_t* p = panelCreate(scr, geo::pads_x, geo::pads_y, geo::pads_w, geo::pads_h);
    panelHead(p, Icon::Grid, "PADS");
    p = panelCreate(scr, geo::browser_x, geo::browser_y, geo::browser_w, geo::browser_h);
    panelHead(p, Icon::Grid, "BROWSER");
    panelCreate(scr, geo::macros_x, geo::macros_y, geo::macros_w, geo::macros_h);
    p = panelCreate(scr, geo::timeline_x, geo::timeline_y, geo::timeline_w, geo::timeline_h);
    panelHead(p, Icon::Grid, "SCENES");
}

// ------------------------------------------------------------------ SEQUENCE
static void testSequence()
{
    show(Shown::Seq);
    lv_obj_t* scr = lv_screen_active();
    app().setLane(StepLane::Trig);
    app().setSeqBank(0);
    app().setSeqPage(0);
    // the demo's scenes are 2 bars (0.4.9: at 54 % swing); these checks start from one bar at 54 %
    app().setSceneLength(16);
    app().setSwing(54);
    lvhost_run(300);
    CHECK(findLabel(scr, "PATTERN EDITOR / SCENE A") != nullptr);
    CHECK(findLabel(scr, "DRUM MACHINE \xC2\xB7 16 STEPS") != nullptr);
    CHECK(findLabel(scr, "KICK") != nullptr);
    CHECK(findLabel(scr, "PLAY \xE2\x96\xB6") != nullptr);
    CHECK(findLabel(scr, "SWING 54%") != nullptr || scene().swing != 54);
    lvhost_run(700);   // let ripples / toasts of the start settle
    shot("sequence");

    // ---- refresh() with nothing changed invalidates nothing
    flushInvalid();
    view_sequence::refresh();
    lvhost_run(40);   // timers run, LVGL refresh happens inside: check before by direct call
    flushInvalid();
    view_sequence::refresh();
    CHECK(invalidCount() == 0);
    // ---- one step changed -> only that cell is invalidated
    app().toggleStep(0, 7);
    view_sequence::refresh();
    CHECK(invalidCount() >= 1 && invalidCount() <= 2);
    CHECK(invalidPixels() <= (33 + 6) * (29 + 6) * 2);
    app().toggleStep(0, 7);
    flushInvalid();

    // ---- tap toggles a cell
    {
        const bool before = app().step(0, 1).on != 0;
        lvhost_tap(cellX(1), cellY(0));
        CHECK((app().step(0, 1).on != 0) == !before);
        lvhost_tap(cellX(1), cellY(0));
        CHECK((app().step(0, 1).on != 0) == before);
    }
    // ---- drag paints: the first cell decides, every crossed cell is set once
    {
        const Scene keep = scene();
        app().setStepOn(1, 2, false);
        lvhost_drag(cellX(2), cellY(1), cellX(9), cellY(1), 200);
        bool all = true;
        for (int s = 2; s <= 9; ++s) all &= app().step(1, s).on != 0;
        CHECK(all);
        lvhost_drag(cellX(9), cellY(1), cellX(2), cellY(1), 200);   // starts on an "on" cell: erases
        bool none = true;
        for (int s = 2; s <= 9; ++s) none &= app().step(1, s).on == 0;
        CHECK(none);
        // a diagonal sweep (fast: 60 ms) stays on the first cell's row: every crossed column of
        // row 3 is set, the rows below are not touched
        for (int r = 3; r <= 6; ++r)
            for (int s = 12; s < 16; ++s) app().setStepOn(r, s, false);
        lvhost_drag(cellX(12), cellY(3), cellX(15), cellY(6), 60);
        CHECK(app().step(3, 12).on && app().step(3, 13).on && app().step(3, 14).on && app().step(3, 15).on);
        {
            int below = 0;
            for (int r = 4; r <= 6; ++r)
                for (int s = 12; s < 16; ++s) below += app().step(r, s).on;
            CHECK(below == 0);
        }
        // the reported case: a 15-step swipe along KICK drifting 21 px down ends over SNARE; only
        // KICK is painted
        for (int s = 0; s < 16; ++s) {
            app().setStepOn(0, s, false);
            app().setStepOn(1, s, false);
        }
        lvhost_drag(cellX(0), cellY(0), cellX(15), cellY(0) + 21, 300);
        {
            int kick = 0, snare = 0;
            for (int s = 0; s < 16; ++s) {
                kick += app().step(0, s).on;
                snare += app().step(1, s).on;
            }
            printf("  drifting swipe: KICK %d steps, SNARE %d steps\n", kick, snare);
            CHECK(kick == 16 && snare == 0);
        }
        // a drag that starts out vertical paints that column (step 6 of rows 2..5)
        for (int r = 0; r < 8; ++r) {   // columns 6 and 7 empty (the demo's hats play every 16th)
            app().setStepOn(r, 5, false);
            app().setStepOn(r, 6, false);
        }
        lvhost_drag(cellX(5), cellY(2), cellX(5) + 6, cellY(5), 200);
        CHECK(app().step(2, 5).on && app().step(3, 5).on && app().step(4, 5).on && app().step(5, 5).on);
        CHECK(!app().step(1, 5).on && !app().step(6, 5).on && !app().step(2, 6).on);
        app().project().scene_data[app().project().scene] = keep;
        app().bump(&Revs::pattern);
    }
    // ---- long pad names stay on one line (dots), in the row label and in the track button
    {
        char old_name[kNameLen];
        snprintf(old_name, sizeof old_name, "%s", app().project().pad[0].name);
        app().renamePad(0, "Very Long Kick Name");
        lvhost_run(100);
        lv_obj_t* l = findLabel(scr, "VERY L", true);   // DOTS: "VERY LO…"
        CHECK(l != nullptr);
        if (l) CHECK(lv_obj_get_height(l) <= 20);
        lvhost_run(300);
        shot("long_name");
        app().renamePad(0, old_name);
        lvhost_run(100);
    }
    // ---- row label: tap selects the pad
    {
        lvhost_tap(360, cellY(2));
        CHECK(app().ui().pad == 2);
        lvhost_tap(360, cellY(0));
        CHECK(app().ui().pad == 0);
    }
    // ---- FILL is held
    {
        lv_obj_t* l = findLabel(scr, "FILL");
        int x, y;
        CHECK(center(l, &x, &y));
        lvhost_pointer(x, y, true);
        lvhost_run(150);
        CHECK(app().ui().fill == 1);
        lvhost_pointer(x, y, false);
        lvhost_run(100);
        CHECK(app().ui().fill == 0);
    }
    // ---- PLAY -> playhead outline, PAUSE
    {
        CHECK(!app().playing());
        CHECK(tapText(scr, "PLAY \xE2\x96\xB6"));
        CHECK(app().playing());
        lvhost_run(800);
        CHECK(findLabel(scr, "PAUSE \xE2\x85\xA1") != nullptr);
        const int st = app().playStep();
        CHECK(st >= 0);
        lvhost_run(40);
        int lit = 0;
        for (int r = 0; r < step_grid::kRows; ++r) lit += step_grid::playheadCol(r) >= 0;
        CHECK(lit == step_grid::kRows);
        shot("playing");
        // sparks: a step that sounds lights up under the playhead, then fades out behind it
        {
            int sr = -1, sc = -1;
            for (int k = 0; k < 200 && sr < 0; ++k) {
                lvhost_run(4);
                for (int r = 0; r < step_grid::kRows && sr < 0; ++r)
                    for (int c = 0; c < step_grid::kCols; ++c)
                        if (step_grid::sparkPct(r, c) >= 85) {
                            sr = r;
                            sc = c;
                            break;
                        }
            }
            CHECK(sr >= 0);
            if (sr >= 0) {
                CHECK(scene().track[app().ui().seq_bank * step_grid::kRows + sr].step[sc].on);
                CHECK(step_grid::playheadCol(sr) == sc || step_grid::playheadCol(sr) == sc - 1);
                shot("sparks_hit");
                lvhost_run(110);
                const int mid = step_grid::sparkPct(sr, sc);
                printf("  spark row %d col %d: %d%% after 110 ms\n", sr, sc, mid);
                CHECK(mid > 0 && mid < 85);
                shot("sparks_trail");
                lvhost_run(400);
                CHECK(step_grid::sparkPct(sr, sc) == 0 || step_grid::playheadCol(sr) == sc);   // faded (or hit again)
            }
            // Settings > Display > Animations off: no sparks at all
            const auto anim = app().settings().animations;
            app().settingsEdit().animations = 0;
            app().applySettings();
            lvhost_run(500);
            int any = 0;
            for (int r = 0; r < step_grid::kRows; ++r)
                for (int c = 0; c < step_grid::kCols; ++c) any += step_grid::sparkPct(r, c);
            CHECK(any == 0);
            app().settingsEdit().animations = anim;
            app().applySettings();
        }
        // polymeter: a 3-step track has its own playhead inside steps 1..3
        app().setTrackLength(0, 3);
        int seen[3] = {0, 0, 0};
        for (int k = 0; k < 40; ++k) {
            lvhost_run(25);
            const int c0 = step_grid::playheadCol(0);
            CHECK(c0 >= 0 && c0 < 3);
            if (c0 >= 0 && c0 < 3) seen[c0] = 1;
        }
        CHECK(seen[0] && seen[1] && seen[2]);
        app().setTrackLength(0, 0);
        // editing another scene while this one plays: no playhead until the queued switch happens
        app().selectScene(1);
        lvhost_run(40);
        if (app().playScene() != 1) CHECK(step_grid::playheadCol(0) == -1);
        lvhost_run(2200);   // past the bar line: scene B plays and is edited -> playhead back
        CHECK(app().playScene() == 1);
        CHECK(step_grid::playheadCol(0) >= 0);
        app().selectScene(0);
        lvhost_run(2200);
        CHECK(tapText(scr, "PAUSE \xE2\x85\xA1"));
        CHECK(!app().playing());
        lvhost_run(100);
        CHECK(step_grid::playheadCol(0) == -1);
    }
    // ---- RANDOM changes the pattern and turns CLEAR into UNDO (a stray tap next to FILL is undoable)
    const Scene saved = scene();
    {
        const uint32_t h = patternHash(), rev = app().revs().pattern;
        CHECK(tapText(scr, "RANDOM"));
        CHECK(app().revs().pattern != rev);
        CHECK(patternHash() != h);
        lvhost_run(100);
        CHECK(app().canUndoPattern());
        CHECK(findLabel(scr, "UNDO") != nullptr && findLabel(scr, "CLEAR") == nullptr);
        CHECK(findLabel(lv_layer_top(), "tap UNDO", true) != nullptr);
        CHECK(tapText(scr, "UNDO"));   // restores, never clears
        CHECK(patternHash() == h);
        CHECK(!app().canUndoPattern());
        lvhost_run(100);
        CHECK(findLabel(scr, "CLEAR") != nullptr);
        CHECK(findLabel(lv_layer_top(), "Undid the change of scene A", true) != nullptr);
        // the UNDO window ends after 8 s
        CHECK(tapText(scr, "RANDOM"));
        lvhost_run(7000);
        CHECK(findLabel(scr, "UNDO") != nullptr);
        lvhost_run(1200);
        CHECK(findLabel(scr, "CLEAR") != nullptr && findLabel(scr, "UNDO") == nullptr);
        app().project().scene_data[app().project().scene] = saved;
        app().bump(&Revs::pattern);
        lvhost_run(3000);   // toast gone
    }
    // ---- CLEAR empties the scene, toast + UNDO
    {
        const uint32_t h = patternHash();
        CHECK(tapText(scr, "CLEAR"));
        int on = 0;
        for (int t = 0; t < kPads; ++t)
            for (int i = 0; i < kMaxSteps; ++i) on += scene().track[t].step[i].on;
        CHECK(on == 0);
        lvhost_run(300);
        CHECK(findLabel(lv_layer_top(), "Cleared scene A", true) != nullptr);
        CHECK(findLabel(scr, "UNDO") != nullptr);
        // UNDO after a scene switch restores the scene that was cleared, and says so
        app().selectScene(1);
        lvhost_run(100);
        // "UNDO A" with App::undoPatternScene(), plain "UNDO" without it
        lv_obj_t* u = findLabel(scr, "UNDO A");
        CHECK(u != nullptr || findLabel(scr, "UNDO") != nullptr);
        CHECK(tapText(scr, u ? "UNDO A" : "UNDO"));
        CHECK(app().project().scene == 1);
        lvhost_run(100);
        CHECK(findLabel(lv_layer_top(), "Undid the change of scene A", true) != nullptr);
        CHECK(findLabel(lv_layer_top(), "Restored scene", true) == nullptr);
        app().selectScene(0);
        lvhost_run(100);
        CHECK(patternHash() == h);
        app().project().scene_data[app().project().scene] = saved;
        app().bump(&Revs::pattern);
        lvhost_run(3000);   // toast gone
    }
    // ---- SWING: popup list and vertical drag (+-1 % per 6 px)
    {
        CHECK(tapText(scr, "SWING 54%"));
        lvhost_run(100);
        CHECK(tapText(lv_layer_top(), "62%"));
        CHECK(scene().swing == 62);
        lv_obj_t* l = findLabel(scr, "SWING 62%");
        int x, y;
        CHECK(center(l, &x, &y));
        lvhost_drag(x, y, x, y - 40, 300);
        printf("  swing after drag up 40 px: %d\n", (int)scene().swing);
        CHECK(scene().swing >= 66 && scene().swing <= 69);
        CHECK(lv_obj_get_child_count(lv_layer_top()) <= 1);   // the drag did not open the list
        app().setSwing(54);
    }
    // ---- division
    {
        lvhost_run(50);
        CHECK(tapText(scr, "1/16 \xE2\x96\xBE"));
        CHECK(tapText(lv_layer_top(), "1/8T"));
        CHECK(scene().div == Division::D1_8T);
        app().setDivision(Division::D1_16);
    }
    // ---- 32 steps -> BAR 1/2 ▾ (one 34 px button with the list of bars), bar 2
    {
        lvhost_run(50);
        CHECK(findLabel(scr, "BAR 1/2 \xE2\x96\xBE") == nullptr);   // one bar: no BAR button
        CHECK(tapText(scr, "16 STEPS"));
        CHECK(tapText(lv_layer_top(), "32 STEPS"));
        CHECK(scene().length == 32);
        lvhost_run(50);
        CHECK(findLabel(scr, "DRUM MACHINE \xC2\xB7 32 STEPS \xC2\xB7 BAR 1/2") != nullptr);
        CHECK(tapText(scr, "BAR 1/2 \xE2\x96\xBE"));
        CHECK(tapText(lv_layer_top(), "BAR 2"));
        CHECK(app().ui().seq_page == 1);
        lvhost_run(50);
        CHECK(findLabel(scr, "BAR 2/2 \xE2\x96\xBE") != nullptr);
        lvhost_run(50);
        CHECK(findLabel(scr, "DRUM MACHINE \xC2\xB7 32 STEPS \xC2\xB7 BAR 2/2") != nullptr);
        const bool b = app().step(0, 16 + 4).on != 0;
        lvhost_tap(cellX(4), cellY(0));
        CHECK((app().step(0, 20).on != 0) == !b);
        app().setStepOn(0, 18, true);
        app().setStepOn(2, 23, true);
        lvhost_run(500);
        shot("bar2");
    }
    // ---- bank C1..D4
    {
        CHECK(tapText(scr, "C1\xE2\x80\x93" "D4"));
        CHECK(app().ui().seq_bank == 1);
        lvhost_run(50);
        char nm[32];
        snprintf(nm, sizeof nm, "%s", app().project().pad[8].name);
        for (char* p = nm; *p; ++p)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        CHECK(findLabel(scr, nm) != nullptr);
        const bool b = app().step(9, 16 + 3).on != 0;
        lvhost_tap(cellX(3), cellY(1));   // row 1 of bank 2 = pad 9 (B2... C2)
        CHECK((app().step(9, 19).on != 0) == !b);
        lvhost_run(500);
        shot("bank2");
        CHECK(tapText(scr, "A1\xE2\x80\x93" "B4"));
        CHECK(app().ui().seq_bank == 0);
        app().setSeqPage(0);
        app().setSceneLength(16);
    }
    // ---- VELOCITY lane: a sweep from bottom-left to top-right writes a ramp
    {
        lvhost_run(50);
        CHECK(tapFooter("VELOCITY"));
        CHECK(app().ui().lane == StepLane::Velocity);
        lvhost_run(50);
        CHECK(findLabel(scr, "VELOCITY \xC2\xB7 KICK") != nullptr);
        lvhost_drag(cellX(0), step_grid::kLaneBarsY1 - 20, cellX(15), step_grid::kLaneBarsY0, 300);
        const int v0 = app().stepLaneValue(0, 0, StepLane::Velocity);
        const int v8 = app().stepLaneValue(0, 8, StepLane::Velocity);
        const int v15 = app().stepLaneValue(0, 15, StepLane::Velocity);
        printf("  velocity ramp: %d %d %d\n", v0, v8, v15);
        CHECK(v0 < v8 && v8 < v15 && v15 >= 120 && v0 <= 20);
        // vertical drag on one column
        lvhost_drag(cellX(5), step_grid::kLaneBarsY0 + 10, cellX(5), (step_grid::kLaneBarsY0 + step_grid::kLaneBarsY1) / 2, 200);
        const int v5 = app().stepLaneValue(0, 5, StepLane::Velocity);
        CHECK(v5 > 55 && v5 < 72);
        lvhost_run(500);
        shot("lane_velocity");
        CHECK(tapFooter("VELOCITY"));   // tapping the active lane returns to TRIG
        CHECK(app().ui().lane == StepLane::Trig);
    }
    // ---- PROBABILITY lane
    {
        CHECK(tapFooter("PROBABILITY"));
        CHECK(app().ui().lane == StepLane::Probability);
        lvhost_tap(cellX(4), (step_grid::kLaneBarsY0 + step_grid::kLaneBarsY1) / 2);
        const int p4 = app().stepLaneValue(0, 4, StepLane::Probability);
        CHECK(p4 >= 45 && p4 <= 55);
        lvhost_tap(cellX(12), step_grid::kLaneBarsY1 - 62);
        lvhost_run(500);
        shot("lane_probability");
    }
    // ---- COND lane through MORE: a tap opens the list of conditions (never changes the step by
    // itself), a vertical drag sets the condition like the other lanes
    {
        CHECK(tapText(scr, "MORE \xE2\x96\xBE"));
        CHECK(tapText(lv_layer_top(), "COND"));
        CHECK(app().ui().lane == StepLane::Cond);
        lvhost_run(50);
        CHECK(findLabel(scr, "COND \xE2\x96\xBE") != nullptr);
        app().setStepLane(0, 4, StepLane::Cond, 0);
        lvhost_tap(cellX(4), 300);
        CHECK(app().stepLaneValue(0, 4, StepLane::Cond) == 0);   // only the list opened
        CHECK(findLabel(lv_layer_top(), "!FILL") != nullptr && findLabel(lv_layer_top(), "ALWAYS") != nullptr);
        CHECK(tapText(lv_layer_top(), "FILL"));
        CHECK(app().stepLaneValue(0, 4, StepLane::Cond) == (int)TrigCond::Fill);
        lvhost_tap(cellX(4), 300);   // FILL -> back to always: one pick, not 3 more taps
        CHECK(tapText(lv_layer_top(), "ALWAYS"));
        CHECK(app().stepLaneValue(0, 4, StepLane::Cond) == (int)TrigCond::Always);
        lvhost_tap(cellX(4), 300);
        CHECK(tapText(lv_layer_top(), "FILL"));
        // drag up on step 9: bottom = always, top = FIRST
        app().setStepLane(0, 8, StepLane::Cond, 0);
        lvhost_drag(cellX(8), step_grid::kLaneBarsY1 - 5, cellX(8), step_grid::kLaneBarsY0 + 3, 250);
        CHECK(app().stepLaneValue(0, 8, StepLane::Cond) == (int)TrigCond::First);
        CHECK(lv_obj_get_child_count(lv_layer_top()) <= 1);   // the drag opened no list
        lvhost_drag(cellX(12), step_grid::kLaneBarsY1 - 5, cellX(12), step_grid::kLaneBarsY1 - 40, 200);
        const int c12 = app().stepLaneValue(0, 12, StepLane::Cond);
        printf("  COND drag up 35 px on step 13: %s\n", trigCondName((TrigCond)c12));
        CHECK(c12 >= 1 && c12 <= 2);
        lvhost_run(3000);
        shot("lane_cond");
        // the other MORE lanes
        CHECK(tapText(scr, "COND \xE2\x96\xBE"));
        CHECK(tapText(lv_layer_top(), "RATCHET"));
        CHECK(app().ui().lane == StepLane::Ratchet);
        lvhost_tap(cellX(8), step_grid::kLaneBarsY0 + 5);
        CHECK(app().stepLaneValue(0, 8, StepLane::Ratchet) == 4);
        CHECK(tapText(scr, "RATCHET \xE2\x96\xBE"));
        CHECK(tapText(lv_layer_top(), "PITCH"));
        CHECK(app().ui().lane == StepLane::Pitch);
        lvhost_tap(cellX(0), step_grid::kLaneBarsY0 + 30);
        CHECK(app().stepLaneValue(0, 0, StepLane::Pitch) > 15);
        lvhost_run(500);
        shot("lane_pitch");
        // the track name in the label column lists the pads: another track's lane, silently
        {
            lvhost_tap(370, step_grid::kRowY0 + 10);
            lvhost_run(100);
            char item[48];
            snprintf(item, sizeof item, "A2  %s", app().project().pad[1].name);
            for (char* p = item; *p; ++p)
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            CHECK(findLabel(lv_layer_top(), "A1  ", true) != nullptr && findLabel(lv_layer_top(), "D4  ", true) != nullptr);
            lvhost_run(300);
            shot("lane_pad_list");
            CHECK(tapText(lv_layer_top(), item));
            CHECK(app().ui().pad == 1 && app().ui().lane == StepLane::Pitch);
            lvhost_run(50);
            char head[64];
            snprintf(head, sizeof head, "PITCH \xC2\xB7 %s", item + 4);
            CHECK(findLabel(scr, head) != nullptr);
            app().selectPad(0, false);
            lvhost_run(50);
        }
        // the pad name button returns to TRIG
        char nm[32];
        snprintf(nm, sizeof nm, "%s", "KICK");
        CHECK(tapFooter(nm));
        CHECK(app().ui().lane == StepLane::Trig);
    }
    // ---- trig marks + track menu (long press on the row label)
    {
        lvhost_run(50);
        CHECK(tapFooter("VELOCITY") && tapFooter("VELOCITY"));   // on and off again
        longPress(360, cellY(0));
        CHECK(findLabel(lv_layer_top(), "CLEAR TRACK") != nullptr);
        CHECK(findLabel(lv_layer_top(), "MUTE") != nullptr);
        lvhost_run(300);
        shot("track_popup");
        CHECK(tapText(lv_layer_top(), "LENGTH \xE2\x86\x92"));
        lvhost_run(200);
        CHECK(findLabel(lv_layer_top(), "FOLLOW") != nullptr);
        shot("track_length");
        CHECK(tapText(lv_layer_top(), "12"));
        CHECK(scene().track[0].length == 12);
        longPress(360, cellY(0));
        CHECK(tapText(lv_layer_top(), "MUTE"));
        CHECK(app().project().pad[0].mute == 1);
        lvhost_run(3000);
        shot("track_len12_muted");
        longPress(360, cellY(0));
        CHECK(tapText(lv_layer_top(), "UNMUTE"));
        CHECK(app().project().pad[0].mute == 0);
        app().setStepOn(3, 2, true);
        const uint32_t before_clear = patternHash();
        longPress(360, cellY(3));
        CHECK(tapText(lv_layer_top(), "CLEAR TRACK"));
        int on = 0;
        for (int i = 0; i < kMaxSteps; ++i) on += scene().track[3].step[i].on;
        CHECK(on == 0);
        lvhost_run(50);
        CHECK(tapText(scr, "UNDO"));   // CLEAR TRACK is undoable from CLEAR -> UNDO
        CHECK(patternHash() == before_clear);
        longPress(360, cellY(3));
        CHECK(tapText(lv_layer_top(), "CLEAR TRACK"));
        longPress(360, cellY(0));
        CHECK(tapText(lv_layer_top(), "LENGTH \xE2\x86\x92"));
        CHECK(tapText(lv_layer_top(), "FOLLOW"));
        CHECK(scene().track[0].length == 0);
        app().project().scene_data[app().project().scene] = saved;
        app().bump(&Revs::pattern);
        lvhost_run(3000);
    }
}

// ------------------------------------------------------------------ MIXER
static void testMixer()
{
    show(Shown::Mix);
    lv_obj_t* scr = lv_screen_active();
    CHECK(findLabel(scr, "8-CHANNEL MIXER") != nullptr);
    CHECK(findLabel(scr, "MASTER") != nullptr);
    CHECK(findLabel(scr, app().project().bus[0].name) != nullptr);
    app().play();
    const Telemetry& t = app().engine().telemetry();
    float mx = 0.f, mm = 0.f;
    for (int k = 0; k < 75; ++k) {   // 1.5 s, peaks sampled like the 30 Hz meters
        lvhost_run(20);
        for (int i = 0; i < kBuses; ++i) mx = fmaxf(mx, t.bus_peak[i].load());
        mm = fmaxf(mm, t.master_peak[0].load());
    }
    printf("  max bus peak while playing: %.3f, master %.3f\n", (double)mx, (double)mm);
    CHECK(mx > 0.01f);
    shot("mixer");
    // fader drag: strip 1 up 60 px
    {
        const float before = app().project().bus[1].level;
        const int x = 408 + 31 + 12;
        const int y = 151 + 141;
        lvhost_drag(x, y, x, y - 60, 300);
        CHECK(app().project().bus[1].level > before + 0.1f);
        char buf[24];
        app().paramText(ParamRef::busLevel(1), buf, sizeof buf);
        lvhost_run(50);
        CHECK(findLabel(scr, buf) != nullptr);
    }
    // M / S
    {
        lvhost_tap(328 + 10 + 12, 457 + 12);
        CHECK(app().project().bus[0].mute == 1);
        lvhost_tap(408 + 38 + 12, 457 + 12);
        CHECK(app().project().bus[1].solo == 1);
        lvhost_run(500);
        shot("mixer_mute_solo");
        lvhost_tap(328 + 10 + 12, 457 + 12);
        lvhost_tap(408 + 38 + 12, 457 + 12);
        CHECK(app().project().bus[0].mute == 0 && app().project().bus[1].solo == 0);
    }
    // MASTER L (limiter) / C (metronome)
    {
        const int lim = app().project().limiter_on;
        lvhost_tap(967 + 10 + 12, 457 + 12);
        CHECK(app().project().limiter_on == !lim);
        lvhost_tap(967 + 10 + 12, 457 + 12);
        CHECK(app().project().limiter_on == lim);
        const int met = app().project().metronome;
        lvhost_tap(967 + 38 + 12, 457 + 12);
        CHECK(app().project().metronome == !met);
        lvhost_tap(967 + 38 + 12, 457 + 12);
        CHECK(app().project().metronome == met);
    }
    // M / S hit areas do not overlap: the right edge of M is M (the old 25 px buttons with 8 px
    // margins gave it to S), the left edge of S is S; on MASTER the right edge of L is L, not C
    {
        const int x0 = 328;   // strip 0
        lvhost_tap(x0 + 34, 470);
        CHECK(app().project().bus[0].mute == 1 && app().project().bus[0].solo == 0);
        lvhost_tap(x0 + 34, 470);
        CHECK(app().project().bus[0].mute == 0 && app().project().bus[0].solo == 0);
        lvhost_tap(x0 + 38, 470);
        CHECK(app().project().bus[0].solo == 1 && app().project().bus[0].mute == 0);
        lvhost_tap(x0 + 38, 470);
        CHECK(app().project().bus[0].solo == 0);
        const int lim = app().project().limiter_on, met = app().project().metronome;
        lvhost_tap(967 + 34, 470);
        CHECK(app().project().limiter_on == !lim && app().project().metronome == met);
        lvhost_tap(967 + 34, 470);
        CHECK(app().project().limiter_on == lim && app().project().metronome == met);
    }
    // fader double tap: default level (0 dB)
    {
        const int x = 408 + 31 + 12, y = 151 + 100;
        CHECK(fabsf(app().project().bus[1].level - 0.7071f) > 0.01f);   // moved by the drag above
        lvhost_double_tap(x, y);
        lvhost_run(50);
        CHECK(fabsf(app().project().bus[1].level - 0.7071f) < 0.002f);
    }
    // bus PAN: horizontal drag (the readout shows the pan meanwhile), double tap -> centre
    {
        const int sx = 328 + (2 * 719 + 4) / 9;   // strip 2
        const int px = sx + 7 + 29, py = 489 + 12;
        CHECK(fabsf(app().project().bus[2].pan) < 1e-4f);
        lvhost_pointer(px, py, true);
        lvhost_run(40);
        for (int k = 1; k <= 10; ++k) {
            lvhost_pointer(px - 4 * k, py, true);
            lvhost_run(10);
        }
        lvhost_run(30);
        char buf[24];
        app().paramText(ParamRef::busPan(2), buf, sizeof buf);
        printf("  bus 3 pan after 40 px left: %.2f (%s)\n", (double)app().project().bus[2].pan, buf);
        CHECK(app().project().bus[2].pan < -0.6f);
        CHECK(findLabel(scr, buf) != nullptr);   // readout = pan while dragging
        lvhost_pointer(px - 40, py, false);
        lvhost_run(100);
        app().paramText(ParamRef::busLevel(2), buf, sizeof buf);
        CHECK(findLabel(scr, buf) != nullptr);   // back to dB
        lvhost_run(500);
        shot("mixer_pan");
        lvhost_double_tap(px - 20, py);
        lvhost_run(50);
        CHECK(fabsf(app().project().bus[2].pan) < 1e-4f);
        // a small drag near the centre snaps to C
        lvhost_drag(px, py, px + 1, py, 100);
        CHECK(fabsf(app().project().bus[2].pan) < 1e-4f);
    }
    // meters off in Settings -> meters fall to zero (visual only; checked by screenshot)
    app().stop();
    lvhost_run(3000);
}

// ------------------------------------------------------------------ FX
static void testFx()
{
    show(Shown::Fx);
    lv_obj_t* scr = lv_screen_active();
    for (int i = 0; i < 4; ++i)
        if (app().macro(i)) app().toggleMacro(i);
    lvhost_run(3000);
    CHECK(findLabel(scr, "FX & MASTER") != nullptr);
    CHECK(findLabel(scr, "REVERB") != nullptr && findLabel(scr, "DELAY") != nullptr);
    CHECK(findLabel(scr, "MACROS") != nullptr && findLabel(scr, "LIMITER") != nullptr);
    shot("fx");
    // macros on: LO-FI and PUMP
    CHECK(tapText(scr, "LO-FI"));
    CHECK(app().macro(0));
    CHECK(tapText(scr, "PUMP"));
    CHECK(app().macro(2));
    // knob drag: reverb SIZE up
    {
        lv_obj_t* l = findLabel(scr, "SIZE");
        int x, y;
        CHECK(center(l, &x, &y));
        const float before = app().paramGet(ParamRef::global(GP_REV_SIZE));
        lvhost_drag(x, y + 40, x, y - 20, 300);
        CHECK(app().paramGet(ParamRef::global(GP_REV_SIZE)) > before + 0.1f);
    }
    // PING-PONG, LIMITER, METRONOME toggles
    {
        const bool pp = app().paramGet(ParamRef::global(GP_DLY_PINGPONG)) >= 0.5f;
        lvhost_tap(708 + 1 + 330 - 12 - 22, 118 + 1 + 4 + 12);
        CHECK((app().paramGet(ParamRef::global(GP_DLY_PINGPONG)) >= 0.5f) == !pp);
        const int lim = app().project().limiter_on;
        lvhost_tap(848 + 1 + 190 - 12 - 22, 258 + 1 + 35 + 12);
        CHECK(app().project().limiter_on == !lim);
        lvhost_tap(848 + 1 + 190 - 12 - 22, 258 + 1 + 35 + 12);
        CHECK(app().project().limiter_on == lim);
        const int met = app().project().metronome;
        lvhost_tap(848 + 1 + 190 - 12 - 22, 258 + 1 + 240 + 12);
        CHECK(app().project().metronome == !met);
        lvhost_tap(848 + 1 + 190 - 12 - 22, 258 + 1 + 240 + 12);
    }
    // PUMP source select
    {
        char cur[40];
        snprintf(cur, sizeof cur, "%c%d %s", 'A' + app().project().pump_src / 4, app().project().pump_src % 4 + 1,
                 app().project().pad[app().project().pump_src].name);
        CHECK(tapText(scr, cur));
        char want[40];
        snprintf(want, sizeof want, "A3 %s", app().project().pad[2].name);
        CHECK(tapText(lv_layer_top(), want));
        CHECK(app().project().pump_src == 2);
    }
    app().play();
    lvhost_run(1200);
    app().stop();
    lvhost_run(200);
    shot("fx_macros");
    CHECK(tapText(scr, "LO-FI"));
    CHECK(!app().macro(0));
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // an LVGL assert (abort) keeps the lines printed before it
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    char sd[600];
    snprintf(sd, sizeof sd, "%s/sdcard", cwd);
    remove("host/out/seqmix_settings.bin");
    halHostSetSettingsPath("host/out/seqmix_settings.bin");
    halHostSetSdRoot(sd);

    g_disp = lvhost_init(1280, 720);
    themeInit();
    CHECK(app().init(sd, 48000));
    CHECK(hal::audioStart(48000, 128, &App::render, &app().engine()));
    app().tick();

    lv_obj_t* scr = lv_screen_active();
    contextPanels(scr);
    lv_obj_t* work = panelCreate(scr, geo::work_x, geo::work_y, geo::work_w, geo::work_h);
    g_view[0] = view_sequence::create(work);
    g_view[1] = view_mixer::create(work);
    g_view[2] = view_fx::create(work);
    CHECK(g_view[0] && g_view[1] && g_view[2]);
    g_audio_ms = lvhost_ms();
    lv_timer_create(frameTimer, 16, nullptr);
    lvhost_run(500);

    testSequence();
    testMixer();
    testFx();

    hal::audioStop();
    printf("%d checks, %d failed\n", g_checks, g_fails);
    printf(g_fails ? "shot_seqmix: FAILED\n" : "shot_seqmix: passed\n");
    return g_fails ? 1 : 0;
}
