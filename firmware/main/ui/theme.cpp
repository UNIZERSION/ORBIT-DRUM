// ORBIT LIGHT theme: the base palettes and accent colours, font pointers, shared styles and a minimal
// LVGL theme.
//
// The LVGL theme installed by themeInit() deliberately gives plain objects NO default look
// (transparent, no border, no padding: like lv_obj_remove_style_all), so screens style only what
// they need and nothing is drawn twice. It adds: the stage background on screens, a thin scrollbar,
// and usable defaults for the few stock widgets the kit relies on (textarea cursor, slider,
// buttonmatrix keys).
#include "ui/theme.h"

#include <math.h>

#include <atomic>

#include "core/settings.h"
#include "src/themes/lv_theme_private.h"

extern "C" {
extern const lv_font_t orbit_font_r12, orbit_font_r13, orbit_font_r14;
extern const lv_font_t orbit_font_m12, orbit_font_m13, orbit_font_m14, orbit_font_m16, orbit_font_m20,
    orbit_font_m24;
extern const lv_font_t orbit_font_s11, orbit_font_s13, orbit_font_s15;
extern const lv_font_t orbit_font_b34, orbit_font_mono12;
}

namespace orbit::ui {

const lv_font_t* f_r12 = &orbit_font_r12;
const lv_font_t* f_r13 = &orbit_font_r13;
const lv_font_t* f_r14 = &orbit_font_r14;
const lv_font_t* f_m12 = &orbit_font_m12;
const lv_font_t* f_m13 = &orbit_font_m13;
const lv_font_t* f_m14 = &orbit_font_m14;
const lv_font_t* f_m16 = &orbit_font_m16;
const lv_font_t* f_m20 = &orbit_font_m20;
const lv_font_t* f_m24 = &orbit_font_m24;
const lv_font_t* f_s11 = &orbit_font_s11;
const lv_font_t* f_s13 = &orbit_font_s13;
const lv_font_t* f_s15 = &orbit_font_s15;
const lv_font_t* f_b34 = &orbit_font_b34;
const lv_font_t* f_mono12 = &orbit_font_mono12;

// ---------------------------------------------------------------- colour helpers
namespace {

// a * (1 - t) + b * t per channel, 0xRRGGBB
uint32_t mixT(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float ca = (float)((a >> sh) & 0xFFu), cb = (float)((b >> sh) & 0xFFu);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

float luminance(uint32_t c)   // WCAG relative luminance
{
    float l[3];
    for (int i = 0; i < 3; ++i) {
        const float v = (float)((c >> (16 - 8 * i)) & 0xFFu) / 255.f;
        l[i] = v <= 0.03928f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    return 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
}

float contrast(uint32_t a, uint32_t b)
{
    const float la = luminance(a), lb = luminance(b);
    return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ---------------------------------------------------------------- base palettes
static_assert(kThemeCount == ::orbit::kAccentPresets && kThemeCustom == ::orbit::kAccentCustom,
              "accent presets / CUSTOM: core/settings.h");

namespace {

enum BaseField : int {
#define ORBIT_COL_ENUM(name, light) BF_##name,
    ORBIT_BASE_COLORS(ORBIT_COL_ENUM)
#undef ORBIT_COL_ENUM
    BF_COUNT
};
constexpr uint32_t kLight[BF_COUNT] = {
#define ORBIT_COL_LIGHT(name, light) light,
    ORBIT_BASE_COLORS(ORBIT_COL_LIGHT)
#undef ORBIT_COL_LIGHT
};

struct Palette {
    uint32_t v[BF_COUNT];
    uint8_t hilite_opa;
};

// A base: the pair the LIGHT palette is mapped onto, and what the accent's tints mix towards.
struct BaseDef {
    const char* name;
    uint32_t panel, ink;
    uint32_t tint;   // light bases: a near-white of the base; dark bases: their panel (tints darken there)
    bool dark;
};
// Ink on panel: LIGHT 12.5, WARM 13.8, SILVER 10.9, SLATE 10.6, DARK 11.9, BLACK 14.2 : 1; --muted 3.5 / 3.6 /
// 3.4 / 4.5 / 4.8 / 5.3 : 1 (host/tests/test_widgets.cpp checks every text pair on every base and accent).
const BaseDef kBases[kBaseCount] = {
    {"LIGHT", 0xE8EAEB, 0x252629, 0xFFFFFF, false},
    {"WARM", 0xF2EBDF, 0x1F1A16, 0xFFFBF3, false},
    {"SILVER", 0xC6CACD, 0x15171A, 0xF0F2F4, false},
    {"SLATE", 0x29313B, 0xE2E8EF, 0x29313B, true},
    {"DARK", 0x2A2B2C, 0xE5E6E8, 0x2A2B2C, true},
    {"BLACK", 0x141414, 0xE0E1E2, 0x141414, true},
};

// A LIGHT colour on another base: per channel, its place on LIGHT's panel -> ink axis is kept on the base's
// axis (text stays text, lines stay lines; on a dark base the light surfaces turn dark and the text light).
uint32_t mapColor(uint32_t c, const BaseDef& b)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float lp = (float)((kLight[BF_panel] >> sh) & 0xFFu), li = (float)((kLight[BF_ink] >> sh) & 0xFFu);
        const float np = (float)((b.panel >> sh) & 0xFFu), ni = (float)((b.ink >> sh) & 0xFFu);
        const float t = ((float)((c >> sh) & 0xFFu) - lp) / (li - lp);
        float v = np + t * (ni - np);
        v = v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
        out |= (uint32_t)(v + 0.5f) << sh;
    }
    return out;
}

// A colour on the RGB565 grid as c() fills it, with the green level that keeps the colour's own green offset
// from red / blue: the 6-bit green rounds apart from the 5-bit red / blue and would tint the derived greys
// magenta or green (LIGHT's palette avoids that by hand: nav_fill, list_bg).
uint32_t snap565(uint32_t v)
{
    const uint32_t r = (v >> 16) & 0xFFu, g = (v >> 8) & 0xFFu, b = v & 0xFFu;
    const uint32_t r8 = c565(r, 5), b8 = c565(b, 5);
    const float want = (float)(r8 + b8) * 0.5f + (float)g - (float)(r + b) * 0.5f;
    uint32_t best = 0;
    float err = 1e9f;
    for (int n = 0; n < 64; ++n) {
        const uint32_t e = ((uint32_t)n << 2) | ((uint32_t)n >> 4);
        const float d = fabsf((float)e - want);
        if (d < err) {
            err = d;
            best = e;
        }
    }
    return (r8 << 16) | (best << 8) | b8;
}

void darkTweaks(const BaseDef& b, Palette& p);

// The whole base palette of a base (constant tables only: safe in any task).
void basePalette(int index, Palette& p)
{
    p.hilite_opa = 150;
    if (index <= 0 || index >= kBaseCount) {
        for (int f = 0; f < BF_COUNT; ++f) p.v[f] = kLight[f];
        return;
    }
    const BaseDef& b = kBases[index];
    for (int f = 0; f < BF_COUNT; ++f) p.v[f] = mapColor(kLight[f], b);
    if (b.dark) darkTweaks(b, p);
    for (int f = 0; f < BF_COUNT; ++f) p.v[f] = snap565(p.v[f]);
    p.v[BF_nav_fill] = p.v[BF_settings_nav];   // LIGHT's RGB565 pre-roundings: here snap565 does that
    p.v[BF_list_bg] = p.v[BF_browser_bg];
    // LVGL blends RGB565 per channel with the result rounded down: under a black scrim the 5-bit red / blue lose
    // up to a step more than the 6-bit green, which turns the dark panels teal. One step of each brings it back.
    p.v[BF_scrim] = b.dark ? 0x080408 : 0x000000;
}

void darkTweaks(const BaseDef& b, Palette& p)
{
    // Dark bases: what the mapping would turn inside out keeps its look. On = towards the ink, down = towards black.
    auto on = [&](float t) { return mixT(b.panel, b.ink, t); };
    auto down = [&](float t) { return mixT(b.panel, 0x000000, t); };
    p.v[BF_stage_a] = down(0.22f);   // the stage lies below the panels
    p.v[BF_stage_b] = down(0.42f);
    p.v[BF_top_a] = on(0.05f);
    p.v[BF_top_b] = down(0.10f);
    p.v[BF_wave_frame] = down(0.45f);   // the waveform frame stays a dark screen (white / accent drawing on it)
    p.v[BF_wave_bg] = down(0.40f);
    p.v[BF_wave_border] = mixT(p.v[BF_wave_frame], b.ink, 0.13f);
    p.v[BF_wave_text] = kLight[BF_wave_text];
    p.v[BF_knob_base] = on(0.13f);    // knob bodies: lit from the top left like on LIGHT
    p.v[BF_knob_shine] = on(0.36f);
    p.v[BF_knob_border] = down(0.45f);
    p.v[BF_mknob_a] = on(0.62f);      // macro knobs: light graphite instead of white
    p.v[BF_mknob_b] = on(0.30f);
    p.v[BF_mknob_border] = on(0.22f);
    p.v[BF_pad_a] = on(0.12f);        // pads a little above the panel, lit from the top left
    p.v[BF_pad_b] = on(0.05f);
    p.v[BF_pad_border] = on(0.24f);
    p.v[BF_pad_shadow] = down(0.50f);
    p.v[BF_pad_hl] = on(0.26f);
    p.v[BF_fader_track] = on(0.24f);
    p.v[BF_key_white] = on(0.12f);    // the keyboard: light keys a little above the panel, black ones below it
    p.v[BF_key_black] = down(0.55f);
    p.v[BF_key_black_down] = on(0.30f);
    p.v[BF_hilite] = b.ink;           // a faint light edge instead of the white line
    p.hilite_opa = 22;
}

std::atomic<int> g_base{0};
int g_theme_index = 0;
int g_hue = 0, g_sat = 0;   // CUSTOM in use

}  // namespace

const char* baseName(int index) { return kBases[index >= 0 && index < kBaseCount ? index : 0].name; }

bool baseDark(int index) { return kBases[index >= 0 && index < kBaseCount ? index : 0].dark; }

void baseSwatch(int index, uint32_t& panel, uint32_t& ink, uint32_t& line)
{
    Palette p;
    basePalette(index, p);
    panel = p.v[BF_panel];
    ink = p.v[BF_ink];
    line = p.v[BF_line];
}

void baseSelect(int index)
{
    if (index < 0 || index >= kBaseCount) index = 0;
    Palette p;
    basePalette(index, p);
#define ORBIT_COL_SET(name, light) col::name = p.v[BF_##name];
    ORBIT_BASE_COLORS(ORBIT_COL_SET)
#undef ORBIT_COL_SET
    col::hilite_opa = p.hilite_opa;
    g_base.store(index, std::memory_order_release);
    themeSelect(g_theme_index, g_hue, g_sat);   // the accent's shades are derived against the base
}

int baseIndex() { return g_base.load(std::memory_order_acquire); }

StageColors stageColors(int base)
{
    Palette p;
    basePalette(base, p);
    return {p.v[BF_stage_a], p.v[BF_stage_b], p.v[BF_top_a], p.v[BF_top_b], p.v[BF_top_line], p.v[BF_scan]};
}

// ---------------------------------------------------------------- accent colours
const ThemeDef kThemes[kThemeCount] = {
    {"ORANGE", 0xFF6531}, {"RED", 0xEF4352},  {"PINK", 0xF0528E},  {"VIOLET", 0x8B5CF6},
    {"BLUE", 0x3B82F6},   {"CYAN", 0x12B5C9}, {"GREEN", 0x2BBF5F}, {"AMBER", 0xF2A516},
};

uint32_t hueColor(float hue, float sat)
{
    while (hue < 0.f) hue += 360.f;
    while (hue >= 360.f) hue -= 360.f;
    sat = sat < 0.f ? 0.f : (sat > 1.f ? 1.f : sat);
    const float h6 = hue / 60.f;
    const int i = (int)h6;
    const float f = h6 - (float)i;
    const float p = 1.f - sat, q = 1.f - sat * f, t = 1.f - sat * (1.f - f);
    float r, g, b;
    switch (i) {
    case 0: r = 1.f, g = t, b = p; break;
    case 1: r = q, g = 1.f, b = p; break;
    case 2: r = p, g = 1.f, b = t; break;
    case 3: r = p, g = q, b = 1.f; break;
    case 4: r = t, g = p, b = 1.f; break;
    default: r = 1.f, g = p, b = q; break;
    }
    return ((uint32_t)(r * 255.f + 0.5f) << 16) | ((uint32_t)(g * 255.f + 0.5f) << 8) | (uint32_t)(b * 255.f + 0.5f);
}

// Full-value hue + saturation, then kept in the brightness band that works as an accent on the base: on a
// light base no lighter than AMBER (luminance 0.45: fills and lines still stand out from the panels), on a
// dark one no darker than VIOLET (0.20: a deep blue would vanish on the black panels).
uint32_t customAccent(int hue, int sat, int base)
{
    hue = clampInt(hue, 0, 359);
    sat = sat <= 0 ? ::orbit::kAccentSatDefault : clampInt(sat, ::orbit::kAccentSatMin, 100);
    const uint32_t pure = hueColor((float)hue, (float)sat / 100.f);
    uint32_t a = pure;
    float k = 0.f;
    if (baseDark(base)) {
        while (luminance(a) < 0.20f && k < 0.7f) a = mixT(pure, 0xFFFFFF, k += 0.02f);
    } else {
        while (luminance(a) > 0.45f && k < 0.6f) a = mixT(pure, 0x000000, k += 0.02f);
    }
    return a;
}

uint32_t onAccentFor(uint32_t accent) { return contrast(0x252628, accent) >= contrast(0xFFFFFF, accent) ? 0x252628 : 0xFFFFFF; }

void presetHueSat(int index, int& hue, int& sat)
{
    const uint32_t a = kThemes[index >= 0 && index < kThemeCount ? index : 0].accent;
    const float r = (float)((a >> 16) & 0xFFu) / 255.f, g = (float)((a >> 8) & 0xFFu) / 255.f, b = (float)(a & 0xFFu) / 255.f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    float h = 0.f;
    if (d > 0.f) {
        if (mx == r) h = 60.f * fmodf((g - b) / d, 6.f);
        else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
        else h = 60.f * ((r - g) / d + 4.f);
    }
    if (h < 0.f) h += 360.f;
    hue = clampInt((int)(h + 0.5f) % 360, 0, 359);
    sat = clampInt((int)(100.f * (mx > 0.f ? d / mx : 0.f) + 0.5f), ::orbit::kAccentSatMin, 100);
}

void themeSelect(int index, int hue, int sat)
{
    if (index < 0 || index > kThemeCustom) index = 0;
    g_theme_index = index;
    g_hue = g_sat = 0;
    const int base = baseIndex();
    const BaseDef& b = kBases[base];
    if (index == 0 && base == 0) {   // LIGHT + ORANGE: the prototype's own shades
        col::accent = 0xFF6531;
        col::accent_dark = 0xE95021;
        col::on_accent = 0x252628;
        col::glow = 0xFF6931;
        col::active_bg = 0xFBDCCE;
        col::pad_on_a = 0xFF8E63;
        col::pad_on_b = 0xFF5422;
        col::pad_on_border = 0xE54817;
        col::pad_on_shadow = 0xDB5530;
        col::cell_playing = 0xFF8869;
        col::cell_pulse = 0xFFC0AA;
        col::file_sel = 0xFFDDD0;
        col::row_press = 0xF9E1D7;
        col::macro_ink_on = col::macro_ink;
        col::scene_text_on = col::scene_text;
        return;
    }
    uint32_t a;
    if (index == kThemeCustom) {
        g_hue = clampInt(hue, 0, 359);
        g_sat = sat <= 0 ? ::orbit::kAccentSatDefault : clampInt(sat, ::orbit::kAccentSatMin, 100);
        a = customAccent(g_hue, g_sat, base);
    } else {
        a = kThemes[index].accent;
    }
    col::accent = a;
    col::glow = a;
    // Text in the accent colour: on the light bases darkened until it reads (>= 3:1 on --panel, like LIGHT
    // always did), on the dark ones lightened until it is plain text contrast (>= 4.5:1).
    uint32_t text = a;
    if (!b.dark) {
        float k = 0.12f;
        text = mixT(a, 0, k);
        while (contrast(text, col::panel) < 3.0f && k < 0.6f) {
            k += 0.04f;
            text = mixT(a, 0, k);
        }
    } else {
        float k = 0.f;
        while (contrast(text, col::panel) < 4.5f && k < 0.8f) {
            k += 0.04f;
            text = mixT(a, 0xFFFFFF, k);
        }
    }
    col::accent_dark = text;
    col::on_accent = onAccentFor(a);
    // Tints mix towards the base's near-white (light bases) or its panel (dark ones: they darken).
    col::active_bg = mixT(a, b.tint, 0.78f);
    col::file_sel = mixT(a, b.tint, 0.82f);
    col::row_press = mixT(a, b.tint, 0.80f);
    col::cell_playing = mixT(a, b.tint, 0.22f);
    col::cell_pulse = mixT(a, b.tint, 0.6f);
    col::pad_on_a = mixT(a, 0xFFFFFF, 0.25f);
    col::pad_on_b = mixT(a, 0, 0.04f);
    col::pad_on_border = mixT(a, 0, 0.14f);
    col::pad_on_shadow = mixT(a, 0, 0.18f);
    // The transport / scene buttons keep their own dark ink on the accent where it reads (light bases, >= 3:1),
    // else they use the on-accent colour.
    const bool dark_text = !b.dark && col::on_accent == 0x252628;
    col::macro_ink_on = dark_text && contrast(col::macro_ink, a) >= 3.0f ? col::macro_ink : col::on_accent;
    col::scene_text_on = dark_text && contrast(col::scene_text, a) >= 3.0f ? col::scene_text : col::on_accent;
}

int themeIndex() { return g_theme_index; }
int themeHue() { return g_hue; }
int themeSat() { return g_sat; }

namespace {

Styles g_styles;
bool g_ready = false;
lv_theme_t g_theme;
lv_style_t s_scrollbar, s_cursor, s_slider_main, s_slider_ind, s_slider_knob, s_btnm_main, s_btnm_item,
    s_btnm_item_pr, s_btnm_item_chk;

// CSS filter: brightness(1.16) approximated as a mix towards white; opa = mix amount (0 = none),
// so a transition from 0 fades in smoothly (LVGL's shade filter jumps at opa 0).
lv_color_t brightenCb(const lv_color_filter_dsc_t*, lv_color_t c, lv_opa_t opa)
{
    return opa ? lv_color_lighten(c, opa) : c;
}
lv_color_filter_dsc_t g_brighten;

// Press feedback transition (.press: transform .12s, colors ~.17s).
const lv_style_prop_t kPressProps[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
                                       LV_STYLE_COLOR_FILTER_OPA, LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR,
                                       LV_STYLE_TEXT_COLOR, LV_STYLE_PROP_INV};
lv_style_transition_dsc_t g_press_tr;

void initBox(lv_style_t* s, uint32_t bg, uint32_t border, int radius)
{
    lv_style_init(s);
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_bg_color(s, c(bg));
    lv_style_set_border_width(s, 1);
    lv_style_set_border_color(s, c(border));
    lv_style_set_radius(s, radius);
}

void applyCb(lv_theme_t*, lv_obj_t* obj)
{
    Styles& st = g_styles;
    if (lv_obj_get_parent(obj) == nullptr) {
        lv_obj_add_style(obj, &st.screen, 0);
        return;
    }
    // Only plain containers can scroll in this UI: keep the style list of everything else empty.
    if (lv_obj_get_class(obj) == &lv_obj_class) {
        lv_obj_add_style(obj, &s_scrollbar, LV_PART_SCROLLBAR);
        return;
    }
#if LV_USE_TEXTAREA
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &st.input, 0);
        lv_obj_add_style(obj, &st.focus, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
        return;
    }
#endif
#if LV_USE_SLIDER
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s_slider_main, 0);
        lv_obj_add_style(obj, &s_slider_ind, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_slider_knob, LV_PART_KNOB);
        return;
    }
#endif
#if LV_USE_BUTTONMATRIX
    if (lv_obj_check_type(obj, &lv_buttonmatrix_class)
#if LV_USE_KEYBOARD
        || lv_obj_check_type(obj, &lv_keyboard_class)
#endif
    ) {
        lv_obj_add_style(obj, &s_btnm_main, 0);
        lv_obj_add_style(obj, &s_btnm_item, LV_PART_ITEMS);
        lv_obj_add_style(obj, &s_btnm_item_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s_btnm_item_chk, LV_PART_ITEMS | LV_STATE_CHECKED);
        return;
    }
#endif
}

}  // namespace

