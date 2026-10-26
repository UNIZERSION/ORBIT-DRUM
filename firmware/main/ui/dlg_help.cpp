// UI GUIDE dialog (prototype .help-grid): cards in two columns describing how the real controls of ORBIT DRUM
// work (0.4.6: English, two more cards for the Tab5 Keyboard).
#include <string.h>

#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui {

namespace dlg_help {
namespace {

constexpr int kCardW = (824 - 2 - 2 * 26 - 18) / 2;   // 376
constexpr int kTextW = kCardW - 2 - 2 * 14;            // card content width
constexpr int kCardCount = 10, kCardRows = kCardCount / 2;

// Greedy word wrap measured with the label font. LVGL 9.3 starts the next line with the space
// when only that space overflows; pre-broken lines avoid the indented line starts.
void wrapText(char* out, size_t cap, const char* text, const lv_font_t* font, int32_t width)
{
    size_t o = 0;
    int32_t line = 0;
    const int32_t space = lv_text_get_width(" ", 1, font, 0);
    const char* p = text;
    while (*p && o + 2 < cap) {
        while (*p == ' ') ++p;
        const char* w = p;
        while (*p && *p != ' ') ++p;
        const size_t n = (size_t)(p - w);
        if (n == 0) break;
        const int32_t ww = lv_text_get_width(w, (uint32_t)n, font, 0);
        if (line > 0) {
            if (line + space + ww > width) {
                out[o++] = '\n';
                line = 0;
            } else {
                out[o++] = ' ';
                line += space;
            }
        }
        if (o + n + 1 >= cap) break;
        memcpy(out + o, w, n);
        o += n;
        line += ww;
    }
    out[o] = 0;
}

struct Card {
    const char* title;
    const char* text;
};

const Card kCards[kCardCount] = {
    {"1. Pads & velocity",
     "Tap a pad to play it at once, with several fingers too (velocity: Settings \xE2\x86\x92 Touch, Fixed / "
     "Position). The tap also picks the pad to edit; REC + PLAY records the hits into the scene."},
    {"2. Mute / Solo",
     "Turn on MUTE or SOLO in the PADS head and tap pads to mute or solo them (M / S mark on the pad). Tap the "
     "button again to go back to playing."},
    {"3. Sequencer, bass & synth",
     "Tap a step or drag to paint; VELOCITY / PROBABILITY / MORE edit vertically. BASS: a 303 line from C0 to C5 "
     "\xE2\x80\x94 pick a step and play a key, or draw notes. SYNTH: pick a step, a key adds or removes a note of "
     "its chord (up to 4; LEN, VEL); knob pages OSC \xE2\x80\xA6 VOICE, PATCH. KEYS: two octaves live (several "
     "fingers), not recorded."},
    {"4. Knobs & faders",
     "Drag vertically; double tap = default. VOLUME: output level (speaker, headphones and Bluetooth keep their "
     "own). Pad level: TRIM \xE2\x86\x92 PAD VOL."},
    {"5. Sample editor",
     "Drag the START / END handles; double tap a handle to snap it to a zero crossing. SLICE: AUTO or GRID, tap a "
     "slice, TO PADS spreads the slices over the pads. Hold LAYER: load / clear / mode."},
    {"6. Scenes & song",
     "A\xE2\x80\x93" "D picks the scene (while playing it changes on the beat \xE2\x80\x94 Launch quantization). The "
     "timeline cells arrange the song, FOLLOW plays it in order. GRID and LENGTH: division and length. UNDO (8 s) "
     "takes back RANDOM and CLEAR."},
    {"7. Mixer & FX",
     "Faders in dB, M / S and PAN per channel, live meters; a pad goes to the channel set in SAMPLE DETAILS "
     "\xE2\x86\x92 OUTPUT. FX view: REVERB, DELAY, the LO-FI / WIDE / PUMP / SPACE macros and the limiter."},
    {"8. Project, SD & settings",
     "SAVE, NEW PROJECT (empty), DELETE, EXPORT JSON. The corner marks enlarge a panel (the main one: next to the "
     "tabs; BASS, SYNTH: the whole screen); in the large BROWSER a category opens its files at once. Colors and "
     "Interface size: Settings \xE2\x86\x92 Display."},
    {"9. Tab5 Keyboard (like Live)",
     "Space play / stop, Shift+Space continue, F9 record, Tab / Shift+Tab next / previous view. A W S E D F T G Y "
     "H U J K: notes (BASS: the 303, SYNTH: the synth, else the pads), Z / X octave, C / V velocity, M notes on / off."},
    {"10. Keyboard: more keys",
     "Ctrl+Z undo, Ctrl+S save, Ctrl+O / Ctrl+N projects, Ctrl+, settings, Esc closes. F1\xE2\x80\x93" "F8 mute pads, "
     "Up / Down scene, Left / Right pad, 0 mutes the pad, F11 enlarges the work area, "
     "Shift+F1 this guide. Text fields take typing."},
};

}  // namespace

void open()
{
    lv_obj_t* body = modalOpen("UI GUIDE / INTERACTIONS", 824, 410, 650);
    if (!body) return;
    // .help-grid: padding 21 26, two equal columns, gap 11 18.
    lv_obj_t* grid = lv_obj_create(body);
    lv_obj_remove_style_all(grid);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(grid, 26, 0);
    lv_obj_set_style_pad_ver(grid, 20, 0);
    lv_obj_set_style_pad_column(grid, 18, 0);
    lv_obj_set_style_pad_row(grid, 11, 0);
    static const int32_t kCols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t kRowsDsc[kCardRows + 1] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                                                    LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, kCols, kRowsDsc);
    lv_obj_t* cards[kCardCount] = {};
    for (int i = 0; i < kCardCount; ++i) {
        // .help-grid > div: control bg, line border, padding 14, radius 7.
        lv_obj_t* card = lv_obj_create(grid);
        lv_obj_remove_style_all(card);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, c(col::control), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, c(col::line), 0);
        lv_obj_set_style_radius(card, 7, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_pad_row(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_grid_cell(card, LV_GRID_ALIGN_START, i % 2, 1, LV_GRID_ALIGN_START, i / 2, 1);
        lv_obj_set_size(card, kCardW, LV_SIZE_CONTENT);
        cards[i] = card;
        label(card, kCards[i].title, f_s15, col::accent);
        char wrapped[512];
        wrapText(wrapped, sizeof wrapped, kCards[i].text, f_r13, kTextW - 2);
        lv_obj_t* t = label(card, wrapped, f_r13, col::muted);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(t, 2, 0);
    }
    // Both cards of a row get the height of the taller one (CSS grid rows stretch their cells).
    lv_obj_update_layout(grid);
    for (int r = 0; r < kCardRows; ++r) {
        const int32_t h0 = lv_obj_get_height(cards[r * 2]), h1 = lv_obj_get_height(cards[r * 2 + 1]);
        const int32_t h = h0 > h1 ? h0 : h1;
        lv_obj_set_height(cards[r * 2], h);
        lv_obj_set_height(cards[r * 2 + 1], h);
    }
    modalScale(modalScaleFor(1.0f));   // Display > Interface size
}

}  // namespace dlg_help
}  // namespace orbit::ui
