// Top bar (#light-top): logo, view navigation (SAMPLE / SEQUENCE / BASS / SYNTH / MIXER / BROWSER / FX),
// PROJECT MANAGER + SETTINGS icons, the live tempo block and the PLAY / REC / ? actions.
// Its gradient and bottom line are part of the baked background (ui_bg.cpp); the bar itself
// is a transparent container. Refreshes on revs.view / revs.transport / revs.project.
// The folder icon carries a 6 px accent dot while the project has unsaved changes (App::dirty(),
// polled: it is one comparison); a long press on it saves at once (App::save() toasts the result),
// a short tap opens PROJECT MANAGER. While MIDI learn is on (Settings > MIDI), an accent chip
// "MIDI LEARN ✕" sits between the navigation and the folder; a tap ends learn (revs.midi).
// While Bluetooth is on, a small "BT" chip sits after the logo subtitle (left of the navigation): accent when
// headphones / a speaker are connected, dim while it waits for one, hidden when off; a tap opens
// Settings > Bluetooth (revs.bt).
// Built for size (-Os): built once, refreshed by a few comparisons (the 4 MB image limit).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif
#include "ui/ui.h"

#include "ui/theme.h"
#include "ui/ui_internal.h"
#include "ui/widgets.h"
#include "ui/logo_orbit_daw.h"

namespace orbit::ui::topbar {

namespace {

// Seven view tabs since 0.4.9 (SYNTH), as wide as their names (8 px or more each side, at least 64 px), 6 apart, in
// the room the six 84 px tabs had (0.3: six, 84 / 8; the prototype's five were 90 / 9): the maximize button stays.
constexpr int kNavX = 377, kNavY = 13, kNavH = 35, kNavGap = 6;
constexpr int kNavW[(int)View::Count] = {72, 92, 64, 64, 64, 86, 64};   // in tab order
constexpr int kWmaxX = 931;                                            // the work area's maximize button
constexpr int kActX = 1153, kActY = 14, kActW = 36, kActH = 33, kActGap = 4;   // 0.4.6: 32 / 5 were too tight
// Tab order (left to right) -> view; BASS and SYNTH sit next to SEQUENCE.
const View kNavView[(int)View::Count] = {View::Sample, View::Sequence, View::Bass,  View::Synth,
                                         View::Mixer,  View::Browser,  View::Fx};
const char* const kNavNames[(int)View::Count] = {"SAMPLE", "SEQUENCE", "BASS", "SYNTH", "MIXER", "BROWSER", "FX"};

constexpr int navTabX(int i)   // left edge of tab i (tab order)
{
    int x = kNavX;
    for (int k = 0; k < i; ++k) x += kNavW[k] + kNavGap;
    return x;
}
static_assert(navTabX((int)View::Count - 1) + kNavW[(int)View::Count - 1] + 8 <= kWmaxX, "tabs reach the maximize button");

lv_obj_t* s_bar = nullptr;
lv_obj_t* s_nav[(int)View::Count] = {};
lv_obj_t* s_bpm = nullptr;
lv_obj_t* s_sig = nullptr;
lv_obj_t* s_play = nullptr;
lv_obj_t* s_play_lbl = nullptr;
lv_obj_t* s_rec = nullptr;
lv_obj_t* s_dirty = nullptr;
lv_obj_t* s_learn = nullptr;
lv_obj_t* s_sub = nullptr;      // the ORBIT DAW wordmark (0.4.8; the MIDI LEARN chip takes its place)
lv_obj_t* s_bt = nullptr;
uint32_t s_rev_view = 0, s_rev_transport = 0, s_rev_project = 0, s_rev_midi = 0, s_rev_bt = 0;
int s_drawn_view = -1;
int s_drawn_playing = -1, s_drawn_rec = -1, s_drawn_dirty = -1, s_drawn_learn = -1, s_drawn_bt = -1;

void navCb(lv_event_t* e)
{
    const int v = (int)(intptr_t)lv_event_get_user_data(e);
    // A maximized work area stays and shows the view; another maximized panel makes room for it.
    if (panelMaxed() != Panel::Count && panelMaxed() != Panel::Work) panelToggleMax(panelMaxed());
    app().setView((View)v);
}
void projectCb(lv_event_t*) { dlg_project::open(); }   // SHORT_CLICKED: not after a long press
void projectLongCb(lv_event_t*) { app().save(); }
void settingsCb(lv_event_t*) { dlg_settings::open(); }
void playCb(lv_event_t*) { app().togglePlay(); }
void recCb(lv_event_t*) { app().toggleRecord(); }
void helpCb(lv_event_t*) { dlg_help::open(); }
void learnCb(lv_event_t*) { app().midiLearn(false); }
void btCb(lv_event_t*) { dlg_settings::openAt(dlg_settings::kTabBluetooth); }

// 0.4.6: the bar's touch targets reach the top of the screen and the bar's bottom and share the gaps between
// neighbours (no overlap: a tap on the right edge of PLAY started REC); a finger that slides a little while it
// presses still taps (PRESS_LOCK).
void barTarget(lv_obj_t* b, int left, int right)
{
    lv_obj_update_layout(b);
    lv_area_t a;
    lv_obj_get_coords(b, &a);
    setHitBox(b, left, a.y1, right, geo::top_h - 1 - a.y2);
    lv_obj_add_flag(b, LV_OBJ_FLAG_PRESS_LOCK);
}

// .lt-actions button: transparent, 1 px #afb2b5 border, radius 6, #31343a glyph (the prototype's
// ▶ / Ⅱ / ● / ? text symbols); .playing / .is-recording: accent border + accent glyph.
lv_obj_t* actionButton(lv_obj_t* parent, int x, const char* glyph, const lv_font_t* font, lv_event_cb_t cb,
                       lv_obj_t** label_out)
{
    lv_obj_t* b = iconBtn(parent, Icon::Play, 16, kActW);   // kit button: press look, ripple, hit area
    lv_obj_set_size(b, kActW, kActH);
    lv_obj_set_pos(b, x, kActY);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, c(col::top_btn), 0);
    lv_obj_set_style_border_color(b, c(col::accent), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, c(col::top_glyph), 0);
    lv_obj_set_style_text_color(b, c(col::accent), LV_STATE_CHECKED);
    lv_obj_set_style_radius(b, 6, 0);
    if (lv_obj_t* img = lv_obj_get_child(b, 0)) lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    if (label_out) *label_out = l;
    return b;
}

}  // namespace

