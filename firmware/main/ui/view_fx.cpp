// FX view (work area, new — no prototype screen): send effects, master macros and the master
// section, in the visual language of the prototype (.l-sec-card cards with overline titles, dark
// editor knobs, .tab toggles, .toggle switches).
//   REVERB: SIZE, DECAY, DAMP, PRE-DLY, LEVEL          DELAY: TIME (synced), FEEDBACK, TONE, LEVEL + PING-PONG
//   MACROS: LO-FI (BITS, RATE, MIX), WIDE (WIDTH),     MASTER: LIMITER + gain-reduction meter,
//           PUMP (AMOUNT, RELEASE, SOURCE pad),                MASTER level, CLICK level, METRONOME
//           SPACE (AMOUNT)
// Layout (screen px): head 82..118 like the mixer; row 1 cards 118..248; row 2 cards 258..547.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui::view_fx {

namespace {

constexpr int kKnobD = 46;
constexpr int kKnobs = 16;
constexpr int kGrMaxDb = 12;

struct View {
    lv_obj_t* root = nullptr;
    int ox = geo::work_x + 1, oy = geo::work_y + 1;
    lv_obj_t* knob[kKnobs] = {};
    int nknobs = 0;
    lv_obj_t* macro[4] = {};
    lv_obj_t* pingpong = nullptr;
    lv_obj_t* limiter = nullptr;
    lv_obj_t* metro = nullptr;
    lv_obj_t* pump_card = nullptr;
    lv_obj_t* pump_src = nullptr;
    int pump_x = 0, pump_y = 0;
    char pump_names[kPads][kNameLen + 4] = {};
    lv_obj_t* gr = nullptr;
    lv_obj_t* gr_text = nullptr;
    float gr_db = 0.f;     // shown
    int gr_px = -1;        // drawn fill width
    Revs drawn{};
    bool drawn_valid = false;
    uint32_t last_meter_ms = 0;
};
View v;

inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

void place(lv_obj_t* o, int x, int y) { lv_obj_set_pos(o, x - v.ox, y - v.oy); }

// .l-sec-card: control bg, line border, radius 6; overline title at (12, 10).
lv_obj_t* card(int x, int y, int w, int h, const char* title)
{
    lv_obj_t* o = lv_obj_create(v.root);
    lv_obj_remove_style_all(o);
    place(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c(col::control), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c(col::line), 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* t = overline(o, title);
    lv_obj_set_style_text_font(t, f_s13, 0);   // 13 px caption (1.2x rule): the box has room
    lv_obj_set_style_text_letter_space(t, ls(13, .13f), 0);
    lv_obj_set_pos(t, 11, 8);
    return o;
}

lv_obj_t* knob(lv_obj_t* parent, int x, int y, GlobalParam p, const char* caption)
{
    lv_obj_t* k = knobCreate(parent, KnobLook::Dark, kKnobD, ParamRef::global(p), caption);
    lv_obj_set_pos(k, x, y);
    if (v.nknobs < kKnobs) v.knob[v.nknobs++] = k;
    return k;
}

// Knobs spread evenly over [x0, x0 + span) (each wrap 64 px wide).
void knobRow(lv_obj_t* parent, int x0, int span, int y, const GlobalParam* ps, const char* const* caps, int n)
{
    const int step = n > 1 ? (span - 64) / (n - 1) : 0;
    for (int i = 0; i < n; ++i) knob(parent, x0 + i * step, y, ps[i], caps[i]);
}

lv_obj_t* rowLabel(lv_obj_t* parent, const char* text, int x, int y)
{
    lv_obj_t* l = label(parent, text, f_m13, col::ink);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ------------------------------------------------------------------ callbacks
void onMacro(lv_event_t* e) { app().toggleMacro((int)(intptr_t)lv_event_get_user_data(e)); }
void onPingPong(bool on, void*) { app().paramSet(ParamRef::global(GP_DLY_PINGPONG), on ? 1.f : 0.f); }
void onLimiter(bool on, void*) { app().setLimiter(on); }
void onMetro(bool on, void*) { app().setMetronome(on); }
void onPumpSrc(int index, void*) { app().setPumpSource(index); }

// ------------------------------------------------------------------ gain-reduction meter
// One custom-drawn bar: meter-bg track, accent fill from the left (0 .. 12 dB of reduction).
void grDraw(lv_event_t* e)
{
    lv_obj_t* o = (lv_obj_t*)lv_event_get_current_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const Xf tf = xfOf(o);   // corners as drawn (maximized panel: larger)
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = c(col::meter_bg);
    d.radius = tf.F(3);
    d.border_width = 1;
    d.border_color = c(col::line);
    d.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &d, &a);
    const int inner = lv_area_get_width(&a) - 2;
    v.gr_px = clampi((int)lroundf(v.gr_db / (float)kGrMaxDb * (float)inner), 0, inner);
    if (v.gr_px > 0) {
        lv_draw_rect_dsc_t f;
        lv_draw_rect_dsc_init(&f);
        f.bg_opa = LV_OPA_COVER;
        f.bg_color = c(col::accent);
        f.radius = tf.F(2);
        lv_area_t fa = {a.x1 + 1, a.y1 + 1, a.x1 + v.gr_px, a.y2 - 1};
        lv_draw_rect(layer, &f, &fa);
    }
    // dB ticks every 3 dB
    lv_draw_rect_dsc_t t;
    lv_draw_rect_dsc_init(&t);
    t.bg_opa = 90;
    t.bg_color = c(col::ink);
    for (int db = 3; db < kGrMaxDb; db += 3) {
        const int x = a.x1 + 1 + db * inner / kGrMaxDb;
        lv_area_t ta = {x, a.y1 + 2, x, a.y2 - 2};
        lv_draw_rect(layer, &t, &ta);
    }
}

void grUpdate()
{
    const Telemetry& t = app().engine().telemetry();
    float db = t.gain_reduction.load(std::memory_order_relaxed);
    if (!(db > 0.f) || !app().project().limiter_on) db = 0.f;
    // Fast attack, ~300 ms release so short peaks stay readable.
    const float fall = v.gr_db * 0.9f;
    v.gr_db = db > fall ? db : fall;
    if (v.gr_db < 0.05f) v.gr_db = 0.f;
    lv_area_t a;
    lv_obj_get_coords(v.gr, &a);
    const int inner = lv_area_get_width(&a) - 2;
    const int px = clampi((int)lroundf(v.gr_db / (float)kGrMaxDb * (float)inner), 0, inner);
    if (px != v.gr_px) {
        lv_area_t band = {a.x1 + (px < v.gr_px ? px : (v.gr_px < 0 ? 0 : v.gr_px)), a.y1,
                          a.x1 + 2 + (px > v.gr_px ? px : v.gr_px), a.y2};
        if (v.gr_px < 0) band = a;
        v.gr_px = px;
        lv_obj_invalidate_area(v.gr, &band);
    }
    char buf[24];
    snprintf(buf, sizeof buf, "GR -%.1f dB", (double)(roundf(v.gr_db * 2.f) * 0.5f));
    if (v.gr_db <= 0.f) snprintf(buf, sizeof buf, "GR 0.0 dB");
    if (strcmp(lv_label_get_text(v.gr_text), buf) != 0) lv_label_set_text(v.gr_text, buf);
}

// ------------------------------------------------------------------ PUMP source select
void pumpNames(char out[kPads][kNameLen + 4])
{
    for (int i = 0; i < kPads; ++i) {
        const char* n = app().project().pad[i].name;
        snprintf(out[i], kNameLen + 4, "%c%d %s", 'A' + i / 4, i % 4 + 1, n);
    }
}

void pumpSelect()
{
    char names[kPads][kNameLen + 4];
    pumpNames(names);
    const int sel = clampi(app().project().pump_src, 0, kPads - 1);
    if (v.pump_src && memcmp(names, v.pump_names, sizeof names) == 0) {
        selectSet(v.pump_src, sel);
        return;
    }
    memcpy(v.pump_names, names, sizeof names);
    if (v.pump_src) lv_obj_delete(v.pump_src);
    const char* opts[kPads];
    for (int i = 0; i < kPads; ++i) opts[i] = v.pump_names[i];
    v.pump_src = selectCreate(v.pump_card, 98, 30, opts, kPads, sel, onPumpSrc, nullptr);
    uiScaleTree(v.pump_src, uiScaleOf(v.pump_card));   // created after the panel: laid out like it (maximize)
    lv_obj_set_pos(v.pump_src, uiSx(v.pump_src, v.pump_x), uiSy(v.pump_src, v.pump_y));
    // one line with dots for long pad names (the list shows them in full)
    if (lv_obj_t* l = lv_obj_get_child(v.pump_src, 0)) {
        if (lv_obj_check_type(l, &lv_label_class)) lv_obj_set_height(l, lv_font_get_line_height(uiFont(l, f_r13)));
    }
}

void updateAll(bool names)
{
    for (int i = 0; i < v.nknobs; ++i) knobRefresh(v.knob[i]);
    for (int i = 0; i < 4; ++i) btnSetActive(v.macro[i], app().macro(i));
    toggleSet(v.pingpong, app().paramGet(ParamRef::global(GP_DLY_PINGPONG)) >= 0.5f);
    toggleSet(v.limiter, app().project().limiter_on != 0);
    toggleSet(v.metro, app().project().metronome != 0);
    if (names) pumpSelect();
    else selectSet(v.pump_src, clampi(app().project().pump_src, 0, kPads - 1));
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

    // ---- head (same as the mixer head)
    lv_obj_t* ic = iconCreate(v.root, Icon::Spark, 18, col::ink);
    place(ic, 328, 87);
    lv_obj_t* t = lv_label_create(v.root);
    lv_label_set_text(t, "FX & MASTER");
    lv_obj_add_style(t, &styles().panel_title, 0);
    place(t, 356, 88);
    lv_obj_t* ov = overline(v.root, "SEND \xC2\xB7 MACROS \xC2\xB7 LIMITER");
    lv_obj_set_width(ov, 260);
    lv_obj_set_style_text_font(ov, f_s13, 0);   // 13 px caption (1.2x rule): the box has room
    lv_obj_set_style_text_letter_space(ov, ls(13, .13f), 0);
    lv_obj_set_style_text_align(ov, LV_TEXT_ALIGN_RIGHT, 0);
    place(ov, 1040 - 260, 89);

    // ---- row 1: REVERB | DELAY
    {
        lv_obj_t* cr = card(328, 118, 370, 130, "REVERB");
        static const GlobalParam ps[5] = {GP_REV_SIZE, GP_REV_DECAY, GP_REV_DAMP, GP_REV_PREDELAY, GP_REV_LEVEL};
        static const char* const caps[5] = {"SIZE", "DECAY", "DAMP", "PRE-DLY", "LEVEL"};
        knobRow(cr, 11, 346, 33, ps, caps, 5);
    }
    {
        lv_obj_t* cd = card(708, 118, 332, 130, "DELAY");
        static const GlobalParam ps[4] = {GP_DLY_DIV, GP_DLY_FEEDBACK, GP_DLY_TONE, GP_DLY_LEVEL};
        static const char* const caps[4] = {"TIME", "FEEDBACK", "TONE", "LEVEL"};
        knobRow(cd, 11, 308, 33, ps, caps, 4);
        lv_obj_t* pl = overline(cd, "PING-PONG");
        lv_obj_set_width(pl, 90);
        lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(pl, 330 - 12 - 45 - 8 - 90, 9);
        v.pingpong = toggleCreate(cd, app().paramGet(ParamRef::global(GP_DLY_PINGPONG)) >= 0.5f, onPingPong, nullptr);
        lv_obj_set_pos(v.pingpong, 330 - 12 - 45, 4);
    }

    // ---- row 2: MACROS (2 x 2 blocks: tab toggle + knobs) | MASTER
    {
        lv_obj_t* cm = card(328, 258, 510, 289, "MACROS");
        static const char* const names[4] = {"LO-FI", "WIDE", "PUMP", "SPACE"};
        const int bw = 236;
        for (int i = 0; i < 4; ++i) {
            const int bx = 11 + (i % 2) * (bw + 12), by = 30 + (i / 2) * 130;
            v.macro[i] = tabBtn(cm, names[i], bw, 30);
            lv_obj_set_pos(v.macro[i], bx, by);
            lv_obj_add_event_cb(v.macro[i], onMacro, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            const int ky = by + 36;
            switch (i) {
            case 0:
                knob(cm, bx, ky, GP_LOFI_BITS, "BITS");
                knob(cm, bx + 72, ky, GP_LOFI_RATE, "RATE");
                knob(cm, bx + 144, ky, GP_LOFI_MIX, "MIX");
                break;
            case 1: knob(cm, bx, ky, GP_WIDE, "WIDTH"); break;
            case 2: {
                knob(cm, bx, ky, GP_PUMP_AMT, "AMOUNT");
                knob(cm, bx + 68, ky, GP_PUMP_RELEASE, "RELEASE");
                lv_obj_t* sl = overline(cm, "SOURCE");
                lv_obj_set_pos(sl, bx + 138, ky);
                v.pump_card = cm;
                v.pump_x = bx + 138;
                v.pump_y = ky + 25;
                break;
            }
            default: knob(cm, bx, ky, GP_SPACE_AMT, "AMOUNT"); break;
            }
        }
    }
    {
        lv_obj_t* cs = card(848, 258, 192, 289, "MASTER");
        rowLabel(cs, "LIMITER", 11, 38);
        v.limiter = toggleCreate(cs, app().project().limiter_on != 0, onLimiter, nullptr);
        lv_obj_set_pos(v.limiter, 190 - 12 - 45, 35);
        v.gr = lv_obj_create(cs);
        lv_obj_remove_style_all(v.gr);
        lv_obj_set_size(v.gr, 166, 10);
        lv_obj_set_pos(v.gr, 11, 72);
        lv_obj_remove_flag(v.gr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(v.gr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(v.gr, grDraw, LV_EVENT_DRAW_MAIN, nullptr);
        v.gr_text = label(cs, "GR 0.0 dB", f_r12, col::muted);
        lv_obj_set_width(v.gr_text, 166);
        lv_obj_set_pos(v.gr_text, 11, 87);
        lv_obj_t* scale = label(cs, "12 dB", f_r12, col::muted);
        lv_obj_set_width(scale, 60);
        lv_obj_set_style_text_align(scale, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(scale, 177 - 60, 87);
        knob(cs, 11, 116, GP_MASTER, "MASTER");
        knob(cs, 190 - 12 - 64, 116, GP_METRO_LEVEL, "CLICK");
        rowLabel(cs, "METRONOME", 11, 243);
        v.metro = toggleCreate(cs, app().project().metronome != 0, onMetro, nullptr);
        lv_obj_set_pos(v.metro, 190 - 12 - 45, 240);
    }
    v.drawn_valid = false;
    refresh();
    return v.root;
}

void refresh()
{
    if (!v.root) return;
    const Revs& r = app().revs();
    const bool all = !v.drawn_valid;
    const bool names = all || r.pads != v.drawn.pads || r.project != v.drawn.project || r.samples != v.drawn.samples;
    if (names || r.params != v.drawn.params || r.fx != v.drawn.fx || r.mixer != v.drawn.mixer ||
        r.transport != v.drawn.transport)
        updateAll(names);
    v.drawn = r;
    v.drawn_valid = true;
    const uint32_t now = lv_tick_get();
    if (now - v.last_meter_ms >= 33) {
        v.last_meter_ms = now;
        grUpdate();
    }
}

void onShow()
{
    v.drawn_valid = false;
    refresh();
}

}  // namespace orbit::ui::view_fx
