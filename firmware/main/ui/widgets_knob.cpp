// Widget kit, part 2: knob, vertical fader, segmented meter. Each is ONE custom-drawn object
// (DRAW_MAIN) that invalidates only when what it shows changes.
// The knob shows its value with the accent arc and a single dot (no pointer line). A value that
// changes without the finger (scene, undo, MIDI jump, pad switch, double-tap reset) glides there.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ui/widgets.h"
#include "ui/widgets_internal.h"

namespace orbit::ui {

namespace {

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// ================================================================ knob body images
// The knob body (radial gradient, 1 px border, outer ring, drop shadow) never changes, so it is
// rendered once per (look, diameter) into an ARGB8888 image and blitted; only the arc, pointer
// and dot are drawn live. Exact CSS gradients without LVGL's complex-gradient engine.
struct BodyImg {
    uint8_t look;
    uint16_t d;
    int16_t margin;
    lv_image_dsc_t dsc;
    uint8_t* px;
};
constexpr int kMaxBodies = 40;   // normal sizes + the maximized panels' larger ones
BodyImg s_bodies[kMaxBodies];
int s_body_count = 0;

struct LookSpec {
    uint32_t shine, base, border, ring;
    float gx, gy, stop;      // gradient centre (fraction of the box) and colour stop (0..1)
    int ring_w;              // box-shadow 0 0 0 Npx ring
    float sh_y, sh_blur, sh_opa;
};

LookSpec lookSpec(KnobLook look)   // the palette of the session (theme.h baseSelect)
{
    // .knob: radial-gradient(circle at 35% 30%, shine, base 75%), border 1, box-shadow
    //        0 4px 6px #0003, 0 0 0 4px ring.
    // .l-macro-stack .knob: radial-gradient(circle at 37% 31%, #fefefe, #ced2d5 80%),
    //        box-shadow 0 2px 8px #0002, 0 0 0 6px #dee0e2.
    if (look == KnobLook::White)
        return {col::mknob_a, col::mknob_b, col::mknob_border, col::mknob_ring, 0.37f, 0.31f, 0.80f, 6, 2.f, 8.f, 0.133f};
    return {col::knob_shine, col::knob_base, col::knob_border, col::knob_ring, 0.35f, 0.30f, 0.75f, 4, 4.f, 6.f, 0.20f};
}

inline float chan(uint32_t hex, int shift) { return (float)((hex >> shift) & 0xFF) / 255.f; }

// 4x4 Bayer thresholds in (0, 1).
const float kBayer[16] = {0.5f / 16, 8.5f / 16, 2.5f / 16, 10.5f / 16, 12.5f / 16, 4.5f / 16, 14.5f / 16, 6.5f / 16,
                          3.5f / 16, 11.5f / 16, 1.5f / 16, 9.5f / 16, 15.5f / 16, 7.5f / 16, 13.5f / 16, 5.5f / 16};

// v 0..1 -> nearest-below/above RGB565 level chosen by the threshold, expanded back to 8 bits.
inline uint8_t dither565(float v, uint32_t bits, float threshold)
{
    const uint32_t m = (1u << bits) - 1u;
    uint32_t n = (uint32_t)(v * (float)m + threshold);
    if (n > m) n = m;
    return (uint8_t)((n << (8u - bits)) | (n >> (2u * bits - 8u)));
}

BodyImg* bodyImage(KnobLook look, int d)
{
    for (int i = 0; i < s_body_count; ++i)
        if (s_bodies[i].look == (uint8_t)look && s_bodies[i].d == d) return &s_bodies[i];
    if (s_body_count >= kMaxBodies) return nullptr;
    const LookSpec& L = lookSpec(look);
    const int m = 12;
    const int W = d + 2 * m;
    // Large static buffer: plain malloc (PSRAM on the Tab5), not the LVGL heap.
    uint8_t* px = (uint8_t*)malloc((size_t)W * W * 4);
    if (!px) return nullptr;
    const float R = d * 0.5f;
    const float cx = W * 0.5f, cy = W * 0.5f;
    // Gradient: circle, farthest-corner size, colour stop at L.stop of that radius.
    const float gx = m + L.gx * d, gy = m + L.gy * d;
    const float fx = fmaxf(gx - m, (float)(m + d) - gx), fy = fmaxf(gy - m, (float)(m + d) - gy);
    const float glen = sqrtf(fx * fx + fy * fy) * L.stop;
    const float sigma = L.sh_blur * 0.5f;
    for (int y = 0; y < W; ++y) {
        for (int x = 0; x < W; ++x) {
            const float pxf = x + 0.5f, pyf = y + 0.5f;
            const float r = sqrtf((pxf - cx) * (pxf - cx) + (pyf - cy) * (pyf - cy));
            // premultiplied accumulation, back to front
            float ar = 0.f, ag = 0.f, ab = 0.f, aa = 0.f;
            auto over = [&](float cr, float cg, float cb, float a) {
                ar = cr * a + ar * (1.f - a);
                ag = cg * a + ag * (1.f - a);
                ab = cb * a + ab * (1.f - a);
                aa = a + aa * (1.f - a);
            };
            const float ring_a = clamp01(R + (float)L.ring_w - r + 0.5f);
            if (ring_a > 0.f) over(chan(L.ring, 16), chan(L.ring, 8), chan(L.ring, 0), ring_a);
            const float rs = sqrtf((pxf - cx) * (pxf - cx) + (pyf - cy - L.sh_y) * (pyf - cy - L.sh_y));
            const float sh_a = L.sh_opa * 0.5f * erfcf((rs - R) / (sigma * 1.41421356f));
            if (sh_a > 0.002f) over(0.f, 0.f, 0.f, sh_a);
            const float body_a = clamp01(R - r + 0.5f);
            if (body_a > 0.f) {
                const float gd = sqrtf((pxf - gx) * (pxf - gx) + (pyf - gy) * (pyf - gy));
                const float t = clamp01(gd / glen);
                float cr = chan(L.shine, 16) + (chan(L.base, 16) - chan(L.shine, 16)) * t;
                float cg = chan(L.shine, 8) + (chan(L.base, 8) - chan(L.shine, 8)) * t;
                float cb = chan(L.shine, 0) + (chan(L.base, 0) - chan(L.shine, 0)) * t;
                const float inner = clamp01(R - 1.f - r + 0.5f);   // 1 px border band
                cr = chan(L.border, 16) + (cr - chan(L.border, 16)) * inner;
                cg = chan(L.border, 8) + (cg - chan(L.border, 8)) * inner;
                cb = chan(L.border, 0) + (cb - chan(L.border, 0)) * inner;
                over(cr, cg, cb, body_a);
            }
            uint8_t* o = px + ((size_t)y * W + x) * 4;
            if (aa <= 0.f) {
                o[0] = o[1] = o[2] = o[3] = 0;
            } else {
                // ARGB8888 in memory: B G R A. Colours are ordered-dithered onto the RGB565 grid
                // (the display truncates to 565, which would band the gradient into rings).
                const float d = kBayer[(y & 3) * 4 + (x & 3)];
                o[0] = dither565(clamp01(ab / aa), 5, d);
                o[1] = dither565(clamp01(ag / aa), 6, d);
                o[2] = dither565(clamp01(ar / aa), 5, d);
                o[3] = (uint8_t)lroundf(clamp01(aa) * 255.f);
            }
        }
    }
    BodyImg& b = s_bodies[s_body_count++];
    b.look = (uint8_t)look;
    b.d = (uint16_t)d;
    b.margin = (int16_t)m;
    b.px = px;
    memset(&b.dsc, 0, sizeof b.dsc);
    b.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    b.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    b.dsc.header.w = (uint32_t)W;
    b.dsc.header.h = (uint32_t)W;
    b.dsc.header.stride = (uint32_t)W * 4;
    b.dsc.data_size = (uint32_t)W * W * 4;
    b.dsc.data = px;
    return &b;
}

// ================================================================ knob
struct Knob {
    ParamRef ref;
    KnobLook look;
    int16_t d;
    lv_obj_t* wrap;
    lv_obj_t* knob;
    lv_obj_t* cap;
    lv_obj_t* val;
    BodyImg* body;
    int16_t body_d;     // diameter body was made for (a maximized panel draws the knob larger)
    float drawn;        // norm drawn last
    float origin;       // arc origin (norm): 0, or the zero point of bipolar parameters
    float drag;         // un-snapped drag accumulator
    float glide_from;   // glide animation: drawn moves from glide_from to glide_to
    float glide_to;
    int32_t dead;       // dead-zone accumulator (px) before a drag starts
    bool dragging;
    bool moved;
    bool gliding;
    uint32_t press_ms;
    uint32_t last_tap_ms;
    char caption[24];   // empty: caption follows the parameter spec
    char text[24];
};

constexpr int kDeadZone = 4;   // px of travel before a press becomes a drag (taps stay exact)

float sensitivityPerPx()
{
    // Prototype: 0.58 % of the range per px at the default 56. Settings 1..100 map exponentially
    // (x2 every 22 steps): 1 -> ~1000 px, 56 -> 172 px, 100 -> ~43 px for the full range.
    const float s = (float)app().settings().knob_sensitivity;
    return 0.0058f * exp2f((s - 56.f) / 22.f);
}

float originOf(const ParamRef& r)
{
    const ParamSpec* s = app().paramSpec(r);
    // Bipolar (roughly symmetric around 0: pan, tune, gain, env depth): the arc starts at 0.
    if (s && s->min < 0.f && s->max > 0.f && s->steps == 0 && fabsf(s->min + s->max) < 0.2f * (s->max - s->min))
        return clamp01(toNorm(*s, 0.f));
    if (r.kind == ParamRef::Kind::BusPan || r.kind == ParamRef::Kind::LayerPan) return 0.5f;
    return 0.f;
}

void knobUpdateText(Knob* k)
{
    char buf[24];
    app().paramText(k->ref, buf, sizeof buf);
    if (strcmp(buf, k->text) != 0) {
        memcpy(k->text, buf, sizeof buf);
        lv_label_set_text(k->val, k->text);
    }
}

// Glide: ease-out over kGlideMs from the dot's current place to the new value. Smaller changes
// (a MIDI CC step, fine edits) follow at once so the knob never lags behind a controller.
constexpr uint32_t kGlideMs = 220;
constexpr float kGlideMin = 0.012f;

void glideExec(lv_anim_t* a, int32_t v)
{
    Knob* k = (Knob*)lv_anim_get_user_data(a);
    k->drawn = k->glide_from + (k->glide_to - k->glide_from) * ((float)v / 1024.f);
    lv_obj_invalidate(k->knob);
}

void glideDone(lv_anim_t* a)
{
    Knob* k = (Knob*)lv_anim_get_user_data(a);
    k->gliding = false;
    k->drawn = k->glide_to;
    lv_obj_invalidate(k->knob);
}

void glideStop(Knob* k)
{
    if (!k->gliding) return;
    k->gliding = false;
    lv_anim_delete(k->knob, nullptr);   // the knob object has no other animations
}

void glideStart(Knob* k, float to)
{
    glideStop(k);   // a new target restarts from where the dot is now
    k->glide_from = k->drawn;
    k->glide_to = to;
    k->gliding = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, k->knob);
    lv_anim_set_custom_exec_cb(&a, glideExec);
    lv_anim_set_user_data(&a, k);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, kGlideMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, glideDone);
    lv_anim_start(&a);
}

