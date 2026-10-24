// UI GUIDE dialog (prototype .help-grid): eight cards in two columns describing how the real
// controls of ORBIT DRUM work (Polish text, English card titles like the prototype).
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

const Card kCards[8] = {
    {"1. Pads & velocity",
     "Dotknięcie padu gra od razu, także kilkoma palcami naraz (siła: Ustawienia "
     "\xE2\x86\x92 Touch, Fixed / Position). Dotknięcie wybiera pad do edycji; "
     "REC + PLAY nagrywa uderzenia do sceny."},
    {"2. Mute / Solo",
     "Włącz MUTE albo SOLO w nagłówku PADS i dotykaj padów, aby je wyciszać lub solować "
     "(znak M / S na padzie). Ponowne dotknięcie przycisku wraca do grania."},
    {"3. Sequencer & bass",
     "Dotknij kroku lub przeciągnij, aby malować; VELOCITY / PROBABILITY / MORE w pionie. BASS: linia "
     "303 \xE2\x80\x94 wybierz krok i zagraj klawisz albo rysuj nuty; KEYS: dwie oktawy na żywo (kilka palców naraz), bez zapisu."},
    {"4. Knobs & faders",
     "Przeciągnij w pionie; podwójne dotknięcie = domyślna. VOLUME: głośność wyjścia (głośnik, "
     "słuchawki i BT pamiętają swoją). Głośność padu: TRIM \xE2\x86\x92 PAD VOL."},
    {"5. Sample editor",
     "Przeciągaj uchwyty START / END; podwójne dotknięcie uchwytu przyciąga do przejścia przez "
     "zero. SLICE: AUTO lub GRID, dotknij plastra, TO PADS rozkłada plastry na pady. "
     "Przytrzymaj LAYER: wczytaj / wyczyść / tryb."},
    {"6. Scenes & song",
     "A\xE2\x80\x93" "D wybiera scenę (podczas grania zmiana w takt \xE2\x80\x94 Launch quantization). "
     "Komórki osi układają piosenkę, FOLLOW gra ją po kolei. GRID i LENGTH: podział i długość. "
     "UNDO (8 s) cofa RANDOM i CLEAR."},
    {"7. Mixer & FX",
     "Suwaki w dB, M / S i PAN na kanał, mierniki na żywo; pad trafia na kanał z SAMPLE DETAILS "
     "\xE2\x86\x92 OUTPUT. Widok FX: REVERB, DELAY, makra LO-FI / WIDE / PUMP / SPACE i limiter."},
    {"8. Project, SD & settings",
     "SAVE, NEW PROJECT (pusty), DELETE, EXPORT JSON. Narożniki powiększają panel (okno główne: obok "
     "zakładek; BASS: cały ekran). Kolory (tło, akcent): Ustawienia \xE2\x86\x92 Display."},
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
    static const int32_t kRowsDsc[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                                       LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, kCols, kRowsDsc);
    lv_obj_t* cards[8] = {};
    for (int i = 0; i < 8; ++i) {
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
    for (int r = 0; r < 4; ++r) {
        const int32_t h0 = lv_obj_get_height(cards[r * 2]), h1 = lv_obj_get_height(cards[r * 2 + 1]);
        const int32_t h = h0 > h1 ? h0 : h1;
        lv_obj_set_height(cards[r * 2], h);
        lv_obj_set_height(cards[r * 2 + 1], h);
    }
}

}  // namespace dlg_help
}  // namespace orbit::ui