Styles& styles() { return g_styles; }

void themeInit()
{
    if (g_ready) return;
    g_ready = true;
    Styles& st = g_styles;
    lv_color_filter_dsc_init(&g_brighten, brightenCb);
    lv_style_transition_dsc_init(&g_press_tr, kPressProps, lv_anim_path_ease_out, 120, 0, nullptr);

    // #stage: linear-gradient(140deg, #f1f2f3, #dadde0 96%). LVGL draws only vertical/horizontal
    // gradients without the (costly) complex gradient engine: vertical is the closest match.
    lv_style_init(&st.screen);
    lv_style_set_bg_opa(&st.screen, LV_OPA_COVER);
    lv_style_set_bg_color(&st.screen, c(col::stage_a));
    lv_style_set_bg_grad_color(&st.screen, c(col::stage_b));
    lv_style_set_bg_grad_dir(&st.screen, LV_GRAD_DIR_VER);
    lv_style_set_bg_main_stop(&st.screen, 0);
    lv_style_set_bg_grad_stop(&st.screen, 245);
    lv_style_set_text_color(&st.screen, c(col::ink));
    lv_style_set_text_font(&st.screen, f_r14);

    // .panel: 1 px line border, radius 9, panel bg, shadow 0 2px 5px #00000010 (static).
    initBox(&st.panel, col::panel, col::line, geo::radius);
    lv_style_set_shadow_width(&st.panel, 5);
    lv_style_set_shadow_offset_y(&st.panel, 2);
    lv_style_set_shadow_color(&st.panel, lv_color_black());
    lv_style_set_shadow_opa(&st.panel, 16);
    lv_style_set_pad_all(&st.panel, 0);

    // .light-panel .panel-head .panel-title: 12 px / 600 / .06em -> 13 px semibold.
    lv_style_init(&st.panel_title);
    lv_style_set_text_font(&st.panel_title, f_s13);
    lv_style_set_text_color(&st.panel_title, c(col::ink));
    lv_style_set_text_letter_space(&st.panel_title, ls(13, 0.06f));

    // .action-btn: control bg, line border, radius 5, 11 px / .04em (-> 13 px medium), pad 0 14.
    initBox(&st.action_btn, col::control, col::line, geo::small_radius);
    lv_style_set_text_color(&st.action_btn, c(col::ink));
    lv_style_set_text_font(&st.action_btn, f_m13);
    lv_style_set_text_letter_space(&st.action_btn, ls(13, 0.04f));
    lv_style_set_pad_hor(&st.action_btn, 14);
    lv_style_set_pad_ver(&st.action_btn, 0);
    lv_style_set_transition(&st.action_btn, &g_press_tr);

    // :hover look used as the touch "pressed" color change: accent border + accent text.
    lv_style_init(&st.action_btn_pr);
    lv_style_set_border_color(&st.action_btn_pr, c(col::accent));
    lv_style_set_text_color(&st.action_btn_pr, c(col::accent));

    // .action-btn.primary
    lv_style_init(&st.action_primary);
    lv_style_set_bg_color(&st.action_primary, c(col::accent));
    lv_style_set_border_color(&st.action_primary, c(col::accent));
    lv_style_set_text_color(&st.action_primary, c(col::on_accent));

    // .tab: tab bg, muted text, radius 5, 11 px -> 13 px medium.
    initBox(&st.tab, col::tab, col::tab, geo::small_radius);
    lv_style_set_border_opa(&st.tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&st.tab, c(col::muted));
    lv_style_set_text_font(&st.tab, f_m13);
    lv_style_set_text_letter_space(&st.tab, ls(13, 0.03f));
    lv_style_set_pad_all(&st.tab, 0);
    lv_style_set_transition(&st.tab, &g_press_tr);

    // .tab.active: accent + glow 0 3px 12px #ff69314a (tabs change rarely: shadow is fine).
    lv_style_init(&st.tab_active);
    lv_style_set_bg_color(&st.tab_active, c(col::accent));
    lv_style_set_text_color(&st.tab_active, c(col::on_accent));
    lv_style_set_shadow_width(&st.tab_active, 12);
    lv_style_set_shadow_offset_y(&st.tab_active, 3);
    lv_style_set_shadow_color(&st.tab_active, c(col::glow));
    lv_style_set_shadow_opa(&st.tab_active, col::glow_opa);

    // .micro-btn: 25x24, muted text, line border, tab bg, radius 3, 10 px -> 12 px medium.
    initBox(&st.micro_btn, col::tab, col::line, 3);
    lv_style_set_text_color(&st.micro_btn, c(col::muted));
    lv_style_set_text_font(&st.micro_btn, f_m12);
    lv_style_set_pad_all(&st.micro_btn, 0);
    lv_style_set_transition(&st.micro_btn, &g_press_tr);

    lv_style_init(&st.micro_lit);
    lv_style_set_bg_color(&st.micro_lit, c(col::accent));
    lv_style_set_border_color(&st.micro_lit, c(col::accent));
    lv_style_set_text_color(&st.micro_lit, c(col::on_accent));

    // .overline: 10 px / 600 / .13em, muted, uppercase -> 11 px semibold, in a darker grey than
    // --muted for contrast on the 294 ppi panel (captions with room switch to f_s13 locally).
    lv_style_init(&st.overline);
    lv_style_set_text_font(&st.overline, f_s11);
    lv_style_set_text_color(&st.overline, c(col::overline));
    lv_style_set_text_letter_space(&st.overline, ls(11, 0.13f));

    lv_style_init(&st.muted_text);
    lv_style_set_text_color(&st.muted_text, c(col::muted));

    // .toggle: 45x24 pill, line border, step-off track, pad 3.
    initBox(&st.toggle, col::step_off, col::line, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&st.toggle, 3);

    // .ui-input / .ui-select: line border, control bg, radius 4, pad 9, 11 px -> 13 px.
    initBox(&st.input, col::control, col::line, 4);
    lv_style_set_text_color(&st.input, c(col::ink));
    lv_style_set_text_font(&st.input, f_r13);
    lv_style_set_pad_hor(&st.input, 10);
    lv_style_set_pad_ver(&st.input, 9);

    // .press:active -> scale(.963) + brightness(1.16), 120 ms (pivot = centre).
    lv_style_init(&st.pressed);
    lv_style_set_transform_scale_x(&st.pressed, 247);
    lv_style_set_transform_scale_y(&st.pressed, 247);
    lv_style_set_transform_pivot_x(&st.pressed, LV_PCT(50));
    lv_style_set_transform_pivot_y(&st.pressed, LV_PCT(50));
    lv_style_set_color_filter_dsc(&st.pressed, &g_brighten);
    lv_style_set_color_filter_opa(&st.pressed, 41);
    lv_style_set_transition(&st.pressed, &g_press_tr);

    // focus-visible: 2 px accent outline, offset 2.
    lv_style_init(&st.focus);
    lv_style_set_outline_width(&st.focus, 2);
    lv_style_set_outline_color(&st.focus, c(col::accent));
    lv_style_set_outline_pad(&st.focus, 2);

    // ---- theme-internal styles
    lv_style_init(&s_scrollbar);
    lv_style_set_bg_opa(&s_scrollbar, 110);
    lv_style_set_bg_color(&s_scrollbar, c(col::muted));
    lv_style_set_radius(&s_scrollbar, 2);
    lv_style_set_width(&s_scrollbar, 4);
    lv_style_set_pad_right(&s_scrollbar, 3);
    lv_style_set_pad_top(&s_scrollbar, 3);
    lv_style_set_pad_bottom(&s_scrollbar, 3);

    lv_style_init(&s_cursor);
    lv_style_set_border_side(&s_cursor, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_cursor, 2);
    lv_style_set_border_color(&s_cursor, c(col::accent));
    lv_style_set_anim_duration(&s_cursor, 500);

    lv_style_init(&s_slider_main);
    lv_style_set_bg_opa(&s_slider_main, LV_OPA_COVER);
    lv_style_set_bg_color(&s_slider_main, c(col::step_off));
    lv_style_set_radius(&s_slider_main, LV_RADIUS_CIRCLE);
    lv_style_init(&s_slider_ind);
    lv_style_set_bg_opa(&s_slider_ind, LV_OPA_COVER);
    lv_style_set_bg_color(&s_slider_ind, c(col::accent));
    lv_style_set_radius(&s_slider_ind, LV_RADIUS_CIRCLE);
    lv_style_init(&s_slider_knob);
    lv_style_set_bg_opa(&s_slider_knob, LV_OPA_COVER);
    lv_style_set_bg_color(&s_slider_knob, c(col::thumb));
    lv_style_set_border_width(&s_slider_knob, 2);
    lv_style_set_border_color(&s_slider_knob, c(col::accent));
    lv_style_set_radius(&s_slider_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s_slider_knob, 6);

    lv_style_init(&s_btnm_main);
    lv_style_set_bg_opa(&s_btnm_main, LV_OPA_TRANSP);
    lv_style_set_pad_all(&s_btnm_main, 0);
    lv_style_set_pad_row(&s_btnm_main, 7);
    lv_style_set_pad_column(&s_btnm_main, 7);
    lv_style_set_text_font(&s_btnm_main, f_m16);
    lv_style_set_text_color(&s_btnm_main, c(col::ink));
    initBox(&s_btnm_item, col::control, col::line, geo::small_radius);
    lv_style_set_text_color(&s_btnm_item, c(col::ink));
    lv_style_init(&s_btnm_item_pr);
    lv_style_set_bg_color(&s_btnm_item_pr, c(col::hover));
    lv_style_set_border_color(&s_btnm_item_pr, c(col::accent));
    lv_style_init(&s_btnm_item_chk);
    lv_style_set_bg_color(&s_btnm_item_chk, c(col::tab));
    lv_style_set_text_color(&s_btnm_item_chk, c(col::muted));

    // Install the theme on the default display and style the screen that already exists.
    lv_memzero(&g_theme, sizeof g_theme);
    g_theme.apply_cb = applyCb;
    g_theme.color_primary = c(col::accent);
    g_theme.color_secondary = c(col::ink);
    g_theme.font_small = f_r12;
    g_theme.font_normal = f_r14;
    g_theme.font_large = f_m20;
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        g_theme.disp = disp;
        lv_display_set_theme(disp, &g_theme);
        lv_obj_t* scr = lv_display_get_screen_active(disp);
        if (scr) {
            lv_obj_remove_style_all(scr);
            lv_obj_add_style(scr, &st.screen, 0);
        }
        // Overlays (modals, toast, popups) live on the top layer, which has no screen style to
        // inherit from: give it the UI font so stray labels never fall back to Montserrat.
        lv_obj_t* layers[2] = {lv_display_get_layer_top(disp), lv_display_get_layer_sys(disp)};
        for (lv_obj_t* l : layers) {
            if (!l) continue;
            lv_obj_set_style_text_font(l, f_r14, 0);
            lv_obj_set_style_text_color(l, c(col::ink), 0);
        }
    }
}

}  // namespace orbit::ui