// force: redraw even when the value did not change. animate: a jump may glide (never while the
// finger drags the knob, never on the first draw, never with animations off).
void knobUpdate(Knob* k, bool force, bool animate)
{
    const float n = clamp01(app().paramNorm(k->ref));
    const float shown = k->gliding ? k->glide_to : k->drawn;
    if (force || fabsf(n - shown) > 0.0005f) {
        if (animate && k->drawn >= 0.f && !k->dragging && wk::animOn() && fabsf(n - k->drawn) >= kGlideMin) {
            glideStart(k, n);
        } else {
            glideStop(k);
            k->drawn = n;
            lv_obj_invalidate(k->knob);
        }
    }
    knobUpdateText(k);
}

void knobDraw(Knob* k, lv_layer_t* layer)
{
    lv_area_t a;
    lv_obj_get_coords(k->knob, &a);
    const float f = uiScaleOf(k->knob).f;   // inside a maximized panel everything is drawn f times larger
    const int d = f == 1.f ? k->d : (int)lroundf((float)k->d * f);
    if (d != k->body_d) {
        k->body = bodyImage(k->look, d);
        k->body_d = (int16_t)d;
    }
    const float R = d * 0.5f;
    // the body's corner: centred in the object (a maximized panel may stretch the object's box)
    const int32_t bx = a.x1 + (lv_area_get_width(&a) - d) / 2, by = a.y1 + (lv_area_get_height(&a) - d) / 2;
    const int32_t cx = bx + d / 2, cy = by + d / 2;
    const LookSpec& L = lookSpec(k->look);
    const float ring_w = (float)L.ring_w * f;
    auto F = [f](float v) {
        const int32_t r = (int32_t)lroundf(v * f);
        return r < 1 ? 1 : r;
    };
    if (k->body) {
        lv_draw_image_dsc_t id;
        lv_draw_image_dsc_init(&id);
        id.src = &k->body->dsc;
        const int m = k->body->margin;
        lv_area_t ia = {bx - m, by - m, bx - m + d + 2 * m - 1, by - m + d + 2 * m - 1};
        lv_draw_image(layer, &id, &ia);
    }
    // .knob.dragging: box-shadow 0 0 0 5px var(--glow) (drawn as a ring).
    if (k->dragging && wk::feedbackOn()) {
        lv_draw_arc_dsc_t g;
        lv_draw_arc_dsc_init(&g);
        g.center.x = cx;
        g.center.y = cy;
        g.radius = (uint16_t)(R + 5.f * f);
        g.width = F(5.f);
        g.start_angle = 0;
        g.end_angle = 360;
        g.color = c(col::glow);
        g.opa = col::glow_opa;
        lv_draw_arc(layer, &g);
    }
    // Value arc on the -135..+135 sweep, just outside the ring (.knob::before: 3 px accent).
    const float n = k->drawn;
    float a0 = k->origin, a1 = n;
    if (a1 < a0) { const float t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 > 0.004f) {
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.center.x = cx;
        ad.center.y = cy;
        ad.radius = (uint16_t)(R + ring_w + 3.f * f);
        ad.width = F(3.f);
        ad.start_angle = (lv_value_precise_t)lroundf(135.f + 270.f * a0);
        ad.end_angle = (lv_value_precise_t)lroundf(135.f + 270.f * a1);
        ad.rounded = 1;
        ad.color = c(col::accent);
        ad.opa = 230;
        lv_draw_arc(layer, &ad);
    }
    // The dot alone (.knob-dot; the prototype's pointer line .knob::after is left out), angle
    // -135..+135 from 12 o'clock.
    const float ang = (-135.f + 270.f * n) * 0.01745329f;
    const float sx = sinf(ang), sy = -cosf(ang);
    const float r_dot = 0.46f * R;
    const float fcx = bx + R, fcy = by + R;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = c(col::accent);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    const int dot = F(k->d >= 64 ? 8.f : 7.f);
    const int32_t dx = (int32_t)lroundf(fcx + sx * r_dot - dot * 0.5f);
    const int32_t dy = (int32_t)lroundf(fcy + sy * r_dot - dot * 0.5f);
    lv_area_t da = {dx, dy, dx + dot - 1, dy + dot - 1};
    lv_draw_rect(layer, &rd, &da);
}

