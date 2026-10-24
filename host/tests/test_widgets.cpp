// Build (Git Bash, project root; first: bash host/build_lvgl.sh):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -DLV_CONF_INCLUDE_SIMPLE -Ifirmware/main -Ihost -Ifirmware/managed_components/lvgl__lvgl host/tests/test_widgets.cpp host/tests/fake_app.cpp host/lvgl_host.cpp firmware/main/ui/theme.cpp firmware/main/ui/widgets.cpp firmware/main/ui/widgets_knob.cpp firmware/main/ui/widgets_input.cpp firmware/main/ui/widgets_overlay.cpp firmware/main/core/params.cpp firmware/main/core/settings.cpp host/lib/liborbit_assets.a host/lib/liblvgl.a -o host/bin/test_widgets.exe && host/bin/test_widgets.exe
//
// Widget kit gallery + interaction tests on the headless LVGL host. Writes host/shots/widgets_*.png:
//   gallery (every widget in several states), replica (editor + macro panel at prototype
//   coordinates, compare with reference/shots/view-sample.png), icons, fonts, overlays (modal,
//   toast, select popup) and keyboard.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/settings.h"
#include "lvgl_host.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using namespace orbit;
using namespace orbit::ui;

static int g_fails = 0;
static int g_checks = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                      \
            ++g_fails;                                                              \
        }                                                                           \
    } while (0)

static lv_obj_t* scr() { return lv_screen_active(); }

static void reset()
{
    lv_obj_clean(lv_layer_top());
    lv_obj_clean(scr());
    lvhost_pointer(0, 0, false);
    lvhost_run(50);
}

static void center(lv_obj_t* o, int* x, int* y)
{
    lv_obj_update_layout(o);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

static void shot(const char* name)
{
    char path[160];
    snprintf(path, sizeof path, "host/shots/widgets_%s.png", name);
    CHECK(lvhost_screenshot(path));
}

static int g_inv = 0;
static int64_t g_inv_px = 0;
static void invCb(lv_event_t* e)
{
    const lv_area_t* a = (const lv_area_t*)lv_event_get_param(e);
    ++g_inv;
    if (a) g_inv_px += (int64_t)lv_area_get_width(a) * lv_area_get_height(a);
}

// The toast: the non-clickable child of the top layer whose first child is a label.
static lv_obj_t* toastObj()
{
    lv_obj_t* top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); ++i) {
        lv_obj_t* o = lv_obj_get_child(top, (int32_t)i);
        lv_obj_t* l = lv_obj_get_child(o, 0);
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && l && lv_obj_check_type(l, &lv_label_class)) return o;
    }
    return nullptr;
}
static bool toastVisible()
{
    lv_obj_t* t = toastObj();
    return t && !lv_obj_has_flag(t, LV_OBJ_FLAG_HIDDEN);
}
static const char* toastText()
{
    lv_obj_t* t = toastObj();
    return t ? lv_label_get_text(lv_obj_get_child(t, 0)) : "";
}

// perf-12: the first pressFeedbackSync() (ui::build calls it before the first frame) must not
// restyle every button when the theme's press style already matches Settings.
static void presyncTests()
{
    lv_display_t* d = lv_display_get_default();
    lv_obj_t* r = lv_obj_create(scr());
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 1280, 720);
    for (int i = 0; i < 24; ++i) {
        lv_obj_t* b = actionBtn(r, "BTN", false, 60, 34);
        lv_obj_set_pos(b, 20 + (i % 8) * 80, 20 + (i / 8) * 50);
    }
    lvhost_run(100);
    lv_display_add_event_cb(d, invCb, LV_EVENT_INVALIDATE_AREA, nullptr);
    g_inv = 0;
    g_inv_px = 0;
    pressFeedbackSync();
    lvhost_run(50);
    printf("press feedback presync: %d invalidations, %lld px\n", g_inv, (long long)g_inv_px);
    CHECK(g_inv == 0);
    lv_display_remove_event_cb_with_user_data(d, invCb, nullptr);
    lv_obj_delete(r);
    lvhost_run(50);
}

