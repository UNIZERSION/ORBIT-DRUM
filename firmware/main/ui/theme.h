#pragma once
// ORBIT LIGHT theme: colors, fonts and shared styles taken from the prototype's CSS variables
// (reference/TAB5_DAW_ORBIT_LIGHT.html, body[data-theme=light]) and measured geometry
// (reference/geometry.json). Geometry stays the prototype's 1280x720 layout; text sizes are
// raised ~1.2x where the box allows, because 1280x720 on a 5" panel is ~294 ppi.
#include "core/settings.h"   // the counts of the Display colours
#include "lvgl.h"

namespace orbit::ui {

// ---- colors (CSS variable -> palette entry)
// Nothing here is constant: baseSelect() sets the base colours (Settings > Display > Base color) and then
// themeSelect() the accent family (Settings > Display > Accent color, marked "theme"), both before
// themeInit() and before any screen is built; the palette stays fixed for the session. The initial values
// are the prototype's light palette (base 0, LIGHT) with its orange accent (theme 0).
//
// Base colours: name, LIGHT value. Every other base derives its palette from these (theme.cpp), so a new
// colour of the UI is one more line here (and never a literal in a screen).
// .overline captions are darker than --muted (#777d81 is ~3.5:1 on the panel; #585e62 is ~5.4:1 on
// #e8eaeb and ~4.8:1 on the darker #dbdddf), so the small letter-spaced captions stay legible.
#define ORBIT_BASE_COLORS(X)                                                                         \
    X(bg, 0xECEEEF)               /* --bg */                                                        \
    X(stage_a, 0xF1F2F3)          /* #stage gradient 140deg start */                                \
    X(stage_b, 0xDADDE0)          /* ... end */                                                     \
    X(panel, 0xE8EAEB)            /* --panel */                                                     \
    X(control, 0xE4E6E7)          /* --control */                                                   \
    X(ink, 0x252629)              /* --ink */                                                       \
    X(muted, 0x777D81)            /* --muted */                                                     \
    X(line, 0xBFC3C5)             /* --line */                                                      \
    X(hover, 0xD4D7D9)            /* --hover (pressed state on touch) */                            \
    X(tab, 0xDFE1E3)              /* --tab */                                                       \
    X(wave_bg, 0x202225)          /* --wave-bg */                                                   \
    X(wave_frame, 0x1D2022)       /* .l-wave background */                                          \
    X(wave_border, 0x343638)      /* .l-wave / preview frame border */                              \
    X(wave_text, 0xC9CDD0)        /* text on the dark frame (preview grid) */                       \
    X(knob_base, 0x2C2D2F)        /* dark editor knobs */                                           \
    X(knob_shine, 0x626467)                                                                         \
    X(knob_ring, 0xE9ECEE)                                                                          \
    X(knob_border, 0x28292A)                                                                        \
    X(knob_text, 0x343434)                                                                          \
    X(mknob_a, 0xFEFEFE)          /* macro (white) knobs radial gradient */                         \
    X(mknob_b, 0xCED2D5)                                                                            \
    X(mknob_border, 0xD6DADE)                                                                       \
    X(mknob_ring, 0xDEE0E2)                                                                         \
    X(step_off, 0xBEC3C5)         /* --step-off */                                                  \
    X(step_border, 0xB6B8B9)      /* --step-border */                                               \
    X(meter_bg, 0xBDC1C2)         /* --meter-bg */                                                  \
    X(mixer_bg, 0xE4E6E7)         /* --mixer-bg */                                                  \
    X(master_bg, 0xD4D7D8)        /* --master-bg */                                                 \
    X(toast_bg, 0x26272A)         /* --toast-bg */                                                  \
    X(toast_fg, 0xF2F2F2)         /* --toast-fg */                                                  \
    X(modal_bg, 0xE9EBEC)         /* --modal-bg */                                                  \
    X(settings_nav, 0xDADCDE)     /* --settings-nav */                                              \
    X(nav_fill, 0xDEDDDE)         /* settings section list: --settings-nav pre-rounded for RGB565 */ \
    X(top_a, 0xF2F3F4)            /* #light-top gradient */                                         \
    X(top_b, 0xE3E5E6)                                                                              \
    X(top_line, 0xBEC1C3)                                                                           \
    X(top_btn, 0xAFB2B5)          /* .lt-actions border */                                          \
    X(top_glyph, 0x31343A)        /* .lt-actions glyph */                                           \
    X(top_sub, 0x8D9397)          /* .lt-logo "SAMPLER / SEQUENCER" */                              \
    X(top_sep, 0xBFC4C7)          /* .lt-tools separator */                                         \
    X(top_sig, 0x9A9B9E)          /* .lt-bpm "4 / 4" */                                             \
    X(nav_btn, 0xE1E3E4)          /* .lt-nav button */                                              \
    X(nav_text, 0x717578)                                                                           \
    X(pad_a, 0xE9ECED)            /* .pad gradient 145deg */                                        \
    X(pad_b, 0xD9DCDE)                                                                              \
    X(pad_border, 0xAEB2B4)                                                                         \
    X(pad_shadow, 0xACB0B1)                                                                         \
    X(pad_hl, 0xFFFFFF)           /* .pad inset 0 1px 0 #fff */                                     \
    X(pad_index, 0x777A7D)        /* pad A1 ... D4 */                                               \
    X(pad_text, 0x323538)         /* pad sample name */                                             \
    X(badge_fg, 0xFFFFFF)         /* pad M badge text (on --muted) */                               \
    X(cat_line, 0xD4D7D9)         /* .l-category bottom border */                                   \
    X(cat_active, 0xD7DADD)                                                                         \
    X(cat_press, 0xD5D7D8)        /* .l-category:hover */                                           \
    X(cat_text, 0x34383A)                                                                           \
    X(cat_icon, 0x656B6E)                                                                           \
    X(cell, 0xDADCDE)             /* .timeline-cell */                                              \
    X(cell_border, 0xD1D3D4)                                                                        \
    X(cell_clip, 0xAAAEB0)                                                                          \
    X(song_num, 0x888888)         /* song timeline header numbers */                                \
    X(scene_text, 0x111111)       /* .l-scene-buttons text */                                       \
    X(seq_bg, 0xDBDDDF)           /* .l-sequence-screen .seq-grid-wrap */                           \
    X(file_line, 0xCDD0D1)                                                                          \
    X(browser_bg, 0xDFE1E2)                                                                         \
    X(preview_bg, 0xE4E6E8)                                                                         \
    X(list_bg, 0xDEDFDF)          /* BROWSER file list: browser_bg pre-rounded for RGB565 */         \
    X(search_bg, 0xE9EBEC)        /* BROWSER search field */                                        \
    X(search_line, 0xBAC0C3)                                                                        \
    X(search_text, 0x292B2C)                                                                        \
    X(dur_text, 0x8A8F92)         /* BROWSER duration column */                                     \
    X(dark_btn, 0x292B2E)         /* ONE SHOT button */                                             \
    X(dark_btn_fg, 0xFFFFFF)                                                                        \
    X(layer_bg, 0xE6E8EA)         /* SAMPLE layer buttons */                                        \
    X(layer_border, 0xC4C7C8)                                                                       \
    X(layer_on, 0x333333)                                                                           \
    X(layer_loaded, 0x4E5255)                                                                       \
    X(layer_empty, 0x777B7F)                                                                        \
    X(divider, 0xC7C9CB)          /* SAMPLE view divider */                                         \
    X(transport_btn, 0xE3E5E6)    /* .l-transport buttons */                                        \
    X(transport_border, 0xAEB3B4)                                                                   \
    X(macro_ink, 0x3D3D3F)        /* .l-transport text / icons */                                   \
    X(macro_dot, 0xC2C6C9)        /* unlit level dots */                                            \
    X(macro_line, 0xC2C4C6)       /* .l-transport top line */                                       \
    X(pan_thumb, 0x3A3B3D)        /* mixer pan thumb */                                             \
    X(fader_track, 0x3A3B3D)      /* fader track above the thumb */                                 \
    X(mini_select, 0xE3E5E7)      /* .mini-select */                                                \
    X(thumb, 0xFEFEFE)            /* slider knob */                                                 \
    X(disabled_fg, 0xA9AEB1)      /* disabled buttons */                                            \
    X(disabled_line, 0xCDD0D2)                                                                      \
    X(swatch_ring, 0xFFFFFF)      /* unselected colour swatches (Settings > Display) */             \
    X(key_white, 0xF6F7F8)        /* BASS keyboard */                                               \
    X(key_black, 0x2A2C2F)                                                                          \
    X(key_black_down, 0x55585C)                                                                     \
    X(hilite, 0xFFFFFF)           /* .panel inset 0 1px 1px #fff9 (with hilite_opa) */              \
    X(scan, 0xFFFFFF)             /* #stage:before lines */                                         \
    X(scrim, 0x000000)            /* modal / keyboard scrim (#0009) */                              \
    X(overline, 0x585E62)         /* .overline captions */

namespace col {
#define ORBIT_COL_VAR(name, light) inline uint32_t name = light;
ORBIT_BASE_COLORS(ORBIT_COL_VAR)
#undef ORBIT_COL_VAR
inline uint8_t hilite_opa = 150;           // opacity of the panel's top highlight line
// accent family (theme)
inline uint32_t accent = 0xFF6531;         // --accent
inline uint32_t accent_dark = 0xE95021;    // accent-coloured text (active category ...): legible on --panel
inline uint32_t on_accent = 0x252628;      // --on-accent: text on accent
inline uint32_t glow = 0xFF6931;           // --glow (#ff69314a: use with opa 74)
constexpr uint8_t glow_opa = 74;
inline uint32_t active_bg = 0xFBDCCE;      // --active-bg
inline uint32_t pad_on_a = 0xFF8E63;       // .pad.active gradient 135deg
inline uint32_t pad_on_b = 0xFF5422;
inline uint32_t pad_on_border = 0xE54817;
inline uint32_t pad_on_shadow = 0xDB5530;
inline uint32_t cell_playing = 0xFF8869;
inline uint32_t cell_pulse = 0xFFC0AA;     // the playing cell pulses towards this tint
inline uint32_t file_sel = 0xFFDDD0;       // .file-item.selected
inline uint32_t row_press = 0xF9E1D7;      // BROWSER file row pressed
inline uint32_t macro_ink_on = 0x3D3D3F;   // .l-transport text / icons on an active (accent) button
inline uint32_t scene_text_on = 0x111111;  // .l-scene-buttons text on the active (accent) scene
}  // namespace col

// Rounds to the nearest RGB565 colour: LVGL truncates 888 -> 565, which gives every light grey
// of this palette a visible magenta cast (G loses up to 3/255 more than R and B).
inline uint8_t c565(uint32_t v, uint32_t bits)
{
    const uint32_t m = (1u << bits) - 1u, n = (v * m + 127u) / 255u;
    return (uint8_t)((n << (8u - bits)) | (n >> (2u * bits - 8u)));
}
inline lv_color_t c(uint32_t hex)
{
    return lv_color_make(c565((hex >> 16) & 0xFFu, 5), c565((hex >> 8) & 0xFFu, 6), c565(hex & 0xFFu, 5));
}

// ---- fonts (generated by tools/fontgen into ui/fonts/*.c, UTF-8 with Polish letters)
// Naming: f<weight><px>. Weights: r = regular(400), m = medium(500), s = semibold(600), b = bold(700).
extern const lv_font_t* f_r12;   // muted notes, settings notes
extern const lv_font_t* f_r13;
extern const lv_font_t* f_r14;   // body, list rows
extern const lv_font_t* f_m12;   // tags, small buttons, step numbers
extern const lv_font_t* f_m13;   // action buttons, tabs, nav
extern const lv_font_t* f_m14;
extern const lv_font_t* f_m16;   // setting names, toast
extern const lv_font_t* f_m20;   // section titles
extern const lv_font_t* f_m24;   // sample name, settings h3
extern const lv_font_t* f_s11;   // overline / knob labels (letter-spaced)
extern const lv_font_t* f_s13;   // panel titles
extern const lv_font_t* f_s15;
extern const lv_font_t* f_b34;   // TAB5 logo
extern const lv_font_t* f_mono12;// wave time readout ("0.000s — 0.420s  44 100 Hz")

// ---- geometry constants from the prototype (px in the 1280x720 stage)
namespace geo {
constexpr int top_h = 62;
constexpr int pads_x = 14, pads_y = 73, pads_w = 288, pads_h = 331;
constexpr int browser_x = 14, browser_y = 413, browser_w = 288, browser_h = 294;
constexpr int work_x = 311, work_y = 73, work_w = 746, work_h = 486;
constexpr int macros_x = 1066, macros_y = 73, macros_w = 200, macros_h = 486;
constexpr int timeline_x = 311, timeline_y = 568, timeline_w = 955, timeline_h = 139;
constexpr int radius = 9, small_radius = 5;
constexpr int panel_head_h = 36;
}  // namespace geo

// ---- shared styles (initialised by themeInit())
struct Styles {
    lv_style_t screen;          // stage background gradient
    lv_style_t panel;           // .panel: border line, radius 9, panel bg, subtle shadow
    lv_style_t panel_title;     // .panel-title text
    lv_style_t action_btn;      // .action-btn
    lv_style_t action_btn_pr;   // pressed / hover look
    lv_style_t action_primary;  // .action-btn.primary
    lv_style_t tab;             // .tab
    lv_style_t tab_active;      // .tab.active (accent + glow)
    lv_style_t micro_btn;       // .micro-btn
    lv_style_t micro_lit;       // .micro-btn.lit
    lv_style_t overline;        // .overline
    lv_style_t muted_text;
    lv_style_t toggle;          // .toggle track
    lv_style_t input;           // .ui-input / .ui-select
    lv_style_t pressed;         // generic :active (scale .963 + brightness) helper
    lv_style_t focus;           // focus ring (accent outline 2 px)
};
Styles& styles();
void themeInit();               // call once after lv_init and before building screens

// ---- base palettes (Settings > Display > Base color): background, panels, pads, knobs, text. LIGHT is the
// prototype's palette; every other base maps it onto its own panel / ink pair and adjusts the parts that
// keep their look on any base (the dark waveform frame, knob bodies, shadows). Pick one with baseSelect()
// before themeSelect() and themeInit(): app_main uses Settings::base, so a new choice applies at the next start.
using ::orbit::kBaseCount;      // core/settings.h (Settings::base 0 .. kBaseCount - 1)
const char* baseName(int index);            // "LIGHT" ... (out of range: LIGHT)
bool baseDark(int index);                   // light text on dark panels
void baseSwatch(int index, uint32_t& panel, uint32_t& ink, uint32_t& line);   // Settings swatch colours
void baseSelect(int index);   // out of range -> 0; before the UI is built (re-derives the accent chosen so far)
int baseIndex();
// The stage background of a base (ui_bg.cpp bakes it, possibly on another core while baseSelect() runs):
// computed from constant tables only, safe in any task.
struct StageColors {
    uint32_t stage_a, stage_b, top_a, top_b, top_line, scan;
};
StageColors stageColors(int base);

// ---- accent colours: 8 presets and CUSTOM (hue + saturation); the rest of the accent family is derived
// from the accent against the selected base (LIGHT + ORANGE keeps the prototype's exact shades). Pick one
// with themeSelect() after baseSelect() and before themeInit(): app_main uses Settings::theme / accent_hue /
// accent_sat, so a new choice applies at the next start of ORBIT.
struct ThemeDef {
    const char* name;           // Settings > Display (English, like the rest of the UI)
    uint32_t accent;
};
constexpr int kThemeCount = 8;             // presets (== orbit::kAccentPresets)
constexpr int kThemeCustom = kThemeCount;  // Settings::theme of CUSTOM (== orbit::kAccentCustom)
extern const ThemeDef kThemes[kThemeCount];
// index 0..7 preset, kThemeCustom: hue 0..359 / sat (0 = default, else kAccentSatMin..100); out of range -> 0
void themeSelect(int index, int hue = 0, int sat = 0);
int themeIndex();
int themeHue();                 // CUSTOM in use: its hue / saturation (else 0)
int themeSat();
// The CUSTOM accent colour on a base (the dialog preview shows exactly what themeSelect() will use).
uint32_t customAccent(int hue, int sat, int base);
// Hue / saturation of a preset (CUSTOM starts there the first time).
void presetHueSat(int index, int& hue, int& sat);
// Hue 0..359 at saturation 0..1 and full value (the slider tracks).
uint32_t hueColor(float hue, float sat);
// Text on a fill of this accent: the dark ink or white, whichever contrasts more (col::on_accent's rule).
uint32_t onAccentFor(uint32_t accent);

// Spacing helper: letter spacing in px for a font size and CSS em value (e.g. .09em).
inline int ls(int font_px, float em) { return (int)(font_px * em + 0.5f); }

}  // namespace orbit::ui