void knobEventCb(lv_event_t* e)
{
    Knob* k = (Knob*)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_DRAW_MAIN:
        knobDraw(k, lv_event_get_layer(e));
        break;
    case LV_EVENT_REFR_EXT_DRAW_SIZE: {
        const int ext = 12 > lookSpec(k->look).ring_w + 4 ? 12 : lookSpec(k->look).ring_w + 4;
        lv_event_set_ext_draw_size(e, (int32_t)lroundf((float)ext * uiScaleOf(k->knob).f));
        break;
    }
    case LV_EVENT_PRESSED:
        wk::syncFeedback();
        k->drag = clamp01(app().paramNorm(k->ref));
        k->dead = 0;
        k->moved = false;
        k->press_ms = lv_tick_get();
        break;
    case LV_EVENT_PRESSING: {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) break;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.y == 0) break;
        int32_t dy = v.y;
        if (!k->moved) {
            k->dead += dy;
            if (abs(k->dead) < kDeadZone) break;
            k->moved = true;
            k->dragging = true;
            dy = k->dead > 0 ? k->dead - kDeadZone : k->dead + kDeadZone;
            lv_obj_invalidate(k->knob);
        }
        k->drag = clamp01(k->drag - (float)dy * sensitivityPerPx());
        app().paramSetNorm(k->ref, k->drag);
        knobUpdate(k, false, false);   // the dot follows the finger at once
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        const uint32_t now = lv_tick_get();
        if (k->dragging) {
            k->dragging = false;
            lv_obj_invalidate(k->knob);
        }
        if (code == LV_EVENT_RELEASED && !k->moved && now - k->press_ms < 350) {
            if (k->last_tap_ms && now - k->last_tap_ms < 400 && app().settings().double_tap_reset) {
                k->last_tap_ms = 0;
                app().paramReset(k->ref);
                knobUpdate(k, false, true);    // glides back to the default
            } else {
                k->last_tap_ms = now;
            }
        } else {
            k->last_tap_ms = 0;
        }
        break;
    }
    default:
        break;
    }
}

