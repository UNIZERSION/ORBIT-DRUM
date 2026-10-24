// BROWSER categories panel (#l-browser, left bottom): head with the search icon and the eight
// category rows (.l-category). Tapping a row selects the library category and opens the BROWSER
// view; the search icon opens the BROWSER view with its search field focused.
// Refresh: only the two rows whose active look changes are touched.
#include "ui/ui.h"

#include "ui/theme.h"
#include "ui/ui_internal.h"
#include "ui/widgets.h"

namespace orbit::ui::catpanel {

namespace {

constexpr int kRows = 8;
constexpr int kRowX = 8, kRowY = 36, kRowW = 270, kRowH = 30;   // relative to the panel content
const Category kCats[kRows] = {Category::Kits,  Category::All,    Category::Drums, Category::Bass,
                               Category::Synth, Category::Vocals, Category::Fx,    Category::User};
const Icon kIcons[kRows] = {Icon::Layers, Icon::Folder, Icon::Wave,  Icon::Eq,
                            Icon::Mixer,  Icon::Volume, Icon::Spark, Icon::Folder};
const uint32_t &kText = col::cat_text, &kIcon = col::cat_icon;   // active rows: col::accent_dark (theme)

struct Row {
    lv_obj_t* obj;
    lv_obj_t* icon;
    lv_obj_t* text;
    lv_obj_t* chev;
};
Row s_rows[kRows] = {};
lv_obj_t* s_panel = nullptr;
int s_drawn = -1;
bool s_first = true;
uint32_t s_rev_library = 0, s_rev_view = 0;
lv_style_t s_row_style, s_row_pressed, s_row_active;
bool s_styles = false;

void initStyles()
{
    if (s_styles) return;
    s_styles = true;
    // .l-category: transparent, 1 px #d4d7d9 bottom line, 11 px #34383a -> 13 px.
    lv_style_init(&s_row_style);
    lv_style_set_bg_opa(&s_row_style, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s_row_style, c(col::cat_press));
    lv_style_set_border_side(&s_row_style, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_row_style, 1);
    lv_style_set_border_color(&s_row_style, c(col::cat_line));
    lv_style_set_text_font(&s_row_style, f_r13);
    lv_style_set_text_color(&s_row_style, c(kText));
    // :hover (= touch pressed) #d5d7d8
    lv_style_init(&s_row_pressed);
    lv_style_set_bg_opa(&s_row_pressed, LV_OPA_COVER);
    // .active: #d7dadd background, #e95021 text
    lv_style_init(&s_row_active);
    lv_style_set_bg_opa(&s_row_active, LV_OPA_COVER);
    lv_style_set_bg_color(&s_row_active, c(col::cat_active));
    lv_style_set_text_color(&s_row_active, c(col::accent_dark));
}

void rowCb(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kRows) return;
    app().setCategory(kCats[i]);
    app().setView(View::Browser);
}

void searchCb(lv_event_t*)
{
    app().setView(View::Browser);
    ensureView(View::Browser);   // views are created lazily
    view_browser::focusSearch();
}

void setActive(int i, bool on)
{
    if (i < 0 || i >= kRows) return;
    Row& r = s_rows[i];
    lv_obj_set_state(r.obj, LV_STATE_CHECKED, on);
    const lv_color_t ic = c(on ? col::accent_dark : kIcon);
    lv_obj_set_style_image_recolor(r.icon, ic, 0);
    lv_obj_set_style_image_recolor(r.chev, ic, 0);
}

}  // namespace

lv_obj_t* create(lv_obj_t* scr)
{
    initStyles();
    lv_obj_t* panel = panelCreate(scr, geo::browser_x, geo::browser_y, geo::browser_w, geo::browser_h);
    if (bg::shadowsBaked()) lv_obj_set_style_shadow_width(panel, 0, 0);
    s_panel = panel;
    panelTopHighlight(panel, geo::browser_w);
    lv_obj_t* right = panelHead(panel, Icon::Grid, "BROWSER");
    lv_obj_set_style_pad_column(right, 6, 0);
    panelMaxButton(right, Panel::Browser);   // 0.3: maximize
    lv_obj_t* search = iconBtn(right, Icon::Search, 20, 30);
    lv_obj_add_event_cb(search, searchCb, LV_EVENT_CLICKED, nullptr);

    for (int i = 0; i < kRows; ++i) {
        lv_obj_t* o = lv_obj_create(panel);
        lv_obj_remove_style_all(o);
        lv_obj_add_style(o, &s_row_style, 0);
        lv_obj_add_style(o, &s_row_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(o, &s_row_active, LV_STATE_CHECKED);
        // No shared scale-on-press style here: on a 270x30 row it renders through a ~32 KB ARGB
        // layer (PSRAM) on every frame of the press transition; the #d5d7d8 tint is the feedback.
        lv_obj_set_pos(o, kRowX, kRowY + i * kRowH);
        lv_obj_set_size(o, kRowW, kRowH);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_ext_click_area(o, 0);
        rippleAttach(o);
        lv_obj_add_event_cb(o, rowCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        Row& r = s_rows[i];
        r.obj = o;
        r.icon = iconCreate(o, kIcons[i], 16, kIcon);
        lv_obj_align(r.icon, LV_ALIGN_LEFT_MID, 16, 0);
        r.text = lv_label_create(o);
        lv_label_set_text(r.text, categoryName(kCats[i]));
        lv_obj_align(r.text, LV_ALIGN_LEFT_MID, 42, 0);
        r.chev = iconCreate(o, Icon::Chev, 14, kIcon);
        lv_obj_align(r.chev, LV_ALIGN_LEFT_MID, 240, 0);
    }
    s_drawn = -1;
    s_first = true;
    refresh();
    return panel;
}

void refresh()
{
    if (!s_panel) return;
    App& a = app();
    const Revs& r = a.revs();
    if (r.library == s_rev_library && r.view == s_rev_view && !s_first) return;
    s_first = false;
    s_rev_library = r.library;
    s_rev_view = r.view;
    int want = -1;
    for (int i = 0; i < kRows; ++i) {
        if (kCats[i] == a.ui().category) want = i;
    }
    if (want == s_drawn) return;
    setActive(s_drawn, false);
    setActive(want, true);
    s_drawn = want;
}

}  // namespace orbit::ui::catpanel