static lv_obj_t* row(lv_obj_t* parent, int x, int y, int gap)
{
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

// Places a knob so that the knob body's top-left lands at (kx, ky) (prototype coordinates).
static lv_obj_t* knobAt(KnobLook look, int d, ParamRef ref, const char* cap, int kx, int ky)
{
    lv_obj_t* w = knobCreate(scr(), look, d, ref, cap);
    lv_obj_update_layout(w);
    lv_obj_t* body = lv_obj_get_child(w, 1);
    const int bx = lv_obj_get_x(body), by = lv_obj_get_y(body);
    lv_obj_set_pos(w, kx - bx, ky - by);
    return w;
}

// ---------------------------------------------------------------- gallery
static int g_toggle_cb = -1;
static void onToggle(bool on, void*) { g_toggle_cb = on ? 1 : 0; }
static int g_select_cb = -1;
static void onSelect(int i, void*) { g_select_cb = i; }

static void gallery()
{
    reset();
    lv_obj_t* p1 = panelCreate(scr(), 14, 14, 700, 330);
    lv_obj_t* right = panelHead(p1, Icon::Grid, "KNOBS");
    microBtn(right, "MUTE", 43, 24);
    lv_obj_t* lit = microBtn(right, "SOLO", 43, 24);
    btnSetActive(lit, true);
    const float vals[5] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
    for (int i = 0; i < 5; ++i) {
        const ParamRef r = ParamRef::pad(PP_DECAY, i);
        app().paramSetNorm(r, vals[i]);
        knobAt(KnobLook::Dark, 53, r, nullptr, 40 + i * 82, 80);
    }
    {
        const ParamRef r = ParamRef::pad(PP_PAN, 5);
        app().paramSet(r, -0.4f);
        knobAt(KnobLook::Dark, 56, r, nullptr, 450, 80);
        const ParamRef t = ParamRef::pad(PP_TUNE, 5);
        app().paramSet(t, 7.f);
        knobAt(KnobLook::Dark, 56, t, nullptr, 540, 80);
    }
    for (int i = 0; i < 4; ++i) {
        const ParamRef r = ParamRef::pad(PP_SEND, 6 + i);
        app().paramSetNorm(r, vals[i + 1]);
        knobAt(i < 2 ? KnobLook::White : KnobLook::White, i < 2 ? 69 : 78, r, i == 0 ? "VOLUME" : nullptr,
               40 + i * 130, 210);
    }
    {
        const ParamRef r = ParamRef::pad(PP_PAN, 10);
        app().paramSet(r, 0.f);
        knobAt(KnobLook::White, 69, r, nullptr, 560, 210);
    }

    lv_obj_t* p2 = panelCreate(scr(), 724, 14, 542, 330);
    panelHead(p2, Icon::Mixer, "FADERS / METERS");
    const float lv[4] = {0.15f, 0.5f, 0.8f, 1.f};
    for (int i = 0; i < 4; ++i) {
        const ParamRef r = ParamRef::busLevel(i);
        app().paramSetNorm(r, lv[i]);
        lv_obj_t* m = meterCreate(scr(), 8, 240);
        lv_obj_set_pos(m, 750 + i * 60, 80);
        meterSet(m, powf(10.f, (-48.f + 51.f * lv[i]) / 20.f));
        lv_obj_t* f = faderCreate(scr(), 24, 240, r);
        lv_obj_set_pos(f, 764 + i * 60, 80);
    }
    lv_obj_t* tg1 = toggleCreate(scr(), false, onToggle, nullptr);
    lv_obj_set_pos(tg1, 1010, 80);
    lv_obj_t* tg2 = toggleCreate(scr(), true, nullptr, nullptr);
    lv_obj_set_pos(tg2, 1070, 80);
    static const char* const opts[] = {"Internal codec / ES8388", "USB audio", "Headphones"};
    lv_obj_t* s1 = selectCreate(scr(), 240, 38, opts, 3, 0, onSelect, nullptr);
    lv_obj_set_pos(s1, 1010, 120);
    static const char* const grid[] = {"1/16", "1/8", "1/4"};
    lv_obj_t* s2 = selectCreate(scr(), 95, 24, grid, 3, 0, nullptr, nullptr);
    lv_obj_set_pos(s2, 1010, 172);
    lv_obj_t* hs = hsliderCreate(scr(), 200, 0, 100, 72, nullptr, nullptr);
    lv_obj_set_pos(hs, 1020, 222);
    lv_obj_t* tf = textFieldCreate(scr(), 240, "Projekt żółwia 01", 32, nullptr, nullptr, "PROJECT NAME");
    lv_obj_set_pos(tf, 1010, 250);
    lv_obj_t* tf2 = textFieldCreate(scr(), 240, "", 32, nullptr, nullptr, "Szukaj próbek…");
    lv_obj_set_pos(tf2, 1010, 294);

    lv_obj_t* p3 = panelCreate(scr(), 14, 354, 1252, 352);
    panelHead(p3, Icon::Layers, "BUTTONS / TABS / LABELS");
    lv_obj_t* r1 = row(scr(), 34, 410, 10);
    actionBtn(r1, "FOLLOW", false, 95, 34);
    actionBtn(r1, "APPLY", true, 0, 34);
    lv_obj_t* act = actionBtn(r1, "ACTIVE", false, 0, 34);
    btnSetActive(act, true);
    lv_obj_t* pressed_btn = actionBtn(r1, "PRESSED", false, 0, 34);
    lv_obj_t* os = actionBtn(r1, "ONE SHOT ▾", false, 135, 37);
    lv_obj_set_style_bg_color(os, c(col::dark_btn), 0);
    lv_obj_set_style_text_color(os, c(0xFFFFFF), 0);
    lv_obj_set_style_border_width(os, 0, 0);
    microBtn(r1, "M");
    lv_obj_t* ms = microBtn(r1, "S");
    btnSetActive(ms, true);
    iconBtn(r1, Icon::Folder, 19, 31);
    lv_obj_t* ib = iconBtn(r1, Icon::Settings, 19, 31);
    btnSetActive(ib, true);
    iconBtn(r1, Icon::Pencil, 16, 30);
    lv_obj_t* r2 = row(scr(), 34, 470, 5);
    const char* tabs[] = {"TRIM", "SLICE", "ENVELOPE", "FILTER", "PITCH", "LFO"};
    lv_obj_t* tab_pressed = nullptr;
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* t = tabBtn(r2, tabs[i], 116, 32);
        if (i == 0) btnSetActive(t, true);
        if (i == 2) tab_pressed = t;
    }
    lv_obj_t* r3 = row(scr(), 34, 530, 18);
    overline(r3, "TRIM · EDIT MODE");
    overline(r3, "GRID");
    label(r3, "Kick 01", f_m24, col::ink);
    label(r3, "120 BPM", f_m16, col::ink);
    label(r3, "Wersja demonstracyjna interfejsu", f_r13, col::muted);
    lv_obj_t* wave = lv_obj_create(scr());
    lv_obj_remove_style_all(wave);
    lv_obj_set_style_bg_opa(wave, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(wave, c(col::wave_frame), 0);
    lv_obj_set_pos(wave, 34, 580);
    lv_obj_set_size(wave, 330, 34);
    lv_obj_t* wt = label(wave, "0.000s — 0.420s   44 100 Hz", f_mono12, 0xB0B1B2);
    lv_obj_align(wt, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_t* r4 = row(scr(), 34, 640, 12);
    for (int i = 0; i < 8; ++i) iconCreate(r4, (Icon)(i * 4), 22, i & 1 ? col::accent : col::ink);
    lvhost_run(400);

    // pressed looks: hold the pointer on a button and a tab
    int x, y;
    center(pressed_btn, &x, &y);
    lvhost_pointer(x, y, true);
    lvhost_run(200);
    shot("gallery");
    lvhost_pointer(x, y, false);
    lvhost_run(700);
    center(tab_pressed, &x, &y);
    lvhost_pointer(x, y, true);
    lvhost_run(150);
    shot("gallery_tab_pressed");
    lvhost_pointer(x, y, false);
    lvhost_run(700);

    // toggle flip
    center(tg1, &x, &y);
    g_toggle_cb = -1;
    lvhost_tap(x, y);
    lvhost_run(200);
    CHECK(g_toggle_cb == 1);
    CHECK(lv_obj_has_state(tg1, LV_STATE_CHECKED));
    lvhost_tap(x, y);
    lvhost_run(200);
    CHECK(g_toggle_cb == 0);
    toggleSet(tg1, true);
    CHECK(lv_obj_has_state(tg1, LV_STATE_CHECKED));

    // select popup choice
    const uint32_t top_before = lv_obj_get_child_count(lv_layer_top());
    center(s1, &x, &y);
    lvhost_tap(x, y);
    lvhost_run(100);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == top_before + 1);
    shot("select_open");
    // rows: 36 px + 2 px gap below the select (box pad 4 + border 1)
    lv_area_t sa;
    lv_obj_get_coords(s1, &sa);
    g_select_cb = -1;
    lvhost_tap(x, sa.y2 + 4 + 5 + 2 * 38 + 18);
    lvhost_run(100);
    CHECK(g_select_cb == 2);
    CHECK(strcmp(lv_label_get_text(lv_obj_get_child(s1, 0)), "Headphones") == 0);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == top_before);
    // tap outside closes without a choice
    lvhost_tap(x, y);
    lvhost_run(100);
    g_select_cb = -1;
    lvhost_tap(600, 100);
    lvhost_run(100);
    CHECK(g_select_cb == -1);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == top_before);
    selectSet(s1, 1);
    CHECK(strcmp(lv_label_get_text(lv_obj_get_child(s1, 0)), "USB audio") == 0);
}