void knobWrapCb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    Knob* k = (Knob*)lv_event_get_user_data(e);
    glideStop(k);   // its animation points at k (the knob object itself is deleted after this)
    lv_free(k);
}

void knobSetCaption(Knob* k, const char* caption)
{
    if (caption) {
        strncpy(k->caption, caption, sizeof k->caption - 1);
        k->caption[sizeof k->caption - 1] = 0;
    } else {
        k->caption[0] = 0;
    }
    const char* t = k->caption[0] ? k->caption : app().paramCaption(k->ref);
    wk::setTextIfChanged(k->cap, t ? t : "");
}

}  // namespace

lv_obj_t* knobCreate(lv_obj_t* parent, KnobLook look, int diameter, const ParamRef& ref, const char* caption)
{
    Knob* k = (Knob*)lv_malloc(sizeof(Knob));
    if (!k) return nullptr;
    *k = Knob{};
    k->ref = ref;
    k->look = look;
    k->d = (int16_t)diameter;
    k->body = bodyImage(look, diameter);
    k->body_d = (int16_t)diameter;
    const bool large = diameter >= 64;

    // .knob-wrap: column, centred, gap 7 (12 for .large).
    lv_obj_t* w = lv_obj_create(parent);
    lv_obj_remove_style_all(w);
    lv_obj_set_size(w, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(w, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(w, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(w, large ? 12 : 7, 0);
    k->wrap = w;

    // .knob-label: 10 px / 600 / .075em (macro: 500) -> 11-12 px.
    k->cap = lv_label_create(w);
    const lv_font_t* cap_font = look == KnobLook::White ? f_m12 : f_s11;
    lv_obj_set_style_text_font(k->cap, cap_font, 0);
    lv_obj_set_style_text_color(k->cap, c(col::knob_text), 0);
    lv_obj_set_style_text_letter_space(k->cap, 1, 0);

    lv_obj_t* kn = lv_obj_create(w);
    lv_obj_remove_style_all(kn);
    lv_obj_set_size(kn, diameter, diameter);
    lv_obj_add_flag(kn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(kn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(kn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(kn, LV_OBJ_FLAG_SCROLL_CHAIN);   // a vertical drag must never scroll a parent
    lv_obj_remove_flag(kn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_ext_click_area(kn, (large ? 10 : 12) + hitBonus());
    k->knob = kn;

    // .knob-value: 11 px muted (12 px large) -> 13/14 px; fixed width so updates never relayout.
    k->val = lv_label_create(w);
    const lv_font_t* vf = large ? f_r14 : f_r13;
    lv_obj_set_style_text_font(k->val, vf, 0);
    lv_obj_set_style_text_color(k->val, c(col::muted), 0);
    lv_obj_set_style_text_align(k->val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(k->val, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(k->val, diameter + (large ? 22 : 14) < 64 ? 64 : diameter + (large ? 22 : 14), vf->line_height);

    lv_obj_add_event_cb(w, knobWrapCb, LV_EVENT_DELETE, k);
    lv_obj_add_event_cb(kn, knobEventCb, LV_EVENT_ALL, k);
    knobSetCaption(k, caption);
    k->origin = originOf(ref);
    k->drawn = -1.f;
    k->text[0] = 0;
    knobUpdate(k, true, false);
    lv_obj_refresh_ext_draw_size(kn);
    return w;
}

void knobBind(lv_obj_t* knob_wrap, const ParamRef& ref, const char* caption)
{
    Knob* k = (Knob*)wk::findState(knob_wrap, knobWrapCb);
    if (!k) return;
    k->ref = ref;
    k->origin = originOf(ref);
    knobSetCaption(k, caption);
    knobUpdate(k, true, true);   // another pad / layer: the dot sweeps to its value
}

void knobRefresh(lv_obj_t* knob_wrap)
{
    Knob* k = (Knob*)wk::findState(knob_wrap, knobWrapCb);
    if (k && !k->dragging) knobUpdate(k, false, true);
}

ParamRef knobRef(lv_obj_t* knob_wrap)
{
    Knob* k = (Knob*)wk::findState(knob_wrap, knobWrapCb);
    return k ? k->ref : ParamRef{};
}

// ================================================================ fader
namespace {

// Chrome's vertical range with accent-color: 8 px rounded track (dark above, accent below),
// 16 px accent thumb. Relative drag: the thumb follows the finger 1:1.
struct Fader {
    ParamRef ref;
    lv_obj_t* obj;
    float drawn;
    float drag;
};
constexpr int kThumb = 16;
const uint32_t& kFaderTrack = col::fader_track;   // #3a3b3d

int32_t thumbPx(lv_obj_t* o)   // thumb diameter as drawn (maximized panels: larger)
{
    const float f = uiScaleOf(o).f;
    return f == 1.f ? kThumb : (int32_t)lroundf((float)kThumb * f);
}

int32_t faderThumbY(lv_obj_t* o, float v)   // thumb centre (absolute y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int32_t h = lv_area_get_height(&a), th = thumbPx(o);
    return a.y1 + th / 2 + (int32_t)lroundf((1.f - v) * (float)(h - th));
}

void faderInvalidateBand(lv_obj_t* o, float v0, float v1)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t y0 = faderThumbY(o, v0), y1 = faderThumbY(o, v1);
    if (y0 > y1) { const int32_t t = y0; y0 = y1; y1 = t; }
    const int32_t th = thumbPx(o);
    lv_area_t band = {a.x1, y0 - th / 2 - 2, a.x2, y1 + th / 2 + 2};
    lv_obj_invalidate_area(o, &band);
}

void faderUpdate(Fader* f)
{
    const float v = clamp01(app().paramNorm(f->ref));
    if (fabsf(v - f->drawn) < 0.0005f) return;
    faderInvalidateBand(f->obj, f->drawn < 0.f ? v : f->drawn, v);
    if (f->drawn < 0.f) lv_obj_invalidate(f->obj);
    f->drawn = v;
}

void faderDraw(Fader* f, lv_layer_t* layer)
{
    lv_area_t a;
    lv_obj_get_coords(f->obj, &a);
    const int32_t w = lv_area_get_width(&a);
    const Xf t = xfOf(f->obj);
    const int32_t track = t.F(8), thumb = thumbPx(f->obj);
    const int32_t tx = a.x1 + w / 2 - track / 2;
    const int32_t ty = faderThumbY(f->obj, f->drawn < 0.f ? 0.f : f->drawn);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = t.F(4);
    rd.bg_color = c(kFaderTrack);
    lv_area_t top = {tx, a.y1 + t.F(2), tx + track - 1, ty};
    lv_draw_rect(layer, &rd, &top);
    rd.bg_color = c(col::accent);
    lv_area_t bot = {tx, ty, tx + track - 1, a.y2 - t.F(2)};
    lv_draw_rect(layer, &rd, &bot);
    rd.radius = LV_RADIUS_CIRCLE;
    const int32_t cx = a.x1 + w / 2;
    lv_area_t th = {cx - thumb / 2, ty - thumb / 2, cx - thumb / 2 + thumb - 1, ty - thumb / 2 + thumb - 1};
    lv_draw_rect(layer, &rd, &th);
}

void faderCb(lv_event_t* e)
{
    Fader* f = (Fader*)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        faderDraw(f, lv_event_get_layer(e));
        break;
    case LV_EVENT_PRESSED:
        wk::syncFeedback();
        f->drag = clamp01(app().paramNorm(f->ref));
        break;
    case LV_EVENT_PRESSING: {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) break;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.y == 0) break;
        const int32_t travel = lv_obj_get_height(f->obj) - thumbPx(f->obj);   // as drawn: 1:1 under the finger
        f->drag = clamp01(f->drag - (float)v.y / (float)(travel > 1 ? travel : 1));
        app().paramSetNorm(f->ref, f->drag);
        faderUpdate(f);
        break;
    }
    case LV_EVENT_DELETE:
        lv_free(f);
        break;
    default:
        break;
    }
}

}  // namespace

lv_obj_t* faderCreate(lv_obj_t* parent, int w, int h, const ParamRef& ref)
{
    Fader* f = (Fader*)lv_malloc(sizeof(Fader));
    if (!f) return nullptr;
    f->ref = ref;
    f->drawn = -1.f;
    f->drag = 0.f;
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_ext_click_area(o, 10 + hitBonus());
    f->obj = o;
    lv_obj_add_event_cb(o, faderCb, LV_EVENT_ALL, f);
    faderUpdate(f);
    return o;
}

void faderRefresh(lv_obj_t* fader)
{
    Fader* f = (Fader*)wk::findState(fader, faderCb);
    if (f && !lv_obj_has_state(fader, LV_STATE_PRESSED)) faderUpdate(f);
}

// ================================================================ meter
namespace {

// .meter: 8 px, radius 3, meter-bg + 1 px line border; lit part = accent 6 px segments with
// 2 px gaps from the bottom. Peak hold with 300 ms exponential fall.
struct Meter {
    lv_obj_t* obj;
    float level;        // displayed 0..1
    int32_t lit;        // lit px drawn last
    uint32_t last_ms;
};

void meterDraw(Meter* m, lv_layer_t* layer)
{
    lv_area_t a;
    lv_obj_get_coords(m->obj, &a);
    const Xf t = xfOf(m->obj);   // maximized: taller segments, same count per height
    const int32_t seg = t.s.sy == 1.f ? 6 : (int32_t)lroundf(6.f * t.s.sy);
    const int32_t pitch = t.s.sy == 1.f ? 8 : (int32_t)lroundf(8.f * t.s.sy);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    rd.bg_color = c(col::meter_bg);
    rd.radius = t.F(3);
    rd.border_width = 1;
    rd.border_color = c(col::line);
    rd.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &rd, &a);
    m->lit = (int32_t)lroundf(m->level * (float)(lv_area_get_height(&a) - 2));
    if (m->lit <= 0) return;
    lv_draw_rect_dsc_t sd;
    lv_draw_rect_dsc_init(&sd);
    sd.bg_opa = LV_OPA_COVER;
    sd.bg_color = c(col::accent);
    const int32_t x1 = a.x1 + 1, x2 = a.x2 - 1, bottom = a.y2 - 1;
    const int32_t top = bottom - m->lit + 1;
    for (int32_t y = bottom; y >= top; y -= pitch) {
        int32_t y0 = y - (seg - 1);
        if (y0 < top) y0 = top;
        lv_area_t s = {x1, y0, x2, y};
        lv_draw_rect(layer, &sd, &s);
    }
}

void meterCb(lv_event_t* e)
{
    Meter* m = (Meter*)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) meterDraw(m, lv_event_get_layer(e));
    else if (lv_event_get_code(e) == LV_EVENT_DELETE) lv_free(m);
}

}  // namespace

lv_obj_t* meterCreate(lv_obj_t* parent, int w, int h)
{
    Meter* m = (Meter*)lv_malloc(sizeof(Meter));
    if (!m) return nullptr;
    m->level = 0.f;
    m->lit = -1;
    m->last_ms = lv_tick_get();
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    m->obj = o;
    lv_obj_add_event_cb(o, meterCb, LV_EVENT_ALL, m);
    return o;
}

void meterSet(lv_obj_t* meter, float peak_linear)
{
    Meter* m = (Meter*)wk::findState(meter, meterCb);
    if (!m) return;
    const uint32_t now = lv_tick_get();
    const float dt = (float)(now - m->last_ms);
    m->last_ms = now;
    float target = 0.f;
    if (app().settings().meters && peak_linear > 0.f) {
        const float db = 20.f * log10f(peak_linear > 1e-6f ? peak_linear : 1e-6f);
        target = clamp01((db + 48.f) / 51.f);   // -48 dB .. +3 dB
    }
    const float fall = m->level * expf(-dt / 300.f);
    m->level = target > fall ? target : fall;
    if (m->level < 0.002f) m->level = 0.f;
    lv_area_t a;
    lv_obj_get_coords(meter, &a);
    const int32_t inner = lv_area_get_height(&a) - 2;
    if (inner <= 0 || m->lit < 0) {   // not laid out yet: draw computes the lit height
        m->lit = -1;
        lv_obj_invalidate(meter);
        return;
    }
    const int32_t lit = (int32_t)lroundf(m->level * (float)inner);
    if (lit == m->lit) return;
    // Invalidate only the rows between the old and the new top of the lit part.
    const int32_t bottom = a.y2 - 1;
    int32_t ya = bottom - m->lit, yb = bottom - lit;
    if (ya > yb) { const int32_t t = ya; ya = yb; yb = t; }
    const float sy = uiScaleOf(meter).sy;
    lv_area_t band = {a.x1, ya - (sy == 1.f ? 6 : (int32_t)lroundf(6.f * sy) + 1), a.x2, yb + 1};
    m->lit = lit;
    lv_obj_invalidate_area(meter, &band);
}

}  // namespace orbit::ui