int tabCenterX(View v)
{
    for (int i = 0; i < (int)View::Count; ++i)
        if (kNavView[i] == v) return navTabX(i) + kNavW[i] / 2;
    return -1;
}

lv_obj_t* create(lv_obj_t* scr)
{
    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LV_PCT(100), geo::top_h);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    if (!bg::shadowsBaked()) {   // no baked background: draw the bar's own gradient + line
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, c(col::top_a), 0);
        lv_obj_set_style_bg_grad_color(bar, c(col::top_b), 0);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_border_color(bar, c(col::top_line), 0);
    }
    s_bar = bar;

    // 0.4.8: the ORBIT DAW wordmark (tools/logogen, A8 recoloured with the ink) in place of "TAB5" and its
    // "SAMPLER / SEQUENCER" subtitle; 0.4.9: a slanted speed style, about 260 x 26 (20 .. ~280), left of the BT
    // chip (296) and the tabs (377).
    lv_obj_t* logo = lv_image_create(bar);
    lv_image_set_src(logo, &kLogoOrbitDaw);
    lv_obj_set_style_image_recolor(logo, c(col::ink), 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(logo, 20, (geo::top_h - kLogoOrbitDaw.header.h) / 2);
    s_sub = logo;   // hidden while the MIDI LEARN chip is shown

    // .lt-nav: 90x35 buttons, gap 9, #e1e3e4 / #717578, 11 px / .07em; active: accent, #252525,
    // box-shadow 0 2px 6px #ff663933.
    for (int i = 0; i < (int)View::Count; ++i) {
        const int view = (int)kNavView[i];
        lv_obj_t* b = tabBtn(bar, kNavNames[i], kNavW[i], kNavH);
        lv_obj_set_pos(b, navTabX(i), kNavY);
        lv_obj_set_style_bg_color(b, c(col::nav_btn), 0);
        lv_obj_set_style_border_color(b, c(col::nav_btn), 0);
        lv_obj_set_style_text_color(b, c(col::nav_text), 0);
        lv_obj_set_style_text_font(b, f_r13, 0);   // 11 px / 400 / .07em
        lv_obj_set_style_text_letter_space(b, 1, 0);
        lv_obj_set_style_text_color(b, c(col::on_accent), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(b, c(col::accent), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(b, c(col::accent), LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(b, 6, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_offset_y(b, 2, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_color(b, c(col::glow), LV_STATE_CHECKED);
        lv_obj_set_style_shadow_opa(b, 51, LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, navCb, LV_EVENT_CLICKED, (void*)(intptr_t)view);
        s_nav[view] = b;
        barTarget(b, i == 0 ? 20 : kNavGap / 2, kNavGap / 2);
    }
    // 0.3: maximize the work area (the view the tabs pick), right after the last tab.
    lv_obj_t* wmax = panelMaxButton(bar, Panel::Work);
    lv_obj_set_pos(wmax, kWmaxX, kNavY + (kNavH - 26) / 2);
    barTarget(wmax, 5, 10);

    // .lt-tools: folder (PROJECT MANAGER), gear (SETTINGS), separator, tempo block.
    lv_obj_t* fold = iconBtn(bar, Icon::Folder, 22, 31);
    lv_obj_set_size(fold, 31, 30);
    lv_obj_set_pos(fold, 978, 16);
    lv_obj_add_event_cb(fold, projectCb, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(fold, projectLongCb, LV_EVENT_LONG_PRESSED, nullptr);
    barTarget(fold, 10, 5);
    // unsaved-changes dot: top-right corner of the folder button (inside it: no extra geometry)
    s_dirty = lv_obj_create(fold);
    lv_obj_remove_style_all(s_dirty);
    lv_obj_set_size(s_dirty, 6, 6);
    lv_obj_set_style_radius(s_dirty, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dirty, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dirty, c(col::accent), 0);
    lv_obj_set_pos(s_dirty, 31 - 6, 1);
    lv_obj_add_flag(s_dirty, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_dirty, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dirty, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(s_dirty, LV_OBJ_FLAG_HIDDEN);
    // MIDI learn chip (hidden unless learning): over the wordmark, which hides meanwhile (the six tabs and the
    // maximize button reach x 957 since 0.3: the old place covered FX)
    s_learn = actionBtn(bar, "MIDI LEARN \xE2\x9C\x95", true, 98, 30);
    lv_obj_set_pos(s_learn, 118, 16);
    lv_obj_set_style_text_font(s_learn, f_m12, 0);
    lv_obj_set_style_text_letter_space(s_learn, 0, 0);
    lv_obj_set_style_pad_hor(s_learn, 4, 0);
    lv_obj_add_event_cb(s_learn, learnCb, LV_EVENT_CLICKED, nullptr);
    barTarget(s_learn, 8, 8);
    lv_obj_add_flag(s_learn, LV_OBJ_FLAG_HIDDEN);
    // Bluetooth chip (hidden while off): x 296..333, with the logo subtitle (ends ~280) as a status, well apart
    // from the navigation (377) so it never reads as a sixth view button
    s_bt = actionBtn(bar, "BT", false, 38, 26);
    lv_obj_set_pos(s_bt, 296, 18);
    lv_obj_set_style_text_font(s_bt, f_m12, 0);
    lv_obj_set_style_pad_hor(s_bt, 4, 0);
    lv_obj_set_style_text_color(s_bt, c(col::muted), 0);
    lv_obj_add_event_cb(s_bt, btCb, LV_EVENT_CLICKED, nullptr);
    barTarget(s_bt, 10, 20);
    lv_obj_add_flag(s_bt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* gear = iconBtn(bar, Icon::Settings, 22, 31);
    lv_obj_set_size(gear, 31, 30);
    lv_obj_set_pos(gear, 1020, 16);
    lv_obj_add_event_cb(gear, settingsCb, LV_EVENT_CLICKED, nullptr);
    barTarget(gear, 5, 10);
    lv_obj_t* sep = lv_obj_create(bar);
    lv_obj_remove_style_all(sep);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sep, c(col::top_sep), 0);
    lv_obj_set_pos(sep, 1062, 17);
    lv_obj_set_size(sep, 1, 27);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);

    // .lt-bpm: "120 BPM" 14 px / 500, "4 / 4" 9 px #9a9b9e, centred in a fixed-width column.
    s_bpm = label(bar, "120 BPM", f_m16, col::ink);
    lv_obj_set_style_text_align(s_bpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_bpm, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(s_bpm, 84, 20);
    lv_obj_set_pos(s_bpm, 1066, 12);
    s_sig = label(bar, "4 / 4", f_r12, col::top_sig);
    lv_obj_set_style_text_align(s_sig, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_sig, 84, 15);
    lv_obj_set_pos(s_sig, 1066, 32);

    // .lt-actions: PLAY (Play/Pause), REC, ? (help).
    s_play = actionButton(bar, kActX, "\xE2\x96\xB6", f_m14, playCb, &s_play_lbl);   // play
    s_rec = actionButton(bar, kActX + (kActW + kActGap), "\xE2\x97\x8F", f_r12, recCb, nullptr);   // record dot
    lv_obj_t* help = actionButton(bar, kActX + 2 * (kActW + kActGap), "?", f_m14, helpCb, nullptr);
    barTarget(s_play, 3, kActGap / 2);
    barTarget(s_rec, kActGap / 2, kActGap / 2);
    barTarget(help, kActGap / 2, 1279 - (kActX + 3 * kActW + 2 * kActGap));

    s_rev_view = s_rev_transport = s_rev_project = s_rev_midi = s_rev_bt = 0;   // force the first refresh
    s_drawn_view = s_drawn_playing = s_drawn_rec = s_drawn_dirty = s_drawn_learn = s_drawn_bt = -1;
    refresh();
    return bar;
}

void refresh()
{
    if (!s_bar) return;
    App& a = app();
    const Revs& r = a.revs();
    const int dirty = a.dirty() ? 1 : 0;
    if (dirty != s_drawn_dirty) {
        s_drawn_dirty = dirty;
        if (dirty) lv_obj_remove_flag(s_dirty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_dirty, LV_OBJ_FLAG_HIDDEN);
    }
    if (r.midi != s_rev_midi) {
        s_rev_midi = r.midi;
        const int learn = a.midiLearning() ? 1 : 0;
        if (learn != s_drawn_learn) {
            s_drawn_learn = learn;
            if (learn) lv_obj_remove_flag(s_learn, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_learn, LV_OBJ_FLAG_HIDDEN);
            if (s_sub) {
                if (learn) lv_obj_add_flag(s_sub, LV_OBJ_FLAG_HIDDEN);
                else lv_obj_remove_flag(s_sub, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (r.bt != s_rev_bt) {
        s_rev_bt = r.bt;
        const int bt = !a.btEnabled() ? 0 : (a.btStatus().connected ? 2 : 1);   // off / waiting / connected
        if (bt != s_drawn_bt) {
            s_drawn_bt = bt;
            if (bt) lv_obj_remove_flag(s_bt, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_bt, LV_OBJ_FLAG_HIDDEN);
            btnSetActive(s_bt, bt == 2);   // the action button's primary (accent) look
            lv_obj_set_style_text_color(s_bt, c(bt == 2 ? col::on_accent : col::muted), 0);
        }
    }
    if (r.view != s_rev_view) {
        s_rev_view = r.view;
        const int v = (int)a.ui().view;
        if (v != s_drawn_view) {
            if (s_drawn_view >= 0 && s_drawn_view < (int)View::Count) btnSetActive(s_nav[s_drawn_view], false);
            if (v >= 0 && v < (int)View::Count) btnSetActive(s_nav[v], true);
            s_drawn_view = v;
        }
    }
    if (r.transport != s_rev_transport || r.project != s_rev_project) {
        s_rev_transport = r.transport;
        s_rev_project = r.project;
        const Project& p = a.project();
        char buf[24];
        formatBpm(buf, sizeof buf, p.bpm, true);
        labelSet(s_bpm, buf);
        snprintf(buf, sizeof buf, "%d / 4", (int)p.beats_per_bar);
        labelSet(s_sig, buf);
        const int playing = a.playing() ? 1 : 0;
        if (playing != s_drawn_playing) {
            s_drawn_playing = playing;
            btnSetActive(s_play, playing != 0);
            labelSet(s_play_lbl, playing ? "\xE2\x85\xA1" : "\xE2\x96\xB6");   // pause / play
        }
        const int rec = a.recording() ? 1 : 0;
        if (rec != s_drawn_rec) {
            s_drawn_rec = rec;
            btnSetActive(s_rec, rec != 0);
        }
    }
}

}  // namespace orbit::ui::topbar