// ---------------------------------------------------------------- replica of the SAMPLE view parts
static void replica()
{
    reset();
    panelCreate(scr(), 311, 73, 746, 486);
    panelCreate(scr(), 1066, 73, 200, 486);
    lv_obj_t* name = label(scr(), "Kick 01", f_m24, col::ink);
    lv_obj_set_pos(name, 330, 80);
    lv_obj_t* pen = iconBtn(scr(), Icon::Pencil, 16, 30);
    lv_obj_set_pos(pen, 411, 82);
    const char* tabs[] = {"TRIM", "SLICE", "ENVELOPE", "FILTER", "PITCH", "LFO"};
    const int tx[] = {324, 445, 565, 686, 807, 928};
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* t = tabBtn(scr(), tabs[i], 116, 32);
        lv_obj_set_pos(t, tx[i], 341);
        if (i == 0) btnSetActive(t, true);
    }
    const Icon tools[] = {Icon::Wave, Icon::Envelope, Icon::Slice};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* b = actionBtn(scr(), "", false, 39, 38);
        lv_obj_set_pos(b, 347 + i * 44, 397);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_t* ic = iconCreate(b, tools[i], 18, i == 0 ? col::accent : col::ink);
        lv_obj_center(ic);
        if (i == 0) lv_obj_set_style_border_color(b, c(col::accent), 0);
    }
    lv_obj_t* os = actionBtn(scr(), "ONE SHOT ▾", false, 135, 37);
    lv_obj_set_pos(os, 343, 443);
    lv_obj_set_style_bg_color(os, c(col::dark_btn), 0);
    lv_obj_set_style_text_color(os, c(0xFFFFFF), 0);
    lv_obj_t* ov = overline(scr(), "TRIM · EDIT MODE");
    lv_obj_set_pos(ov, 352, 491);
    const int dvx[2] = {516, 858};
    for (int x : dvx) {
        lv_obj_t* dv = lv_obj_create(scr());
        lv_obj_remove_style_all(dv);
        lv_obj_set_style_bg_opa(dv, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dv, c(0xC7C9CB), 0);
        lv_obj_set_pos(dv, x, 397);
        lv_obj_set_size(dv, 1, 114);
    }
    app().paramSet(ParamRef::pad(PP_ATTACK, 0), 50.f);
    app().paramSet(ParamRef::pad(PP_DECAY, 0), 256.f);
    app().paramSet(ParamRef::pad(PP_SUSTAIN, 0), 0.72f);
    app().paramSet(ParamRef::pad(PP_RELEASE, 0), 202.f);
    app().paramSet(ParamRef::pad(PP_CUTOFF, 0), 4200.f);
    app().paramSet(ParamRef::pad(PP_RESO, 0), 0.29f);
    app().paramSet(ParamRef::pad(PP_VOLUME, 0), 0.f);
    app().paramSet(ParamRef::pad(PP_SEND, 0), 0.3f);
    app().paramSet(ParamRef::pad(PP_PAN, 0), 0.f);
    const PadParam ps[] = {PP_ATTACK, PP_DECAY, PP_SUSTAIN, PP_RELEASE, PP_CUTOFF, PP_RESO};
    const int kx[] = {550, 624, 698, 772, 892, 971};
    for (int i = 0; i < 6; ++i) knobAt(KnobLook::Dark, 53, ParamRef::pad(ps[i], 0), nullptr, kx[i], 418);
    knobAt(KnobLook::White, 69, ParamRef::pad(PP_VOLUME, 0), "VOLUME", 1131, 108);
    knobAt(KnobLook::White, 69, ParamRef::pad(PP_SEND, 0), "FX SEND", 1131, 235);
    knobAt(KnobLook::White, 69, ParamRef::pad(PP_PAN, 0), "PAN", 1131, 361);
    // transport row
    lv_obj_t* line = lv_obj_create(scr());
    lv_obj_remove_style_all(line);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, c(0xC2C4C6), 0);
    lv_obj_set_pos(line, 1078, 467);
    lv_obj_set_size(line, 176, 1);
    const Icon tr[] = {Icon::Back, Icon::Play, Icon::Record};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* b = iconBtn(scr(), tr[i], 18, 33);
        lv_obj_set_size(b, 55, 33);
        lv_obj_set_pos(b, 1078 + i * 60, 474);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, c(col::transport_btn), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, c(col::transport_border), 0);
        lv_obj_set_style_radius(b, 5, 0);
    }
    lv_obj_t* tap = microBtn(scr(), "TAP", 46, 23);
    lv_obj_set_pos(tap, 1078, 512);
    lvhost_run(300);
    shot("replica");
}

// Mixer strips + PADS head at the coordinates of reference/shots/view-mixer.png.
static void replicaMixer()
{
    reset();
    lv_obj_t* pads = panelCreate(scr(), 14, 73, 288, 331);
    lv_obj_t* right = panelHead(pads, Icon::Grid, "PADS");
    lv_obj_t* mute = microBtn(right, "MUTE", 43, 24);
    lv_obj_set_style_text_font(mute, f_m12, 0);
    lv_obj_t* solo = microBtn(right, "SOLO", 43, 24);
    lv_obj_update_layout(scr());
    lv_area_t ra, ma, sa;
    lv_obj_get_coords(right, &ra);
    lv_obj_get_coords(mute, &ma);
    lv_obj_get_coords(solo, &sa);
    CHECK(ma.x1 >= ra.x1 && sa.x2 <= ra.x2 && ma.x2 < sa.x1);   // both inside the head's right box
    CHECK(sa.x2 <= 14 + 288 - 13);                               // right padding 13 (+ border)
    lv_obj_t* work = panelCreate(scr(), 311, 73, 746, 486);
    lv_obj_t* wr = panelHead(work, Icon::Mixer, "8-CHANNEL MIXER");
    (void)wr;
    const char* names[] = {"DRUMS", "BASS", "SYNTH", "VOCALS"};
    const float lvl[] = {0.8f, 0.5f, 0.66f, 0.3f};
    const float pk[] = {0.83f, 0.53f, 0.68f, 0.2f};
    for (int i = 0; i < 4; ++i) {
        const int x = 328 + i * 80;
        lv_obj_t* strip = lv_obj_create(scr());
        lv_obj_remove_style_all(strip);
        lv_obj_set_pos(strip, x, 118);
        lv_obj_set_size(strip, 73, 401);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(strip, c(col::mixer_bg), 0);
        lv_obj_set_style_border_width(strip, 1, 0);
        lv_obj_set_style_border_color(strip, c(col::line), 0);
        lv_obj_set_style_radius(strip, 6, 0);
        lv_obj_t* t = label(scr(), names[i], f_m12, col::ink);
        lv_obj_update_layout(t);
        lv_obj_set_pos(t, x + 36 - lv_obj_get_width(t) / 2, 131);
        lv_obj_t* m = meterCreate(scr(), 8, 282);
        lv_obj_set_pos(m, x + 17, 151);
        meterSet(m, powf(10.f, (-48.f + 51.f * pk[i]) / 20.f));
        const ParamRef r = ParamRef::busLevel(i);
        app().paramSetNorm(r, lvl[i]);
        lv_obj_t* f = faderCreate(scr(), 24, 282, r);
        lv_obj_set_pos(f, x + 31, 151);
        char buf[16];
        app().paramText(r, buf, sizeof buf);
        lv_obj_t* v = label(scr(), buf, f_r12, col::muted);
        lv_obj_update_layout(v);
        lv_obj_set_pos(v, x + 36 - lv_obj_get_width(v) / 2, 438);
        lv_obj_t* mb = microBtn(scr(), "M");
        lv_obj_set_pos(mb, x + 10, 457);
        lv_obj_t* sb = microBtn(scr(), "S");
        lv_obj_set_pos(sb, x + 38, 457);
        if (i == 1) btnSetActive(sb, true);
    }
    lvhost_run(300);
    shot("replica_mixer");
}

