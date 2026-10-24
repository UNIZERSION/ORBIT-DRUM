// PROJECT MANAGER dialog (prototype .project-list): the current project card (name, pads /
// scenes / tempo, ACTIVE, "● niezapisane zmiany"), the projects saved on the SD card
// (/sdcard/ORBIT/PROJECTS/*.orb) as .project-entry rows, and NEW PROJECT / EXPORT JSON / SAVE.
// Opening another project or starting a new one while there are unsaved changes needs a second
// tap within 3 s (a toast explains it); so does DELETE on a row (or on the card: the open
// project's file goes, the project stays open and unsaved). A private 250 ms timer keeps the card and the list in
// step with the App (save finished, project loaded) while the dialog is open.
#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui {

namespace dlg_project {
namespace {

constexpr int kMaxList = 64;
constexpr uint32_t kConfirmMs = 3000;
const char* const kNewKey = "\x01new";

struct Dlg {
    bool open = false;
    lv_obj_t* name = nullptr;
    lv_obj_t* info = nullptr;
    lv_obj_t* dirty = nullptr;
    lv_obj_t* entries = nullptr;
    lv_obj_t* save = nullptr;
    lv_obj_t* exp = nullptr;
    lv_obj_t* card_del = nullptr;   // DELETE on the card: only while the open project has its file
    lv_timer_t* timer = nullptr;
    uint32_t rev_project = 0;
    uint32_t rev_transport = 0;
    int shown_dirty = -1;
    char pending[32] = {};
    uint32_t pending_ms = 0;
    int count = 0;
};
Dlg d;
char g_names[kMaxList][32];

lv_style_t s_entry, s_entry_pr, s_card;
bool s_styles = false;

void initStyles()
{
    if (s_styles) return;
    s_styles = true;
    // .project-entry: control bg, line border, radius 7, padding 15, gap 14; :hover accent border.
    lv_style_init(&s_entry);
    lv_style_set_bg_opa(&s_entry, LV_OPA_COVER);
    lv_style_set_bg_color(&s_entry, c(col::control));
    lv_style_set_border_width(&s_entry, 1);
    lv_style_set_border_color(&s_entry, c(col::line));
    lv_style_set_radius(&s_entry, 7);
    lv_style_set_pad_hor(&s_entry, 15);
    lv_style_set_pad_column(&s_entry, 14);
    lv_style_set_text_font(&s_entry, f_r14);
    lv_style_set_text_color(&s_entry, c(col::ink));
    lv_style_init(&s_entry_pr);
    lv_style_set_border_color(&s_entry_pr, c(col::accent));
    lv_style_set_bg_color(&s_entry_pr, c(col::hover));
    // .project-current: accent border, active-bg.
    lv_style_init(&s_card);
    lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
    lv_style_set_bg_color(&s_card, c(col::active_bg));
    lv_style_set_border_width(&s_card, 1);
    lv_style_set_border_color(&s_card, c(col::accent));
    lv_style_set_radius(&s_card, 7);
    lv_style_set_pad_hor(&s_card, 15);
    lv_style_set_pad_column(&s_card, 14);
}

lv_obj_t* plain(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// Bounded UTF-8 safe copy (no "%s" formatting: GCC's truncation check knows the source sizes).
void copyText(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) return;
    size_t n = 0;
    if (src) {
        while (n + 1 < cap && src[n]) ++n;
        if (src[n])
            while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;   // do not split a character
        memcpy(dst, src, n);
    }
    dst[n] = 0;
}

void setText(lv_obj_t* l, const char* t)
{
    if (!l || !t) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

// A second tap on the same target within 3 s confirms dropping unsaved changes.
bool confirmDiscard(const char* key)
{
    if (!app().dirty()) return true;
    if (strcmp(d.pending, key) == 0 && lv_tick_elaps(d.pending_ms) < kConfirmMs) {
        d.pending[0] = 0;
        return true;
    }
    copyText(d.pending, sizeof d.pending, key);
    d.pending_ms = lv_tick_get();
    app().toast("Dotknij ponownie, aby porzucić zmiany");
    return false;
}

void onEntry(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= d.count) return;
    char name[32];
    copyText(name, sizeof name, g_names[i]);
    if (!confirmDiscard(name)) return;
    if (app().open(name)) modalClose();
    else app().toast("Nie można otworzyć projektu");
}

void update(bool force);

// DELETE: a second tap on the same project within 3 s deletes it (i = -1: the open project's file).
void onDelete(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    char name[32];
    if (i == -1) copyText(name, sizeof name, app().project().name);
    else if (i >= 0 && i < d.count) copyText(name, sizeof name, g_names[i]);
    else return;
    char key[32];
    key[0] = '';
    copyText(key + 1, sizeof key - 1, name);
    if (strcmp(d.pending, key) != 0 || lv_tick_elaps(d.pending_ms) >= kConfirmMs) {
        copyText(d.pending, sizeof d.pending, key);
        d.pending_ms = lv_tick_get();
        app().toast("Dotknij DELETE ponownie, aby usunąć %s", name);
        return;
    }
    d.pending[0] = 0;
    app().deleteProject(name);
    update(true);
}

void updateCard()
{
    const Project& p = app().project();
    setText(d.name, p.name);
    if (d.card_del) {
        if (app().projectHasFile()) lv_obj_remove_flag(d.card_del, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(d.card_del, LV_OBJ_FLAG_HIDDEN);
    }
    char t[96];
    snprintf(t, sizeof t, "%d pads \xE2\x80\xA2 %d scenes \xE2\x80\xA2 %d BPM", kPads, kScenes, (int)(p.bpm + 0.5f));
    setText(d.info, t);
}

void rebuildList()
{
    if (!d.entries) return;
    lv_obj_clean(d.entries);
    d.count = 0;
    if (!app().sdReady()) {
        lv_obj_t* l = label(d.entries,
                            "Brak karty SD. Projekty zapisują się w /sdcard/ORBIT/PROJECTS/ \xE2\x80\x94 włóż kartę "
                            "(FAT32 lub exFAT), aby zapisywać i otwierać projekty. Bieżący projekt działa dalej.",
                            f_r14, col::muted);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_ver(l, 8, 0);
        return;
    }
    static char all[kMaxList][32];
    const int n = app().listProjects(all, kMaxList);
    const char* cur = app().project().name;
    // The card above is the current project; its name is hidden from the list only when that file IS the
    // current project (opened or saved this session). Otherwise a same-named project on the card (e.g.
    // yesterday's ORBIT_01) is another project and stays listed.
    const bool own = app().projectHasFile();
    for (int i = 0; i < n && d.count < kMaxList; ++i) {
        if (own && strcmp(all[i], cur) == 0) continue;   // shown as the card above
        copyText(g_names[d.count], sizeof g_names[d.count], all[i]);
        ++d.count;
    }
    if (d.count == 0) {
        lv_obj_t* l = label(d.entries, "Brak innych zapisanych projektów w /sdcard/ORBIT/PROJECTS/", f_r14, col::muted);
        lv_obj_set_style_pad_ver(l, 8, 0);
        return;
    }
    for (int i = 0; i < d.count; ++i) {
        lv_obj_t* r = lv_obj_create(d.entries);
        lv_obj_remove_style_all(r);
        lv_obj_add_style(r, &s_entry, 0);
        lv_obj_add_style(r, &s_entry_pr, LV_STATE_PRESSED);
        lv_obj_set_size(r, LV_PCT(100), 50);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        iconCreate(r, Icon::Folder, 18, col::ink);
        lv_obj_t* t = lv_label_create(r);
        lv_label_set_text(t, g_names[i]);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_t* del = actionBtn(r, "DELETE", false, 0, 32);
        lv_obj_add_event_cb(del, onDelete, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        iconCreate(r, Icon::Chev, 18, col::ink);
        lv_obj_add_event_cb(r, onEntry, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        rippleAttach(r);
    }
}

void update(bool force)
{
    if (!d.open) return;
    const Revs& r = app().revs();
    if (force || r.project != d.rev_project) {
        d.rev_project = r.project;
        updateCard();
        rebuildList();
    } else if (r.transport != d.rev_transport) {
        updateCard();   // tempo
    }
    d.rev_transport = r.transport;
    const int dirty = app().dirty() ? 1 : 0;
    if (dirty != d.shown_dirty) {
        d.shown_dirty = dirty;
        if (dirty) lv_obj_remove_flag(d.dirty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(d.dirty, LV_OBJ_FLAG_HIDDEN);
    }
}

void onTimer(lv_timer_t*) { update(false); }

void onNew(lv_event_t*)
{
    if (!confirmDiscard(kNewKey)) return;
    app().newProject();
    modalClose();
}

void onExport(lv_event_t*) { app().exportJson(); }

void onSave(lv_event_t*)
{
    app().save();
    update(true);
}

void onClosed(void*)
{
    if (d.timer) lv_timer_delete(d.timer);
    d = Dlg{};
}

}  // namespace

void open()
{
    initStyles();
    lv_obj_t* body = modalOpen("PROJECT MANAGER", 824, 415, 650);
    if (!body) return;
    d = Dlg{};
    d.open = true;
    modalOnClose(onClosed, nullptr);

    // .project-list: padding 20 26, entries 10 px apart.
    lv_obj_t* list = plain(body);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(list, 26, 0);
    lv_obj_set_style_pad_ver(list, 20, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* card = plain(list);
    lv_obj_add_style(card, &s_card, 0);
    lv_obj_set_size(card, LV_PCT(100), 69);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    iconCreate(card, Icon::Folder, 26, col::ink);
    lv_obj_t* text = plain(card);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 4, 0);
    d.name = label(text, "", f_s15, col::ink);
    d.info = label(text, "", f_r13, col::muted);
    d.dirty = label(card, "\xE2\x97\x8F niezapisane zmiany", f_r13, col::accent_dark);
    lv_obj_add_flag(d.dirty, LV_OBJ_FLAG_HIDDEN);
    label(card, "ACTIVE", f_m12, col::accent);
    d.card_del = actionBtn(card, "DELETE", false, 0, 32);
    lv_obj_add_event_cb(d.card_del, onDelete, LV_EVENT_CLICKED, (void*)(intptr_t)-1);

    d.entries = plain(list);
    lv_obj_set_size(d.entries, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(d.entries, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(d.entries, 10, 0);

    lv_obj_t* foot = modalFooter("");
    lv_obj_t* b = actionBtn(foot, "NEW PROJECT", false, 0, 34);
    lv_obj_add_event_cb(b, onNew, LV_EVENT_CLICKED, nullptr);
    d.exp = actionBtn(foot, "EXPORT JSON", false, 0, 34);
    lv_obj_add_event_cb(d.exp, onExport, LV_EVENT_CLICKED, nullptr);
    d.save = actionBtn(foot, "SAVE", true, 0, 34);
    lv_obj_add_event_cb(d.save, onSave, LV_EVENT_CLICKED, nullptr);
    if (!app().sdReady()) {
        lv_obj_add_state(d.exp, LV_STATE_DISABLED);
        lv_obj_add_state(d.save, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(d.save, c(col::tab), LV_STATE_DISABLED);
        lv_obj_set_style_border_color(d.save, c(col::line), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(d.save, c(col::muted), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(d.exp, c(col::muted), LV_STATE_DISABLED);
    }

    update(true);
    d.timer = lv_timer_create(onTimer, 250, nullptr);
}

}  // namespace dlg_project
}  // namespace orbit::ui
