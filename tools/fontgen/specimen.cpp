// Font specimen renderer (tools/fontgen/specimen.sh builds it): draws every candidate font of
// specimen_cands.h with the headless LVGL host, one row per candidate, so rows can be stacked
// next to crops of reference/shots with pngtool.py. Usage: specimen out.png
#include <cstdio>
#include "lvgl_host.h"

#define X(sym, text, ls, bg, fg) extern "C" const lv_font_t sym;
#include "specimen_cands.h"
#undef X

struct Cand {
    const lv_font_t* font;
    const char* text;
    int ls;
    uint32_t bg, fg;
    const char* name;
};

static const Cand kCands[] = {
#define X(sym, text, ls, bg, fg) {&sym, text, ls, bg, fg, #sym},
#include "specimen_cands.h"
#undef X
};

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "specimen.png";
    lvhost_init(1280, 720);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x808488), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0, x = 0;
    for (const Cand& c : kCands) {
        const int h = c.font->line_height + 8;
        if (y + h > 720) { y = 0; x += 640; }
        lv_obj_t* row = lv_obj_create(scr);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, x, y);
        lv_obj_set_size(row, 400, h);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(c.bg), 0);
        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, c.text);
        lv_obj_set_style_text_font(l, c.font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(c.fg), 0);
        lv_obj_set_style_text_letter_space(l, c.ls, 0);
        lv_obj_set_pos(l, 6, 4);
        lv_obj_t* n = lv_label_create(scr);
        lv_label_set_text(n, c.name);
        lv_obj_set_style_text_color(n, lv_color_hex(0xffffff), 0);
        lv_obj_set_pos(n, x + 410, y + 2);
        printf("%d %d %d %s\n", x, y, h, c.name);
        y += h + 2;
    }
    lvhost_run(50);
    return lvhost_screenshot(out) ? 0 : 1;
}