// ---------------------------------------------------------------- icons + fonts
static void icons()
{
    reset();
    lv_obj_t* p = panelCreate(scr(), 10, 10, 1260, 700);
    (void)p;
    static const char* names[] = {"Play", "Pause", "Stop", "Record", "Folder", "Settings", "Search", "Grid",
                                  "Wave", "Mixer", "Spark", "Close", "Chev", "Back", "Save", "Help",
                                  "Layers", "Key", "Home", "Minus", "Plus", "Headphones", "Menu", "Check",
                                  "Reload", "Trash", "Info", "Keyboard", "Eq", "Volume", "Dots", "Pencil",
                                  "Envelope", "Slice", "Rewind", "Link"};
    static_assert(sizeof names / sizeof names[0] == (size_t)Icon::Count, "names");
    const int sizes[] = {14, 16, 18, 20, 22, 26};
    for (int i = 0; i < (int)Icon::Count; ++i) {
        const int col_ = i / 12, row_ = i % 12;
        const int x0 = 30 + col_ * 415, y0 = 24 + row_ * 56;
        label(scr(), names[i], f_r12, col::muted);
        lv_obj_set_pos(lv_obj_get_child(scr(), -1), x0, y0 + 16);
        int x = x0 + 86;
        for (int s = 0; s < 6; ++s) {
            lv_obj_t* im = iconCreate(scr(), (Icon)i, sizes[s], s == 5 ? col::accent : col::ink);
            lv_obj_set_pos(im, x, y0 + (26 - sizes[s]) / 2 + 10);
            x += sizes[s] + 14;
        }
        lv_obj_t* hi = iconCreate(scr(), (Icon)i, 18, 0xFFFFFF);
        lv_obj_set_pos(hi, x + 6, y0 + 14);
        lv_obj_set_style_bg_opa(hi, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(hi, c(col::accent), 0);
    }
    lvhost_run(100);
    shot("icons");
}

static uint32_t utf8Next(const char* s, uint32_t* len)
{
    const unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) { *len = 1; return c0; }
    if ((c0 & 0xE0) == 0xC0) { *len = 2; return ((c0 & 0x1Fu) << 6) | ((unsigned char)s[1] & 0x3Fu); }
    *len = 3;
    return ((c0 & 0x0Fu) << 12) | (((unsigned char)s[1] & 0x3Fu) << 6) | ((unsigned char)s[2] & 0x3Fu);
}

static void fonts()
{
    reset();
    struct F { const lv_font_t* f; const char* n; };
    const F list[] = {{f_r12, "r12"}, {f_r13, "r13"}, {f_r14, "r14"}, {f_m12, "m12"}, {f_m13, "m13"},
                      {f_m14, "m14"}, {f_m16, "m16"}, {f_m20, "m20"}, {f_m24, "m24"}, {f_s11, "s11"},
                      {f_s13, "s13"}, {f_s15, "s15"}, {f_b34, "b34"}, {f_mono12, "mono12"}};
    panelCreate(scr(), 10, 10, 1260, 700);
    int y = 22;
    for (const F& e : list) {
        char t[200];
        snprintf(t, sizeof t, "%-6s Zażółć gęślą jaźń ŻÓŁW 0123456789  ▶ ▾ ● ✕ ✎ – — • …  ATTACK DECAY", e.n);
        lv_obj_t* l = label(scr(), t, e.f, col::ink);
        lv_obj_set_pos(l, 30, y);
        CHECK(e.f->line_height > 0);
        y += e.f->line_height + 8;
    }
    lv_obj_t* tb = label(scr(), "TAB5", f_b34, col::ink);
    lv_obj_set_style_text_letter_space(tb, -2, 0);
    lv_obj_set_pos(tb, 1080, 640);
    // every Polish letter must map to a real glyph in every font
    const char* pl = "ąćęłńóśźżĄĆĘŁŃÓŚŹŻ";
    for (const F& e : list) {
        const char* p = pl;
        while (*p) {
            uint32_t i = 0;
            const uint32_t cp = utf8Next(p, &i);
            lv_font_glyph_dsc_t g;
            const bool ok = lv_font_get_glyph_dsc(e.f, &g, cp, 0) && g.box_w > 0;
            if (!ok) printf("missing U+%04X in %s\n", (unsigned)cp, e.n);
            CHECK(ok);
            p += i;
        }
    }
    lvhost_run(100);
    shot("fonts");
}

// ---------------------------------------------------------------- interactions
static void knobTests()
{
    reset();
    Settings& s = app().settingsEdit();
    s.knob_sensitivity = 56;
    s.double_tap_reset = 1;
    const ParamRef r = ParamRef::pad(PP_RESO, 3);
    app().paramReset(r);
    lv_obj_t* w = knobCreate(scr(), KnobLook::Dark, 56, r, nullptr);
    lv_obj_set_pos(w, 400, 200);
    lvhost_run(50);
    CHECK(knobRef(w).id == PP_RESO && knobRef(w).index == 3);
    lv_obj_t* body = lv_obj_get_child(w, 1);
    lv_obj_t* val = lv_obj_get_child(w, 2);
    int x, y;
    center(body, &x, &y);
    char before_txt[24];
    snprintf(before_txt, sizeof before_txt, "%s", lv_label_get_text(val));
    const float n0 = app().paramNorm(r);
    lvhost_drag(x, y, x, y - 50, 300);   // 50 px up: (50 - 4 dead zone) * 0.58 % = 26.7 %
    const float n1 = app().paramNorm(r);
    printf("knob drag: %.3f -> %.3f (delta %.3f, expected 0.267)\n", (double)n0, (double)n1, (double)(n1 - n0));
    CHECK(fabsf((n1 - n0) - 0.267f) < 0.03f);
    CHECK(strcmp(before_txt, lv_label_get_text(val)) != 0);
    // drag continues outside the knob without scrolling anything
    lvhost_drag(x, y, x + 120, y + 400, 400);
    CHECK(app().paramNorm(r) < 0.001f);
    // double tap resets to the default
    app().paramSetNorm(r, 0.9f);
    knobRefresh(w);
    lvhost_run(400);
    lvhost_double_tap(x, y);
    const ParamSpec* sp = app().paramSpec(r);
    CHECK(sp && fabsf(app().paramGet(r) - sp->def) < 1e-4f);
    // a single tap changes nothing
    app().paramSetNorm(r, 0.9f);
    knobRefresh(w);
    lvhost_run(500);
    lvhost_tap(x, y);
    lvhost_run(500);
    CHECK(fabsf(app().paramNorm(r) - 0.9f) < 1e-3f);
    // double tap reset disabled in Settings
    s.double_tap_reset = 0;
    lvhost_double_tap(x, y);
    CHECK(fabsf(app().paramNorm(r) - 0.9f) < 1e-3f);
    s.double_tap_reset = 1;
    lvhost_run(500);
    // higher sensitivity moves further for the same drag
    s.knob_sensitivity = 100;
    app().paramSetNorm(r, 0.f);
    lvhost_drag(x, y, x, y - 30, 200);
    const float hi = app().paramNorm(r);
    s.knob_sensitivity = 56;
    app().paramSetNorm(r, 0.f);
    lvhost_run(500);
    lvhost_drag(x, y, x, y - 30, 200);
    const float mid = app().paramNorm(r);
    printf("sensitivity 100: %.3f, 56: %.3f\n", (double)hi, (double)mid);
    CHECK(hi > mid * 3.f);
    // knobBind switches the parameter and caption
    knobBind(w, ParamRef::pad(PP_CUTOFF, 3), "FREQ");
    CHECK(strcmp(lv_label_get_text(lv_obj_get_child(w, 0)), "FREQ") == 0);
    CHECK(knobRef(w).id == PP_CUTOFF);
    // knob inside a scrollable container: dragging must not scroll it
    lv_obj_t* cont = lv_obj_create(scr());
    lv_obj_remove_style_all(cont);
    lv_obj_set_pos(cont, 700, 100);
    lv_obj_set_size(cont, 200, 300);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* k2 = knobCreate(cont, KnobLook::Dark, 53, ParamRef::pad(PP_DECAY, 4), nullptr);
    for (int i = 0; i < 6; ++i) label(cont, "filler filler", f_r14, col::ink), lv_obj_set_height(lv_obj_get_child(cont, -1), 60);
    lvhost_run(50);
    app().paramSetNorm(ParamRef::pad(PP_DECAY, 4), 0.5f);
    knobRefresh(k2);
    center(lv_obj_get_child(k2, 1), &x, &y);
    lvhost_drag(x, y, x, y - 60, 300);
    CHECK(lv_obj_get_scroll_y(cont) == 0);
    CHECK(app().paramNorm(ParamRef::pad(PP_DECAY, 4)) > 0.75f);
}

