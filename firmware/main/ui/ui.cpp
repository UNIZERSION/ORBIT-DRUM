// UI root: builds the persistent chrome (baked background, top bar, PADS, BROWSER categories,
// work panel, macros, SCENES timeline), creates the five views once inside the work panel and
// runs the single LVGL refresh timer (16 ms, 33 ms in low power):
//   app().tick() -> persistent panels' refresh() -> view switch (on revs.view) -> visible view's
//   refresh() -> dlg_settings::refresh() / dlg_sample::refresh() (no-ops while closed).
// Every refresh() compares App revision counters / telemetry with what it drew last and touches
// LVGL only when something changed.
#include "ui/ui.h"

#include <math.h>
#include <string.h>

#include "ui/theme.h"
#include "ui/ui_internal.h"
#include "ui/widgets.h"
#include "core/platform.h"
#include "hal/hal.h"

namespace orbit::ui {

namespace {

struct ViewFns {
    lv_obj_t* (*create)(lv_obj_t* work);
    void (*refresh)();
    void (*onShow)();
};
const ViewFns kViews[(int)View::Count] = {
    {view_sample::create, view_sample::refresh, view_sample::onShow},
    {view_sequence::create, view_sequence::refresh, view_sequence::onShow},
    {view_mixer::create, view_mixer::refresh, view_mixer::onShow},
    {view_browser::create, view_browser::refresh, view_browser::onShow},
    {view_fx::create, view_fx::refresh, view_fx::onShow},
    {view_bass::create, view_bass::refresh, view_bass::onShow},
};

lv_obj_t* s_work = nullptr;
lv_obj_t* s_view_obj[(int)View::Count] = {};
int s_shown = -1;
uint32_t s_view_rev = 0;
lv_timer_t* s_timer = nullptr;
uint32_t s_period = 16;
uint32_t s_ticks = 0;
// Views not needed for the first frame are created after it, one at a time (or at once when shown first).
const View kBackgroundOrder[] = {View::Sequence, View::Browser, View::Mixer, View::Fx, View::Bass, View::Sample};

lv_obj_t* s_scr = nullptr;

void createView(int v)
{
    if (v < 0 || v >= (int)View::Count || s_view_obj[v] || !s_work) return;
    const UiScale ws = uiScaleOf(s_work);
    lv_obj_t* o = nullptr;
    if (ws.unit()) {
        o = kViews[v].create(s_work);
    } else {
        // Maximized work area: the view is built in a stand-in panel at the design place (views measure and
        // place themselves in design px), laid out at the panel's scale and moved into the real panel.
        lv_obj_t* sand = panelCreate(s_scr, geo::work_x, geo::work_y, geo::work_w, geo::work_h);
        lv_obj_set_style_opa(sand, LV_OPA_TRANSP, 0);
        {
            UiBuildScope scope(ws);
            o = kViews[v].create(sand);
        }
        if (o) {
            uiScaleRegister(sand, ws);   // after create (it works in design px), before the re-layout
            uiScaleTree(o, ws);
            lv_obj_set_parent(o, s_work);
        }
        lv_obj_delete(sand);
    }
    s_view_obj[v] = o;
    if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// One pending view per call; false when all exist.
bool createNextView()
{
    for (View v : kBackgroundOrder) {
        if (!s_view_obj[(int)v]) {
            createView((int)v);
            return true;
        }
    }
    return false;
}
bool s_views_pending = true;

void showView(int v, bool animate)
{
    if (v < 0 || v >= (int)View::Count) return;
    createView(v);   // not created yet: build it now
    if (s_shown >= 0 && s_shown != v && s_view_obj[s_shown]) {
        lv_obj_add_flag(s_view_obj[s_shown], LV_OBJ_FLAG_HIDDEN);
        if (s_shown == (int)View::Bass) view_bass::onHide();   // its KEYS keyboard stops playing
    }
    const bool changed = s_shown != v;
    s_shown = v;
    lv_obj_t* o = s_view_obj[v];
    if (o) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        if (animate && changed) animScreenEnter(o);
    }
    if (changed) kViews[v].onShow();
}

void tickCb(lv_timer_t* t)
{
    App& a = app();
    a.tick();
    // ~0.5 s after the first frames, the remaining views are created one every ~100 ms (each takes 50-250 ms on
    // the P4), so the start shows the UI without waiting for views nobody looks at yet.
    if (s_views_pending && ++s_ticks >= 30 && s_ticks % 6 == 0) s_views_pending = createNextView();
    const uint32_t want = a.settings().low_power ? 33u : 16u;
    if (want != s_period) {
        s_period = want;
        lv_timer_set_period(t, want);
    }
    keys::poll();   // the Tab5 Keyboard (0.4.6)
    topbar::refresh();
    pads::refresh();
    catpanel::refresh();
    macros::refresh();
    timeline::refresh();
    if (a.revs().view != s_view_rev) {
        s_view_rev = a.revs().view;
        const int v = (int)a.ui().view;
        // The whole screen belongs to the BASS view (its own button brings the top bar back): another view
        // (e.g. chosen from outside the screen) restores everything first.
        if (v != s_shown && v != (int)View::Bass && panelMaxedFull()) panelToggleMax(Panel::Work);
        if (v != s_shown) showView(v, true);
    }
    if (s_shown >= 0) kViews[s_shown].refresh();
    dlg_settings::refresh();
    dlg_sample::refresh();
}

// ---------------------------------------------------------------- panel maximize
// A maximized panel is built again at its normal size, laid out natively larger (ui_scale.h) and moved into the
// area under the top bar, or onto the whole screen (the BASS view's own button). Restore builds it again at the
// normal size. Rebuilding replaces the panel's objects, so the buttons act after their event (lv_async_call).
lv_obj_t* s_panel[(int)Panel::Count] = {};
lv_obj_t* s_topbar = nullptr;
lv_obj_t* s_backdrop = nullptr;     // covers the baked panel shadows (and the top bar) while a panel is maximized
Panel s_max = Panel::Count;
bool s_max_full = false;
constexpr int kMaxX = 14, kMaxY = 73, kMaxW = 1252, kMaxH = 634;   // the area under the top bar
constexpr float kFontScaleMax = 2.0f;                              // the font ladder ends at 2x

struct PanelGeo {
    int x, y, w, h;
    float stretch;   // how much more one axis may grow than the other (fills more of the screen)
};
const PanelGeo kPanelGeo[(int)Panel::Count] = {
    {geo::pads_x, geo::pads_y, geo::pads_w, geo::pads_h, 1.15f},
    {geo::browser_x, geo::browser_y, geo::browser_w, geo::browser_h, 1.15f},
    {geo::work_x, geo::work_y, geo::work_w, geo::work_h, 1.30f},
    {geo::macros_x, geo::macros_y, geo::macros_w, geo::macros_h, 1.25f},
    {geo::timeline_x, geo::timeline_y, geo::timeline_w, geo::timeline_h, 1.30f},
};

// Scale and screen area of panel p maximized (centred in the target area).
UiScale maxScale(Panel p, bool full, lv_area_t* out)
{
    const PanelGeo& g = kPanelGeo[(int)p];
    const int tx = full ? 0 : kMaxX, ty = full ? 0 : kMaxY, tw = full ? 1280 : kMaxW, th = full ? 720 : kMaxH;
    const float fx = (float)tw / (float)g.w, fy = (float)th / (float)g.h;
    const float s = fminf(fx, fy);
    UiScale sc;
    sc.sx = fminf(fx, s * g.stretch);
    sc.sy = fminf(fy, s * g.stretch);
    sc.f = fminf(s, kFontScaleMax);
    const int w = (int)lroundf((float)g.w * sc.sx), h = (int)lroundf((float)g.h * sc.sy);
    out->x1 = tx + (tw - w) / 2;
    out->y1 = ty + (th - h) / 2;
    out->x2 = out->x1 + w - 1;
    out->y2 = out->y1 + h - 1;
    return sc;
}

lv_obj_t* createWork()
{
    lv_obj_t* w = panelCreate(s_scr, geo::work_x, geo::work_y, geo::work_w, geo::work_h);
    if (bg::shadowsBaked()) lv_obj_set_style_shadow_width(w, 0, 0);
    panelTopHighlight(w, geo::work_w);
    return w;
}

lv_obj_t* createPanel(Panel p)
{
    switch (p) {
    case Panel::Pads: return pads::create(s_scr);
    case Panel::Browser: return catpanel::create(s_scr);
    case Panel::Work: return createWork();
    case Panel::Macros: return macros::create(s_scr);
    case Panel::Timeline: return timeline::create(s_scr);
    default: return nullptr;
    }
}

// Builds panel p again: at its design place (scale unit) or maximized into area.
void rebuildPanel(Panel p, UiScale s, const lv_area_t& area)
{
    if ((int)p >= (int)Panel::Count) return;
    lv_obj_t* old = s_panel[(int)p];
    const int32_t idx = old ? lv_obj_get_index(old) : -1;
    if (p == Panel::Work) {
        for (lv_obj_t*& o : s_view_obj) o = nullptr;
        s_shown = -1;
        s_work = nullptr;
    }
    if (old) lv_obj_delete(old);
    lv_obj_t* root;
    {
        UiBuildScope scope(s);
        root = createPanel(p);
    }
    s_panel[(int)p] = root;
    if (!root) return;
    if (!s.unit()) {
        uiScaleRegister(root, s);
        uiScaleTree(root, s);
        lv_obj_set_pos(root, area.x1, area.y1);
    }
    if (idx >= 0) lv_obj_move_to_index(root, idx);
    if (p == Panel::Work) {
        s_work = root;
        s_view_rev = app().revs().view;
        showView((int)app().ui().view, false);
        // Normal size: the other views follow one by one again. Maximized: only when their tab is tapped (each is
        // built and laid out larger then, 0.1-0.3 s on the P4: not worth a stutter for views nobody opened).
        s_views_pending = s.unit();
    }
}

void maxLine(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = c(col::ink);
    ld.width = w;
    ld.round_start = 1;
    ld.round_end = 1;
    ld.p1.x = x1;
    ld.p1.y = y1;
    ld.p2.x = x2;
    ld.p2.y = y2;
    lv_draw_line(layer, &ld);
}

// The panel a button stands for, and whether its tap means "whole screen" (BASS view).
struct MaxBtn {
    Panel p;
    bool full;
};
inline void* packBtn(Panel p, bool full) { return (void*)(intptr_t)((int)p | (full ? 0x100 : 0)); }
inline MaxBtn unpackBtn(void* u)
{
    const intptr_t v = (intptr_t)u;
    return MaxBtn{(Panel)(v & 0xFF), (v & 0x100) != 0};
}

// Four corner marks: pointing out (maximize) or, while its panel is maximized, in (restore).
void maxBtnDraw(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const bool maxed = unpackBtn(lv_event_get_user_data(e)).p == s_max;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    // a square in the middle (a maximized panel may stretch the button's box); proportions of the 26 px design,
    // rounded at the end only, strokes thickened in whole px so the marks keep their gaps
    const int32_t side = lv_area_get_width(&a) < lv_area_get_height(&a) ? lv_area_get_width(&a) : lv_area_get_height(&a);
    const float k = (float)side / 26.f;
    const float fx0 = (float)a.x1 + (float)(lv_area_get_width(&a) - side) * 0.5f;
    const float fy0 = (float)a.y1 + (float)(lv_area_get_height(&a) - side) * 0.5f;
    const float in = 6.f * k, fL = 5.f * k, far = (float)(side - 1) - in;
    const int32_t w = k < 1.f ? 2 : (int32_t)(2.f * k);
    const int32_t x1 = (int32_t)lroundf(fx0 + in), y1 = (int32_t)lroundf(fy0 + in);
    const int32_t x2 = (int32_t)lroundf(fx0 + far), y2 = (int32_t)lroundf(fy0 + far);
    const int32_t L = (int32_t)lroundf(fL);
    if (!maxed) {
        maxLine(layer, x1, y1, x1 + L, y1, w), maxLine(layer, x1, y1, x1, y1 + L, w);
        maxLine(layer, x2, y1, x2 - L, y1, w), maxLine(layer, x2, y1, x2, y1 + L, w);
        maxLine(layer, x1, y2, x1 + L, y2, w), maxLine(layer, x1, y2, x1, y2 - L, w);
        maxLine(layer, x2, y2, x2 - L, y2, w), maxLine(layer, x2, y2, x2, y2 - L, w);
    } else {
        maxLine(layer, x1 + L, y1, x1 + L, y1 + L, w), maxLine(layer, x1, y1 + L, x1 + L, y1 + L, w);
        maxLine(layer, x2 - L, y1, x2 - L, y1 + L, w), maxLine(layer, x2, y1 + L, x2 - L, y1 + L, w);
        maxLine(layer, x1 + L, y2, x1 + L, y2 - L, w), maxLine(layer, x1, y2 - L, x1 + L, y2 - L, w);
        maxLine(layer, x2 - L, y2, x2 - L, y2 - L, w), maxLine(layer, x2, y2 - L, x2 - L, y2 - L, w);
    }
}

void maxAsync(void* u)
{
    const MaxBtn b = unpackBtn(u);
    if (b.full && s_max != Panel::Count) panelToggleMax(s_max);   // restore whatever is maximized
    else panelToggleMaxEx(b.p, b.full);
}
void maxBtnCb(lv_event_t* e) { lv_async_call(maxAsync, lv_event_get_user_data(e)); }

}  // namespace

lv_obj_t* panelMaxButton(lv_obj_t* parent, Panel p, bool full_screen)
{
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 26, 26);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, c(col::hover), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, maxBtnDraw, LV_EVENT_DRAW_MAIN, packBtn(p, full_screen));
    lv_obj_add_event_cb(b, maxBtnCb, LV_EVENT_CLICKED, packBtn(p, full_screen));
    return b;
}

