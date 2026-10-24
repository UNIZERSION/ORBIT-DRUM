// Baked stage background (see ui_internal.h): one 1280x720 RGB565 image in PSRAM, computed once
// at start with ordered dithering. It replaces LVGL's banded RGB565 gradients and draws, in one
// static blit, everything behind the panels that never changes:
//   #stage           linear-gradient(140deg, #f1f2f3, #dadde0 96%)
//   #stage:before    1 px lines of #fff3 every 5 px at opacity .55 (alpha ~.11)
//   #light-top       linear-gradient(#f2f3f4, #e3e5e6) + border-bottom 1 px #bec1c3
//   .panel           box-shadow 0 2px 5px #00000010 (the six panels drop their own LVGL shadow)
//   #stage:after     box-shadow inset 0 0 20px #00000012 (vignette)
#include <math.h>
#include <string.h>

#include <atomic>

#include "core/platform.h"
#include "src/core/lv_obj_event_private.h"   // lv_cover_check_info_t (the public setter can only weaken)
#include "ui/theme.h"
#include "ui/ui_internal.h"

namespace orbit::ui {

// ================================================================ rasteriser helpers
namespace bake {

float rrectDist(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    float qx = fabsf(px - cx) - hx, qy = fabsf(py - cy) - hy;
    const float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
    const float outside = sqrtf(ox * ox + oy * oy);
    const float inside = qx > qy ? qx : qy;
    return outside + (inside < 0.f ? inside : 0.f) - r;
}

namespace {
// floorf() without the libm call (three per pixel made it the bulk of the boot-time bake on the P4):
// exact for |v| < 2^31, which every operand here is.
inline int floorInt(float v)
{
    const int i = (int)v;
    return i - ((float)i > v ? 1 : 0);
}
}  // namespace

uint16_t to565(Rgb c, int x, int y)
{
    // Interleaved gradient noise (Jimenez 2014): a cheap low-discrepancy threshold in [0,1)
    // that dithers smooth gradients without visible patterns.
    float f = 0.06711056f * (float)x + 0.00583715f * (float)y;
    f -= (float)floorInt(f);
    f *= 52.9829189f;
    const float d = f - (float)floorInt(f);
    auto q = [d](float v, float levels) {
        int n = floorInt(v * (levels / 255.f) + d + 1e-4f);   // grid values stay exact
        const int max = (int)levels;
        return n < 0 ? 0 : (n > max ? max : n);
    };
    const int r = q(c.r, 31.f), g = q(c.g, 63.f), b = q(c.b, 31.f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

lv_image_dsc_t* image565(int w, int h)
{
    const size_t bytes = (size_t)w * (size_t)h * 2u;
    lv_image_dsc_t* d = (lv_image_dsc_t*)bigAlloc(sizeof(lv_image_dsc_t));
    uint8_t* px = (uint8_t*)bigAlloc(bytes);
    if (!d || !px) {
        if (d) bigFree(d);
        if (px) bigFree(px);
        return nullptr;
    }
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.flags = 0;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 2u;
    d->data_size = (uint32_t)bytes;
    d->data = px;
    return d;
}

}  // namespace bake

// ================================================================ the stage
namespace bg {

namespace {

bool s_baked = false;

// The baked image: made by bgPrebake() (any task, e.g. core 1 during boot) or by create() itself.
// s_state publishes it: 0 none, 1 baking, 2 ready (s_img / s_w / s_h / s_base valid), -1 failed (out of memory).
// s_base: the base palette it shows (theme.h baseSelect: app_main picks it while core 1 may already bake).
std::atomic<int> s_state{0};
lv_image_dsc_t* s_img = nullptr;
int s_w = 0, s_h = 0, s_base = -1;

struct Box {
    float x, y, w, h;
};
const Box kPanels[] = {
    {(float)geo::pads_x, (float)geo::pads_y, (float)geo::pads_w, (float)geo::pads_h},
    {(float)geo::browser_x, (float)geo::browser_y, (float)geo::browser_w, (float)geo::browser_h},
    {(float)geo::work_x, (float)geo::work_y, (float)geo::work_w, (float)geo::work_h},
    {(float)geo::macros_x, (float)geo::macros_y, (float)geo::macros_w, (float)geo::macros_h},
    {(float)geo::timeline_x, (float)geo::timeline_y, (float)geo::timeline_w, (float)geo::timeline_h},
};

// LVGL counts an lv_image as transparent for the "top object" search; this one is opaque
// RGB565 and covers its area, so refreshes may start at it (the screen below is never drawn).
void coverCb(lv_event_t* e)
{
    lv_cover_check_info_t* info = (lv_cover_check_info_t*)lv_event_get_param(e);
    if (!info || info->res == LV_COVER_RES_MASKED) return;
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const lv_area_t* q = info->area;
    if (q && q->x1 >= a.x1 && q->y1 >= a.y1 && q->x2 <= a.x2 && q->y2 <= a.y2) info->res = LV_COVER_RES_COVER;
}

// Returns the base palette baked, or -1 when baseSelect() picked another base meanwhile (bake again).
int bake(uint16_t* out, int W, int H)
{
    using namespace bake;
    const int base = baseIndex();
    const StageColors sc = stageColors(base);   // constant tables: no race with baseSelect()
    const Rgb ga = rgb(sc.stage_a), gb = rgb(sc.stage_b);
    const Rgb ta = rgb(sc.top_a), tb = rgb(sc.top_b), tline = rgb(sc.top_line);
    const Rgb white = rgb(sc.scan), black = rgb(0x000000);
    // 140deg: direction (sin, -cos) in screen coordinates (y down); CSS gradient line length.
    const float sn = 0.64278761f, cs = -0.76604444f;
    const float dx = sn, dy = -cs;
    const float L = (float)W * fabsf(sn) + (float)H * fabsf(cs);
    const float kScanA = 0.2f * 0.55f;        // #fff3 at opacity .55
    const float kShadowA = 16.f / 255.f;      // #00000010
    const float kShadowS = 2.5f;              // blur 5px
    const float kVigA = 18.f / 255.f;         // #00000012
    const float kVigS = 10.f;                 // blur 20px
    const int top_h = geo::top_h;
    const float inv = 1.f / (kVigS * 1.41421356f);
    for (int y = 0; y < H; ++y) {
        if ((y & 31) == 0 && baseIndex() != base) return -1;
        const float py = (float)y + 0.5f;
        const float vy = 0.5f * (erff(py * inv) - erff((py - (float)H) * inv));
        uint16_t* row = out + (size_t)y * (size_t)W;
        for (int x = 0; x < W; ++x) {
            const float px = (float)x + 0.5f;
            Rgb c;
            if (y < top_h - 1) {
                c = mix(ta, tb, py / (float)(top_h - 1));
            } else if (y == top_h - 1) {
                c = tline;
            } else {
                float t = ((px - (float)W * 0.5f) * dx + (py - (float)H * 0.5f) * dy) / L + 0.5f;
                t = clamp01(t / 0.96f);
                c = mix(ga, gb, t);
                if (y % 5 == 0) c = mix(c, white, kScanA);
                for (const Box& b : kPanels) {
                    // Only the part outside the panel is visible; skip far pixels quickly.
                    if (px < b.x - 9.f || px > b.x + b.w + 9.f || py < b.y - 6.f || py > b.y + b.h + 11.f) continue;
                    if (px > b.x + 10.f && px < b.x + b.w - 10.f && py > b.y + 10.f && py < b.y + b.h - 10.f) continue;
                    const float d = rrectDist(px, py, b.x, b.y + 2.f, b.x + b.w, b.y + 2.f + b.h, (float)geo::radius);
                    if (d > 8.f) continue;
                    c = mix(c, black, kShadowA * blurCov(d, kShadowS));
                }
            }
            if (x < 40 || x >= W - 40 || y < 40 || y >= H - 40) {
                const float vx = 0.5f * (erff(px * inv) - erff((px - (float)W) * inv));
                c = mix(c, black, kVigA * (1.f - vx * vy));
            }
            row[x] = to565(c, x, y);
        }
    }
    return base;
}

// Allocates (or reuses: another base, same size) and bakes the W x H image (the caller owns s_state == 1).
// No LVGL calls: any task.
bool bakeImage(int W, int H)
{
    lv_image_dsc_t* img = (s_img && s_w == W && s_h == H) ? s_img : bake::image565(W, H);
    if (!img) {
        ::orbit::logf("ui: no memory for the baked background (%d x %d)", W, H);
        return false;
    }
    const uint64_t t0 = nowUs();
    int base;
    while ((base = bake(const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(img->data)), W, H)) < 0) {}
    ::orbit::logf("ui: background baked in %u ms", (unsigned)((nowUs() - t0) / 1000u));
    s_img = img;
    s_w = W;
    s_h = H;
    s_base = base;
    return true;
}

}  // namespace

bool shadowsBaked() { return s_baked; }

void prebake(int W, int H)
{
    int idle = 0;
    if (!s_state.compare_exchange_strong(idle, 1, std::memory_order_acq_rel)) return;   // done / running
    s_state.store(bakeImage(W, H) ? 2 : -1, std::memory_order_release);
}

lv_obj_t* create(lv_obj_t* scr)
{
    if (!scr) return nullptr;
    lv_display_t* disp = lv_obj_get_display(scr);
    const int W = disp ? (int)lv_display_get_horizontal_resolution(disp) : 1280;
    const int H = disp ? (int)lv_display_get_vertical_resolution(disp) : 720;
    // A pre-bake still running (bgPrebake on another core) is waited for instead of baking twice;
    // otherwise this task claims the bake (none yet, failed before, or made for another size).
    for (;;) {
        int st = s_state.load(std::memory_order_acquire);
        if (st == 1) {
            sleepMs(2);
            continue;
        }
        if (st == 2 && s_w == W && s_h == H && s_base == baseIndex()) break;
        if (!s_state.compare_exchange_strong(st, 1, std::memory_order_acq_rel)) continue;
        const bool ok = bakeImage(W, H);
        s_state.store(ok ? 2 : -1, std::memory_order_release);
        if (!ok) return nullptr;
        break;
    }
    lv_obj_t* img = lv_image_create(scr);
    lv_image_set_src(img, s_img);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(img, coverCb, LV_EVENT_COVER_CHECK, nullptr);
    lv_obj_move_to_index(img, 0);
    // The theme's screen gradient would never be visible again: plain color, no gradient work.
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_color(scr, c(col::bg), 0);
    s_baked = true;
    return img;
}

}  // namespace bg

void bgPrebake(int w, int h) { bg::prebake(w, h); }

}  // namespace orbit::ui