static void faderMeterTests()
{
    reset();
    const ParamRef r = ParamRef::busLevel(2);
    app().paramSetNorm(r, 0.5f);
    lv_obj_t* f = faderCreate(scr(), 24, 216, r);
    lv_obj_set_pos(f, 300, 200);
    lvhost_run(50);
    int x, y;
    center(f, &x, &y);
    lvhost_drag(x, y, x, y + 40, 200);   // down 40 px of a 200 px travel
    const float n = app().paramNorm(r);
    printf("fader: 0.500 -> %.3f (expected 0.300)\n", (double)n);
    CHECK(fabsf(n - 0.3f) < 0.02f);
    app().paramSetNorm(r, 0.9f);
    faderRefresh(f);
    lvhost_run(30);

    lv_obj_t* m = meterCreate(scr(), 8, 200);
    lv_obj_set_pos(m, 400, 200);
    lvhost_run(30);
    meterSet(m, 1.f);
    lvhost_run(30);
    lv_area_t a;
    lv_obj_get_coords(m, &a);
    meterSet(m, 0.f);
    lvhost_run(1000);
    meterSet(m, 0.f);
    lvhost_run(30);
}

static int g_closed = 0;
static void onModalClose(void*) { ++g_closed; }
static int g_rep = 0;
static void onRepeat(void*) { ++g_rep; }

static void overlayTests()
{
    reset();
    // page behind the overlays
    lv_obj_t* p = panelCreate(scr(), 14, 73, 1252, 634);
    panelHead(p, Icon::Grid, "BACKGROUND");
    lv_obj_t* body = modalOpen("SETTINGS / TAB5", 824, 410, 650);
    CHECK(body != nullptr);
    CHECK(modalIsOpen());
    modalOnClose(onModalClose, nullptr);
    lv_obj_t* h = label(body, "Audio engine", f_m24, col::ink);
    lv_obj_set_style_pad_left(h, 26, 0);
    lv_obj_set_style_pad_top(h, 18, 0);
    static const char* const rates[] = {"44.1 kHz", "48 kHz"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* r = lv_obj_create(body);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), 62);
        lv_obj_set_style_pad_hor(r, 26, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* n = label(r, i == 0 ? "Sample rate" : i == 1 ? "Master limiter" : "Master volume", f_m16, col::ink);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, -8);
        lv_obj_t* note = label(r, i == 0 ? "Próbkowanie" : i == 1 ? "Ogranicznik sygnału" : "Głośność wyjścia", f_r12,
                               col::muted);
        lv_obj_align(note, LV_ALIGN_LEFT_MID, 0, 12);
        lv_obj_t* ctl = i == 0 ? selectCreate(r, 174, 38, rates, 2, 1, nullptr, nullptr)
                      : i == 1 ? toggleCreate(r, true, nullptr, nullptr)
                               : hsliderCreate(r, 128, 0, 100, 72, nullptr, nullptr);
        lv_obj_align(ctl, LV_ALIGN_RIGHT_MID, i == 2 ? -6 : 0, 0);
    }
    lv_obj_t* foot = modalFooter("Wersja demonstracyjna interfejsu");
    actionBtn(foot, "APPLY", true, 0, 34);
    // perf-1: the modal opens in one frame: scrim at its final opacity, window in place, no
    // window shadow (no fade / slide steps repainting the whole screen)
    {
        lv_obj_t* win = lv_obj_get_parent(body);
        lv_obj_t* scrim = lv_obj_get_parent(win);
        CHECK(lv_obj_get_style_bg_opa(scrim, 0) == 153);
        CHECK(lv_obj_get_style_translate_y(win, 0) == 0);
        CHECK(lv_obj_get_style_shadow_width(win, 0) == 0);
        lvhost_run(16);
        CHECK(lv_obj_get_style_bg_opa(scrim, 0) == 153);
        CHECK(lv_anim_get(scrim, nullptr) == nullptr && lv_anim_get(win, nullptr) == nullptr);
    }
    toast("Zapisano projekt „Żółw 01”");
    lvhost_run(400);
    shot("modal_toast");
    // close button closes
    lv_obj_update_layout(lv_layer_top());
    lvhost_tap(1052 - 30, 97 + 28);   // close icon (window centred, 56 px head)
    lvhost_run(100);
    CHECK(!modalIsOpen());
    CHECK(g_closed == 1);
    // scrim tap closes, a tap inside the window does not
    modalOpen("PROJECT MANAGER");
    lvhost_run(300);
    lvhost_tap(640, 360);
    lvhost_run(100);
    CHECK(modalIsOpen());
    lvhost_tap(60, 60);
    lvhost_run(100);
    CHECK(!modalIsOpen());
    // opening a modal replaces the previous one
    modalOpen("A");
    modalOpen("B");
    lvhost_run(300);
    CHECK(modalIsOpen());
    modalClose();
    lvhost_run(100);
    CHECK(!modalIsOpen());
    // toast hides itself after 2.6 s
    lvhost_run(3000);
    CHECK(!toastVisible());
    // ux-22: short texts (< 24 bytes) stay 1.6 s, long ones 2.6 s (+ 240 ms fade)
    toast("Krótki tekst");
    lvhost_run(1500);
    CHECK(toastVisible());
    lvhost_run(500);
    CHECK(!toastVisible());
    toast("Zapisano projekt „Żółw 01” na karcie SD");
    lvhost_run(2400);
    CHECK(toastVisible());
    lvhost_run(600);
    CHECK(!toastVisible());
    // same key ("TAP TEMPO:"): the text is replaced, the timer is NOT restarted
    toast("TAP TEMPO: 120 BPM");
    lvhost_run(1000);
    toast("TAP TEMPO: 121 BPM");
    lvhost_run(20);
    CHECK(strcmp(toastText(), "TAP TEMPO: 121 BPM") == 0);
    lvhost_run(980);   // 2.0 s after the first: 1.6 s + 240 ms fade are over
    CHECK(!toastVisible());
    // a different key restarts it
    toast("TAP TEMPO: 122 BPM");
    lvhost_run(1000);
    toast("REC off");
    lvhost_run(1000);   // 2.0 s after the first, 1.0 s after the second
    CHECK(toastVisible());
    CHECK(strcmp(toastText(), "REC off") == 0);
    lvhost_run(1000);
    CHECK(!toastVisible());

    // hold-to-repeat
    lv_obj_t* plus = microBtn(scr(), "+", 40, 30);
    lv_obj_set_pos(plus, 600, 300);
    btnRepeat(plus, onRepeat, nullptr);
    lvhost_run(30);
    int x, y;
    center(plus, &x, &y);
    lvhost_pointer(x, y, true);
    lvhost_run(1500);
    lvhost_pointer(x, y, false);
    lvhost_run(100);
    printf("repeat: %d calls in 1.5 s\n", g_rep);
    CHECK(g_rep >= 5);
    const int after = g_rep;
    lvhost_run(500);
    CHECK(g_rep == after);

    // ripple appears on press and is gone after 560 ms
    const uint32_t kids = lv_obj_get_child_count(plus);
    lvhost_pointer(x, y, true);
    lvhost_run(40);
    CHECK(lv_obj_get_child_count(plus) == kids + 1);
    lvhost_pointer(x, y, false);
    lvhost_run(700);
    CHECK(lv_obj_get_child_count(plus) == kids);
}