void panelToggleMaxEx(Panel p, bool full_screen)
{
    if ((int)p >= (int)Panel::Count || !s_scr) return;
#ifdef ESP_PLATFORM
    const uint64_t t0 = nowUs();
#endif
    const Panel was = s_max;
    lv_area_t area = {};
    if (was != Panel::Count) {   // the maximized one goes back first
        s_max = Panel::Count;
        s_max_full = false;
        rebuildPanel(was, UiScale{}, area);
    }
    if (was != p) {
        const UiScale s = maxScale(p, full_screen, &area);
        s_max = p;
        s_max_full = full_screen;
        rebuildPanel(p, s, area);
    }
    for (int i = 0; i < (int)Panel::Count; ++i) {
        if (!s_panel[i]) continue;
        if (s_max != Panel::Count && i != (int)s_max) lv_obj_add_flag(s_panel[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_panel[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_topbar) {
        if (s_max_full) lv_obj_add_flag(s_topbar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_topbar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_backdrop) {
        const int32_t y = s_max_full ? 0 : geo::top_h + 2;
        lv_obj_set_pos(s_backdrop, 0, y);
        lv_obj_set_size(s_backdrop, 1280, 720 - y);
        if (s_max != Panel::Count) lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    pads::touchGeometry();
    lv_obj_invalidate(lv_screen_active());
#ifdef ESP_PLATFORM
    logf("ui   %s panel %d%s: %u ms, internal %u B free, PSRAM %u B", s_max == Panel::Count ? "restore" : "maximize", (int)p,
         s_max_full ? " (screen)" : "", (unsigned)((nowUs() - t0) / 1000u), (unsigned)hal::freeInternal(),
         (unsigned)hal::freePsram());
#endif
}

void panelToggleMax(Panel p) { panelToggleMaxEx(p, false); }

Panel panelMaxed() { return s_max; }
bool panelMaxedFull() { return s_max != Panel::Count && s_max_full; }

lv_obj_t* panelRoot(Panel p) { return (int)p < (int)Panel::Count ? s_panel[(int)p] : nullptr; }

void ensureView(View v) { createView((int)v); }

void build(lv_display_t* disp)
{
    if (!disp) disp = lv_display_get_default();
    lv_obj_t* scr = lv_display_get_screen_active(disp);
    if (!scr || s_timer) return;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
#ifdef ESP_PLATFORM
    uint64_t t_step = nowUs();
    auto mark = [&](const char* what) {
        const uint64_t now = nowUs();
        logf("ui   %-14s %5u ms", what, (unsigned)((now - t_step) / 1000u));
        t_step = now;
    };
#else
    auto mark = [](const char*) {};
#endif

    s_scr = scr;
    bg::create(scr);
    mark("background");
    // Maximize backdrop: right above the baked background, below every panel (hidden until a panel is maximized).
    s_backdrop = lv_obj_create(scr);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_pos(s_backdrop, 0, geo::top_h + 2);
    lv_obj_set_size(s_backdrop, 1280, 720 - geo::top_h - 2);
    lv_obj_set_style_bg_color(s_backdrop, c(col::bg), 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    s_topbar = topbar::create(scr);
    mark("topbar");
    s_panel[(int)Panel::Pads] = pads::create(scr);
    mark("pads");
    s_panel[(int)Panel::Browser] = catpanel::create(scr);
    s_work = createWork();
    s_panel[(int)Panel::Work] = s_work;
    s_panel[(int)Panel::Macros] = macros::create(scr);
    mark("cat+work+macros");
    s_panel[(int)Panel::Timeline] = timeline::create(scr);
    mark("timeline");

    // Only the view on screen now; the others follow after the first frame (tickCb) or when shown.
    s_view_rev = app().revs().view;
    showView((int)app().ui().view, false);
    mark("first view");

    pads::installTouchHook();
    // Shared press style in line with Settings before the first frame: otherwise the first tap
    // after boot restyles every button (invalidate + relayout of the whole screen).
    pressFeedbackSync();
    mark("press style");
    s_period = app().settings().low_power ? 33u : 16u;
    s_timer = lv_timer_create(tickCb, s_period, nullptr);
}

}  // namespace orbit::ui