static char g_text[64];
static void onText(const char* t, void*) { snprintf(g_text, sizeof g_text, "%s", t); }

static void keyboardTests()
{
    reset();
    lv_obj_t* tf = textFieldCreate(scr(), 300, "Ab", 24, onText, nullptr, "PROJECT NAME");
    lv_obj_set_pos(tf, 100, 100);
    lvhost_run(50);
    int x, y;
    center(tf, &x, &y);
    lvhost_tap(x, y);
    lvhost_run(400);
    // key geometry: matrix at (24, 720-352+70), 1232 x 266, 5 rows, 7 px gaps
    const int mx = 24, my = 720 - 352 + 70, mw = 1280 - 48, mh = 352 - 86, gap = 7;
    const int rh = (mh - 4 * gap) / 5;
    auto key = [&](int row_, int units_before, int units, int total) {
        const int uw = (mw - gap * 9) / 10;   // only used for 10/11-unit rows below
        (void)uw;
        const float unit = (float)(mw - gap * 0) / (float)total;
        lvhost_tap(mx + (int)(unit * ((float)units_before + (float)units * 0.5f)), my + row_ * (rh + gap) + rh / 2);
    };
    // row 2 (q..p): 10 x 1 unit; row 3 (a..l - _): 11 units
    key(1, 2, 1, 10);   // e
    key(2, 0, 1, 11);   // a
    shot("keyboard");
    // long press on 'e' -> 'ę'
    {
        const float unit = (float)mw / 10.f;
        const int kx = mx + (int)(unit * 2.5f), ky = my + 1 * (rh + gap) + rh / 2;
        lvhost_pointer(kx, ky, true);
        lvhost_run(700);
        lvhost_pointer(kx, ky, false);
        lvhost_run(100);
    }
    // PL map: row 5 = #+=(2) PL(2) SPACE(7) OK(3) -> 14 units
    key(4, 2, 2, 14);   // PL
    lvhost_run(50);
    shot("keyboard_pl");
    key(1, 3, 1, 9);    // ł (row 2 of the PL map: 9 keys)
    key(4, 11, 3, 14);  // OK
    lvhost_run(100);
    printf("keyboard text: '%s'\n", g_text);
    CHECK(strcmp(g_text, "Abeaęł") == 0);
    CHECK(strcmp(lv_label_get_text(lv_obj_get_child(tf, 0)), "Abeaęł") == 0);
    // cancel via the scrim keeps the text
    lvhost_tap(x, y);
    lvhost_run(300);
    key(2, 0, 1, 11);
    lvhost_tap(640, 100);   // scrim above the sheet
    lvhost_run(100);
    CHECK(strcmp(g_text, "Abeaęł") == 0);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);

    // perf-13: the sheet is in place at once (no slide)
    keyboardOpen("TEXT", "", 8, onText, nullptr);
    {
        lv_obj_t* ov = lv_obj_get_child(lv_layer_top(), -1);
        lv_obj_t* sheet = ov ? lv_obj_get_child(ov, 0) : nullptr;
        CHECK(sheet && lv_obj_get_style_translate_y(sheet, 0) == 0 && lv_anim_get(sheet, nullptr) == nullptr);
    }
    lvhost_tap(640, 100);
    lvhost_run(100);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);

    // ux-9: numeric pad (TEMPO): 3 rows x 4 keys "1 2 3 DEL / 4 5 6 . / 7 8 9 0", 560 px wide,
    // centred; DEL DEL DEL 9 5 . 5 OK -> "95.5"
    keyboardOpenNumeric("TEMPO (BPM)", "120", 6, onText, nullptr);
    lvhost_run(50);
    shot("keyboard_numeric");
    {
        const int nx = (1280 - 560) / 2, ny = 720 - 352 + 70, nh = 352 - 86;
        const float kw = (560.f - 3 * 7.f) / 4.f, kh = (nh - 2 * 7.f) / 3.f;
        auto nkey = [&](int r, int col) {
            lvhost_tap(nx + (int)(col * (kw + 7.f) + kw / 2.f), ny + (int)(r * (kh + 7.f) + kh / 2.f));
        };
        for (int i = 0; i < 3; ++i) nkey(0, 3);   // DEL
        nkey(2, 2);   // 9
        nkey(1, 1);   // 5
        nkey(1, 3);   // .
        nkey(1, 1);   // 5
        lvhost_tap(1280 - 24 - 60, 720 - 352 + 14 + 21);   // OK button
        lvhost_run(100);
        printf("numeric keyboard text: '%s'\n", g_text);
        CHECK(strcmp(g_text, "95.5") == 0);
        CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
    }
}

static void settingsTests()
{
    reset();
    Settings& s = app().settingsEdit();
    // animations off: screen enter does not touch the object
    s.animations = 0;
    lv_obj_t* o = panelCreate(scr(), 100, 100, 200, 100);
    animScreenEnter(o);
    CHECK(lv_obj_get_style_opa(o, 0) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_translate_y(o, 0) == 0);
    // perf-5: on: 4 px low, then one settle step after 90 ms; never an opacity fade
    s.animations = 1;
    animScreenEnter(o);
    CHECK(lv_obj_get_style_translate_y(o, 0) == 4);
    lvhost_run(40);
    CHECK(lv_obj_get_style_translate_y(o, 0) == 4);
    CHECK(lv_obj_get_style_opa(o, 0) == LV_OPA_COVER);
    lvhost_run(200);
    CHECK(lv_obj_get_style_opa(o, 0) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_translate_y(o, 0) == 0);
    CHECK(lv_anim_get(o, nullptr) == nullptr);
    animHit(o);
    lvhost_run(120);
    CHECK(lv_obj_get_style_shadow_opa(o, 0) > 0);
    lvhost_run(300);
    // :active look: scale .963 while pressed (feedback on), none when Touch > feedback is off
    lv_obj_t* b = actionBtn(scr(), "X", false, 60, 34);
    lv_obj_set_pos(b, 400, 400);
    lvhost_run(30);
    int x, y;
    center(b, &x, &y);
    lvhost_pointer(x, y, true);
    lvhost_run(200);
    CHECK(lv_obj_get_style_transform_scale_x(b, 0) == 247);
    lvhost_pointer(x, y, false);
    lvhost_run(700);   // press transition back + ripple gone
    CHECK(lv_obj_get_style_transform_scale_x(b, 0) == 256);
    // perf-14: a wide button keeps the brightness but never scales (no ARGB transform layer)
    {
        lv_obj_t* wide = actionBtn(scr(), "WIDE ROW BUTTON", false, 270, 34);
        lv_obj_set_pos(wide, 400, 500);
        lvhost_run(30);
        int wx, wy;
        center(wide, &wx, &wy);
        lvhost_pointer(wx, wy, true);
        lvhost_run(60);
        CHECK(lv_obj_has_state(wide, LV_STATE_PRESSED));
        CHECK(lv_obj_get_style_transform_scale_x(wide, 0) == 256);
        CHECK(lv_obj_get_style_transform_scale_y(wide, 0) == 256);
        CHECK(lv_obj_get_style_color_filter_opa(wide, 0) > 0);
        lvhost_run(140);
        CHECK(lv_obj_get_style_transform_scale_x(wide, 0) == 256);
        lvhost_pointer(wx, wy, false);
        lvhost_run(700);
        // the small one still scales
        lvhost_pointer(x, y, true);
        lvhost_run(200);
        CHECK(lv_obj_get_style_transform_scale_x(b, 0) == 247);
        lvhost_pointer(x, y, false);
        lvhost_run(700);
        lv_obj_delete(wide);
    }
    s.touch_feedback = 0;
    const uint32_t kids = lv_obj_get_child_count(b);
    lvhost_pointer(x, y, true);
    lvhost_run(200);
    CHECK(lv_obj_get_child_count(b) == kids);   // no ripple
    CHECK(lv_obj_get_style_transform_scale_x(b, 0) == 256);
    lvhost_pointer(x, y, false);
    lvhost_run(300);
    s.touch_feedback = 1;
    // labels on the top layer inherit the UI font
    lv_obj_t* tl = lv_label_create(lv_layer_top());
    CHECK(lv_obj_get_style_text_font(tl, 0) == f_r14);
    lv_obj_delete(tl);
}

// ---------------------------------------------------------------- palettes (Settings > Display colours)
static float lumOf(uint32_t c)   // WCAG relative luminance
{
    float l[3];
    for (int i = 0; i < 3; ++i) {
        const float v = (float)((c >> (16 - 8 * i)) & 0xFFu) / 255.f;
        l[i] = v <= 0.03928f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    return 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
}
static float contrastOf(uint32_t a, uint32_t b)
{
    const float la = lumOf(a), lb = lumOf(b);
    return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

// Text / mark on its background with the target it should meet (4.5 text, 3 secondary or large).
struct Pair {
    const char* name;
    const uint32_t* fg;
    const uint32_t* bg;
    float target;
};
static const Pair kPairs[] = {
    {"ink/panel", &col::ink, &col::panel, 4.5f},
    {"ink/modal", &col::ink, &col::modal_bg, 4.5f},
    {"ink/seq", &col::ink, &col::seq_bg, 4.5f},
    {"ink/control", &col::ink, &col::control, 4.5f},
    {"ink/file_sel", &col::ink, &col::file_sel, 4.5f},
    {"ink/active_bg", &col::ink, &col::active_bg, 4.5f},
    {"muted/panel", &col::muted, &col::panel, 3.f},
    {"muted/seq", &col::muted, &col::seq_bg, 3.f},
    {"muted/modal", &col::muted, &col::modal_bg, 3.f},
    {"overline/panel", &col::overline, &col::panel, 4.5f},
    {"overline/seq", &col::overline, &col::seq_bg, 4.5f},
    {"nav_text/nav_btn", &col::nav_text, &col::nav_btn, 3.f},
    {"pad_text/pad_a", &col::pad_text, &col::pad_a, 4.5f},
    {"pad_text/pad_b", &col::pad_text, &col::pad_b, 4.5f},
    {"pad_index/pad_a", &col::pad_index, &col::pad_a, 3.f},
    {"badge/muted", &col::badge_fg, &col::muted, 3.f},
    {"knob_text/panel", &col::knob_text, &col::panel, 4.5f},
    {"cat_text/panel", &col::cat_text, &col::panel, 4.5f},
    {"cat_text/cat_active", &col::cat_text, &col::cat_active, 4.5f},
    {"top_glyph/top_b", &col::top_glyph, &col::top_b, 4.5f},
    {"top_sub/top_a", &col::top_sub, &col::top_a, 3.f},
    {"top_sig/top_b", &col::top_sig, &col::top_b, 3.f},
    {"song_num/panel", &col::song_num, &col::panel, 3.f},
    {"scene_text/tab", &col::scene_text, &col::tab, 4.5f},
    {"macro_ink/transport", &col::macro_ink, &col::transport_btn, 4.5f},
    {"layer_on/layer_bg", &col::layer_on, &col::layer_bg, 4.5f},
    {"layer_loaded/layer_bg", &col::layer_loaded, &col::layer_bg, 4.5f},
    {"layer_empty/layer_bg", &col::layer_empty, &col::layer_bg, 3.f},
    {"search_text/search_bg", &col::search_text, &col::search_bg, 4.5f},
    {"dur_text/list_bg", &col::dur_text, &col::list_bg, 3.f},
    {"toast_fg/toast_bg", &col::toast_fg, &col::toast_bg, 4.5f},
    {"dark_btn_fg/dark_btn", &col::dark_btn_fg, &col::dark_btn, 4.5f},
    {"wave_text/wave_frame", &col::wave_text, &col::wave_frame, 4.5f},
    {"key_label/key_white", &col::muted, &col::key_white, 3.f},
    {"line/panel", &col::line, &col::panel, 1.4f},
    {"step_off/seq", &col::step_off, &col::seq_bg, 1.4f},
    {"cell_clip/cell", &col::cell_clip, &col::cell, 1.5f},
    {"pad_border/panel", &col::pad_border, &col::panel, 1.5f},
    // accent family
    {"accent_dark/panel", &col::accent_dark, &col::panel, 3.f},
    {"on_accent/accent", &col::on_accent, &col::accent, 3.f},
    {"macro_ink_on/accent", &col::macro_ink_on, &col::accent, 3.f},
    {"scene_text_on/accent", &col::scene_text_on, &col::accent, 3.f},
};
constexpr int kPairCount = (int)(sizeof kPairs / sizeof kPairs[0]);

static void paletteTests()
{
    printf("palettes\n");
    // LIGHT + ORANGE is the prototype's palette, exactly
    baseSelect(0);
    themeSelect(0);
    CHECK(col::panel == 0xE8EAEB && col::ink == 0x252629 && col::bg == 0xECEEEF && col::wave_frame == 0x1D2022);
    CHECK(col::accent == 0xFF6531 && col::accent_dark == 0xE95021 && col::active_bg == 0xFBDCCE && col::file_sel == 0xFFDDD0);
    CHECK(col::row_press == 0xF9E1D7 && col::macro_ink_on == 0x3D3D3F && col::scene_text_on == 0x111111);
    CHECK(col::hilite == 0xFFFFFF && col::hilite_opa == 150 && col::scrim == 0x000000 && col::nav_fill == 0xDEDDDE);
    float light[kPairCount];
    for (int i = 0; i < kPairCount; ++i) light[i] = contrastOf(*kPairs[i].fg, *kPairs[i].bg);
    // the light / dark split and the names
    CHECK(!baseDark(0) && !baseDark(1) && !baseDark(2) && baseDark(3) && baseDark(4) && baseDark(5));
    CHECK(strcmp(baseName(0), "LIGHT") == 0 && strcmp(baseName(5), "BLACK") == 0 && strcmp(baseName(9), "LIGHT") == 0);
    baseSelect(99);
    CHECK(baseIndex() == 0);
    themeSelect(-3);
    CHECK(themeIndex() == 0);
    themeSelect(kThemeCustom + 1);
    CHECK(themeIndex() == 0);
    // CUSTOM: hue clamped to 0..359, saturation 0 = default, else kAccentSatMin..100
    themeSelect(kThemeCustom, 500, 5);
    CHECK(themeIndex() == kThemeCustom && themeHue() == 359 && themeSat() == kAccentSatMin);
    themeSelect(kThemeCustom, 120, 0);
    CHECK(themeHue() == 120 && themeSat() == kAccentSatDefault && col::accent == customAccent(120, 0, 0));
    themeSelect(3);
    CHECK(themeHue() == 0 && themeSat() == 0);
    // a preset's own hue / saturation give (nearly) the preset back as CUSTOM
    for (int t = 0; t < kThemeCount; ++t) {
        int h = -1, s = -1;
        presetHueSat(t, h, s);
        CHECK(h >= 0 && h < 360 && s >= kAccentSatMin && s <= 100);
        const uint32_t a = kThemes[t].accent, b = customAccent(h, s, 0);
        CHECK(contrastOf(a, b) < 1.35f);   // same colour family, at most a little brighter / darker
    }
    int oh = 0, os = 0;
    presetHueSat(0, oh, os);
    CHECK(oh >= 13 && oh <= 16 && os >= 79 && os <= 83);   // ORANGE #ff6531: 15 deg, 81 %
    // CUSTOM stays a usable accent: never paler than AMBER on a light base, never darker than VIOLET on a dark one
    for (int b = 0; b < kBaseCount; ++b)
        for (int h = 0; h < 360; h += 15)
            for (int s = kAccentSatMin; s <= 100; s += 20) {
                const float l = lumOf(customAccent(h, s, b));
                if (baseDark(b)) CHECK(l >= 0.195f);
                else CHECK(l <= 0.455f);
            }
    // every base x accent: text reaches its target, or at least LIGHT's own contrast of that pair
    float worst = 99.f;
    for (int b = 0; b < kBaseCount; ++b) {
        for (int t = 0; t <= kThemeCustom + 2; ++t) {
            const int idx = t <= kThemeCustom ? t : kThemeCustom;
            const int hue = t == kThemeCustom ? 210 : (t == kThemeCustom + 1 ? 60 : 280);
            baseSelect(b);
            themeSelect(idx, hue, t == kThemeCustom + 2 ? 100 : 75);
            for (int i = 0; i < kPairCount; ++i) {
                const Pair& p = kPairs[i];
                const float cr = contrastOf(*p.fg, *p.bg);
                const float need = (p.target < light[i] ? p.target : light[i]) - 0.02f;
                if (cr < need) printf("  base %s accent %d: %s %.2f < %.2f\n", baseName(b), t, p.name, (double)cr, (double)need);
                CHECK(cr >= need);
                if (p.target >= 4.5f && light[i] >= 4.5f && cr < worst) worst = cr;
            }
            // tints darken on dark bases (towards the panel), lighten on light ones
            if (baseDark(b)) CHECK(lumOf(col::active_bg) < lumOf(col::accent) && lumOf(col::file_sel) < lumOf(col::accent));
            else CHECK(lumOf(col::active_bg) > lumOf(col::accent) && lumOf(col::file_sel) > lumOf(col::accent));
        }
        // the stage the background bake uses is the base's own
        baseSelect(b);
        const StageColors sc = stageColors(b);
        CHECK(sc.stage_a == col::stage_a && sc.stage_b == col::stage_b && sc.top_a == col::top_a && sc.top_b == col::top_b &&
              sc.top_line == col::top_line && sc.scan == col::scan);
        uint32_t panel = 0, ink = 0, line = 0;
        baseSwatch(b, panel, ink, line);
        CHECK(panel == col::panel && ink == col::ink && line == col::line);
        themeSelect(0);
        printf("  %-6s panel #%06X ink #%06X (%.1f:1) muted #%06X (%.1f:1) ORANGE text #%06X (%.1f:1)\n", baseName(b),
               (unsigned)col::panel, (unsigned)col::ink, (double)contrastOf(col::ink, col::panel), (unsigned)col::muted,
               (double)contrastOf(col::muted, col::panel), (unsigned)col::accent_dark,
               (double)contrastOf(col::accent_dark, col::panel));
    }
    printf("  lowest text contrast (4.5 pairs): %.2f\n", (double)worst);
    baseSelect(0);
    themeSelect(0);
}

int main()
{
    lvhost_init(1280, 720);
    themeInit();
    presyncTests();   // first: needs the press style's never-synced state
    gallery();
    replica();
    replicaMixer();
    icons();
    fonts();
    knobTests();
    faderMeterTests();
    overlayTests();
    keyboardTests();
    settingsTests();
    paletteTests();   // last: switches the palette (the styles above keep LIGHT + ORANGE)
    printf("test_widgets: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAILED" : "passed");
    return g_fails ? 1 : 0;
}
